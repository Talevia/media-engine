/*
 * gen_vp9_p2_fixture — build-time helper that writes a tiny VP9
 * Profile 2 (10-bit YUV 4:2:0) mp4 to argv[1].
 *
 * Why this exists. M10 exit criterion §M10:119 requires "decode 路径覆盖
 * HEVC Main 10、VP9 Profile 2 (10/12-bit)、AV1 10-bit；每路径有
 * 像素级 round-trip 测试" — HEVC Main10 is covered by `gen_hdr_fixture`
 * + `test_pq_hlg_roundtrip`. VP9 Profile 2 is the second leg. The
 * shape mirrors `gen_hdr_fixture.cpp`:
 *
 *   - libvpx-vp9 software encoder (BSD-2-clause, ships with the
 *     dev-box Homebrew FFmpeg per `--enable-libvpx`; LGPL-clean).
 *   - Pix_fmt `AV_PIX_FMT_YUV420P10LE` — VP9 Profile 2's canonical
 *     layout (10/12-bit 4:2:0). 12-bit P3 pix_fmts are also supported
 *     by libvpx-vp9 but P2-10 covers the "decode reuses 10-bit
 *     convert path" axis we care about; 12-bit doesn't add a
 *     codepath that 10-bit doesn't.
 *   - Plain BT.709 SDR colorimetry — the test exercises decode +
 *     libswscale 10→8 reduction, not HDR side-data round-trip
 *     (HEVC Main10 covers that). Keeping VP9 P2 SDR avoids
 *     over-specifying the test contract.
 *
 * The fixture is "good enough to round-trip the decoded gray
 * pixel value", not byte-identical across runs: libvpx-vp9 with
 * deterministic-flag input is repeatable per-host but the test
 * doesn't assert byte-equality against a baked baseline (see
 * Coverage in the `decode-vp9-profile2` commit body).
 *
 * Skipped on hosts without libvpx-vp9. CMake guards the target
 * by checking encoder availability at fixture-build time.
 */

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
}

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

namespace {

/* 320×240 @ 24 fps × 5 frames mirrors gen_hdr_fixture's payload size.
 * libvpx-vp9 is happy with sub-128-pixel dimensions (unlike VideoToolbox)
 * but matching the HDR fixture's geometry keeps test-side comparisons
 * uniform. */
constexpr int kWidth     = 320;
constexpr int kHeight    = 240;
constexpr int kFrameRate = 24;
constexpr int kNumFrames = 5;

struct CodecCtxDel  { void operator()(AVCodecContext* p) const noexcept { if (p) avcodec_free_context(&p); } };
struct FrameDel     { void operator()(AVFrame* p)        const noexcept { if (p) av_frame_free(&p); } };
struct PacketDel    { void operator()(AVPacket* p)       const noexcept { if (p) av_packet_free(&p); } };

using CodecCtxPtr = std::unique_ptr<AVCodecContext, CodecCtxDel>;
using FramePtr    = std::unique_ptr<AVFrame,        FrameDel>;
using PacketPtr   = std::unique_ptr<AVPacket,       PacketDel>;

/* Fill a YUV420P10LE frame with a constant 10-bit gray (Y=512,
 * UV=512 — i.e. midtone neutral). Stored in 16-bit little-endian
 * words; the high 6 bits are zero (unlike P010 which pads in the
 * high bits). 10-bit limited-range gray of 512 maps to ~127 in
 * 8-bit RGB after libswscale's BT.709 limited→full conversion. */
void fill_yuv420p10_gray(AVFrame* f, int frame_index) {
    /* Modulate Y slightly per frame to keep the encoder from
     * collapsing identical input into a single packet. */
    const uint16_t y_val  = static_cast<uint16_t>(512 + (frame_index & 0x07));
    const uint16_t uv_val = 512;

    /* Y plane: full size. linesize is in BYTES; one sample = 2 bytes. */
    for (int y = 0; y < kHeight; ++y) {
        auto* row = reinterpret_cast<uint16_t*>(
            f->data[0] + y * f->linesize[0]);
        for (int x = 0; x < kWidth; ++x) row[x] = y_val;
    }
    /* U + V planar (NOT interleaved — that's P010's NV12 cousin).
     * Each plane is width/2 × height/2 16-bit samples. */
    const int ch = kHeight / 2;
    const int cw = kWidth / 2;
    for (int plane = 1; plane <= 2; ++plane) {
        for (int y = 0; y < ch; ++y) {
            auto* row = reinterpret_cast<uint16_t*>(
                f->data[plane] + y * f->linesize[plane]);
            for (int x = 0; x < cw; ++x) row[x] = uv_val;
        }
    }
}

int die(const char* where, int rc) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(rc, buf, sizeof(buf));
    std::fprintf(stderr, "gen_vp9_p2_fixture: %s failed: %s\n", where, buf);
    return 1;
}

int drain_packets(AVCodecContext* enc, AVFormatContext* oc, AVPacket* pkt,
                   AVRational enc_tb, AVRational mux_tb, int stream_index) {
    for (;;) {
        const int rc = avcodec_receive_packet(enc, pkt);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return 0;
        if (rc < 0) return die("avcodec_receive_packet", rc);
        av_packet_rescale_ts(pkt, enc_tb, mux_tb);
        pkt->stream_index = stream_index;
        const int wr = av_interleaved_write_frame(oc, pkt);
        av_packet_unref(pkt);
        if (wr < 0) return die("av_interleaved_write_frame", wr);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: gen_vp9_p2_fixture <out.mp4>\n");
        return 2;
    }
    const char* out_path = argv[1];

    const AVCodec* codec = avcodec_find_encoder_by_name("libvpx-vp9");
    if (!codec) {
        std::fprintf(stderr,
            "gen_vp9_p2_fixture: libvpx-vp9 not available "
            "(FFmpeg built without --enable-libvpx?). Skipping.\n");
        return 0;   /* Not a build failure — tests skip via ME_REQUIRE_FIXTURE. */
    }

    AVFormatContext* oc_raw = nullptr;
    int rc = avformat_alloc_output_context2(&oc_raw, nullptr, "mp4", out_path);
    if (rc < 0 || !oc_raw) return die("avformat_alloc_output_context2", rc);

    struct MuxGuard {
        AVFormatContext* oc;
        ~MuxGuard() {
            if (!oc) return;
            if (oc->pb) avio_closep(&oc->pb);
            avformat_free_context(oc);
        }
    } mux_guard{oc_raw};
    AVFormatContext* oc = oc_raw;
    oc->flags |= AVFMT_FLAG_BITEXACT;

    CodecCtxPtr enc(avcodec_alloc_context3(codec));
    if (!enc) return die("avcodec_alloc_context3", AVERROR(ENOMEM));
    enc->width        = kWidth;
    enc->height       = kHeight;
    enc->pix_fmt      = AV_PIX_FMT_YUV420P10LE;   /* VP9 Profile 2 */
    enc->time_base    = AVRational{1, kFrameRate};
    enc->framerate    = AVRational{kFrameRate, 1};
    enc->gop_size     = kFrameRate;
    enc->max_b_frames = 0;
    enc->thread_count = 1;
    /* SDR BT.709 limited-range tags — the test only cares about
     * decode + 10→8 conversion, not HDR side-data preservation. */
    enc->color_primaries = AVCOL_PRI_BT709;
    enc->color_trc       = AVCOL_TRC_BT709;
    enc->colorspace      = AVCOL_SPC_BT709;
    enc->color_range     = AVCOL_RANGE_MPEG;
    enc->bit_rate        = 800'000;   /* generous for uniform gray */
    if (oc->oformat->flags & AVFMT_GLOBALHEADER) {
        enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    enc->flags |= AV_CODEC_FLAG_BITEXACT;

    /* libvpx-vp9 needs `deadline=good` (default) to emit packets
     * promptly on tiny inputs; `realtime` skips quality work but
     * is fine here too. Default works without further tweaking. */
    rc = avcodec_open2(enc.get(), codec, nullptr);
    if (rc < 0) return die("avcodec_open2(libvpx-vp9)", rc);

    AVStream* st = avformat_new_stream(oc, nullptr);
    if (!st) {
        std::fprintf(stderr, "gen_vp9_p2_fixture: avformat_new_stream\n");
        return 1;
    }
    st->time_base = enc->time_base;
    rc = avcodec_parameters_from_context(st->codecpar, enc.get());
    if (rc < 0) return die("avcodec_parameters_from_context", rc);

    rc = avio_open(&oc->pb, out_path, AVIO_FLAG_WRITE);
    if (rc < 0) return die("avio_open", rc);
    rc = avformat_write_header(oc, nullptr);
    if (rc < 0) return die("avformat_write_header", rc);

    FramePtr  frame(av_frame_alloc());
    PacketPtr pkt(av_packet_alloc());
    if (!frame || !pkt) return die("alloc frame/pkt", AVERROR(ENOMEM));
    frame->format = enc->pix_fmt;
    frame->width  = enc->width;
    frame->height = enc->height;
    rc = av_frame_get_buffer(frame.get(), 32);
    if (rc < 0) return die("av_frame_get_buffer", rc);

    for (int i = 0; i < kNumFrames; ++i) {
        rc = av_frame_make_writable(frame.get());
        if (rc < 0) return die("av_frame_make_writable", rc);
        fill_yuv420p10_gray(frame.get(), i);
        frame->pts = i;
        rc = avcodec_send_frame(enc.get(), frame.get());
        if (rc < 0) return die("avcodec_send_frame", rc);
        if (int r = drain_packets(enc.get(), oc, pkt.get(),
                                   enc->time_base, st->time_base, st->index);
            r != 0) {
            return r;
        }
    }
    rc = avcodec_send_frame(enc.get(), nullptr);
    if (rc < 0) return die("avcodec_send_frame(flush)", rc);
    if (int r = drain_packets(enc.get(), oc, pkt.get(),
                               enc->time_base, st->time_base, st->index);
        r != 0) {
        return r;
    }

    rc = av_write_trailer(oc);
    if (rc < 0) return die("av_write_trailer", rc);

    return 0;
}
