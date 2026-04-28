/* `me::io::KvazaarHevcEncoder` — software HEVC Main encoder built
 * on Kvazaar (BSD-3, LGPL-clean per VISION §3.4 ship line). Fills
 * the M10 exit-criterion gap that VideoToolbox HW HEVC leaves open
 * on Linux / Windows / older Apple silicon: a deterministic-software
 * fallback that hosts opt into when the platform lacks
 * hevc_videotoolbox, when bit-exact output is required across
 * machines, or when a known-license SW path is needed for ship
 * builds (homebrew FFmpeg's libx265 is GPL — excluded by
 * CLAUDE.md anti-requirements).
 *
 * Bit depth. This cycle ships 8-bit YUV420P (HEVC Main profile).
 * Kvazaar's 10-bit support requires either a `KVZ_BIT_DEPTH=10`
 * library build or careful byte-buffer reinterpretation of the
 * 8-bit `picture_alloc` allocation; the homebrew bottle ships
 * with the default 8-bit configuration. Main 10 (10-bit YUV420P
 * for HDR PQ/HLG output) is tracked as the follow-up bullet
 * `encode-hevc-main10-kvazaar-source-build` — a from-source
 * Kvazaar CMake build with `-DKVZ_BIT_DEPTH=10` is the canonical
 * path. Until then SW HEVC is SDR-only; HDR Main 10 stays on
 * the VideoToolbox HW path.
 *
 * Build gating. CMake links this TU only when both
 * `ME_WITH_KVAZAAR=ON` and `pkg_check_modules(KVAZAAR libkvazaar)`
 * (homebrew installs the `kvazaar.pc`) succeeds. `ME_HAS_KVAZAAR`
 * is set as PRIVATE on the engine target; tests re-run the same
 * probe and propagate the macro to their own source so the gating
 * stays self-contained.
 *
 * Determinism. Per VISION §3.4 SW HEVC encoding is **not**
 * byte-identical across Kvazaar versions / x86 vs ARM SIMD paths;
 * however, within a single Kvazaar build + host architecture +
 * encoding parameters, the same input bytes produce the same
 * output bitstream. The encoder ctor sets `--threads=0` +
 * `--owf=0` so no thread-scheduling variation enters the encode.
 * Orchestrator integration (a separate cycle) marks the SW HEVC
 * output stream non-deterministic at the timeline level so
 * deterministic-regression tests skip it.
 *
 * 1080p ceiling. Per the M10 exit-criterion text
 * (`docs/MILESTONES.md` line 120) the SW path is "1080p ceiling /
 * fail-loud-on-overflow": `create()` returns nullptr +
 * ME_E_INVALID_ARG with a named diag when `width > 1920 ||
 * height > 1080`. Hosts that need higher resolutions must use
 * the HW VideoToolbox path. The cap is a deliberate scope-limit
 * so the SW path's 720p / 1080p workloads stay within reasonable
 * wall-clock budgets — Kvazaar's single-threaded encode of 4K is
 * minutes per second of source.
 *
 * Multiple-of-8 alignment. Kvazaar requires `width` and `height`
 * to be multiples of 8 (HEVC CTU alignment); `create()` rejects
 * non-multiples with ME_E_INVALID_ARG. Callers must pad the
 * frame buffer; the encoder doesn't crop on output.
 *
 * Pimpl. The `kvz_api` / `kvz_encoder` / `kvz_picture` lifetimes
 * live inside the `Impl` so this header stays free of
 * `<kvazaar.h>`.
 *
 * Threading. `send_frame` / `flush_packets` are NOT reentrant;
 * the encoder is a per-stream object. Concurrent encoding of
 * multiple streams uses one `KvazaarHevcEncoder` per stream.
 */
#pragma once

#include "media_engine/types.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace me::io {

class KvazaarHevcEncoder {
public:
    /* Factory: open a Kvazaar HEVC Main (8-bit) encoder.
     *
     * `width`/`height`: ≤ 1920×1080 and multiples of 8.
     * `framerate_num`/`framerate_den`: target output rate for
     * Kvazaar's rate-control. 0/0 falls back to 30/1.
     * `bitrate_bps`: target average bitrate. 0 picks 6 Mbps
     * (matches the h264 HW-path default at 1080p).
     *
     * Returns nullptr on (invalid args, Kvazaar config_init
     * failure, encoder_open failure) + populates `*error_msg`. */
    static std::unique_ptr<KvazaarHevcEncoder> create(
        int          width,
        int          height,
        int          framerate_num,
        int          framerate_den,
        std::int64_t bitrate_bps,
        std::string* error_msg);

    ~KvazaarHevcEncoder();

    /* Submit one YUV420P 8-bit frame. `y` covers `width × height`
     * bytes with stride `stride_y`. `u` and `v` cover `(width/2) ×
     * (height/2)` bytes each with strides `stride_u` / `stride_v`.
     * `pic_pts_us` is the frame's presentation timestamp in
     * microseconds (Kvazaar carries this through to output
     * packets).
     *
     * Output bytes are not produced inline — call `flush_packets`
     * after each `send_frame` (or in a drain loop after
     * `send_eof()` at end-of-stream) to retrieve them. */
    me_status_t send_frame(
        const std::uint8_t* y, std::size_t stride_y,
        const std::uint8_t* u, std::size_t stride_u,
        const std::uint8_t* v, std::size_t stride_v,
        std::int64_t        pic_pts_us,
        std::string*        error_msg);

    /* Signal end-of-stream — no more frames will be sent. After
     * this, call `flush_packets` until the callback fires zero
     * times to drain the encoder's reorder buffer. */
    void send_eof();

    /* Drain queued packets. The callback receives bytes for each
     * NAL packet — these are Annex-B-formatted bytes (start-code
     * `0x00 0x00 0x00 0x01` prefix already attached). For MP4 /
     * MOV mux, callers convert to length-prefixed via
     * libavformat's `h264_mp4toannexb` filter (or similar) at
     * the orchestrator boundary.
     *
     * Returns ME_OK on success (0+ packets emitted). Returns
     * ME_E_ENCODE if Kvazaar's `encoder_encode` returns 0
     * (failure). */
    me_status_t flush_packets(
        const std::function<void(std::span<const std::uint8_t>)>& on_packet,
        std::string*                                                error_msg);

private:
    KvazaarHevcEncoder();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace me::io
