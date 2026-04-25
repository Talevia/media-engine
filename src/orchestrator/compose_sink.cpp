/*
 * Public surface of the multi-track compose path.
 *
 * Two responsibilities:
 *   1. is_gpu_compose_usable — predicate the future GPU compose
 *      branch + its unit tests both consult.
 *   2. make_compose_sink — validates the Timeline + spec against
 *      the compose path's preconditions, then delegates to the
 *      private factory in compose_sink_impl.cpp which constructs
 *      the actual ComposeSink class.
 *
 * The class itself + the per-frame compose loop wiring lives in
 * compose_sink_impl.cpp (split out at the 399-line point to keep
 * both files comfortably under the §1a 400-line ceiling). Public
 * callers continue to go through this header — the impl split is
 * a pure structural refactor.
 */
#include "orchestrator/compose_sink.hpp"

#include "gpu/gpu_backend.hpp"
#include "orchestrator/compose_sink_impl.hpp"
#include "timeline/timeline_impl.hpp"

#include <cstring>
#include <utility>

namespace me::orchestrator {

namespace {

bool streq(const char* a, const char* b) {
    return a && b && std::strcmp(a, b) == 0;
}

}  // namespace

bool is_gpu_compose_usable(const me::gpu::GpuBackend* gpu) noexcept {
    if (!gpu) return false;
    if (!gpu->available()) return false;
    const char* n = gpu->name();
    if (!n) return false;
    /* Must be a bgfx-<real-renderer> backend. Null backend's name
     * is "null"; Noop fallback's is "bgfx-Noop" — both unsuitable
     * for actual GPU compose (Noop reports available=true but its
     * draws are silent no-ops, producing no pixels). */
    if (std::strncmp(n, "bgfx-", 5) != 0) return false;
    if (std::strcmp(n, "bgfx-Noop") == 0) return false;
    return true;
}

std::unique_ptr<OutputSink> make_compose_sink(
    const me::Timeline&            tl,
    const me_output_spec_t&        spec,
    SinkCommon                     common,
    std::vector<ClipTimeRange>     clip_ranges,
    me::resource::CodecPool*       pool,
    const me::gpu::GpuBackend*     gpu_backend,
    std::string*                   err) {

    if (!streq(spec.video_codec, "h264") || !streq(spec.audio_codec, "aac")) {
        if (err) *err = "compose path (used for multi-track or transitions) "
                         "currently requires video_codec=h264 + audio_codec=aac; "
                         "other codecs (including passthrough) unsupported";
        return nullptr;
    }
    if (!pool) {
        if (err) *err = "compose path requires a CodecPool (engine->codecs)";
        return nullptr;
    }
    if (clip_ranges.empty()) {
        if (err) *err = "compose path requires at least one clip";
        return nullptr;
    }
    /* Accept multi-track OR any timeline with transitions. Single-
     * track no-transition timelines route through make_output_sink. */
    if (tl.tracks.size() < 2 && tl.transitions.empty()) {
        if (err) *err = "make_compose_sink: expected 2+ tracks or non-empty "
                         "transitions (simpler timelines route through "
                         "make_output_sink, not here)";
        return nullptr;
    }

    /* Per-track clip-count rule:
     *   - Track without any transition on it: exactly 1 clip
     *     (multi-clip concat on a non-transition track is
     *     the old "multi-clip-single-track compose" gap that
     *     needs decoder seek / source_start=0 semantics — not
     *     in this phase).
     *   - Track with at least one transition declared on it: 2+
     *     clips allowed (a transition's from_clip_id + to_clip_id
     *     point at two distinct clips on the same track). */
    std::vector<std::size_t> per_track_count(tl.tracks.size(), 0);
    for (const auto& c : tl.clips) {
        for (std::size_t ti = 0; ti < tl.tracks.size(); ++ti) {
            if (tl.tracks[ti].id == c.track_id) {
                ++per_track_count[ti];
                break;
            }
        }
    }
    std::vector<bool> track_has_transition(tl.tracks.size(), false);
    for (const auto& tr : tl.transitions) {
        for (std::size_t ti = 0; ti < tl.tracks.size(); ++ti) {
            if (tl.tracks[ti].id == tr.track_id) {
                track_has_transition[ti] = true;
                break;
            }
        }
    }
    for (std::size_t ti = 0; ti < per_track_count.size(); ++ti) {
        if (track_has_transition[ti]) continue;   /* transition tracks exempt */
        if (per_track_count[ti] != 1) {
            if (err) {
                *err = "compose phase-1: non-transition tracks must have "
                       "exactly 1 clip; track[" + std::to_string(ti) +
                       "] has " + std::to_string(per_track_count[ti]);
            }
            return nullptr;
        }
    }

    return detail::make_compose_sink_impl(
        tl,
        std::move(common),
        std::move(clip_ranges),
        pool,
        gpu_backend,
        spec.video_bitrate_bps,
        spec.audio_bitrate_bps);
}

}  // namespace me::orchestrator
