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

bool is_h264_aac_spec(const me_output_spec_t& s) {
    return streq(s.video_codec, "h264") && streq(s.audio_codec, "aac");
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
 * H264AacSink — h264_videotoolbox (video) + libavcodec aac (audio).
 * Phase-1 single-clip only; the factory rejects multi-clip specs so
 * process() doesn't need to guard.
 * ========================================================================== */
class H264AacSink final : public OutputSink {
public:
    H264AacSink(SinkCommon common, const me_output_spec_t& spec,
                 me::resource::CodecPool* pool)
        : common_(std::move(common)),
          pool_(pool),
          video_bitrate_(spec.video_bitrate_bps),
          audio_bitrate_(spec.audio_bitrate_bps) {}

    me_status_t process(
        std::vector<std::shared_ptr<me::io::DemuxContext>> demuxes,
        std::string*                                       err) override {
        if (demuxes.size() != 1) {
            if (err) *err = "h264/aac sink: expected exactly one demux (phase-1)";
            return ME_E_INTERNAL;
        }
        if (!demuxes.front()) {
            if (err) *err = "h264/aac sink: null demux context";
            return ME_E_INVALID_ARG;
        }
        ReencodeOptions opts;
        opts.out_path          = common_.out_path;
        opts.container         = common_.container;
        opts.video_codec       = "h264";
        opts.audio_codec       = "aac";
        opts.video_bitrate_bps = video_bitrate_;
        opts.audio_bitrate_bps = audio_bitrate_;
        opts.cancel            = common_.cancel;
        opts.on_ratio          = common_.on_ratio;
        opts.pool              = pool_;
        return reencode_mux(*demuxes.front(), opts, err);
    }

private:
    SinkCommon                common_;
    me::resource::CodecPool*  pool_          = nullptr;
    int64_t                   video_bitrate_ = 0;
    int64_t                   audio_bitrate_ = 0;
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
    if (is_h264_aac_spec(spec)) {
        if (clip_ranges.size() != 1) {
            if (err) *err = "phase-1: re-encode path supports a single clip only "
                             "(see backlog: reencode-multi-clip)";
            return nullptr;
        }
        if (!codec_pool) {
            if (err) *err = "re-encode requires a codec pool (engine->codecs)";
            return nullptr;
        }
        return std::make_unique<H264AacSink>(std::move(common), spec, codec_pool);
    }

    if (err) *err = "phase-1: supported specs are "
                     "(video=passthrough, audio=passthrough) or (video=h264, audio=aac)";
    return nullptr;
}

}  // namespace me::orchestrator
