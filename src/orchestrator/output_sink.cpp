#include "orchestrator/output_sink.hpp"

#include "io/demux_context.hpp"
#include "orchestrator/muxer_state.hpp"
#include "orchestrator/reencode_pipeline.hpp"
#include "resource/codec_pool.hpp"

#include <string>
#include <utility>
#include <vector>

namespace me::orchestrator {

namespace {

bool streq(const char* a, std::string_view b) {
    return a && std::string_view{a} == b;
}

bool is_passthrough_spec(const me_output_spec_t& s) {
    return streq(s.video_codec, "passthrough") && streq(s.audio_codec, "passthrough");
}

/* Accept any LGPL-clean re-encode video codec we ship (h264 +
 * hevc as of M10's `encode-hevc-output-sink-wiring` cycle, plus
 * the SW HEVC fallback `hevc-sw` per
 * `encode-hevc-output-sink-runtime-sw-dispatch`) paired with AAC
 * audio. Adding a new video codec here means a new `streq` and a
 * corresponding branch in
 * `me::orchestrator::detail::open_video_encoder`. */
bool is_video_aac_spec(const me_output_spec_t& s) {
    if (!streq(s.audio_codec, "aac")) return false;
    return streq(s.video_codec, "h264")
        || streq(s.video_codec, "hevc")
        || streq(s.video_codec, "hevc-sw");
}

/* ==========================================================================
 * PassthroughSink — stream-copy concat of N segments into one container.
 * ========================================================================== */
class PassthroughSink final : public OutputSink {
public:
    PassthroughSink(SinkCommon common, std::vector<ClipTimeRange> ranges)
        : common_(std::move(common)), ranges_(std::move(ranges)) {}

    me_status_t process(
        std::vector<std::shared_ptr<me::io::DemuxContext>> demuxes,
        std::string*                                       err) override {
        if (demuxes.size() != ranges_.size()) {
            if (err) *err = "passthrough sink: demuxes / ranges size mismatch";
            return ME_E_INTERNAL;
        }
        PassthroughMuxOptions opts;
        opts.out_path  = common_.out_path;
        opts.container = common_.container;
        opts.cancel    = common_.cancel;
        opts.on_ratio  = common_.on_ratio;
        opts.segments.reserve(demuxes.size());
        for (size_t i = 0; i < demuxes.size(); ++i) {
            opts.segments.push_back(PassthroughSegment{
                std::move(demuxes[i]),
                ranges_[i].source_start,
                ranges_[i].source_duration,
                ranges_[i].time_offset,
            });
        }
        return passthrough_mux(opts, err);
    }

private:
    SinkCommon                 common_;
    std::vector<ClipTimeRange> ranges_;
};

/* ==========================================================================
 * VideoAacSink — h264_videotoolbox / hevc_videotoolbox (video) + libavcodec
 * aac (audio). The video codec name flows from `spec.video_codec` so
 * `(h264, aac)` and `(hevc, aac)` both route here; the
 * `open_video_encoder` helper picks the matching encoder by name.
 * Supports N-segment concat; decoders are per-clip, the encoder is shared
 * across all segments so the output carries one consistent video
 * bitstream and AAC sample stream regardless of segment count.
 * ========================================================================== */
class VideoAacSink final : public OutputSink {
public:
    VideoAacSink(SinkCommon common, const me_output_spec_t& spec,
                  std::vector<ClipTimeRange> ranges,
                  me::resource::CodecPool*   pool)
        : common_(std::move(common)),
          ranges_(std::move(ranges)),
          pool_(pool),
          video_codec_(spec.video_codec ? spec.video_codec : ""),
          video_bitrate_(spec.video_bitrate_bps),
          audio_bitrate_(spec.audio_bitrate_bps) {}

    me_status_t process(
        std::vector<std::shared_ptr<me::io::DemuxContext>> demuxes,
        std::string*                                       err) override {
        if (demuxes.size() != ranges_.size()) {
            if (err) *err = "video/aac sink: demuxes / ranges size mismatch";
            return ME_E_INTERNAL;
        }
        if (demuxes.empty()) {
            if (err) *err = "video/aac sink: empty segment list";
            return ME_E_INVALID_ARG;
        }
        ReencodeOptions opts;
        opts.out_path            = common_.out_path;
        opts.container           = common_.container;
        opts.video_codec         = video_codec_;
        opts.audio_codec         = "aac";
        opts.video_bitrate_bps   = video_bitrate_;
        opts.audio_bitrate_bps   = audio_bitrate_;
        opts.cancel              = common_.cancel;
        opts.on_ratio            = common_.on_ratio;
        opts.pool                = pool_;
        opts.target_color_space  = common_.target_color_space;
        opts.ocio_config_path    = common_.ocio_config_path;
        opts.segments.reserve(demuxes.size());
        for (size_t i = 0; i < demuxes.size(); ++i) {
            if (!demuxes[i]) {
                if (err) *err = "video/aac sink: null demux context at segment " +
                                 std::to_string(i);
                return ME_E_INVALID_ARG;
            }
            opts.segments.push_back(ReencodeSegment{
                std::move(demuxes[i]),
                ranges_[i].source_start,
                ranges_[i].source_duration,
                ranges_[i].source_color_space,
            });
        }
        return reencode_mux(opts, err);
    }

private:
    SinkCommon                 common_;
    std::vector<ClipTimeRange> ranges_;
    me::resource::CodecPool*   pool_          = nullptr;
    std::string                video_codec_;
    int64_t                    video_bitrate_ = 0;
    int64_t                    audio_bitrate_ = 0;
};

}  // namespace

std::unique_ptr<OutputSink> make_output_sink(
    const me_output_spec_t&     spec,
    SinkCommon                  common,
    std::vector<ClipTimeRange>  clip_ranges,
    me::resource::CodecPool*    codec_pool,
    std::string*                err) {

    if (!spec.path) {
        if (err) *err = "output.path is required";
        return nullptr;
    }
    if (clip_ranges.empty()) {
        if (err) *err = "phase-1: timeline must have at least one clip";
        return nullptr;
    }

    if (is_passthrough_spec(spec)) {
        /* Passthrough doesn't touch AVCodecContext; codec_pool ignored. */
        return std::make_unique<PassthroughSink>(std::move(common), std::move(clip_ranges));
    }
    if (is_video_aac_spec(spec)) {
        if (!codec_pool) {
            if (err) *err = "re-encode requires a codec pool (engine->codecs)";
            return nullptr;
        }
        return std::make_unique<VideoAacSink>(std::move(common), spec,
                                                std::move(clip_ranges), codec_pool);
    }

    if (err) *err = "phase-1: supported specs are "
                     "(video=passthrough, audio=passthrough) or "
                     "(video=h264|hevc|hevc-sw, audio=aac)";
    return nullptr;
}

}  // namespace me::orchestrator
