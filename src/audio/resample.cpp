/*
 * me::audio::resample impl. See resample.hpp for contract.
 */
#include "audio/resample.hpp"

#include "io/av_err.hpp"
#include "io/ffmpeg_raii.hpp"

extern "C" {
#include <libavutil/frame.h>
#include <libswresample/swresample.h>
}

namespace me::audio {

me_status_t resample_to(const AVFrame*          src,
                         int                    dst_rate,
                         AVSampleFormat         dst_fmt,
                         const AVChannelLayout& dst_ch_layout,
                         AVFrame**              out,
                         std::string*           err) {
    if (!src || !out || dst_rate <= 0) {
        if (err) *err = "resample_to: null src/out or non-positive dst_rate";
        if (out) *out = nullptr;
        return ME_E_INVALID_ARG;
    }
    *out = nullptr;

    /* SwrContext setup. We build+free one per call — caching is a
     * future cycle's profile-driven perf optimization. */
    SwrContext* raw = nullptr;
    int rc = swr_alloc_set_opts2(
        &raw,
        &dst_ch_layout, dst_fmt,         dst_rate,
        &src->ch_layout,
        static_cast<AVSampleFormat>(src->format),
        src->sample_rate,
        /*log_offset=*/0, /*log_ctx=*/nullptr);
    if (rc < 0 || !raw) {
        if (err) *err = "swr_alloc_set_opts2: " + me::io::av_err_str(rc);
        return ME_E_INTERNAL;
    }
    me::io::SwrContextPtr swr(raw);

    rc = swr_init(swr.get());
    if (rc < 0) {
        if (err) *err = "swr_init: " + me::io::av_err_str(rc);
        return ME_E_INTERNAL;
    }

    /* Allocate the destination AVFrame. swr_get_out_samples gives the
     * upper bound for `nb_samples` this conversion can produce.
     * Allocating that much and letting swr_convert tell us the actual
     * count is the standard pattern. */
    AVFrame* dst = av_frame_alloc();
    if (!dst) {
        if (err) *err = "av_frame_alloc";
        return ME_E_OUT_OF_MEMORY;
    }
    dst->format         = dst_fmt;
    dst->sample_rate    = dst_rate;
    rc = av_channel_layout_copy(&dst->ch_layout, &dst_ch_layout);
    if (rc < 0) {
        av_frame_free(&dst);
        if (err) *err = "av_channel_layout_copy: " + me::io::av_err_str(rc);
        return ME_E_INTERNAL;
    }
    dst->nb_samples = swr_get_out_samples(swr.get(), src->nb_samples);
    if (dst->nb_samples < 0) {
        av_frame_free(&dst);
        if (err) *err = "swr_get_out_samples: " + me::io::av_err_str(dst->nb_samples);
        return ME_E_INTERNAL;
    }
    if (dst->nb_samples == 0) {
        /* Nothing to convert (e.g. src->nb_samples == 0). Return the
         * empty dst frame — caller can check `nb_samples`. */
        *out = dst;
        return ME_OK;
    }

    rc = av_frame_get_buffer(dst, 0);
    if (rc < 0) {
        av_frame_free(&dst);
        if (err) *err = "av_frame_get_buffer: " + me::io::av_err_str(rc);
        return ME_E_OUT_OF_MEMORY;
    }

    const int converted = swr_convert(
        swr.get(),
        dst->extended_data, dst->nb_samples,
        src->extended_data, src->nb_samples);
    if (converted < 0) {
        av_frame_free(&dst);
        if (err) *err = "swr_convert: " + me::io::av_err_str(converted);
        return ME_E_INTERNAL;
    }

    /* swr_convert may have produced fewer samples than requested;
     * update nb_samples so downstream consumers see the real count. */
    dst->nb_samples = converted;
    *out = dst;
    return ME_OK;
}

}  // namespace me::audio
