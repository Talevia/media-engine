/*
 * test_encode_hevc_main10 — exercises the codec dispatch in
 * `me::orchestrator::detail::open_video_encoder`. The
 * `encode-hevc-main10-hw` cycle taught the helper to switch between
 * `h264_videotoolbox` + NV12 and `hevc_videotoolbox` + P010LE based
 * on `ReencodeOptions::video_codec`; this suite pins:
 *
 *   - Default / "h264" still selects h264_videotoolbox + NV12.
 *   - "hevc" selects hevc_videotoolbox + P010LE (the M10 HDR
 *     ship-path target).
 *   - Color tags propagate from the dec context to the encoder.
 *   - Unknown codec name → ME_E_UNSUPPORTED with named diagnostic.
 *
 * Skips when VideoToolbox encoders aren't registered in the FFmpeg
 * build (Linux / Windows hosts) — same shape as the LEGIT comment
 * inside the helper itself (reencode_video.cpp). On macOS the four
 * non-skip TEST_CASEs run; on other hosts only the unknown-codec
 * rejection runs (which doesn't hit `avcodec_find_encoder_by_name`).
 *
 * The test allocates a small AVCodecContext as the synthetic `dec`
 * parameter (the helper reads `width`/`height`/`framerate` /
 * `color_*` fields off it). Real reencode-flow integration coverage
 * lives with the upcoming `test-hdr-metadata-propagate` cycle which
 * runs an HDR-tagged HEVC fixture end-to-end through
 * `setup_h264_aac_encoder_mux`.
 */
#include <doctest/doctest.h>

#include "orchestrator/reencode_video.hpp"
#include "resource/codec_pool.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
}

#include <memory>
#include <string>

namespace {

struct CodecCtxDel {
    void operator()(AVCodecContext* p) const noexcept {
        if (p) avcodec_free_context(&p);
    }
};
using CodecCtxOwned = std::unique_ptr<AVCodecContext, CodecCtxDel>;

/* A synthetic "decoder" context populated with the few fields
 * `open_video_encoder` reads (dimensions, framerate, color tags).
 * We use the mpeg4 decoder identity here purely so allocation
 * succeeds without touching real demux / seek paths; pix_fmt /
 * codec_id are not consulted by the encoder-open code. */
CodecCtxOwned make_synthetic_dec(int w, int h,
                                  AVColorPrimaries primaries = AVCOL_PRI_BT709,
                                  AVColorTransferCharacteristic trc = AVCOL_TRC_BT709,
                                  AVColorSpace cs = AVCOL_SPC_BT709,
                                  AVColorRange range = AVCOL_RANGE_MPEG) {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_MPEG4);
    REQUIRE(codec != nullptr);
    CodecCtxOwned ctx(avcodec_alloc_context3(codec));
    REQUIRE(ctx != nullptr);
    ctx->width                = w;
    ctx->height               = h;
    ctx->framerate            = AVRational{30, 1};
    ctx->sample_aspect_ratio  = AVRational{1, 1};
    ctx->color_primaries      = primaries;
    ctx->color_trc            = trc;
    ctx->colorspace           = cs;
    ctx->color_range          = range;
    return ctx;
}

bool have_encoder(const char* name) {
    return avcodec_find_encoder_by_name(name) != nullptr;
}

}  // namespace

TEST_CASE("open_video_encoder: default '' codec → h264_videotoolbox + NV12") {
    if (!have_encoder("h264_videotoolbox")) return;
    me::resource::CodecPool pool;
    auto dec = make_synthetic_dec(640, 480);

    me::resource::CodecPool::Ptr enc(nullptr,
        me::resource::CodecPool::Deleter{nullptr});
    AVPixelFormat target_pix = AV_PIX_FMT_NONE;
    std::string err;

    const me_status_t s = me::orchestrator::detail::open_video_encoder(
        pool, dec.get(),
        AVRational{1, 30},   /* stream_time_base */
        0,                    /* default bitrate */
        false,                /* global_header */
        ME_VIDEO_CODEC_NONE,  /* legacy: "" → H264 fallback */
        "",                   /* video_codec — default */
        enc, target_pix, &err);
    REQUIRE_MESSAGE(s == ME_OK, err);
    REQUIRE(enc != nullptr);
    CHECK(target_pix == AV_PIX_FMT_NV12);
    CHECK(enc->codec_id == AV_CODEC_ID_H264);
}

TEST_CASE("open_video_encoder: 'hevc' → hevc_videotoolbox + P010LE") {
    if (!have_encoder("hevc_videotoolbox")) return;
    me::resource::CodecPool pool;
    auto dec = make_synthetic_dec(640, 480);

    me::resource::CodecPool::Ptr enc(nullptr,
        me::resource::CodecPool::Deleter{nullptr});
    AVPixelFormat target_pix = AV_PIX_FMT_NONE;
    std::string err;

    const me_status_t s = me::orchestrator::detail::open_video_encoder(
        pool, dec.get(),
        AVRational{1, 30}, 0, false,
        ME_VIDEO_CODEC_HEVC, "hevc",
        enc, target_pix, &err);
    REQUIRE_MESSAGE(s == ME_OK, err);
    REQUIRE(enc != nullptr);
    CHECK(target_pix == AV_PIX_FMT_P010LE);
    CHECK(enc->codec_id == AV_CODEC_ID_HEVC);
    /* HEVC default bitrate is higher than h264 because Main10 carries
     * more bits per pixel and HDR10 needs higher fidelity to avoid
     * banding. The helper picks 12 Mbps when caller passes 0. */
    CHECK(enc->bit_rate == 12'000'000);
}

TEST_CASE("open_video_encoder: 'hevc' propagates HDR color tags to encoder") {
    if (!have_encoder("hevc_videotoolbox")) return;
    me::resource::CodecPool pool;
    /* HDR10 input: BT.2020 primaries + PQ transfer + BT.2020NC matrix
     * + limited range. The encoder's color_* fields must mirror the
     * decoder's so VideoToolbox emits ST 2086 / CTA-861.3 SEI from
     * the codec's defaults — that auto-emission is the criterion-3
     * deliverable for sources tagged HDR10 end-to-end. */
    auto dec = make_synthetic_dec(640, 480,
        AVCOL_PRI_BT2020, AVCOL_TRC_SMPTE2084, AVCOL_SPC_BT2020_NCL,
        AVCOL_RANGE_MPEG);

    me::resource::CodecPool::Ptr enc(nullptr,
        me::resource::CodecPool::Deleter{nullptr});
    AVPixelFormat target_pix = AV_PIX_FMT_NONE;
    std::string err;

    const me_status_t s = me::orchestrator::detail::open_video_encoder(
        pool, dec.get(),
        AVRational{1, 30}, 0, false, ME_VIDEO_CODEC_HEVC, "hevc",
        enc, target_pix, &err);
    REQUIRE_MESSAGE(s == ME_OK, err);
    CHECK(enc->color_primaries == AVCOL_PRI_BT2020);
    CHECK(enc->color_trc       == AVCOL_TRC_SMPTE2084);
    CHECK(enc->colorspace      == AVCOL_SPC_BT2020_NCL);
    CHECK(enc->color_range     == AVCOL_RANGE_MPEG);
}

TEST_CASE("open_video_encoder: unknown codec → ME_E_UNSUPPORTED with named diag") {
    /* Runs without VideoToolbox — the rejection happens before
     * avcodec_find_encoder_by_name. */
    me::resource::CodecPool pool;
    auto dec = make_synthetic_dec(640, 480);

    me::resource::CodecPool::Ptr enc(nullptr,
        me::resource::CodecPool::Deleter{nullptr});
    AVPixelFormat target_pix = AV_PIX_FMT_NONE;
    std::string err;

    const me_status_t s = me::orchestrator::detail::open_video_encoder(
        pool, dec.get(),
        AVRational{1, 30}, 0, false,
        ME_VIDEO_CODEC_NONE,  /* unknown string → resolver maps to NONE */
        "av1",                /* unsupported by this helper */
        enc, target_pix, &err);
    CHECK(s == ME_E_UNSUPPORTED);
    CHECK(err.find("av1") != std::string::npos);
    CHECK(err.find("expected") != std::string::npos);
}

TEST_CASE("open_video_encoder: explicit bitrate override is respected (h264)") {
    if (!have_encoder("h264_videotoolbox")) return;
    me::resource::CodecPool pool;
    auto dec = make_synthetic_dec(640, 480);

    me::resource::CodecPool::Ptr enc(nullptr,
        me::resource::CodecPool::Deleter{nullptr});
    AVPixelFormat target_pix = AV_PIX_FMT_NONE;
    std::string err;

    const me_status_t s = me::orchestrator::detail::open_video_encoder(
        pool, dec.get(),
        AVRational{1, 30},
        2'500'000,            /* explicit 2.5 Mbps */
        false, ME_VIDEO_CODEC_H264, "h264",
        enc, target_pix, &err);
    REQUIRE_MESSAGE(s == ME_OK, err);
    CHECK(enc->bit_rate == 2'500'000);
}

/* M10 SW HEVC fallback dispatch. Five TEST_CASEs exercise the
 * runtime preflight in `open_video_encoder`'s `is_hevc_sw` branch:
 *
 *   - encode-loop-pending diagnostic mentions the BACKLOG bullet so
 *     callers can find the follow-up work.
 *   - 1080p ceiling rejection has the dimension in the diag.
 *   - multiple-of-8 rejection has "multiples of 8".
 *   - non-positive width/height is INVALID_ARG, not UNSUPPORTED.
 *   - HW-missing diagnostic on the `is_hevc` path explicitly names
 *     `'hevc-sw'` so Linux / Windows hosts know the fallback exists.
 *
 * The preflight runs without any Kvazaar/VideoToolbox call, so all
 * cases run on every host (gated only by ME_HAS_KVAZAAR for the
 * "encode-loop-pending" path; without the macro the diag mentions
 * ME_WITH_KVAZAAR=ON instead). */
TEST_CASE("open_video_encoder: 'hevc-sw' rejects > 1080p with named diag") {
    me::resource::CodecPool pool;
    auto dec = make_synthetic_dec(1920, 1200);

    me::resource::CodecPool::Ptr enc(nullptr,
        me::resource::CodecPool::Deleter{nullptr});
    AVPixelFormat target_pix = AV_PIX_FMT_NONE;
    std::string err;

    const me_status_t s = me::orchestrator::detail::open_video_encoder(
        pool, dec.get(),
        AVRational{1, 30}, 0, false, ME_VIDEO_CODEC_HEVC_SW, "hevc-sw",
        enc, target_pix, &err);
#ifdef ME_HAS_KVAZAAR
    CHECK(s == ME_E_UNSUPPORTED);
    CHECK(err.find("1920x1080") != std::string::npos);
    CHECK(err.find("hevc-sw") != std::string::npos);
#else
    /* Without ME_HAS_KVAZAAR the link-time gate fires before the
     * dimension check; either rejection point is acceptable as long
     * as the diag names ME_WITH_KVAZAAR. */
    CHECK(s == ME_E_UNSUPPORTED);
    CHECK(err.find("ME_WITH_KVAZAAR") != std::string::npos);
#endif
}

TEST_CASE("open_video_encoder: 'hevc-sw' rejects non-multiple-of-8 dims") {
#ifdef ME_HAS_KVAZAAR
    me::resource::CodecPool pool;
    /* 100 isn't a multiple of 8 but is ≤ 1920 — exercises the
     * alignment branch, not the resolution-cap branch. */
    auto dec = make_synthetic_dec(100, 64);

    me::resource::CodecPool::Ptr enc(nullptr,
        me::resource::CodecPool::Deleter{nullptr});
    AVPixelFormat target_pix = AV_PIX_FMT_NONE;
    std::string err;

    const me_status_t s = me::orchestrator::detail::open_video_encoder(
        pool, dec.get(),
        AVRational{1, 30}, 0, false, ME_VIDEO_CODEC_HEVC_SW, "hevc-sw",
        enc, target_pix, &err);
    CHECK(s == ME_E_INVALID_ARG);
    CHECK(err.find("multiples of 8") != std::string::npos);
#endif
}

TEST_CASE("open_video_encoder: 'hevc-sw' preflight passes with diag pointing at HevcSwSink") {
#ifdef ME_HAS_KVAZAAR
    me::resource::CodecPool pool;
    /* 64x64 is well under the 1080p cap and a clean multiple of 8;
     * preflight passes all checks. The (hevc-sw, aac) MP4-mux path
     * is downstream work — the diag tells callers to route through
     * the working `(hevc-sw, none)` HevcSwSink instead. The cycle
     * that annotated stub markers (debt-stub-annotate-unaccounted)
     * tightened the diag to drop the "preflight" + bullet-name
     * verbiage in favor of an actionable hint pointing at the
     * sibling sink. */
    auto dec = make_synthetic_dec(64, 64);

    me::resource::CodecPool::Ptr enc(nullptr,
        me::resource::CodecPool::Deleter{nullptr});
    AVPixelFormat target_pix = AV_PIX_FMT_NONE;
    std::string err;

    const me_status_t s = me::orchestrator::detail::open_video_encoder(
        pool, dec.get(),
        AVRational{1, 30}, 0, false, ME_VIDEO_CODEC_HEVC_SW, "hevc-sw",
        enc, target_pix, &err);
    CHECK(s == ME_E_UNSUPPORTED);
    CHECK(err.find("HevcSwSink") != std::string::npos);
    CHECK(err.find("audio_codec='none'") != std::string::npos);
#endif
}

TEST_CASE("open_video_encoder: 'hevc-sw' without ME_HAS_KVAZAAR → ME_WITH_KVAZAAR diag") {
#ifndef ME_HAS_KVAZAAR
    me::resource::CodecPool pool;
    auto dec = make_synthetic_dec(64, 64);

    me::resource::CodecPool::Ptr enc(nullptr,
        me::resource::CodecPool::Deleter{nullptr});
    AVPixelFormat target_pix = AV_PIX_FMT_NONE;
    std::string err;

    const me_status_t s = me::orchestrator::detail::open_video_encoder(
        pool, dec.get(),
        AVRational{1, 30}, 0, false, ME_VIDEO_CODEC_HEVC_SW, "hevc-sw",
        enc, target_pix, &err);
    CHECK(s == ME_E_UNSUPPORTED);
    CHECK(err.find("ME_WITH_KVAZAAR") != std::string::npos);
#endif
}

TEST_CASE("open_video_encoder: 'hevc' on missing hevc_videotoolbox suggests 'hevc-sw'") {
    /* Runs only on hosts WITHOUT hevc_videotoolbox (Linux / Windows).
     * On macOS the encoder is registered and this branch is skipped.
     * The diagnostic must name `'hevc-sw'` so callers can pivot to
     * the SW fallback opt-in without combing through release notes. */
    if (have_encoder("hevc_videotoolbox")) return;
    me::resource::CodecPool pool;
    auto dec = make_synthetic_dec(640, 480);

    me::resource::CodecPool::Ptr enc(nullptr,
        me::resource::CodecPool::Deleter{nullptr});
    AVPixelFormat target_pix = AV_PIX_FMT_NONE;
    std::string err;

    const me_status_t s = me::orchestrator::detail::open_video_encoder(
        pool, dec.get(),
        AVRational{1, 30}, 0, false, ME_VIDEO_CODEC_HEVC, "hevc",
        enc, target_pix, &err);
    CHECK(s == ME_E_UNSUPPORTED);
    CHECK(err.find("hevc-sw") != std::string::npos);
    CHECK(err.find("Kvazaar") != std::string::npos);
}

TEST_CASE("open_video_encoder: rejects unknown codec with all four valid names listed") {
    /* The diag must enumerate the supported codec names so callers
     * passing a typo (e.g. `"h265"` for HEVC) see the right options
     * without hunting through docs. The set is closed:
     * '' / 'h264' / 'hevc' / 'hevc-sw'. */
    me::resource::CodecPool pool;
    auto dec = make_synthetic_dec(640, 480);

    me::resource::CodecPool::Ptr enc(nullptr,
        me::resource::CodecPool::Deleter{nullptr});
    AVPixelFormat target_pix = AV_PIX_FMT_NONE;
    std::string err;

    const me_status_t s = me::orchestrator::detail::open_video_encoder(
        pool, dec.get(),
        AVRational{1, 30}, 0, false, ME_VIDEO_CODEC_NONE, "h265",
        enc, target_pix, &err);
    CHECK(s == ME_E_UNSUPPORTED);
    CHECK(err.find("h264") != std::string::npos);
    CHECK(err.find("hevc") != std::string::npos);
    CHECK(err.find("hevc-sw") != std::string::npos);
}
