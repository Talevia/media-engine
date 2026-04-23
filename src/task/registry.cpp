#include "task/registry.hpp"

#include <map>
#include <mutex>
#include <utility>

namespace me::task {

namespace {

struct VariantKey {
    TaskKindId kind;
    Affinity   affinity;
    bool operator<(const VariantKey& o) const noexcept {
        if (kind != o.kind) return kind < o.kind;
        return affinity < o.affinity;
    }
};

struct Registry {
    std::mutex                      mtx;
    std::map<TaskKindId, KindInfo>  primary;
    std::map<VariantKey, KernelFn>  variants;
};

Registry& g_registry() {
    static Registry r;
    return r;
}

}  // namespace

void register_kind(const KindInfo& info) {
    auto& r = g_registry();
    std::lock_guard<std::mutex> lk(r.mtx);
    r.primary[info.kind] = info;
    /* Primary registration also populates the variants table for its own
     * affinity, so best_kernel_for can treat primary and explicit variants
     * uniformly. */
    r.variants[{info.kind, info.affinity}] = info.kernel;
}

void register_variant(TaskKindId kind, Affinity aff, KernelFn fn) {
    auto& r = g_registry();
    std::lock_guard<std::mutex> lk(r.mtx);
    r.variants[{kind, aff}] = fn;
}

const KindInfo* lookup(TaskKindId kind) {
    auto& r = g_registry();
    std::lock_guard<std::mutex> lk(r.mtx);
    auto it = r.primary.find(kind);
    return it == r.primary.end() ? nullptr : &it->second;
}

KernelFn best_kernel_for(TaskKindId kind, Affinity hint) {
    auto& r = g_registry();
    std::lock_guard<std::mutex> lk(r.mtx);
    /* Preferred: explicit variant matching the hint. */
    auto v = r.variants.find({kind, hint});
    if (v != r.variants.end()) return v->second;
    /* Fallback: primary kernel regardless of hint. */
    auto p = r.primary.find(kind);
    return p == r.primary.end() ? nullptr : p->second.kernel;
}

void reset_registry_for_testing() {
    auto& r = g_registry();
    std::lock_guard<std::mutex> lk(r.mtx);
    r.primary.clear();
    r.variants.clear();
}

}  // namespace me::task
