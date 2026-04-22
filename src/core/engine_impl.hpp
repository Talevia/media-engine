/* Internal — not exported. */
#pragma once

#include "media_engine/engine.h"

#include <string>

struct me_engine {
    me_engine_config_t config{};
    std::string        last_error;
};
