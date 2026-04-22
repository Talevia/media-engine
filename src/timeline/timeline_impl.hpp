/* Internal timeline IR. Minimal — expands as schema v1 coverage grows.
 *
 * Phase-1 scope: a single video clip on a single video track, no effects,
 * no transforms, sourceRange == timeRange == full composition. Everything
 * else is rejected at load time with ME_E_UNSUPPORTED. */
#pragma once

#include "media_engine/types.h"

#include <string>
#include <vector>

namespace me {

struct Clip {
    std::string   asset_uri;        /* resolved to local path at remux time */
    me_rational_t time_start   { 0, 1 };
    me_rational_t time_duration{ 0, 1 };
    me_rational_t source_start { 0, 1 };
};

struct Timeline {
    me_rational_t frame_rate   { 30, 1 };
    me_rational_t duration     { 0, 1 };
    int           width        { 0 };
    int           height       { 0 };

    /* Phase-1 invariant: exactly one clip. Larger timelines rejected at load. */
    std::vector<Clip> clips;
};

}  // namespace me

struct me_timeline {
    me::Timeline tl;
};
