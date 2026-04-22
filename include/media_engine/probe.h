/*
 * Media probe — inspect a single media file without decoding its content.
 *
 * probe is cheap and side-effect free. It may read file headers over the
 * network when the URI is http(s); results are not cached by default.
 */
#ifndef MEDIA_ENGINE_PROBE_H
#define MEDIA_ENGINE_PROBE_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

me_status_t me_probe(me_engine_t* engine, const char* uri, me_media_info_t** out);
void        me_media_info_destroy(me_media_info_t* info);

/* Strings returned below live as long as `info`. */
const char*   me_media_info_container(const me_media_info_t* info);
me_rational_t me_media_info_duration(const me_media_info_t* info);

/* Video stream (0 if none). */
int           me_media_info_has_video(const me_media_info_t* info);
int           me_media_info_video_width(const me_media_info_t* info);
int           me_media_info_video_height(const me_media_info_t* info);
me_rational_t me_media_info_video_frame_rate(const me_media_info_t* info);
const char*   me_media_info_video_codec(const me_media_info_t* info);

/* Audio stream (0 if none). */
int           me_media_info_has_audio(const me_media_info_t* info);
int           me_media_info_audio_sample_rate(const me_media_info_t* info);
int           me_media_info_audio_channels(const me_media_info_t* info);
const char*   me_media_info_audio_codec(const me_media_info_t* info);

#ifdef __cplusplus
}
#endif
#endif /* MEDIA_ENGINE_PROBE_H */
