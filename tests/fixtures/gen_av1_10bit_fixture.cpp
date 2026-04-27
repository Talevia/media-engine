/*
 * gen_av1_10bit_fixture — build-time helper that writes a tiny
 * AV1 10-bit (Main profile, YUV420P10LE) mp4 to argv[1].
 *
 * Why this exists. M10 exit criterion §M10:119 third leg —
 * "decode 路径覆盖 HEVC Main 10、VP9 Profile 2 (10/12-bit)、
 * AV1 10-bit；每路径有像素级 round-trip 测试". HEVC Main 10
 * covered by `gen_hdr_fixture` + `test_pq_hlg_roundtrip`. VP9
 * Profile 2 covered by `gen_vp9_p2_fixture` + `test_decode_vp9_profile2`
 * (cycle 18). AV1 10-bit is the closing leg.
 *
 * Encoder: `libsvtav1` (BSD-3-clause, LGPL-clean, Intel-led — same
 * org that retired the SVT-HEVC project; SVT-AV1 is the actively
 * maintained sibling). Ships with the dev-box Homebrew FFmpeg per
 * `--enable-libsvtav1`. SVT-AV1 supports `yuv420p10le` (AV1 Main
 * profile 10-bit) directly per `ffmpeg -h encoder=libsvtav1`.
 *
 * Decoder side at runtime is `libdav1d` (BSD-2-clause) or FFmpeg's
 * built-in `av1` — either picks up the stream automatically through
 * libavformat's codec auto-selection.
 *
 * Mirrors `gen_vp9_p2_fixture.cpp`'s shape: 320×240 × 5 frames of
 * uniform mid-gray (Y=512 limited-range BT.709) so the test can
 * pin a known centre-pixel RGB value after libswscale's 10→8
 * reduction. SDR colorimetry — the test exercises decode + convert,
 * not HDR side-data preservation (that's HEVC Main 10's job per
 * `test_hdr_metadata_propagate`).
 *
 * Skipped on hosts without libsvtav1. Returns exit 0 so the build
 * doesn't fail; consumers skip via ME_REQUIRE_FIXTURE.
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

/* Same packing as the VP9 P2 fixture — yuv420p10le stores 10-bit
 * samples in 16-bit LE words with the high 6 bits zero (NOT shifted
 * up like P010). Y midpoint = 512, UV midpoint = 512. */
void fill_yuv420p10_gray(AVFrame* f, int frame_index) {
    const uint16_t y_val  = static_cast<uint16_t>(512 + (frame_index & 0x07));
    const uint16_t uv_val = 512;

    for (int y = 0; y < kHeight; ++y) {
        auto* row = reinterpret_cast<uint16_t*>(
            f->data[0] + y * f->linesize[0]);
        for (int x = 0; x < kWidth; ++x) row[x] = y_val;
    }
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
    std::fprintf(stderr, "gen_av1_10bit_fixture: %s failed: %s\n", where, buf);
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
        std::fprintf(stderr, "usage: gen_av1_10bit_fixture <out.mp4>\n");
        return 2;
    }
    const char* out_path = argv[1];

    const AVCodec* codec = avcodec_find_encoder_by_name("libsvtav1");
    if (!codec) {
        std::fprintf(stderr,
            "gen_av1_10bit_fixture: libsvtav1 not available "
            "(FFmpeg built without --enable-libsvtav1?). Skipping.\n");
        return 0;
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
    enc->pix_fmt      = AV_PIX_FMT_YUV420P10LE;   /* AV1 Main 10-bit */
    enc->time_base    = AVRational{1, kFrameRate};
    enc->framerate    = AVRational{kFrameRate, 1};
    enc->gop_size     = kFrameRate;
    enc->max_b_frames = 0;
    enc->thread_count = 1;
    enc->color_primaries = AVCOL_PRI_BT709;
    enc->color_trc       = AVCOL_TRC_BT709;
    enc->colorspace      = AVCOL_SPC_BT709;
    enc->color_range     = AVCOL_RANGE_MPEG;
    enc->bit_rate        = 800'000;
    if (oc->oformat->flags & AVFMT_GLOBALHEADER) {
        enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    enc->flags |= AV_CODEC_FLAG_BITEXACT;

    /* SVT-AV1's `preset 8` is the upstream-recommended fast/balanced
     * choice for short payloads; default is auto-good (-2) which
     * picks 10 for low-latency settings. Setting explicit 8 keeps the
     * fixture-build time bounded under ~1s on uniform input. */
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "preset", "8", 0);

    rc = avcodec_open2(enc.get(), codec, &opts);
    av_dict_free(&opts);
    if (rc < 0) return die("avcodec_open2(libsvtav1)", rc);

    AVStream* st = avformat_new_stream(oc, nullptr);
    if (!st) {
        std::fprintf(stderr, "gen_av1_10bit_fixture: avformat_new_stream\n");
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
