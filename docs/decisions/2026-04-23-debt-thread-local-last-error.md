## 2026-04-23 — debt-thread-local-last-error (Milestone §M1 · Rubric §5.2)

**Context.** `docs/API.md` line 81 states `me_engine_last_error` is
**thread-local per engine**: thread A's errors must not clobber thread
B's view. The bootstrap implementation in `src/core/engine_impl.hpp`
violated that contract — a single `std::mutex`-guarded `std::string`
was shared across all threads that touched the same engine. Any
multi-threaded caller would see whichever thread wrote last, not its
own error. The `engine_impl.hpp` header even had a TODO note pointing
to this backlog item. This cycle pays the debt.

**Decision.**
- **Storage**: move last-error out of `struct me_engine` into a
  function-scope `thread_local std::unordered_map<const me_engine*,
  std::string>` returned by an inline `me::detail::thread_errors()`.
  One map per thread (compiler/runtime manages the lifetime); keyed by
  engine pointer so one thread can track multiple engines
  independently. No locks — insertion/erase on an unordered_map from
  a single thread is trivially serialized.
- **Sync-path plumbing**: `set_error` / `clear_error` / new
  `get_error` all inline in the header; callers unchanged
  (signatures still take `me_engine*` via implicit non-const→const).
  `me_engine_last_error` in `src/api/engine.cpp` now delegates to
  `me::detail::get_error`.
- **Async-path plumbing (the subtle bit)**: the Exporter worker thread
  runs off the caller's thread, so a thread_local slot it writes to
  is invisible to the API caller. Fix: `Exporter::Job` grows an
  `std::string err_msg` field. The worker populates it instead of
  calling `me::detail::set_error`. In `me_render_wait` (which runs on
  the caller's thread and joins the worker), after join, if `result !=
  ME_OK` and `err_msg` is non-empty, call
  `me::detail::set_error(engine, err_msg)` to populate the *caller's*
  slot. `me_render_job` grows an `engine` pointer so wait can call
  set_error without the caller needing to pass engine in again.
- **`struct me_engine` cleanup**: deletes `std::mutex error_mtx` and
  `std::string last_error`. Header no longer needs `<mutex>` for the
  engine struct — only for `std::call_once` elsewhere in
  `src/api/engine.cpp`, preserved.
- **Lifecycle note**: when an engine is destroyed, the current thread's
  slot for that engine is NOT explicitly cleaned up by the destructor
  — cross-thread access to another thread's `thread_local` storage
  would require OS-specific tricks. Stale entries in other threads
  are never queried (querying a destroyed engine is UB under API.md)
  and get reaped on thread exit. Acceptable for M1; noted for a later
  cycle if profiling shows accumulation.

**Tests.** Added to `tests/test_engine.cpp`:
1. `last_error is thread-local per engine` — two threads each induce
   a distinct error on the same engine via `me_timeline_load_json`,
   synchronize via atomic barrier so both errors coexist, then each
   reads `me_engine_last_error` and asserts it sees its OWN error
   (by substring: thread 1 → "json", thread 2 → "schemaVersion").
   Main thread, never calling an engine API, sees empty.
2. `clear_error on success path` — induce a parse error, then run a
   successful load on the same thread: last_error goes back to empty
   (every API entry point calls `clear_error` on enter).

**Alternatives considered.**
- **Per-engine mutex-protected map `<thread::id → string>`**: avoids
  the "stale slots survive until thread exit" property, since
  destroying the engine explicitly clears all threads' slots. But
  lookup cost is mutex + hash per API call instead of pure hash;
  destroys the hot-path optimization that motivates thread-local
  storage in the first place. Rejected.
- **Global `thread_local std::string` (single slot)**: simplest possible
  storage, but loses per-engine isolation. A caller using two engines
  on one thread would see errors from either engine commingled.
  Rejected — violates the "per engine" in the API.md contract.
- **`thread_local me_engine*` active pointer + per-engine latest-errer
  slot**: clever but adds implicit state (which engine is "active").
  API.md doesn't mandate an active-engine concept, and any bug that
  forgets to push/pop the active pointer would silently mis-attribute
  errors. Rejected.
- **Keep the mutex-based impl; document that the contract is
  aspirational**: paves over the violation instead of fixing it.
  Given that VISION §3.2 talks about determinism and debuggability,
  leaving a known-wrong implementation in place sends the wrong
  signal. Rejected.
- **Route async errors through a per-job atomic + spin**: would
  preserve the "worker writes, caller reads" shape without the
  thread-local-via-wait handoff. But `me_render_wait` already
  serializes on `std::thread::join`, which is a stronger barrier than
  any atomic we could add; leaning on it is simpler and more
  obvious. Rejected.

**Coverage.**
- `cmake --build build` + `cmake --build build-rel
  -DCMAKE_BUILD_TYPE=Release -DME_WERROR=ON -DME_BUILD_TESTS=ON` —
  both clean; `-Wall -Wextra -Wpedantic -Werror` all green.
- `ctest` Debug + Release: 5/5 suites pass (new assertions: 2 TC / 6
  assertions added to test_engine, covering multi-thread isolation +
  success clears prior error).
- `01_passthrough` / `05_reencode` regressions: both still produce
  expected output. This confirms the async-path plumbing works — a
  passthrough render calls `set_error` from the worker (now routed
  through `Job::err_msg`, harmlessly empty on success), and
  `me_render_wait` on the caller doesn't wrongly populate
  last_error on OK return.

**License impact.** No dependency changes. Thread-local storage is a
core-language facility; `std::unordered_map` was already in the link
graph.

**Registration.** Changes this cycle:
- `TaskKindId` / kernel registry — untouched.
- Resource factory / orchestrator factory — untouched at the
  factory/interface level. `Exporter::Job` struct grows an `err_msg`
  field (internal; not visible via any public symbol).
- Exported C API — no new or removed symbols. Behavior change:
  `me_engine_last_error` is now actually thread-local per engine,
  matching the long-standing API.md contract.
- CMake / FetchContent — untouched.
- JSON schema — untouched.
- Internal header change: `src/core/engine_impl.hpp` — removed
  `error_mtx` / `last_error` fields; added
  `thread_errors()` inline function + `get_error()`.
- `me_render_job` (internal wrapper in `src/api/render.cpp`) grows an
  `engine` pointer so `me_render_wait` can propagate worker error
  messages into the caller's thread-local slot.
