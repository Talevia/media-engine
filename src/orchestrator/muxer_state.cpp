#include "orchestrator/muxer_state.hpp"

#include "io/demux_context.hpp"
#include "io/mux_context.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
}

#include <cstring>
#include <string>
#include <vector>

namespace me::orchestrator {

namespace {

std::string av_err_str(int rc) {
    char buf[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(rc, buf, sizeof(buf));
    return std::string(buf);
}

/* Convert a me_rational_t (seconds) to a PTS in the given time_base, rounding
 * toward nearest. den<=0 → 0. */
int64_t rat_to_pts(me_rational_t r, AVRational tb) {
    if (r.den <= 0) return 0;
    return av_rescale_q(r.num, AVRational{1, static_cast<int>(r.den)}, tb);
}

/* Shallow codecpar equality for the subset that matters to passthrough:
 * any mismatch breaks stream-copy because the output header is baked from
 * the first segment. */
bool codecpar_compatible(const AVCodecParameters* a, const AVCodecParameters* b) {
    if (!a || !b) return false;
    if (a->codec_type != b->codec_type) return false;
    if (a->codec_id   != b->codec_id)   return false;
    if (a->profile    != b->profile)    return false;
    if (a->level      != b->level)      return false;

    if (a->codec_type == AVMEDIA_TYPE_VIDEO) {
        if (a->width != b->width || a->height != b->height) return false;
        if (a->format != b->format) return false;
    } else if (a->codec_type == AVMEDIA_TYPE_AUDIO) {
        if (a->sample_rate != b->sample_rate) return false;
        if (a->format      != b->format)      return false;
        if (a->ch_layout.nb_channels != b->ch_layout.nb_channels) return false;
    }
    if (a->extradata_size != b->extradata_size) return false;
    if (a->extradata_size > 0 &&
        std::memcmp(a->extradata, b->extradata, a->extradata_size) != 0) {
        return false;
    }
    return true;
}

/* Estimate total output duration (microseconds) summed across segments, for
 * progress ratio reporting. For any segment with source_duration == 0 we
 * fall back to the demuxer-reported duration. */
int64_t total_output_us(const PassthroughMuxOptions& opts) {
    int64_t total = 0;
    for (const auto& seg : opts.segments) {
        if (seg.source_duration.den > 0 && seg.source_duration.num > 0) {
            total += av_rescale_q(seg.source_duration.num,
                                   AVRational{1, static_cast<int>(seg.source_duration.den)},
                                   AV_TIME_BASE_Q);
        } else if (seg.demux && seg.demux->fmt && seg.demux->fmt->duration > 0) {
            int64_t src_start_us = 0;
            if (seg.source_start.den > 0 && seg.source_start.num > 0) {
                src_start_us = av_rescale_q(seg.source_start.num,
                                             AVRational{1, static_cast<int>(seg.source_start.den)},
                                             AV_TIME_BASE_Q);
            }
            total += std::max<int64_t>(0, seg.demux->fmt->duration - src_start_us);
        }
    }
    return total;
}

}  // namespace

me_status_t passthrough_mux(const PassthroughMuxOptions&  opts,
                            std::string*                  err) {
    auto fail = [&](me_status_t s, std::string msg) {
        if (err) *err = std::move(msg);
        return s;
    };

    if (opts.segments.empty()) return fail(ME_E_INVALID_ARG, "no segments");
    if (!opts.segments.front().demux || !opts.segments.front().demux->fmt) {
        return fail(ME_E_INVALID_ARG, "segment[0] has no demux context");
    }

    AVFormatContext* ifmt0 = opts.segments.front().demux->fmt;

    /* --- Open output context templated on first segment's stream layout.
     * MuxContext owns the AVFormatContext lifecycle; its destructor handles
     * avio_closep + free_context regardless of which branch returns. */
    std::string open_err;
    auto mux = me::io::MuxContext::open(opts.out_path, opts.container, &open_err);
    if (!mux) return fail(ME_E_INTERNAL, std::move(open_err));
    AVFormatContext* ofmt = mux->fmt();

    /* stream_map[seg_idx][in_stream_idx] = out_stream_idx (-1 = dropped). */
    std::vector<std::vector<int>> stream_map(opts.segments.size());
    stream_map[0].assign(ifmt0->nb_streams, -1);

    /* Create output streams from segment 0, carry codecpar / tag to later. */
    int rc = 0;
    for (unsigned i = 0; i < ifmt0->nb_streams; ++i) {
        AVStream* in_s = ifmt0->streams[i];
        const AVCodecParameters* par = in_s->codecpar;
        if (par->codec_type != AVMEDIA_TYPE_VIDEO && par->codec_type != AVMEDIA_TYPE_AUDIO) {
            continue;
        }
        AVStream* out_s = avformat_new_stream(ofmt, nullptr);
        if (!out_s) return fail(ME_E_OUT_OF_MEMORY, "new_stream");
        rc = avcodec_parameters_copy(out_s->codecpar, par);
        if (rc < 0) return fail(ME_E_INTERNAL, "codecpar_copy: " + av_err_str(rc));
        out_s->codecpar->codec_tag = 0;
        out_s->time_base = in_s->time_base;   /* propagate tb for clean PTS rescale */
        stream_map[0][i] = out_s->index;
    }

    /* Verify subsequent segments have the SAME stream layout and codec
     * params as segment 0. Phase-1 simplification: match by positional
     * index (stream 0 ↔ stream 0, stream 1 ↔ stream 1, ...). A richer
     * stream mapping can land later if concat across heterogeneous inputs
     * becomes a real need. */
    for (size_t si = 1; si < opts.segments.size(); ++si) {
        const auto& seg = opts.segments[si];
        if (!seg.demux || !seg.demux->fmt) {
            return fail(ME_E_INVALID_ARG,
                        "segment[" + std::to_string(si) + "] has no demux context");
        }
        AVFormatContext* ifmt = seg.demux->fmt;
        stream_map[si].assign(ifmt->nb_streams, -1);
        if (ifmt->nb_streams != ifmt0->nb_streams) {
            return fail(ME_E_UNSUPPORTED,
                        "segment[" + std::to_string(si) + "] stream count " +
                        std::to_string(ifmt->nb_streams) + " != segment[0] " +
                        std::to_string(ifmt0->nb_streams) +
                        " (phase-1: concat requires identical stream layout)");
        }
        for (unsigned i = 0; i < ifmt->nb_streams; ++i) {
            if (stream_map[0][i] < 0) continue;
            if (!codecpar_compatible(ifmt->streams[i]->codecpar, ifmt0->streams[i]->codecpar)) {
                return fail(ME_E_UNSUPPORTED,
                            "segment[" + std::to_string(si) + "] stream[" + std::to_string(i) +
                            "] codecpar mismatch with segment[0] (passthrough requires identical "
                            "codec parameters across concat segments)");
            }
            stream_map[si][i] = stream_map[0][i];
        }
    }

    /* --- Open output file, write header. */
    if (auto s = mux->open_avio(err);    s != ME_OK) return s;
    if (auto s = mux->write_header(err); s != ME_OK) return s;

    /* --- Per-segment packet copy loop ----------------------------------- */
    /* Per-output-stream DTS continuity state: last_end_tb[i] = last written
     * packet's (pts + duration), in ofmt->streams[i]->time_base. Segment N
     * (N >= 1) uses this to compute a per-stream ts_offset so its first
     * packet's DTS lands at the end of segment N-1 — this mirrors the
     * libavformat concat demuxer's approach and is the only way to
     * guarantee monotonic DTS when sources have priming (e.g. AAC first
     * packet pts = -1024) or when GOP-rounding on seek shifts boundaries. */
    if (opts.on_ratio) opts.on_ratio(0.0f);
    const int64_t total_us = total_output_us(opts);
    int64_t elapsed_us = 0;
    me_status_t terminal = ME_OK;

    std::vector<int64_t> last_end_out_tb(ofmt->nb_streams, 0);

    AVPacket* pkt = av_packet_alloc();

    for (size_t si = 0; si < opts.segments.size() && terminal == ME_OK; ++si) {
        const auto& seg = opts.segments[si];
        AVFormatContext* ifmt = seg.demux->fmt;

        /* Optional seek. GOP-rounding is standard "ffmpeg -ss X -c copy"
         * behavior — sample-accurate trimming needs re-encode. */
        const int64_t src_start_us = rat_to_pts(seg.source_start, AV_TIME_BASE_Q);
        if (src_start_us > 0) {
            rc = avformat_seek_file(ifmt, -1, INT64_MIN, src_start_us, src_start_us,
                                     AVSEEK_FLAG_BACKWARD);
            if (rc < 0) {
                terminal = ME_E_IO;
                if (err) *err = "segment[" + std::to_string(si) + "] seek: " + av_err_str(rc);
                break;
            }
        }
        /* Source range end in AV_TIME_BASE (or INT64_MAX for "to EOF"). */
        int64_t src_end_us = INT64_MAX;
        if (seg.source_duration.den > 0 && seg.source_duration.num > 0) {
            const int64_t dur_us = rat_to_pts(seg.source_duration, AV_TIME_BASE_Q);
            src_end_us = src_start_us + dur_us;
        }

        /* Per-segment per-stream offset: resolved lazily on first packet
         * of each output stream (different streams see their first packet
         * at different real times, so each needs its own offset). */
        std::vector<int64_t> ts_offset(ofmt->nb_streams, 0);
        std::vector<bool>    ts_offset_set(ofmt->nb_streams, false);

        while (true) {
            if (opts.cancel && opts.cancel->load(std::memory_order_acquire)) {
                terminal = ME_E_CANCELLED;
                break;
            }
            rc = av_read_frame(ifmt, pkt);
            if (rc == AVERROR_EOF) break;
            if (rc < 0) {
                terminal = ME_E_DECODE;
                if (err) *err = "segment[" + std::to_string(si) + "] read_frame: " + av_err_str(rc);
                break;
            }
            const int in_si = pkt->stream_index;
            const int mapped = (in_si < static_cast<int>(stream_map[si].size()))
                                 ? stream_map[si][in_si] : -1;
            if (mapped < 0) { av_packet_unref(pkt); continue; }

            AVStream* in_s  = ifmt->streams[in_si];
            AVStream* out_s = ofmt->streams[mapped];

            /* Stop this segment once we're past its source_duration. */
            if (pkt->pts != AV_NOPTS_VALUE) {
                const int64_t pts_us = av_rescale_q(pkt->pts, in_s->time_base, AV_TIME_BASE_Q);
                if (pts_us >= src_end_us) {
                    av_packet_unref(pkt);
                    break;
                }
            }

            /* Rescale ts to out_tb (naive — without segment offset). */
            int64_t naive_pts = AV_NOPTS_VALUE;
            int64_t naive_dts = AV_NOPTS_VALUE;
            if (pkt->pts != AV_NOPTS_VALUE) {
                naive_pts = av_rescale_q(pkt->pts, in_s->time_base, out_s->time_base);
            }
            if (pkt->dts != AV_NOPTS_VALUE) {
                naive_dts = av_rescale_q(pkt->dts, in_s->time_base, out_s->time_base);
            }

            /* Establish segment offset on first packet per output stream.
             * Segment 0 uses no offset (pass-through) so priming PTS stays
             * negative, which MP4 audio handles correctly. Segment N >= 1
             * shifts so DTS joins the previous segment's end monotonically. */
            if (!ts_offset_set[mapped]) {
                if (si == 0) {
                    ts_offset[mapped] = 0;
                } else if (naive_dts != AV_NOPTS_VALUE) {
                    ts_offset[mapped] = last_end_out_tb[mapped] - naive_dts;
                } else if (naive_pts != AV_NOPTS_VALUE) {
                    ts_offset[mapped] = last_end_out_tb[mapped] - naive_pts;
                }
                ts_offset_set[mapped] = true;
            }

            if (naive_pts != AV_NOPTS_VALUE) pkt->pts = naive_pts + ts_offset[mapped];
            if (naive_dts != AV_NOPTS_VALUE) pkt->dts = naive_dts + ts_offset[mapped];
            if (pkt->duration > 0) {
                pkt->duration = av_rescale_q(pkt->duration, in_s->time_base, out_s->time_base);
            }
            pkt->stream_index = mapped;
            pkt->pos = -1;

            const int64_t pts_for_progress = pkt->pts;
            const AVRational out_tb = out_s->time_base;

            /* MuxContext::write_and_track does the snapshot-before-write
             * dance + unrefs the packet, so we don't repeat the PAIN_POINTS
             * "av_interleaved_write_frame zeroes pkt" footgun here. */
            std::string write_err;
            const me_status_t w = mux->write_and_track(pkt, last_end_out_tb, &write_err);
            if (w != ME_OK) {
                terminal = w;
                if (err) *err = "segment[" + std::to_string(si) + "] " + write_err;
                break;
            }

            if (opts.on_ratio && total_us > 0 && pts_for_progress != AV_NOPTS_VALUE) {
                const int64_t pts_us = av_rescale_q(pts_for_progress, out_tb, AV_TIME_BASE_Q);
                float ratio = static_cast<float>(pts_us) / static_cast<float>(total_us);
                if (ratio < 0.f) ratio = 0.f;
                if (ratio > 1.f) ratio = 1.f;
                opts.on_ratio(ratio);
            }
        }

        /* Update elapsed for eventual future use (progress driver already
         * uses absolute output PTS). Kept for diagnostics / potential
         * ratio smoothing. */
        if (seg.source_duration.den > 0 && seg.source_duration.num > 0) {
            elapsed_us += rat_to_pts(seg.source_duration, AV_TIME_BASE_Q);
        } else if (ifmt->duration > 0) {
            elapsed_us += std::max<int64_t>(0, ifmt->duration - src_start_us);
        }
    }
    av_packet_free(&pkt);
    (void)elapsed_us;  /* silence unused if future progress paths drop it */

    if (terminal == ME_OK) {
        if (auto s = mux->write_trailer(err); s != ME_OK) {
            terminal = s;
        }
    }
    /* mux destructor handles avio_closep + free_context whether we
     * reached write_trailer or bailed early. */

    if (terminal == ME_OK && opts.on_ratio) opts.on_ratio(1.0f);
    return terminal;
}

}  // namespace me::orchestrator
