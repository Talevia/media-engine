/*
 * Future<T> — typed handle to a scheduled evaluation of a graph terminal.
 *
 * Lazy, scheduler-driven: submit happens in scheduler.evaluate_port(), but
 * user code observes completion via await() / then() / ready() / cancel().
 *
 * This is NOT std::future: there's no eager ownership of a thread, and
 * copies/moves are cheap (shared state lives in EvalInstance).
 *
 * See docs/ARCHITECTURE_GRAPH.md §用户面 API.
 */
#pragma once

#include "graph/types.hpp"
#include "media_engine/types.h"

#include <future>
#include <memory>
#include <stdexcept>
#include <utility>
#include <variant>

namespace me::sched { class EvalInstance; }

namespace me::graph {

template<typename T>
class Future {
public:
    Future() = default;
    Future(std::shared_future<void>               run_future,
           std::shared_ptr<sched::EvalInstance>   eval,
           PortRef                                terminal)
        : run_future_(std::move(run_future)),
          eval_(std::move(eval)),
          terminal_(terminal) {}

    bool ready() const noexcept {
        return run_future_.valid() &&
               run_future_.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    }

    /* Blocking wait + value extraction. */
    T await();

    /* Cancel the underlying EvalInstance; the returned T will throw. */
    void cancel();

private:
    std::shared_future<void>             run_future_;
    std::shared_ptr<sched::EvalInstance> eval_;
    PortRef                              terminal_{};
};

}  // namespace me::graph
