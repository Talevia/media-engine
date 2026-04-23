/*
 * ComposeSink impl. See compose_sink.hpp for scope note.
 *
 * Current landing: skeleton class + factory. process() returns
 * ME_E_UNSUPPORTED; the per-frame compose loop is the next cycle's
 * work (multi-track-compose-frame-loop backlog item).
 */
#include "orchestrator/compose_sink.hpp"

#include "timeline/timeline_impl.hpp"

#include <cstring>
#include <utility>

namespace me::orchestrator {

namespace {

bool streq(const char* a, const char* b) {
    return a && b && std::strcmp(a, b) == 0;
}

class ComposeSink final : public OutputSink {
public:
    ComposeSink(const me::Timeline&        tl,
                SinkCommon                 common,
                std::vector<ClipTimeRange> ranges,
                me::resource::CodecPool*   pool,
                int64_t                    video_bitrate,
                int64_t                    audio_bitrate)
        : tl_(tl),
          common_(std::move(common)),
          ranges_(std::move(ranges)),
          pool_(pool),
          video_bitrate_(video_bitrate),
          audio_bitrate_(audio_bitrate) {}

    me_status_t process(
        std::vector<std::shared_ptr<me::io::DemuxContext>> demuxes,
        std::string*                                       err) override {
        /* The per-frame compose loop lives in the follow-up
         * `multi-track-compose-frame-loop` cycle. All four prerequisites
         * (schema/IR, alpha_over kernel, active_clips resolver, YUV↔RGBA
         * frame_convert) are ready; this method stays a stub for one
         * cycle so the Exporter routing + sink factory scaffolding
         * land independently (reviewable, revertable). */
        /* Reference every stashed member so -Wunused-private-field
         * stays quiet until the frame-loop cycle consumes them. (cast-
         * to-void is the C++ canonical silence.) */
        (void)demuxes;
        (void)tl_; (void)common_; (void)ranges_;
        (void)pool_; (void)video_bitrate_; (void)audio_bitrate_;
        if (err) {
            *err = "ComposeSink::process: per-frame compose loop not yet "
                   "implemented — see multi-track-compose-frame-loop backlog "
                   "item. Four prerequisites are in place "
                   "(me::compose::alpha_over, active_clips_at, frame_to_rgba8, "
                   "rgba8_to_frame); next cycle glues them into the reencode "
                   "pipeline's encoder/mux.";
        }
        return ME_E_UNSUPPORTED;
    }

private:
    const me::Timeline&        tl_;
    SinkCommon                 common_;
    std::vector<ClipTimeRange> ranges_;
    me::resource::CodecPool*   pool_ = nullptr;
    int64_t                    video_bitrate_ = 0;
    int64_t                    audio_bitrate_ = 0;
};

}  // namespace

std::unique_ptr<OutputSink> make_compose_sink(
    const me::Timeline&            tl,
    const me_output_spec_t&        spec,
    SinkCommon                     common,
    std::vector<ClipTimeRange>     clip_ranges,
    me::resource::CodecPool*       pool,
    std::string*                   err) {

    /* Passthrough compose makes no sense (stream-copy can't composite
     * two video streams into one). Compose only supports the h264/aac
     * reencode path; other codec combinations are a future milestone
     * (M3+ adds more encoders). */
    if (!streq(spec.video_codec, "h264") || !streq(spec.audio_codec, "aac")) {
        if (err) *err = "multi-track compose currently requires "
                         "video_codec=h264 + audio_codec=aac; other codecs "
                         "(including passthrough) unsupported for compose path";
        return nullptr;
    }
    if (!pool) {
        if (err) *err = "multi-track compose requires a CodecPool (engine->codecs)";
        return nullptr;
    }
    if (clip_ranges.empty()) {
        if (err) *err = "multi-track compose requires at least one clip";
        return nullptr;
    }
    /* Must be actually multi-track to take this path. A single-track
     * timeline should go through the regular make_output_sink factory
     * (which the Exporter does before calling here). */
    if (tl.tracks.size() < 2) {
        if (err) *err = "make_compose_sink: expected 2+ tracks (single-track "
                         "timelines route through make_output_sink, not here)";
        return nullptr;
    }

    return std::make_unique<ComposeSink>(
        tl,
        std::move(common),
        std::move(clip_ranges),
        pool,
        spec.video_bitrate_bps,
        spec.audio_bitrate_bps);
}

}  // namespace me::orchestrator
