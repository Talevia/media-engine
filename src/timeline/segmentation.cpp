#include "timeline/segmentation.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace me::timeline {

namespace {

/* FNV-1a 64 — matches the hashing used by graph/graph.cpp so boundary_hash
 * and graph.content_hash can be freely combined into cache keys. */
constexpr uint64_t kFnv64Offset = 0xcbf29ce484222325ULL;
constexpr uint64_t kFnv64Prime  = 0x00000100000001b3ULL;

inline uint64_t mix_u64(uint64_t h, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        h ^= (v >> (i * 8)) & 0xff;
        h *= kFnv64Prime;
    }
    return h;
}

/* Rational ordering by cross-multiplication. Assumes den > 0. */
inline bool rat_less(me_rational_t a, me_rational_t b) {
    return a.num * b.den < b.num * a.den;
}
inline bool rat_eq(me_rational_t a, me_rational_t b) {
    return a.num * b.den == b.num * a.den;
}
inline me_rational_t rat_add(me_rational_t a, me_rational_t b) {
    /* a/b + c/d = (a*d + c*b) / (b*d). Bootstrap: no overflow guard. */
    return { a.num * b.den + b.num * a.den, a.den * b.den };
}

}  // namespace

std::vector<Segment> segment(const Timeline& tl) {
    std::vector<Segment> out;
    if (tl.duration.num == 0 || tl.clips.empty()) return out;

    /* Gather event times: each clip's start and end, plus timeline end.
     * Timeline start (t=0) is implicit. */
    std::vector<me_rational_t> events;
    events.reserve(tl.clips.size() * 2 + 2);
    events.push_back({0, 1});
    events.push_back(tl.duration);
    for (const auto& c : tl.clips) {
        events.push_back(c.time_start);
        events.push_back(rat_add(c.time_start, c.time_duration));
    }

    /* Sort + dedupe by rational equality. */
    std::sort(events.begin(), events.end(),
              [](auto a, auto b) { return rat_less(a, b); });
    events.erase(std::unique(events.begin(), events.end(),
                             [](auto a, auto b) { return rat_eq(a, b); }),
                 events.end());

    /* Emit segments for each consecutive (t_i, t_{i+1}) pair, bounded to
     * [0, tl.duration). */
    for (size_t i = 0; i + 1 < events.size(); ++i) {
        me_rational_t s = events[i];
        me_rational_t e = events[i + 1];
        if (!rat_less(s, tl.duration)) break;
        if (!rat_less({0, 1}, e)) continue;
        if (rat_less(tl.duration, e)) e = tl.duration;

        Segment seg;
        seg.start = s;
        seg.end   = e;

        /* A clip is active iff its [start, end) range covers [s, e) wholly
         * (since we split at every clip boundary, any partial overlap has
         * been eliminated). */
        for (uint32_t idx = 0; idx < tl.clips.size(); ++idx) {
            const auto& c = tl.clips[idx];
            me_rational_t ce = rat_add(c.time_start, c.time_duration);
            /* c.time_start <= s  AND  e <= ce */
            if (!rat_less(s, c.time_start) && !rat_less(ce, e)) {
                seg.active_clips.push_back(ClipRef{idx});
            }
        }

        uint64_t h = kFnv64Offset;
        for (const auto& cr : seg.active_clips)       h = mix_u64(h, cr.idx);
        /* transitions reserved for future IR extension */
        for (const auto& tr : seg.active_transitions) h = mix_u64(h, tr.idx);
        seg.boundary_hash = h;

        out.push_back(std::move(seg));
    }

    return out;
}

}  // namespace me::timeline
