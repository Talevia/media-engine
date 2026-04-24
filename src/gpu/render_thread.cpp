#include "gpu/render_thread.hpp"

#include <cassert>
#include <condition_variable>
#include <exception>
#include <mutex>

namespace me::gpu {

RenderThread::RenderThread() {
    thread_ = std::thread([this] { this->run(); });
}

RenderThread::~RenderThread() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        shutdown_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void RenderThread::submit(std::function<void()> work) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        assert(!shutdown_ && "RenderThread::submit after shutdown");
        if (shutdown_) return;
        q_.emplace(std::move(work));
    }
    cv_.notify_one();
}

void RenderThread::submit_sync(std::function<void()> work) {
    /* Synchronous submit: enqueue a wrapper that runs the user's
     * lambda, captures any exception, then flips a done flag +
     * notifies the caller's condition variable. Caller blocks on
     * the condition until done, then rethrows if there was an
     * exception.
     *
     * All state lives on the caller's stack; the lambda captures
     * by reference. The lambda completes (done=true) before the
     * worker returns from executing it, so the caller's wake is
     * guaranteed before `done` / `mu` / `cv` / `err` are destroyed. */
    std::mutex              done_mu;
    std::condition_variable done_cv;
    bool                    done = false;
    std::exception_ptr      err  = nullptr;

    submit([&] {
        try {
            work();
        } catch (...) {
            err = std::current_exception();
        }
        {
            std::lock_guard<std::mutex> lk(done_mu);
            done = true;
        }
        done_cv.notify_one();
    });

    {
        std::unique_lock<std::mutex> lk(done_mu);
        done_cv.wait(lk, [&done] { return done; });
    }

    if (err) std::rethrow_exception(err);
}

bool RenderThread::is_current_thread() const noexcept {
    return std::this_thread::get_id() == thread_.get_id();
}

std::size_t RenderThread::queue_depth() const {
    std::lock_guard<std::mutex> lk(mu_);
    return q_.size();
}

void RenderThread::run() {
    for (;;) {
        std::function<void()> work;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] { return shutdown_ || !q_.empty(); });

            if (q_.empty()) {
                /* shutdown_ && empty → time to exit. Any work
                 * enqueued before shutdown_ was set is already
                 * drained by prior iterations. */
                return;
            }

            work = std::move(q_.front());
            q_.pop();
        }
        work();
    }
}

}  // namespace me::gpu
