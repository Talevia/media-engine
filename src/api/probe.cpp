#include "media_engine/probe.h"
#include "core/engine_impl.hpp"
#include "io/av_err.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/rational.h>
}

#include <algorithm>
#include <cstdint>
#include <exception>
#include <new>
#include <string>
#include <string_view>

/* me_media_info is opaque in the public header — body lives here. Typed
 * per-field, no generic maps (CLAUDE.md invariant 4). Strings own their
 * backing via std::string; accessors hand out .c_str() for the info's
 * lifetime, which matches the API.md contract. */
struct me_media_info {
    std::string   container;
    me_rational_t duration{0, 1};

    bool          has_video = false;
    int           video_width = 0;
    int           video_height = 0;
    me_rational_t video_frame_rate{0, 1};
    std::string   video_codec;

    bool          has_audio = false;
    int           audio_sample_rate = 0;
    int           audio_channels = 0;
    std::string   audio_codec;
};

namespace {

std::string strip_file_scheme(std::string_view uri) {
    constexpr std::string_view p{"file://"};
    if (uri.size() >= p.size() &&
        std::equal(p.begin(), p.end(), uri.begin())) {
        uri.remove_prefix(p.size());
    }
    return std::string{uri};
}

me_rational_t to_me_rational(AVRational r) {
    return me_rational_t{static_cast<int64_t>(r.num),
                         r.den > 0 ? static_cast<int64_t>(r.den) : 1};
}

using me::io::av_err_str;

}  // namespace

extern "C" me_status_t me_probe(me_engine_t* engine, const char* uri, me_media_info_t** out) {
    if (out) *out = nullptr;
    if (!engine || !uri || !out) return ME_E_INVALID_ARG;

    me::detail::clear_error(engine);

    const std::string path = strip_file_scheme(uri);

    AVFormatContext* fmt = nullptr;
    int rc = avformat_open_input(&fmt, path.c_str(), nullptr, nullptr);
    if (rc < 0) {
        me::detail::set_error(engine, "avformat_open_input: " + av_err_str(rc));
        return ME_E_IO;
    }

    rc = avformat_find_stream_info(fmt, nullptr);
    if (rc < 0) {
        me::detail::set_error(engine, "avformat_find_stream_info: " + av_err_str(rc));
        avformat_close_input(&fmt);
        return ME_E_DECODE;
    }

    me_media_info* info = nullptr;
    try {
        info = new me_media_info{};

        if (fmt->iformat && fmt->iformat->name) {
            /* iformat->name is comma-separated for multi-format demuxers
             * (e.g. "mov,mp4,m4a,3gp,3g2,mj2"). Keep the first token — it's
             * the canonical short name callers care about. */
            std::string_view name{fmt->iformat->name};
            const auto comma = name.find(',');
            if (comma != std::string_view::npos) name = name.substr(0, comma);
            info->container.assign(name);
        }

        if (fmt->duration != AV_NOPTS_VALUE) {
            info->duration = me_rational_t{static_cast<int64_t>(fmt->duration),
                                           static_cast<int64_t>(AV_TIME_BASE)};
        }

        const int vstream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (vstream >= 0) {
            AVStream* s = fmt->streams[vstream];
            AVCodecParameters* cp = s->codecpar;
            info->has_video        = true;
            info->video_width      = cp->width;
            info->video_height     = cp->height;
            info->video_frame_rate = to_me_rational(av_guess_frame_rate(fmt, s, nullptr));
            if (const char* name = avcodec_get_name(cp->codec_id)) {
                info->video_codec = name;
            }
        }

        const int astream = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (astream >= 0) {
            AVStream* s = fmt->streams[astream];
            AVCodecParameters* cp = s->codecpar;
            info->has_audio         = true;
            info->audio_sample_rate = cp->sample_rate;
            /* ch_layout is the FFmpeg >= 5.1 API; the project tracks
             * Homebrew FFmpeg (currently 8.x), so the legacy `channels`
             * field is not consulted. */
            info->audio_channels    = cp->ch_layout.nb_channels;
            if (const char* name = avcodec_get_name(cp->codec_id)) {
                info->audio_codec = name;
            }
        }
    } catch (const std::bad_alloc&) {
        delete info;
        avformat_close_input(&fmt);
        return ME_E_OUT_OF_MEMORY;
    } catch (const std::exception& ex) {
        me::detail::set_error(engine, ex.what());
        delete info;
        avformat_close_input(&fmt);
        return ME_E_INTERNAL;
    }

    avformat_close_input(&fmt);
    *out = info;
    return ME_OK;
}

extern "C" void me_media_info_destroy(me_media_info_t* info) {
    delete info;
}

extern "C" const char* me_media_info_container(const me_media_info_t* info) {
    return info ? info->container.c_str() : "";
}

extern "C" me_rational_t me_media_info_duration(const me_media_info_t* info) {
    return info ? info->duration : me_rational_t{0, 1};
}

extern "C" int me_media_info_has_video(const me_media_info_t* info) {
    return (info && info->has_video) ? 1 : 0;
}

extern "C" int me_media_info_video_width(const me_media_info_t* info) {
    return info ? info->video_width : 0;
}

extern "C" int me_media_info_video_height(const me_media_info_t* info) {
    return info ? info->video_height : 0;
}

extern "C" me_rational_t me_media_info_video_frame_rate(const me_media_info_t* info) {
    return info ? info->video_frame_rate : me_rational_t{0, 1};
}

extern "C" const char* me_media_info_video_codec(const me_media_info_t* info) {
    return info ? info->video_codec.c_str() : "";
}

extern "C" int me_media_info_has_audio(const me_media_info_t* info) {
    return (info && info->has_audio) ? 1 : 0;
}

extern "C" int me_media_info_audio_sample_rate(const me_media_info_t* info) {
    return info ? info->audio_sample_rate : 0;
}

extern "C" int me_media_info_audio_channels(const me_media_info_t* info) {
    return info ? info->audio_channels : 0;
}

extern "C" const char* me_media_info_audio_codec(const me_media_info_t* info) {
    return info ? info->audio_codec.c_str() : "";
}
