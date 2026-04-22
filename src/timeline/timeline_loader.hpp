#pragma once

#include "media_engine/types.h"

#include <string>
#include <string_view>

struct me_timeline;

namespace me::timeline {

/* Parse JSON into an owning timeline. On success returns ME_OK and *out
 * must be freed with `delete` (i.e. via me_timeline_destroy).
 * On failure, writes a human-readable message to err. */
me_status_t load_json(std::string_view json, me_timeline** out, std::string* err);

}  // namespace me::timeline
