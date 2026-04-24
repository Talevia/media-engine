/*
 * me::orchestrator::compose_audio_* — extracted audio-side helpers
 * for ComposeSink::process().
 *
 * Scope-A slice of compose_sink.cpp decomposition (was 694 lines
 * pre-split, 633 after transition extraction + subsequent feature
 * additions). The AudioMixer setup block (~45 lines) and the
 * audio drain/flush block (~50 lines) are self-contained — pull
 * them into a sibling TU to keep ComposeSink::process focused on
 * the video compose loop.
 *
 * Contracts:
 *   - `setup_compose_audio_mixer`: given the encoder produced by
 *     setup_h264_aac_encoder_mux + timeline + demuxes + codec
 *     pool, builds AudioMixer if the timeline has audio tracks.
 *     Returns ME_OK (mixer populated) or ME_OK with mixer reset
 *     (no audio tracks / loader-declared but no clips / mixer
 *     cfg invalid). Propagates real errors from
 *     build_audio_mixer_for_timeline.
 *   - `drain_compose_audio`: drains either AudioMixer output
 *     (when mixer != null) or the legacy FIFO (video-only
 *     timelines) into the AAC encoder + mux, then flushes the
 *     encoder. Common flush-null-frame terminates both paths.
 *
 * Neither helper touches video state; safe to call before / after
 * the per-frame video loop as the surrounding orchestration
 * dictates.
 */
#pragma once

#include "audio/mixer.hpp"
#include "io/demux_context.hpp"
#include "media_engine/types.h"
#include "orchestrator/reencode_segment.hpp"
#include "resource/codec_pool.hpp"
#include "timeline/timeline_impl.hpp"

#include <memory>
#include <string>
#include <vector>

namespace me::orchestrator {

/* Detect audio tracks in `tl` and, if any exist, build a mixer
 * whose target format matches the AAC encoder's params (from
 * `shared.aenc`). On OK, `out_mixer` is either populated with a
 * live mixer or reset (no audio tracks found).
 *
 * Error path: propagates build_audio_mixer_for_timeline's
 * non-ME_E_NOT_FOUND errors. `shared.aenc` null = no-op (no
 * audio encoder means no audio output possible). */
me_status_t setup_compose_audio_mixer(
    const me::Timeline&                                       tl,
    const std::vector<std::shared_ptr<me::io::DemuxContext>>& demuxes,
    const detail::SharedEncState&                             shared,
    me::resource::CodecPool*                                  pool,
    std::unique_ptr<me::audio::AudioMixer>&                   out_mixer,
    std::string*                                              err);

/* Drain + flush the audio encoder. With a live mixer: pull mixed
 * frames, stamp PTS from `shared.next_audio_pts`, feed to encoder,
 * advance counter. Without a mixer: drain the legacy FIFO (empty
 * for current ComposeSink callers; produces declared-but-empty
 * audio stream). Common path: flush encoder with null frame. */
me_status_t drain_compose_audio(
    me::audio::AudioMixer*      mixer,
    detail::SharedEncState&     shared,
    std::string*                err);

}  // namespace me::orchestrator
