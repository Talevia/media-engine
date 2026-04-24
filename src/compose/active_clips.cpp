/*
 * me::compose::active_clips_at impl. See active_clips.hpp for contract.
 */
#include "compose/active_clips.hpp"

#include "timeline/timeline_impl.hpp"

namespace me::compose {

namespace {

/* Rational compare: a <= b  <=>  a.num * b.den <= b.num * a.den (both
 * denominators are positive post-loader-validation). Avoids float.
 * Strict-less is analogous. */
inline bool r_lt(me_rational_t a, me_rational_t b) {
    return a.num * b.den < b.num * a.den;
}
inline bool r_le(me_rational_t a, me_rational_t b) {
    return a.num * b.den <= b.num * a.den;
}

/* Rational add: a + b = (a.num * b.den + b.num * a.den) / (a.den * b.den).
 * No normalization — denominators stay bounded because phase-1 frame
 * rates (30, 48000, 90000) multiply to manageable values within i64. */
inline me_rational_t r_add(me_rational_t a, me_rational_t b) {
    return me_rational_t{a.num * b.den + b.num * a.den, a.den * b.den};
}
inline me_rational_t r_sub(me_rational_t a, me_rational_t b) {
    return me_rational_t{a.num * b.den - b.num * a.den, a.den * b.den};
}

}  // namespace

std::vector<TrackActive> active_clips_at(const me::Timeline& tl,
                                          me_rational_t      time) {
    std::vector<TrackActive> out;
    out.reserve(tl.tracks.size());

    /* Walk tracks in declaration order — the compose kernel consumes
     * output in this same order (bottom → top). For each track, find
     * the one clip (if any) whose [time_start, time_start + duration)
     * covers T. Loader enforces within-track non-overlap so at most
     * one match. */
    for (std::size_t ti = 0; ti < tl.tracks.size(); ++ti) {
        const std::string& tid = tl.tracks[ti].id;

        for (std::size_t ci = 0; ci < tl.clips.size(); ++ci) {
            const me::Clip& c = tl.clips[ci];
            if (c.track_id != tid) continue;

            /* Half-open [time_start, time_start + time_duration) */
            const me_rational_t end = r_add(c.time_start, c.time_duration);
            if (r_le(c.time_start, time) && r_lt(time, end)) {
                out.push_back(TrackActive{
                    /*track_idx=*/ti,
                    /*clip_idx=*/ ci,
                    /*source_time=*/ r_add(c.source_start,
                                            r_sub(time, c.time_start)),
                });
                break;   /* one clip per track at any T */
            }
        }
    }

    return out;
}

std::optional<ActiveTransition> active_transition_at(
    const me::Timeline& tl,
    std::size_t         track_idx,
    me_rational_t       time) {

    if (track_idx >= tl.tracks.size()) return std::nullopt;
    const std::string& tid = tl.tracks[track_idx].id;

    /* Find a transition on this track whose window covers `time`.
     * Window: [from.time_end - dur/2, from.time_end + dur/2).
     * `time` in that half-open interval → return it.
     * At most one transition per boundary (loader adjacency
     * enforcement) and no overlapping boundaries on a contiguous
     * track, so linear scan is unambiguous. */
    for (std::size_t ti = 0; ti < tl.transitions.size(); ++ti) {
        const me::Transition& tr = tl.transitions[ti];
        if (tr.track_id != tid) continue;

        /* Find the from_clip on this track. */
        me_rational_t from_end{0, 1};
        bool from_found = false;
        for (const me::Clip& c : tl.clips) {
            if (c.track_id == tid && c.id == tr.from_clip_id) {
                from_end = r_add(c.time_start, c.time_duration);
                from_found = true;
                break;
            }
        }
        if (!from_found) continue;

        /* half_dur = duration / 2. Keep in rational form:
         *   half = {num, den*2}. */
        const me_rational_t half_dur{tr.duration.num, tr.duration.den * 2};
        const me_rational_t window_start = r_sub(from_end, half_dur);
        const me_rational_t window_end   = r_add(from_end, half_dur);

        if (r_le(window_start, time) && r_lt(time, window_end)) {
            /* t = (time - window_start) / duration.
             * All in rational: numerator cross-multiply, then float-
             * divide at the end since t is a float in the kernel. */
            const me_rational_t dt = r_sub(time, window_start);
            const float t = static_cast<float>(dt.num) * static_cast<float>(tr.duration.den) /
                            (static_cast<float>(dt.den) * static_cast<float>(tr.duration.num));
            return ActiveTransition{ti, t};
        }
    }
    return std::nullopt;
}

FrameSource frame_source_at(const me::Timeline& tl,
                            std::size_t         track_idx,
                            me_rational_t       time) {
    FrameSource out;

    if (track_idx >= tl.tracks.size()) return out;

    /* Transition takes precedence over single-clip active resolution. */
    if (auto at = active_transition_at(tl, track_idx, time); at.has_value()) {
        out.kind       = FrameSourceKind::Transition;
        out.transition = *at;

        /* Resolve from_clip_idx + to_clip_idx + their source_time at T.
         * Transition stores clip ids; we need to find them in
         * Timeline::clips and compute source_time = source_start +
         * (T - clip.time_start) for each. Linear scan — one pass per
         * endpoint, same algorithm as active_clips_at but keyed by id
         * rather than by time-coverage. */
        const me::Transition& tr = tl.transitions[at->transition_idx];
        bool found_from = false;
        bool found_to   = false;
        for (std::size_t ci = 0; ci < tl.clips.size(); ++ci) {
            const me::Clip& c = tl.clips[ci];
            if (c.track_id != tr.track_id) continue;
            if (!found_from && c.id == tr.from_clip_id) {
                out.transition_from_clip_idx    = ci;
                out.transition_from_source_time =
                    r_add(c.source_start, r_sub(time, c.time_start));
                found_from = true;
            }
            if (!found_to && c.id == tr.to_clip_id) {
                out.transition_to_clip_idx    = ci;
                out.transition_to_source_time =
                    r_add(c.source_start, r_sub(time, c.time_start));
                found_to = true;
            }
            if (found_from && found_to) break;
        }
        /* Loader enforces from_clip_id / to_clip_id point at real
         * clips on the same track; the linear scan above is guaranteed
         * to find both. If it doesn't (malformed Timeline constructed
         * without going through the loader), the default-zero source
         * times are the least-surprising fallback. */
        return out;
    }

    /* No transition — fall back to single-clip lookup, filtered to
     * this track. active_clips_at does a full multi-track scan; we
     * filter in O(N) which is fine for phase-1 track counts (≤ 8
     * typical, loader has no hard cap). */
    const std::vector<TrackActive> all = active_clips_at(tl, time);
    for (const TrackActive& ta : all) {
        if (ta.track_idx == track_idx) {
            out.kind   = FrameSourceKind::SingleClip;
            out.single = ta;
            return out;
        }
    }

    /* Neither transition nor single-clip covers this track at T. */
    return out;
}

}  // namespace me::compose
