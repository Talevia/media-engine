/*
 * ComposeSink impl. See compose_sink.hpp for scope note.
 *
 * Current landing: skeleton class + factory. process() returns
 * ME_E_UNSUPPORTED; the per-frame compose loop is the next cycle's
 * work (multi-track-compose-frame-loop backlog item).
 */
#include "orchestrator/compose_sink.hpp"

#include "io/demux_context.hpp"
#include "orchestrator/reencode_pipeline.hpp"
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
        /* Phase-1 delegation: the actual alpha-over compose math lands
         * with the `multi-track-compose-actual-composite` follow-up
         * bullet. This cycle routes multi-track timelines to the
         * reencode_mux by filtering down to tracks[0]'s clips — the
         * bottom track in the declared z-order. Output is objectively
         * missing the upper tracks' pixels, but it exercises the full
         * end-to-end sink pipeline (me_render_start → worker →
         * encoder+mux → file on disk). Next cycle replaces this with
         * per-output-frame active_clips_at + decode + frame_to_rgba8 +
         * alpha_over across all tracks + rgba8_to_frame → encoder. */
        if (demuxes.size() != ranges_.size()) {
            if (err) *err = "ComposeSink: demuxes / ranges size mismatch";
            return ME_E_INTERNAL;
        }
        if (tl_.tracks.empty() || tl_.clips.empty()) {
            if (err) *err = "ComposeSink: empty timeline";
            return ME_E_INVALID_ARG;
        }

        /* Collect indices of clips on tracks[0] (bottom of z-order).
         * Flat `tl.clips` may interleave clips from multiple tracks if
         * the JSON declares them that way, so walking by track_id is
         * the safe approach. */
        const std::string& bottom_track_id = tl_.tracks[0].id;
        std::vector<std::size_t> bottom_clip_indices;
        bottom_clip_indices.reserve(tl_.clips.size());
        for (std::size_t i = 0; i < tl_.clips.size(); ++i) {
            if (tl_.clips[i].track_id == bottom_track_id) {
                bottom_clip_indices.push_back(i);
            }
        }
        if (bottom_clip_indices.empty()) {
            if (err) *err = "ComposeSink: bottom track has no clips";
            return ME_E_INVALID_ARG;
        }

        ReencodeOptions opts;
        opts.out_path           = common_.out_path;
        opts.container          = common_.container;
        opts.video_codec        = "h264";
        opts.audio_codec        = "aac";
        opts.video_bitrate_bps  = video_bitrate_;
        opts.audio_bitrate_bps  = audio_bitrate_;
        opts.cancel             = common_.cancel;
        opts.on_ratio           = common_.on_ratio;
        opts.pool               = pool_;
        opts.target_color_space = common_.target_color_space;

        opts.segments.reserve(bottom_clip_indices.size());
        for (std::size_t ci : bottom_clip_indices) {
            if (ci >= demuxes.size() || !demuxes[ci]) {
                if (err) *err = "ComposeSink: missing demux for bottom-track clip";
                return ME_E_INVALID_ARG;
            }
            opts.segments.push_back(ReencodeSegment{
                std::move(demuxes[ci]),
                ranges_[ci].source_start,
                ranges_[ci].source_duration,
                ranges_[ci].source_color_space,
            });
        }
        return reencode_mux(opts, err);
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
