/*
 * StatefulResourcePool<T> — keyed pool for stateful resources that
 * outlive a single kernel invocation but stay engine-owned (kernels
 * borrow, never persist).
 *
 * Why this exists: kernels are pure functions per
 * docs/ARCHITECTURE_GRAPH.md §关键理念 — they don't hold state across
 * frames, encoder lifetimes, etc. But some operations (audio
 * timestretch via SoundTouch, swresample with in-flight buffers) need
 * continuity across chunked invocations to produce correct output.
 * The resolution: kernel stays pure, the resource lives in this pool
 * (resource module), and the kernel borrows the instance for its
 * single evaluation via TaskContext-injected pool pointers.
 *
 * Same shape as resource::CodecPool (which holds AVCodecContext
 * instances), but keyed by an arbitrary uint64_t the orchestrator
 * picks (e.g. a track UUID hash) rather than codec_id. The two pools
 * could merge into one templated facility down the line; phase-1
 * keeps them separate so each can evolve its eviction / lifecycle
 * policy independently.
 *
 * Borrow exclusivity: borrow() removes the instance from the map and
 * hands ownership to the caller via the RAII Handle; release on
 * Handle destruction returns ownership to the map. Concurrent
 * borrows of the SAME key would each get a fresh instance from the
 * factory (the second caller sees an empty slot) — this is
 * structurally fine because in practice the orchestrator serializes
 * accesses on a given track / instance_key (audio pacer is single-
 * threaded; multiple Players hit different keys). If concurrent
 * same-key access ever becomes a real pattern, the next refactor
 * adds a wait-for-return code path; current shape errs on the side
 * of simplicity.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace me::resource {

template <typename T>
class StatefulResourcePool {
public:
    /* Factory creates a fresh T when borrow() finds no cached
     * instance for a key. May return nullptr to signal "no factory
     * available" — borrow() then returns a null Handle. */
    using FactoryFn = std::function<std::unique_ptr<T>()>;

    explicit StatefulResourcePool(FactoryFn factory)
        : factory_(std::move(factory)) {}

    StatefulResourcePool(const StatefulResourcePool&)            = delete;
    StatefulResourcePool& operator=(const StatefulResourcePool&) = delete;
    StatefulResourcePool(StatefulResourcePool&&)                  = delete;
    StatefulResourcePool& operator=(StatefulResourcePool&&)       = delete;

    /* RAII handle — returns T* on destruction. Move-only. */
    class Handle {
    public:
        Handle() = default;
        Handle(Handle&& o) noexcept
            : pool_(o.pool_), key_(o.key_), t_(std::move(o.t_)) {
            o.pool_ = nullptr;
            o.key_  = 0;
        }
        Handle& operator=(Handle&& o) noexcept {
            if (this != &o) {
                release_to_pool();
                pool_  = o.pool_;
                key_   = o.key_;
                t_     = std::move(o.t_);
                o.pool_= nullptr;
                o.key_ = 0;
            }
            return *this;
        }
        ~Handle() { release_to_pool(); }

        Handle(const Handle&)            = delete;
        Handle& operator=(const Handle&) = delete;

        T*       get()        noexcept { return t_.get(); }
        const T* get()  const noexcept { return t_.get(); }
        T&       operator*()  noexcept { return *t_; }
        T*       operator->() noexcept { return t_.get(); }
        explicit operator bool() const noexcept { return static_cast<bool>(t_); }

    private:
        friend class StatefulResourcePool<T>;
        Handle(StatefulResourcePool<T>* pool, uint64_t key, std::unique_ptr<T> t)
            : pool_(pool), key_(key), t_(std::move(t)) {}

        void release_to_pool() {
            if (pool_ && t_) pool_->release(key_, std::move(t_));
        }

        StatefulResourcePool<T>* pool_ = nullptr;
        uint64_t                  key_  = 0;
        std::unique_ptr<T>        t_;
    };

    /* Borrow an instance for `key`. Reuses a previously-released
     * instance for the same key when available; otherwise calls the
     * factory. Caller's returned Handle owns the instance until it
     * goes out of scope. */
    Handle borrow(uint64_t key) {
        std::unique_ptr<T> t;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = instances_.find(key);
            if (it != instances_.end()) {
                t = std::move(it->second);
                instances_.erase(it);
            }
        }
        if (!t && factory_) t = factory_();
        if (!t) return Handle{};
        return Handle{this, key, std::move(t)};
    }

    /* Wrap a caller-constructed instance into a Handle. On Handle
     * destruction the instance is stored under `key`, just like a
     * borrowed-then-returned Handle. Returns an empty Handle when
     * `t` is null.
     *
     * Use case: kernels whose construction requires runtime
     * parameters that the factory closure can't see (e.g.
     * SoundTouch needs sample_rate + channels per instance, which
     * aren't known at engine construction time). Such kernels call
     * borrow() first; on empty Handle, build the instance from
     * input metadata and adopt() it. The pool then preserves
     * state across subsequent same-key invocations. */
    Handle adopt(uint64_t key, std::unique_ptr<T> t) {
        if (!t) return Handle{};
        return Handle{this, key, std::move(t)};
    }

    /* Drop any cached instance for `key` (e.g. after a session
     * destruction or content-hash change). Has no effect on
     * outstanding Handles for the same key — they'll release back
     * into the (now-empty) slot when they expire. */
    void evict(uint64_t key) {
        std::lock_guard<std::mutex> lk(mu_);
        instances_.erase(key);
    }

    /* Drop every cached instance. Live Handles aren't disturbed. */
    void clear() {
        std::lock_guard<std::mutex> lk(mu_);
        instances_.clear();
    }

    /* Number of cached (released) instances. Doesn't count Handles
     * currently borrowed out. Tests + diagnostics. */
    std::size_t size() const {
        std::lock_guard<std::mutex> lk(mu_);
        return instances_.size();
    }

private:
    friend class Handle;
    void release(uint64_t key, std::unique_ptr<T>&& t) {
        std::lock_guard<std::mutex> lk(mu_);
        instances_[key] = std::move(t);
    }

    mutable std::mutex                                  mu_;
    std::unordered_map<uint64_t, std::unique_ptr<T>>    instances_;
    FactoryFn                                            factory_;
};

}  // namespace me::resource
