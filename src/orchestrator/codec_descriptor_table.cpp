/* codec_descriptor_table impl. See header. */
#include "orchestrator/codec_descriptor_table.hpp"

#include <array>
#include <cstring>
#include <string_view>

namespace me::orchestrator {

namespace {

/* Indexed by enum value (verified below). */
constexpr std::array<VideoCodecDescriptor, 5> kVideoCodecs{{
    /* NONE: no encoder, no pix_fmt. */
    {ME_VIDEO_CODEC_NONE,        "none",        nullptr,
     AV_PIX_FMT_NONE,    0},

    /* PASSTHROUGH: routed to PassthroughSink before hitting
     * the reencode path. enc_name=nullptr → reencoder rejects. */
    {ME_VIDEO_CODEC_PASSTHROUGH, "passthrough", nullptr,
     AV_PIX_FMT_NONE,    0},

    /* H264 (VideoToolbox HW on macOS). */
    {ME_VIDEO_CODEC_H264,        "h264",        "h264_videotoolbox",
     AV_PIX_FMT_NV12,    6'000'000},

    /* HEVC (VideoToolbox HW). 12 Mbps default per Apple ProRes
     * VideoToolbox bitrate table for 1080p30 HDR10. */
    {ME_VIDEO_CODEC_HEVC,        "hevc",        "hevc_videotoolbox",
     AV_PIX_FMT_P010LE,  12'000'000},

    /* HEVC_SW (Kvazaar). The encoder name + pix_fmt here are
     * placeholders for code-path completeness; the SW HEVC
     * route in open_video_encoder rejects via preflight before
     * these values are consulted, and the actual SW encode
     * runs through KvazaarHevcEncoder (see hevc_sw_sink). */
    {ME_VIDEO_CODEC_HEVC_SW,     "hevc-sw",     "hevc_videotoolbox",
     AV_PIX_FMT_P010LE,  12'000'000},
}};

static_assert(kVideoCodecs[ME_VIDEO_CODEC_NONE].enum_value        == ME_VIDEO_CODEC_NONE);
static_assert(kVideoCodecs[ME_VIDEO_CODEC_PASSTHROUGH].enum_value == ME_VIDEO_CODEC_PASSTHROUGH);
static_assert(kVideoCodecs[ME_VIDEO_CODEC_H264].enum_value        == ME_VIDEO_CODEC_H264);
static_assert(kVideoCodecs[ME_VIDEO_CODEC_HEVC].enum_value        == ME_VIDEO_CODEC_HEVC);
static_assert(kVideoCodecs[ME_VIDEO_CODEC_HEVC_SW].enum_value     == ME_VIDEO_CODEC_HEVC_SW);

}  // namespace

std::span<const VideoCodecDescriptor> video_codec_descriptors() {
    return std::span<const VideoCodecDescriptor>(kVideoCodecs.data(),
                                                   kVideoCodecs.size());
}

me_video_codec_t lookup_video_codec_by_string(const char* s) noexcept {
    if (!s || s[0] == '\0') return ME_VIDEO_CODEC_NONE;
    const std::string_view sv{s};
    for (const auto& d : kVideoCodecs) {
        if (sv == d.legacy_string) return d.enum_value;
    }
    return ME_VIDEO_CODEC_NONE;
}

const VideoCodecDescriptor* lookup_video_codec_by_enum(
    me_video_codec_t enum_value) noexcept {
    const std::size_t idx = static_cast<std::size_t>(enum_value);
    if (idx >= kVideoCodecs.size()) return nullptr;
    return &kVideoCodecs[idx];
}

}  // namespace me::orchestrator
