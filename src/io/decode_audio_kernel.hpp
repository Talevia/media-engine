/*
 * io::decode_audio kernel registration.
 *
 * Registers TaskKindId::IoDecodeAudio. Symmetric to IoDecodeVideo:
 * opens an audio decoder for the first audio stream of the upstream
 * DemuxContext, seeks to the requested asset-local time, and emits
 * the AVFrame whose pts is >= target. Caller decides what to do with
 * the audio samples — RenderConvertRgba8's audio analogue is
 * AudioResample (commit 10).
 *
 * Schema:
 *   inputs:  [source: DemuxCtx]
 *   outputs: [frame:  AvFrameHandle]   — audio AVFrame at source format/rate
 *   params:  [source_t_num: Int64,
 *             source_t_den: Int64]    (both required)
 *
 * Same content-addressing model as IoDecodeVideo: source_t lives in
 * props (not ctx.time) so distinct chunk timestamps produce distinct
 * content_hash entries — chunk-per-Task dispatch lands repeated
 * identical reads in the OutputCache.
 */
#pragma once

namespace me::io {
void register_decode_audio_kind();
}
