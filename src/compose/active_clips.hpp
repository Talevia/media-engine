/*
 * me::compose::active_clips_at — per-frame multi-track active-clip resolver.
 *
 * Given a Timeline and a timeline-coordinate time T, returns the list of
 * (track_idx, clip_idx, source_time) triples identifying which clip is
 * "playing" on each track at that moment, and what source-media time
 * maps to T. This is the first half of the multi-track compose
 * scheduler: the kernel (alpha_over.hpp) knows how to blend pixels;
 * this helper knows which pixels to ask for.
 *
 * Semantics (aligns with NLE convention):
 *   - Each clip occupies [clip.time_start, clip.time_start + clip.time_duration)
 *     on its track (half-open — the end boundary belongs to the next
 *     clip if any, nothing otherwise).
 *   - A track contributes 0 or 1 TrackActive entry per T; the loader
 *     already rejects within-track overlap (phase-1), so the active-
 *     clip lookup is single-result per track.
 *   - source_time = clip.source_start + (T - clip.time_start), using
 *     rational arithmetic throughout (no floats — rational time is the
 *     CLAUDE.md invariant).
 *
 * Output vector size equals the number of tracks with an active clip
 * at T (may be 0 if T is out of bounds for every track). Order is
 * stable: entries appear in Timeline::tracks declaration order,
 * which the compose kernel uses as bottom→top z-order. Tracks with
 * no active clip are SKIPPED (not emitted as a placeholder) — the
 * compositor simply doesn't draw that layer at T.
 *
 * Deterministic: linear walk over Timeline::clips, integer-only
 * rational compare. Same Timeline + same T → same result across hosts.
 */
#pragma once

#include "media_engine/types.h"

#include <cstddef>
#include <vector>

namespace me {
struct Timeline;
}

namespace me::compose {

struct TrackActive {
    std::size_t   track_idx;    /* index into Timeline::tracks */
    std::size_t   clip_idx;     /* index into Timeline::clips  */
    me_rational_t source_time;  /* clip.source_start + (T - clip.time_start) */
};

std::vector<TrackActive> active_clips_at(const me::Timeline& tl,
                                          me_rational_t      time);

}  // namespace me::compose
