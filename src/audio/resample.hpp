/*
 * me::audio::resample — libswresample wrapper for per-call sample-rate /
 * format / channel-layout conversion.
 *
 * Scope-A slice of `audio-mix-resample` (bullet 3-4 cycles total).
 * Provides a pure one-shot conversion helper that the upcoming
 * AudioMixScheduler will call per audio source per mix window. The
 * scheduler + sink integration + e2e tests are separate follow-ups.
 *
 * Shape:
 *   - Input: a decoded `AVFrame` (any libav sample format / rate /
 *     channel layout — typically what the codec decoded to).
 *   - Output: target rate + sample format + channel layout, returned
 *     as a freshly-allocated AVFrame. Caller owns; free via
 *     `av_frame_free`.
 *   - Each call builds + frees its own SwrContext. The scheduler
 *     cycle can cache per (src_fmt/dst_fmt) SwrContext if profiling
 *     says this is a hot path.
 *
 * Determinism: libswresample with a given (src_fmt, src_rate,
 * src_ch_layout, dst_fmt, dst_rate, dst_ch_layout) produces the same
 * bytes across hosts (no SIMD runtime dispatch beyond the build-time
 * choice). Same AVFrame in → same bytes out.
 */
#pragma once

#include "media_engine/types.h"

#include <string>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

struct AVFrame;

namespace me::audio {

/* Resample `src` into a newly-allocated AVFrame matching the target
 * parameters. The output AVFrame has format == dst_fmt, sample_rate
 * == dst_rate, ch_layout == dst_ch_layout, and nb_samples set to
 * whatever swr_convert produced this call.
 *
 * Ownership: on success, `*out` is a new AVFrame the caller owns
 * (free via `av_frame_free(&f)`). On failure, `*out` is nullptr and
 * err gets a diagnostic.
 *
 * Common failures:
 *   - null args                       → ME_E_INVALID_ARG
 *   - swr_alloc/swr_init failure      → ME_E_INTERNAL
 *   - out-of-memory allocating dst    → ME_E_OUT_OF_MEMORY
 *   - swr_convert failure             → ME_E_INTERNAL */
me_status_t resample_to(const AVFrame*          src,
                         int                    dst_rate,
                         AVSampleFormat         dst_fmt,
                         const AVChannelLayout& dst_ch_layout,
                         AVFrame**              out,
                         std::string*           err);

}  // namespace me::audio
