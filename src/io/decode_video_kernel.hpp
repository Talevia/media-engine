/*
 * io::decode_video kernel registration.
 *
 * Registers TaskKindId::IoDecodeVideo. Opens a video decoder for the first
 * video stream of the upstream DemuxContext, seeks to the requested asset-
 * local time, and emits the AVFrame whose pts is >= target.
 *
 * Schema:
 *   inputs:  [source: DemuxCtx]
 *   outputs: [frame:  AvFrameHandle]
 *   params:  [source_t_num: Int64,
 *             source_t_den: Int64]   (both required)
 *
 * Properties carry the asset-local target time as a rational so the same
 * graph compiled for different timeline moments produces distinct
 * content_hash entries (D1 in the implementation plan). EvalContext.time
 * is *also* available — kernels that want to participate in the cache key
 * via time can read it from TaskContext, but this kernel reads source_t
 * from props for content-addressing.
 */
#pragma once

namespace me::io {
void register_decode_video_kind();
}
