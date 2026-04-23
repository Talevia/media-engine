/*
 * Global TaskKindId → KindInfo registry.
 *
 * Kernels register themselves at engine startup (or on-demand). The
 * registry is process-wide, lock-protected, and append-only during runtime.
 * Lookups are O(log N) via std::map; kernels are invoked by scheduler via
 * the KernelFn pointer stored here.
 */
#pragma once

#include "task/task_kind.hpp"

namespace me::task {

/* Register a kind. Repeated registration of the same (kind, affinity) pair
 * overwrites the previous entry — deterministic for test fixtures. */
void register_kind(const KindInfo&);

/* Convenience: register an alternative kernel for an already-registered
 * kind but a different affinity (e.g. GPU variant of a CPU blur). */
void register_variant(TaskKindId, Affinity, KernelFn);

/* Look up the primary KindInfo for a kind. Returns nullptr if unknown. */
const KindInfo* lookup(TaskKindId);

/* Pick the best kernel given an affinity hint. Falls back to the primary
 * registration if no matching variant exists. Returns nullptr on unknown kind. */
KernelFn best_kernel_for(TaskKindId, Affinity hint);

/* Testing helper — wipes the registry. Not thread-safe with running kernels. */
void reset_registry_for_testing();

}  // namespace me::task
