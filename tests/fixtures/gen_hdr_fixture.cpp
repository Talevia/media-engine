/*
 * gen_hdr_fixture — build-time helper that writes a tiny HDR-tagged
 * HEVC Main 10 mp4 to argv[1].
 *
 * Mirror of `gen_fixture.cpp`, but:
 *
 *   - HEVC Main 10 via `hevc_videotoolbox` (LGPL-clean Apple HW
 *     encoder, ship-path per M10 exit criterion 3 first half).
 *     Same encoder this loop's cycle 11 (`encode-hevc-main10-hw`)
 *     wired into the orchestrator.
 *
 *   - Pix_fmt `AV_PIX_FMT_P010LE` (10-bit YUV, high-bit packed —
 *     the layout cycle 8 verified the convert path handles).
 *
 *   - Color tags BT.2020 / PQ / BT.2020 NCL / limited range so the
 *     encoder + muxer produce HDR10-tagged output.
 *
 *   - `AVMasteringDisplayMetadata` (BT.2020 primaries, 0.0001 →
 *     1000 nits) and `AVContentLightMetadata` (MaxCLL=1000,
 *     MaxFALL=400) attached to the output stream's
 *     `codecpar->coded_side_data` BEFORE `avformat_write_header`.
 *     The mp4 muxer serialises those into the `mdcv` and `clli`
 *     boxes; re-probing pulls them back via
 *     `me_media_info_video_hdr_metadata` (cycle 3).
 *
 * Why this exists. Bullet `debt-probe-hdr-positive-fixture` (M10
 * debt) — `tests/test_probe.cpp` only had negative coverage (SDR
 * fixture → all-zero HDR struct). Without a positive-path
 * fixture, a future regression that mis-reads
 * `display_primaries[i][j]` indices or the side-data type ID
 * silently passes CI. This generator writes the canonical HDR10
 * descriptor so downstream tests (`test-hdr-metadata-propagate`,
 * `test-pq-hlg-roundtrip`, `bench-hdr-roundtrip`) and the new
 * `test_probe.cpp` positive case all share the same artefact.
 *
 * NOT included in `test_determinism` — `hevc_videotoolbox` is HW
 * and intrinsically non-deterministic (frame slicing decisions
 * depend on Apple silicon thermal state, etc). The fixture is
 * "good enough to round-trip the side-data" rather than
 * "byte-identical across runs". Side-data values themselves DO
 * round-trip exactly because the muxer writes them verbatim into
 * fixed-format boxes.
 *
 * Skipped on hosts without VideoToolbox. CMake guards the target
 * behind a runtime check on `hevc_videotoolbox` availability.
 */

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
}

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

namespace {

/* 320×240 @ 24 fps × 5 frames is the smallest HEVC payload that
 * reliably opens hevc_videotoolbox + produces a valid mp4 container.
 * VideoToolbox refuses sub-128 pixel dimensions; 320×240 clears that
 * margin while keeping the fixture file size under ~50 KB. */
constexpr int kWidth     = 320;
constexpr int kHeight    = 240;
constexpr int kFrameRate = 24;
constexpr int kNumFrames = 5;

/* HDR10 mastering-display metadata constants. BT.2020 primary
 * chromaticities at 0.00002 fixed-point precision (the FFmpeg
 * convention used by `av_mastering_display_metadata_alloc`):
 *   Red   x=0.708,  y=0.292
 *   Green x=0.170,  y=0.797
 *   Blue  x=0.131,  y=0.046
 *   White x=0.3127, y=0.3290 (D65)
 *
 * Luminance: max=1000 cd/m² (HDR10 mastering peak), min=0.0001
 * cd/m² (typical OLED black). Stored as AVRational so the values
 * round-trip exactly through libavutil → mp4 mdcv box → re-probe.
 *
 * Content light level: MaxCLL=1000 (peak per-pixel), MaxFALL=400
 * (peak frame-average). Standard HDR10 bright-content reference
 * values per CTA-861.3 §A.2.3. */
constexpr AVRational kRedX{35400, 50000};   /* 0.708 */
constexpr AVRational kRedY{14600, 50000};   /* 0.292 */
constexpr AVRational kGreenX{8500, 50000};  /* 0.170 */
constexpr AVRational kGreenY{39850, 50000}; /* 0.797 */
constexpr AVRational kBlueX{6550, 50000};   /* 0.131 */
constexpr AVRational kBlueY{2300, 50000};   /* 0.046 */
constexpr AVRational kWhiteX{15635, 50000}; /* 0.3127 */
constexpr AVRational kWhiteY{16450, 50000}; /* 0.3290 */
constexpr AVRational kMinLum{1, 10000};     /* 0.0001 cd/m² */
constexpr AVRational kMaxLum{1000, 1};      /* 1000 cd/m² */
constexpr unsigned   kMaxCll  = 1000;
constexpr unsigned   kMaxFall = 400;

struct CodecCtxDel  { void operator()(AVCodecContext* p) const noexcept { if (p) avcodec_free_context(&p); } };
struct FrameDel     { void operator()(AVFrame* p)        const noexcept { if (p) av_frame_free(&p); } };
struct PacketDel    { void operator()(AVPacket* p)       const noexcept { if (p) av_packet_free(&p); } };

using CodecCtxPtr = std::unique_ptr<AVCodecContext, CodecCtxDel>;
using FramePtr    = std::unique_ptr<AVFrame,        FrameDel>;
using PacketPtr   = std::unique_ptr<AVPacket,       PacketDel>;

/* Fill a P010LE frame with a constant 10-bit gray. P010 packs 10
 * bits in the HIGH bits of each 16-bit word (low 6 bits are
 * zero-padded). 10-bit limited-range gray = 512 (0..1023 range),
 * stored as `512 << 6 = 32768`. */
void fill_p010_gray(AVFrame* f, int frame_index) {
    /* Modulate Y slightly per frame to avoid all-identical frames
     * (some encoders special-case still input and emit 1 packet
     * total, which can confuse muxers expecting per-frame packets). */
    const int y10 = 512 + (frame_index & 0x07);
    const uint16_t y_packed = static_cast<uint16_t>(y10 << 6);
    const uint16_t uv_packed = static_cast<uint16_t>(512u << 6);

    /* Y plane: full size, 16-bit samples. linesize is in BYTES. */
    for (int y = 0; y < kHeight; ++y) {
        auto* row = reinterpret_cast<uint16_t*>(
            f->data[0] + y * f->linesize[0]);
        for (int x = 0; x < kWidth; ++x) row[x] = y_packed;
    }
    /* Interleaved UV plane: width samples per row (= cw * 2),
     * height/2 rows. P010 is 4:2:0 subsampled chroma. */
    const int ch = kHeight / 2;
    const int cw = kWidth / 2;
    for (int y = 0; y < ch; ++y) {
        auto* row = reinterpret_cast<uint16_t*>(
            f->data[1] + y * f->linesize[1]);
        for (int x = 0; x < cw; ++x) {
            row[2 * x]     = uv_packed;
            row[2 * x + 1] = uv_packed;
        }
    }
}

int die(const char* where, int rc) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(rc, buf, sizeof(buf));
    std::fprintf(stderr, "gen_hdr_fixture: %s failed: %s\n", where, buf);
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

/* Attach AVMasteringDisplayMetadata + AVContentLightMetadata to the
 * output stream's `codecpar->coded_side_data`. Called AFTER
 * `avcodec_parameters_from_context` (which initialises codecpar's
 * own side-data array from the encoder context's). The mp4 muxer
 * reads these during `avformat_write_header` and writes them into
 * the `mdcv` (mastering display) and `clli` (content light) boxes
 * inside the moov atom; downstream `me_probe` pulls them back via
 * `extract_hdr_metadata`'s `av_packet_side_data_get` call. */
int attach_hdr_side_data(AVCodecParameters* cp) {
    AVPacketSideData* mdcv = av_packet_side_data_new(
        &cp->coded_side_data, &cp->nb_coded_side_data,
        AV_PKT_DATA_MASTERING_DISPLAY_METADATA,
        sizeof(AVMasteringDisplayMetadata), 0);
    if (!mdcv) return die("av_packet_side_data_new(MASTERING_DISPLAY)", AVERROR(ENOMEM));
    auto* m = reinterpret_cast<AVMasteringDisplayMetadata*>(mdcv->data);
    std::memset(m, 0, sizeof(*m));
    /* libavutil's display_primaries[i] indexing: [0]=R, [1]=G, [2]=B;
     * inner [0]=x, [1]=y. Mirrors the shape `extract_hdr_metadata`
     * reads in src/api/probe.cpp. */
    m->display_primaries[0][0] = kRedX;
    m->display_primaries[0][1] = kRedY;
    m->display_primaries[1][0] = kGreenX;
    m->display_primaries[1][1] = kGreenY;
    m->display_primaries[2][0] = kBlueX;
    m->display_primaries[2][1] = kBlueY;
    m->white_point[0]          = kWhiteX;
    m->white_point[1]          = kWhiteY;
    m->min_luminance           = kMinLum;
    m->max_luminance           = kMaxLum;
    m->has_primaries           = 1;
    m->has_luminance           = 1;

    AVPacketSideData* cll = av_packet_side_data_new(
        &cp->coded_side_data, &cp->nb_coded_side_data,
        AV_PKT_DATA_CONTENT_LIGHT_LEVEL,
        sizeof(AVContentLightMetadata), 0);
    if (!cll) return die("av_packet_side_data_new(CONTENT_LIGHT_LEVEL)", AVERROR(ENOMEM));
    auto* c = reinterpret_cast<AVContentLightMetadata*>(cll->data);
    c->MaxCLL  = kMaxCll;
    c->MaxFALL = kMaxFall;
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: gen_hdr_fixture <out.mp4>\n");
        return 2;
    }
    const char* out_path = argv[1];

    /* Encoder is hevc_videotoolbox per the M10 ship-path. The
     * cycle-11 test_encode_hevc_main10 already pinned that this
     * encoder is available on the dev box and accepts P010LE. */
    const AVCodec* codec = avcodec_find_encoder_by_name("hevc_videotoolbox");
    if (!codec) {
        std::fprintf(stderr,
            "gen_hdr_fixture: hevc_videotoolbox not available "
            "(non-macOS host?). Skipping fixture build.\n");
        return 0;   /* Not a build failure on Linux/Windows hosts. */
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
    /* AVFMT_FLAG_BITEXACT strips wall-clock and libav-version
     * metadata from mp4 atoms — the side-data values themselves
     * still round-trip exactly. HW encoder is non-deterministic
     * but the FIXTURE'S side-data fields (which is what tests
     * read) ARE deterministic. */
    oc->flags |= AVFMT_FLAG_BITEXACT;

    CodecCtxPtr enc(avcodec_alloc_context3(codec));
    if (!enc) return die("avcodec_alloc_context3", AVERROR(ENOMEM));
    enc->width        = kWidth;
    enc->height       = kHeight;
    enc->pix_fmt      = AV_PIX_FMT_P010LE;
    enc->time_base    = AVRational{1, kFrameRate};
    enc->framerate    = AVRational{kFrameRate, 1};
    enc->gop_size     = kFrameRate;
    enc->max_b_frames = 0;
    enc->thread_count = 1;
    /* HDR10-tagged colorimetry — BT.2020 / PQ / BT.2020 NCL /
     * limited range. These propagate into both the encoded HEVC
     * stream's VUI parameters AND the mp4 colr atom. Re-probing
     * surfaces them via `me_media_info_video_color_*`. */
    enc->color_primaries = AVCOL_PRI_BT2020;
    enc->color_trc       = AVCOL_TRC_SMPTE2084;
    enc->colorspace      = AVCOL_SPC_BT2020_NCL;
    enc->color_range     = AVCOL_RANGE_MPEG;
    enc->bit_rate        = 2'000'000;
    if (oc->oformat->flags & AVFMT_GLOBALHEADER) {
        enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    enc->flags |= AV_CODEC_FLAG_BITEXACT;

    rc = avcodec_open2(enc.get(), codec, nullptr);
    if (rc < 0) return die("avcodec_open2(hevc_videotoolbox)", rc);

    AVStream* st = avformat_new_stream(oc, nullptr);
    if (!st) {
        std::fprintf(stderr, "gen_hdr_fixture: avformat_new_stream\n");
        return 1;
    }
    st->time_base = enc->time_base;
    rc = avcodec_parameters_from_context(st->codecpar, enc.get());
    if (rc < 0) return die("avcodec_parameters_from_context", rc);

    /* Attach HDR side data AFTER avcodec_parameters_from_context —
     * that call resets `codecpar->coded_side_data`, so we must
     * write to it last. The mp4 muxer reads codecpar's side data
     * during avformat_write_header and serialises mdcv/clli boxes. */
    if (int err = attach_hdr_side_data(st->codecpar); err != 0) return err;

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
        fill_p010_gray(frame.get(), i);
        frame->pts = i;
        rc = avcodec_send_frame(enc.get(), frame.get());
        if (rc < 0) return die("avcodec_send_frame", rc);
        if (int r = drain_packets(enc.get(), oc, pkt.get(),
                                   enc->time_base, st->time_base, st->index);
            r != 0) {
            return r;
        }
    }
    /* Flush. */
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
