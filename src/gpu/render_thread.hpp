/*
 * me::gpu::RenderThread — single-worker-thread MPSC queue for
 * pinning all GPU backend API calls to one thread.
 *
 * Motivation: bgfx's threading contract is "all bgfx::* calls
 * (except bgfx::frame) must come from the same thread — the
 * 'API thread', determined by whoever calls bgfx::init". Today
 * that's the engine-create thread (ctor of BgfxGpuBackend), which
 * works because no other thread calls bgfx yet. Once compose
 * kernels start issuing GPU commands from scheduler worker threads
 * (future `effect-gpu-*` + compose-sink-gpu-path cycles) that
 * contract breaks.
 *
 * Design: RenderThread owns a std::thread and a work queue. External
 * code calls submit_sync(lambda) — the lambda is enqueued, the
 * worker pops and runs it, and submit_sync blocks until done.
 * BgfxGpuBackend routes its bgfx::init / bgfx::shutdown and any
 * future per-frame bgfx calls through submit_sync so bgfx sees
 * exactly one API thread: the worker.
 *
 * Exception propagation: submit_sync rethrows on the caller thread
 * any exception the worker-run lambda threw. This keeps doctest
 * CHECK / REQUIRE behavior intact across the thread boundary
 * (they throw doctest-internal types on failure).
 *
 * Shutdown: dtor sets shutdown_, notifies the worker, and joins.
 * Any work enqueued before shutdown completes before the worker
 * exits. Work submitted after dtor starts is a use-after-destroy
 * bug — asserted in debug. Don't share a RenderThread reference
 * past its owner's lifetime.
 */
#pragma once

#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>

namespace me::gpu {

class RenderThread {
public:
    RenderThread();
    ~RenderThread();

    RenderThread(const RenderThread&)            = delete;
    RenderThread& operator=(const RenderThread&) = delete;
    RenderThread(RenderThread&&)                 = delete;
    RenderThread& operator=(RenderThread&&)      = delete;

    /* Enqueue `work` and block until it completes on the worker.
     * Re-raises any exception the worker saw while running `work`
     * on the caller thread. */
    void submit_sync(std::function<void()> work);

    /* Fire-and-forget submit — work runs on the worker at some
     * unspecified time, the caller does not wait. Exceptions
     * escaping `work` are swallowed; use submit_sync if you need
     * to observe failure. */
    void submit(std::function<void()> work);

    /* True iff the caller is the worker thread — handy for
     * assertions in code that must run on the render thread. */
    bool is_current_thread() const noexcept;

    /* Number of pending work items (approximate; sampled under the
     * lock). Useful for telemetry / test oracles, not for control
     * flow. */
    std::size_t queue_depth() const;

private:
    void run();

    mutable std::mutex                mu_;
    std::condition_variable           cv_;
    std::queue<std::function<void()>> q_;
    bool                              shutdown_ = false;

    /* Worker thread. Declared last so it's constructed after mu_ /
     * cv_ / q_ / shutdown_ are ready and sees a valid this. Dtor
     * order is reverse: thread_ joins first, then the synchronization
     * primitives tear down. */
    std::thread                       thread_;
};

}  // namespace me::gpu
