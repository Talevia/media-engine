/*
 * audio::resample kernel registration.
 *
 * Registers TaskKindId::AudioResample. 1×AvFrameHandle (audio AVFrame)
 * + props → 1×AvFrameHandle (resampled audio AVFrame). Wraps the
 * existing me::audio::resample_to helper into the graph kernel ABI.
 *
 * Schema:
 *   inputs:  [src: AvFrameHandle]   (audio AVFrame, any source format/rate)
 *   outputs: [dst: AvFrameHandle]   (audio AVFrame at target format/rate)
 *   params:
 *     target_rate     (Int64, required — Hz, e.g. 48000)
 *     target_fmt      (Int64, optional — cast to AVSampleFormat, default AV_SAMPLE_FMT_FLTP = 8)
 *     target_channels (Int64, optional — 1=mono / 2=stereo / N for surround,
 *                      fed to av_channel_layout_default; default 2)
 *
 * Why no AVChannelLayout in props: the layout-bag is libav-flavoured
 * + non-trivial to serialize through graph::Properties. For phase-1
 * we accept channel count and call av_channel_layout_default —
 * covers the common 1ch / 2ch cases the audio path uses today. A
 * future extension can grow a per-layout TypeId or string-encoded
 * descriptor.
 *
 * time_invariant = true: same src + same target params → same bytes
 * (libswresample is deterministic for fixed parameters).
 * cacheable = true.
 */
#pragma once

namespace me::audio {
void register_resample_kind();
}
