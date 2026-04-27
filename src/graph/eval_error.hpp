/*
 * me::graph::EvalError — typed exception carrying both a me_status_t
 * and a human-readable message across the scheduler / Future boundary.
 *
 * Purpose: kernels (and Future::await) need to surface specific
 * `me_status_t` codes (ME_E_IO, ME_E_DECODE, ME_E_PARSE, ...) without
 * losing them to a generic `std::runtime_error` that callers can only
 * remap to ME_E_INTERNAL. Before this type, scheduler.hpp:130
 * Future<T>::await threw plain `runtime_error`, and api/thumbnail.cpp
 * caught it as `std::exception` → ME_E_DECODE — so a "no such file"
 * IO failure surfaced as a decode error and the test
 * test_thumbnail.cpp:172 ("returns ME_E_IO for a non-existent URI")
 * failed for the wrong reason.
 *
 * Usage shape
 *
 *   - Kernels that want to surface a specific status code with a
 *     descriptive message THROW EvalError instead of returning the
 *     status. The scheduler catches and records both fields onto the
 *     EvalInstance.
 *
 *   - Future<T>::await rebuilds an EvalError from the recorded status
 *     + message and throws it; callers can catch EvalError to recover
 *     the original kernel status.
 *
 *   - Outer C-API translation layers (api/*.cpp) catch EvalError to
 *     map back to me_status_t + last_error message, and fall through
 *     to a generic `std::exception` catch (→ ME_E_INTERNAL) for
 *     anything else.
 *
 * EvalError publicly inherits std::runtime_error so existing
 * `catch (const std::exception&)` blocks still match (preserving
 * backward compatibility with callers not yet updated to the typed
 * catch).
 */
#pragma once

#include "media_engine/types.h"

#include <stdexcept>
#include <string>
#include <utility>

namespace me::graph {

class EvalError : public std::runtime_error {
public:
    EvalError(me_status_t status, std::string msg)
        : std::runtime_error(std::move(msg)), status_(status) {}

    me_status_t status() const noexcept { return status_; }

private:
    me_status_t status_;
};

}  // namespace me::graph
