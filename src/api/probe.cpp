#include "media_engine/probe.h"

/* Stub: real impl wraps libavformat. */

extern "C" me_status_t me_probe(me_engine_t*, const char*, me_media_info_t** out) {
    if (out) *out = nullptr;
    return ME_E_UNSUPPORTED;
}

extern "C" void me_media_info_destroy(me_media_info_t*) {}

extern "C" const char*   me_media_info_container(const me_media_info_t*)         { return ""; }
extern "C" me_rational_t me_media_info_duration(const me_media_info_t*)          { return {0, 1}; }
extern "C" int           me_media_info_has_video(const me_media_info_t*)         { return 0; }
extern "C" int           me_media_info_video_width(const me_media_info_t*)       { return 0; }
extern "C" int           me_media_info_video_height(const me_media_info_t*)      { return 0; }
extern "C" me_rational_t me_media_info_video_frame_rate(const me_media_info_t*)  { return {0, 1}; }
extern "C" const char*   me_media_info_video_codec(const me_media_info_t*)       { return ""; }
extern "C" int           me_media_info_has_audio(const me_media_info_t*)         { return 0; }
extern "C" int           me_media_info_audio_sample_rate(const me_media_info_t*) { return 0; }
extern "C" int           me_media_info_audio_channels(const me_media_info_t*)    { return 0; }
extern "C" const char*   me_media_info_audio_codec(const me_media_info_t*)       { return ""; }
