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

}  // namespace me::compose
