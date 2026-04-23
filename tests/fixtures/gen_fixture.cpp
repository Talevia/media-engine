/*
 * gen_fixture — build-time helper that writes a tiny deterministic MPEG-4
 * Part 2 / MP4 video to the path given as argv[1].
 *
 * Replaces the previous `find_program(ffmpeg)` + `ffmpeg -f lavfi
 * testsrc=...` dance in tests/CMakeLists.txt. Linking libavformat /
 * libavcodec / libavutil (already required by media_engine) means
 * test_determinism no longer silently skips on CI hosts that lack a system
 * ffmpeg CLI.
 *
 * Output spec matches what test_determinism expects: 10 frames at 10 fps,
 * 320×240 YUV420P, MPEG-4 Part 2 video, MP4 container, no audio track.
 * MPEG-4 Part 2 is LGPL-clean (ships with stock libavcodec, unlike
 * libx264) — this helper stays compliant with docs/INTEGRATION.md's
 * "ship builds must use LGPL FFmpeg" stance even though it runs only at
 * test time.
 *
 * Determinism knobs:
 *   - AV_CODEC_FLAG_BITEXACT on encoder and AVFMT_FLAG_BITEXACT on muxer:
 *     no encoder version strings, no wall-clock metadata in mp4 atoms.
 *   - thread_count = 1: mpeg4 encoder parallelism would make output depend
 *     on scheduling.
 *   - Fixed QP via AV_CODEC_FLAG_QSCALE + global_quality: encoder picks
 *     the same bit allocation every time.
 *   - Synthetic pixel content derives from frame index only — no time(),
 *     no rand(), no uninitialised buffer.
 */

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

#include <cstdint>
#include <cstdio>
#include <memory>

namespace {

/* 640×480 @ 25 fps: passthrough determinism doesn't care about dimensions,
 * but h264_videotoolbox refuses to open below roughly 480×270 or at <15 fps,
 * so the fixture has to clear VT's minimums for test_determinism's reencode
 * case to exercise the h264/aac path instead of skipping. */
constexpr int kWidth     = 640;
constexpr int kHeight    = 480;
constexpr int kFrameRate = 25;
constexpr int kNumFrames = 25;

struct CodecCtxDel  { void operator()(AVCodecContext* p) const noexcept { if (p) avcodec_free_context(&p); } };
struct FrameDel     { void operator()(AVFrame* p)        const noexcept { if (p) av_frame_free(&p); } };
struct PacketDel    { void operator()(AVPacket* p)       const noexcept { if (p) av_packet_free(&p); } };
/* AVFormatContext needs a two-step cleanup (avio_closep then
 * avformat_free_context); handle it manually in main rather than in a
 * deleter that can't see the avio state cleanly. */

using CodecCtxPtr = std::unique_ptr<AVCodecContext, CodecCtxDel>;
using FramePtr    = std::unique_ptr<AVFrame,        FrameDel>;
using PacketPtr   = std::unique_ptr<AVPacket,       PacketDel>;

/* Fill a YUV420P frame with a deterministic gradient that varies per
 * frame so the encoder emits non-trivial inter-frame residuals — avoids
 * degenerate all-same-frame output that some encoders might special-case. */
void fill_yuv(AVFrame* f, int frame_index) {
    for (int y = 0; y < kHeight; ++y) {
        uint8_t* row = f->data[0] + y * f->linesize[0];
        for (int x = 0; x < kWidth; ++x) {
            row[x] = static_cast<uint8_t>((x + y + frame_index * 3) & 0xff);
        }
    }
    const int ch = kHeight / 2;
    const int cw = kWidth / 2;
    for (int y = 0; y < ch; ++y) {
        uint8_t* ur = f->data[1] + y * f->linesize[1];
        uint8_t* vr = f->data[2] + y * f->linesize[2];
        for (int x = 0; x < cw; ++x) {
            ur[x] = static_cast<uint8_t>(128 + ((x + frame_index) & 0x1f));
            vr[x] = static_cast<uint8_t>(128 + ((y + frame_index) & 0x1f));
        }
    }
}

int die(const char* where, int rc) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(rc, buf, sizeof(buf));
    std::fprintf(stderr, "gen_fixture: %s failed: %s\n", where, buf);
    return 1;
}

int drain_packets(AVCodecContext* enc, AVFormatContext* oc, AVPacket* pkt,
                   AVRational enc_tb, AVRational mux_tb) {
    for (;;) {
        const int rc = avcodec_receive_packet(enc, pkt);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return 0;
        if (rc < 0) return die("avcodec_receive_packet", rc);

        av_packet_rescale_ts(pkt, enc_tb, mux_tb);
        pkt->stream_index = 0;
        const int wr = av_interleaved_write_frame(oc, pkt);
        av_packet_unref(pkt);
        if (wr < 0) return die("av_interleaved_write_frame", wr);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: gen_fixture <out.mp4>\n");
        return 2;
    }
    const char* out_path = argv[1];

    AVFormatContext* oc_raw = nullptr;
    int rc = avformat_alloc_output_context2(&oc_raw, nullptr, "mp4", out_path);
    if (rc < 0 || !oc_raw) return die("avformat_alloc_output_context2", rc);

    /* Scope guard for muxer: the two-step cleanup (avio_closep, free_context)
     * gets captured in a lambda so every early return path hits it exactly once. */
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

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    if (!codec) {
        std::fprintf(stderr, "gen_fixture: MPEG-4 Part 2 encoder not available\n");
        return 1;
    }

    CodecCtxPtr enc(avcodec_alloc_context3(codec));
    if (!enc) return die("avcodec_alloc_context3", AVERROR(ENOMEM));
    enc->width        = kWidth;
    enc->height       = kHeight;
    enc->pix_fmt      = AV_PIX_FMT_YUV420P;
    enc->time_base    = AVRational{1, kFrameRate};
    enc->framerate    = AVRational{kFrameRate, 1};
    enc->gop_size     = kFrameRate;
    enc->max_b_frames = 0;
    enc->thread_count = 1;
    enc->flags       |= AV_CODEC_FLAG_BITEXACT;
    enc->flags       |= AV_CODEC_FLAG_QSCALE;
    enc->global_quality = FF_QP2LAMBDA * 5;   /* -q:v 5 equivalent */
    if (oc->oformat->flags & AVFMT_GLOBALHEADER) {
        enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    rc = avcodec_open2(enc.get(), codec, nullptr);
    if (rc < 0) return die("avcodec_open2", rc);

    AVStream* st = avformat_new_stream(oc, nullptr);
    if (!st) { std::fprintf(stderr, "gen_fixture: avformat_new_stream\n"); return 1; }
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
        fill_yuv(frame.get(), i);
        frame->pts = i;

        rc = avcodec_send_frame(enc.get(), frame.get());
        if (rc < 0) return die("avcodec_send_frame", rc);

        if (int r = drain_packets(enc.get(), oc, pkt.get(), enc->time_base, st->time_base); r != 0) {
            return r;
        }
    }

    /* Flush encoder */
    rc = avcodec_send_frame(enc.get(), nullptr);
    if (rc < 0) return die("avcodec_send_frame(flush)", rc);
    if (int r = drain_packets(enc.get(), oc, pkt.get(), enc->time_base, st->time_base); r != 0) {
        return r;
    }

    rc = av_write_trailer(oc);
    if (rc < 0) return die("av_write_trailer", rc);

    return 0;
}
