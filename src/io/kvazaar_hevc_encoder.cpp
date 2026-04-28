#include "io/kvazaar_hevc_encoder.hpp"

extern "C" {
#include <kvazaar.h>
}

#include <cstring>
#include <utility>
#include <vector>

namespace me::io {

namespace {

constexpr int kMaxWidth  = 1920;
constexpr int kMaxHeight = 1080;

}  // namespace

struct KvazaarHevcEncoder::Impl {
    /* Kvazaar API table — fetched once via `kvz_api_get(8)` for
     * Main-profile 8-bit. Lifetime: process-static; no destructor
     * call needed. */
    const ::kvz_api*    api    = nullptr;

    /* Per-encoder state. Both must be destroyed via the API's
     * `config_destroy` / `encoder_close`. */
    ::kvz_config*       cfg    = nullptr;
    ::kvz_encoder*      enc    = nullptr;

    /* Owned input picture buffer. `picture_alloc` returns a
     * ref-counted picture; we own one for the duration of the
     * encoder and re-fill its plane buffers on each `send_frame`.
     * Kvazaar's `encoder_encode` may bump the refcount internally;
     * we still call `picture_free` once at destructor time
     * (matching the alloc). */
    ::kvz_picture*      pic    = nullptr;

    int                 width  = 0;
    int                 height = 0;

    /* When set, the encoder's input stream is closed and we're
     * draining the reorder buffer. */
    bool                eof    = false;

    /* Chunks emitted by the most recent `encoder_encode` call,
     * waiting for the next `flush_packets`. nullptr when none
     * pending. */
    ::kvz_data_chunk*   pending_chunks      = nullptr;
    uint32_t            pending_chunks_len  = 0;
};

KvazaarHevcEncoder::KvazaarHevcEncoder() : impl_(std::make_unique<Impl>()) {}

KvazaarHevcEncoder::~KvazaarHevcEncoder() {
    if (!impl_->api) return;
    if (impl_->pending_chunks) impl_->api->chunk_free(impl_->pending_chunks);
    if (impl_->pic) impl_->api->picture_free(impl_->pic);
    if (impl_->enc) impl_->api->encoder_close(impl_->enc);
    if (impl_->cfg) impl_->api->config_destroy(impl_->cfg);
}

std::unique_ptr<KvazaarHevcEncoder> KvazaarHevcEncoder::create(
    int          width,
    int          height,
    int          framerate_num,
    int          framerate_den,
    std::int64_t bitrate_bps,
    std::string* error_msg) {

    auto fail = [&](const std::string& msg) -> std::unique_ptr<KvazaarHevcEncoder> {
        if (error_msg) *error_msg = msg;
        return nullptr;
    };

    if (width <= 0 || height <= 0) {
        return fail("KvazaarHevcEncoder::create: width / height must be positive");
    }
    if (width > kMaxWidth || height > kMaxHeight) {
        return fail("KvazaarHevcEncoder::create: SW HEVC ceiling is "
                    "1920x1080 (HW VideoToolbox path required for higher); "
                    "see docs/MILESTONES.md M10 §3");
    }
    if ((width & 7) || (height & 7)) {
        return fail("KvazaarHevcEncoder::create: width and height must be "
                    "multiples of 8 (HEVC CTU alignment)");
    }
    if (framerate_num <= 0 || framerate_den <= 0) {
        framerate_num = 30;
        framerate_den = 1;
    }
    if (bitrate_bps <= 0) bitrate_bps = 6'000'000;

    auto rt = std::unique_ptr<KvazaarHevcEncoder>(new KvazaarHevcEncoder());

    rt->impl_->api = ::kvz_api_get(8);
    if (!rt->impl_->api) {
        return fail("KvazaarHevcEncoder::create: kvz_api_get(8) returned NULL "
                    "— libkvazaar build does not include 8-bit support");
    }

    rt->impl_->cfg = rt->impl_->api->config_alloc();
    if (!rt->impl_->cfg) {
        return fail("KvazaarHevcEncoder::create: config_alloc OOM");
    }
    if (rt->impl_->api->config_init(rt->impl_->cfg) != 1) {
        return fail("KvazaarHevcEncoder::create: config_init failed");
    }

    rt->impl_->cfg->width            = width;
    rt->impl_->cfg->height           = height;
    rt->impl_->cfg->framerate_num    = framerate_num;
    rt->impl_->cfg->framerate_denom  = framerate_den;

    /* Determinism knobs — single-threaded encode + no parallel
     * lookahead so the same input always produces the same
     * bitstream within this build. The cost is wall-clock: SW
     * encode of 1080p30 runs ~real-time on modern desktop CPUs
     * with these settings, faster on multicore but with
     * non-deterministic output if threads > 0. */
    rt->impl_->api->config_parse(rt->impl_->cfg, "threads", "0");
    rt->impl_->api->config_parse(rt->impl_->cfg, "owf",     "0");

    /* Bitrate via config_parse — the struct field
     * `target_bitrate` is gated on a non-default `rc-algorithm`,
     * so go through the named option which sets both. The
     * parser expects an integer string of bps. */
    {
        const std::string br = std::to_string(bitrate_bps);
        rt->impl_->api->config_parse(
            rt->impl_->cfg, "bitrate", br.c_str());
    }

    rt->impl_->enc = rt->impl_->api->encoder_open(rt->impl_->cfg);
    if (!rt->impl_->enc) {
        return fail("KvazaarHevcEncoder::create: encoder_open failed "
                    "(check Kvazaar log via stderr for details)");
    }

    rt->impl_->pic = rt->impl_->api->picture_alloc(width, height);
    if (!rt->impl_->pic) {
        return fail("KvazaarHevcEncoder::create: picture_alloc OOM");
    }

    rt->impl_->width  = width;
    rt->impl_->height = height;

    return rt;
}

me_status_t KvazaarHevcEncoder::send_frame(
    const std::uint8_t* y, std::size_t stride_y,
    const std::uint8_t* u, std::size_t stride_u,
    const std::uint8_t* v, std::size_t stride_v,
    std::int64_t        pic_pts_us,
    std::string*        error_msg) {

    if (!impl_->api || !impl_->pic || !impl_->enc) {
        if (error_msg) *error_msg = "KvazaarHevcEncoder::send_frame: encoder closed";
        return ME_E_INVALID_ARG;
    }
    if (impl_->eof) {
        if (error_msg) *error_msg = "KvazaarHevcEncoder::send_frame: stream already EOF";
        return ME_E_INVALID_ARG;
    }
    if (!y || !u || !v) {
        if (error_msg) *error_msg = "KvazaarHevcEncoder::send_frame: null plane pointer";
        return ME_E_INVALID_ARG;
    }

    /* Copy caller's planes into Kvazaar's owned picture. Kvazaar's
     * picture has its own stride (==width when allocated via
     * picture_alloc) — copy row-by-row to handle non-tight caller
     * strides. */
    const int w = impl_->width;
    const int h = impl_->height;
    const int ch_w = w / 2;
    const int ch_h = h / 2;

    auto copy_plane = [](std::uint8_t* dst, std::size_t dst_stride,
                          const std::uint8_t* src, std::size_t src_stride,
                          int row_bytes, int rows) {
        for (int r = 0; r < rows; ++r) {
            std::memcpy(dst + static_cast<std::size_t>(r) * dst_stride,
                         src + static_cast<std::size_t>(r) * src_stride,
                         static_cast<std::size_t>(row_bytes));
        }
    };

    /* Kvazaar's picture stride is `pic->stride` (in pixels = bytes
     * for 8-bit). Use it for destination row pitch. */
    const std::size_t dst_y_stride = static_cast<std::size_t>(impl_->pic->stride);
    const std::size_t dst_c_stride = dst_y_stride / 2;
    copy_plane(impl_->pic->y, dst_y_stride, y, stride_y, w,    h);
    copy_plane(impl_->pic->u, dst_c_stride, u, stride_u, ch_w, ch_h);
    copy_plane(impl_->pic->v, dst_c_stride, v, stride_v, ch_w, ch_h);

    impl_->pic->pts = pic_pts_us;
    impl_->pic->dts = pic_pts_us;

    /* Encode this frame — Kvazaar may emit packets here OR queue
     * them depending on lookahead. Drain via flush_packets. */
    ::kvz_data_chunk* data_out = nullptr;
    uint32_t          len_out  = 0;
    ::kvz_picture*    pic_recon = nullptr;
    ::kvz_picture*    pic_src   = nullptr;
    ::kvz_frame_info  info{};

    const int rc = impl_->api->encoder_encode(
        impl_->enc, impl_->pic,
        &data_out, &len_out,
        &pic_recon, &pic_src, &info);
    if (rc != 1) {
        if (error_msg) *error_msg = "KvazaarHevcEncoder::send_frame: encoder_encode returned 0";
        if (data_out)  impl_->api->chunk_free(data_out);
        if (pic_recon) impl_->api->picture_free(pic_recon);
        if (pic_src)   impl_->api->picture_free(pic_src);
        return ME_E_ENCODE;
    }
    /* Reconstructed / source pictures aren't of interest to us
     * (no reconstruction-quality monitoring here). Free them. */
    if (pic_recon) impl_->api->picture_free(pic_recon);
    if (pic_src)   impl_->api->picture_free(pic_src);

    /* If chunks were emitted now, queue them for the next
     * flush_packets call. We chain into impl_->pending_chunks via
     * a new field — but to keep state minimal we instead require
     * callers to flush after every send_frame. Inline here for
     * simplicity: store the chunk list on impl_ for flush. */
    /* Simplification: cache the chunk list in a thread-local
     * pseudo-queue via the picture struct's reuse — actually
     * cleanest is to just pass the chunks straight through to
     * flush_packets via an Impl-held pointer. */
    impl_->pending_chunks   = data_out;
    impl_->pending_chunks_len = len_out;
    return ME_OK;
}

void KvazaarHevcEncoder::send_eof() {
    impl_->eof = true;
}

me_status_t KvazaarHevcEncoder::flush_packets(
    const std::function<void(std::span<const std::uint8_t>)>& on_packet,
    std::string*                                                error_msg) {

    if (!impl_->api || !impl_->enc) {
        if (error_msg) *error_msg = "KvazaarHevcEncoder::flush_packets: encoder closed";
        return ME_E_INVALID_ARG;
    }

    auto deliver_chunks = [&](::kvz_data_chunk* head) {
        /* Walk the chain, accumulating into one byte buffer per
         * encoder_encode call. Kvazaar's chunks are arbitrary-sized
         * fragments of the bitstream; for the engine's downstream
         * consumers (orchestrator mux, raw .265 dump) the simplest
         * delivery is one concatenated span per call. */
        std::vector<std::uint8_t> buf;
        for (::kvz_data_chunk* c = head; c; c = c->next) {
            buf.insert(buf.end(), c->data, c->data + c->len);
        }
        if (head) impl_->api->chunk_free(head);
        if (!buf.empty()) {
            on_packet(std::span<const std::uint8_t>(buf.data(), buf.size()));
        }
    };

    /* Deliver any chunks queued by the most recent send_frame. */
    if (impl_->pending_chunks) {
        ::kvz_data_chunk* head = impl_->pending_chunks;
        impl_->pending_chunks = nullptr;
        impl_->pending_chunks_len = 0;
        deliver_chunks(head);
    }

    /* When EOF, drain the encoder by calling encoder_encode with
     * pic_in=nullptr until no chunks come back. */
    if (impl_->eof) {
        while (true) {
            ::kvz_data_chunk* data_out = nullptr;
            uint32_t          len_out  = 0;
            ::kvz_picture*    pic_recon = nullptr;
            ::kvz_picture*    pic_src   = nullptr;
            ::kvz_frame_info  info{};
            const int rc = impl_->api->encoder_encode(
                impl_->enc, nullptr,
                &data_out, &len_out,
                &pic_recon, &pic_src, &info);
            if (pic_recon) impl_->api->picture_free(pic_recon);
            if (pic_src)   impl_->api->picture_free(pic_src);
            if (rc != 1) {
                if (error_msg) *error_msg = "KvazaarHevcEncoder::flush_packets: drain encode returned 0";
                if (data_out) impl_->api->chunk_free(data_out);
                return ME_E_ENCODE;
            }
            if (!data_out) break;  /* fully drained */
            deliver_chunks(data_out);
        }
    }

    return ME_OK;
}

}  // namespace me::io
