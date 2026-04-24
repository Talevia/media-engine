/*
 * test_render_thread — unit coverage for me::gpu::RenderThread
 * (the MPSC-queue-backed single-worker-thread class used by
 * BgfxGpuBackend to pin bgfx's API thread).
 *
 * Platform-agnostic: no bgfx link, compiles and runs in every
 * build (ME_WITH_GPU=OFF included).
 */
#include <doctest/doctest.h>

#include "gpu/render_thread.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

TEST_CASE("RenderThread: ctor + dtor without work is clean") {
    /* Smoke: construct, destruct, no work. Asserts the thread
     * spawns and joins cleanly with an empty queue. */
    me::gpu::RenderThread rt;
    CHECK(rt.queue_depth() == 0);
}

TEST_CASE("RenderThread: submit_sync runs work on the worker") {
    me::gpu::RenderThread rt;

    std::thread::id caller_id = std::this_thread::get_id();
    std::thread::id work_id{};
    bool            ran = false;

    rt.submit_sync([&] {
        work_id = std::this_thread::get_id();
        ran     = true;
    });

    CHECK(ran);
    CHECK(work_id != caller_id);
    CHECK(rt.queue_depth() == 0);
}

TEST_CASE("RenderThread: is_current_thread distinguishes caller from worker") {
    me::gpu::RenderThread rt;

    CHECK_FALSE(rt.is_current_thread());  // caller is not the worker

    bool inside_is_current = false;
    rt.submit_sync([&] {
        inside_is_current = rt.is_current_thread();
    });
    CHECK(inside_is_current);  // inside submit_sync, we ARE the worker
}

TEST_CASE("RenderThread: submit_sync blocks until work completes") {
    me::gpu::RenderThread rt;

    /* Work that takes a measurable amount of time; submit_sync must
     * not return until the counter is incremented. Use atomic +
     * small sleep to make the timing unambiguous. */
    std::atomic<int> counter{0};
    rt.submit_sync([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        counter.fetch_add(1);
    });
    CHECK(counter.load() == 1);
}

TEST_CASE("RenderThread: submit_sync preserves FIFO order under rapid submits") {
    me::gpu::RenderThread rt;

    std::vector<int>     order;
    std::mutex           order_mu;

    for (int i = 0; i < 50; ++i) {
        rt.submit_sync([&, i] {
            std::lock_guard<std::mutex> lk(order_mu);
            order.push_back(i);
        });
    }

    REQUIRE(order.size() == 50);
    for (int i = 0; i < 50; ++i) {
        CHECK(order[i] == i);
    }
}

TEST_CASE("RenderThread: submit (fire-and-forget) executes eventually") {
    me::gpu::RenderThread rt;
    std::atomic<bool> ran{false};

    rt.submit([&] { ran.store(true); });

    /* Follow with a submit_sync to flush the queue; by then `ran`
     * must have been set (FIFO ordering). */
    rt.submit_sync([] { /* barrier */ });
    CHECK(ran.load());
}

TEST_CASE("RenderThread: submit_sync rethrows exceptions from worker") {
    me::gpu::RenderThread rt;

    bool caught = false;
    std::string msg;
    try {
        rt.submit_sync([] {
            throw std::runtime_error("boom from worker");
        });
    } catch (const std::runtime_error& e) {
        caught = true;
        msg    = e.what();
    }

    CHECK(caught);
    CHECK(msg == "boom from worker");
    /* The thread must still be alive + serving after propagation. */
    std::atomic<bool> post_ran{false};
    rt.submit_sync([&] { post_ran.store(true); });
    CHECK(post_ran.load());
}

TEST_CASE("RenderThread: pending work drains before dtor returns") {
    std::atomic<int> total{0};
    {
        me::gpu::RenderThread rt;
        /* Submit fire-and-forget work and immediately destroy the
         * thread. Everything queued before dtor should still run. */
        for (int i = 0; i < 20; ++i) {
            rt.submit([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                total.fetch_add(1);
            });
        }
        /* dtor here — must wait for all 20 to complete. */
    }
    CHECK(total.load() == 20);
}
