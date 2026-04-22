#pragma once

#include "media_engine/types.h"

#include <atomic>
#include <functional>
#include <string>

namespace me::io {

/* Stream-copy all video+audio streams from `in_path` to `out_path`.
 * No decode, no encode. Container is inferred from `out_path` extension
 * unless `container_hint` is non-empty (then passed as format name to
 * avformat_alloc_output_context2).
 *
 * `on_progress` receives ratio in [0,1]; guaranteed called at least for
 * start (0.0) and end (1.0). May be null.
 * `cancel` is polled between packets; when true, returns ME_E_CANCELLED.
 * `err` receives a human-readable message on failure. */
me_status_t remux_passthrough(
    const std::string& in_path,
    const std::string& out_path,
    const std::string& container_hint,
    std::function<void(float)> on_progress,
    const std::atomic<bool>& cancel,
    std::string* err);

}  // namespace me::io
