# 2026-04-23 — me-version-real-git-sha

## Gap

`me_version()` returned `{0, 0, 1, ""}` — the git SHA was permanently empty.
Hosts writing bug reports had no way to pin which exact engine build they
were running; "media-engine 0.0.1" alone matches every build off of
`0.0.1` tag, dirty or not.

## 决策

Capture the git short SHA at CMake configure time and inject it into the
engine via a generated header.

1. `cmake/` style: a small block inside `src/CMakeLists.txt` (not a new
   `cmake/git_sha.cmake`) — one call site, one source of truth, under 30
   lines. No reason to abstract yet.
2. Use `find_package(Git QUIET)` + `execute_process(${GIT_EXECUTABLE} rev-parse --short HEAD)`.
3. Add `-dirty` suffix when `git status --porcelain --untracked-files=no`
   reports any modified tracked files. `--untracked-files=no` avoids
   treating unrelated scratch files in the tree (e.g. `.claude/scheduled_tasks.lock`,
   `*.o` leftovers) as "dirty" — only tracked-file modifications count.
4. Fallback to `""` when either `find_package(Git)` fails, or
   `.git/` doesn't exist (release-source-tarball build), or `git rev-parse`
   fails (shallow clone without refs). `me_version().git_sha` returning
   `""` is already documented behavior (tests only require non-null), so
   fallback is safe.
5. `configure_file(... @ONLY)` → `${BINARY_DIR}/core/version.inl` with
   `#define ME_GIT_SHA "…"` + `ME_VERSION_MAJOR/MINOR/PATCH` pulled from
   `PROJECT_VERSION_*` (so the hardcoded `0, 0, 1` in `version.cpp` also
   goes away — the `project()` line is now the single source of truth for
   the triplet too).
6. Add `${CMAKE_CURRENT_BINARY_DIR}` to the media_engine target's PRIVATE
   include dirs so `#include "core/version.inl"` resolves from inside
   `version.cpp`.

## 被拒的替代方案

1. **Runtime `popen("git rev-parse")` in `me_version()`** — rejected:
   engine shouldn't shell out at runtime, adds latency + spawn risk +
   wrong answer for installed builds where the user isn't in a repo.

2. **Embed SHA via `-DME_GIT_SHA=...` compile flag** — rejected: quoting
   a string literal through CMake's compile-options plumbing needs escape
   dances; `configure_file` with `@ONLY` is the idiomatic path and
   rebuilds only the one TU when the SHA changes.

3. **Full `git describe --tags --dirty`** — rejected for *now*: we don't
   tag releases yet, and `describe` without a tag degrades to just the
   SHA anyway. Revisit once the first tag lands; the template change
   will be a one-liner.

4. **Treat untracked files as dirty** — rejected: untracked files creep
   in from unrelated tools (IDE caches, scheduled-task locks, CMake
   build outputs accidentally outside `build/`). Reporting "dirty" for
   them produces noise without signal about whether engine code actually
   diverged from HEAD. `--untracked-files=no` keeps the flag meaningful.

5. **Regenerate `version.inl` on every build (custom command with
   `BYPRODUCTS`)** — rejected: adds a dependency edge that forces the
   version TU to rebuild on every invocation. Configure-time capture is
   sufficient; developers re-run CMake across commits anyway, and the
   value is used for bug reports, not release gating.

## 自查（§3a 设计约束）

- 类型化 effect 参数：N/A.
- 浮点时间：N/A.
- 公共头：`include/media_engine/types.h` **未改**——`me_version_t` struct
  and `me_version()` signature untouched.
- C API 异常 / STL 泄露：N/A, `me_version()` is still POD out.
- GPL：no new dep; `find_package(Git)` uses the host's git binary (not
  linked).
- 确定性：SHA is a compile-time constant per build. Runtime output
  bytes (MP4 muxer, PNG encoder) do NOT include `git_sha`; determinism
  test `test_determinism` still byte-identical (re-ran post-change,
  passed). Across-build determinism is deliberately not preserved here
  — that's the whole point of the SHA.
- Stub 净增：**no change**; `check_stubs.sh` unaffected.
- OpenGL：N/A.
- Schema：N/A.
- ABI：struct layout unchanged, function signature unchanged.

## 验证

- `cmake -B build` → `-- media-engine git SHA: c0d4bfc-dirty` printed
  at configure time (this cycle's HEAD + the uncommitted working tree).
- `cmake --build build` + `ctest --test-dir build` → 6/6 green.
- `cmake -B build-rel -DCMAKE_BUILD_TYPE=Release -DME_WERROR=ON
  -DME_BUILD_TESTS=ON && cmake --build build-rel` → clean; `ctest` 6/6.
- `01_passthrough sample.timeline.json /tmp/out_c22.mp4` → 117195 bytes,
  byte-identical to cycle 21's output (determinism preserved).
- C11 public-header compile check:
  `echo '#include <media_engine/types.h>' | clang -xc -std=c11 -fsyntax-only -Iinclude -`
  → passes.
- Tiny runtime probe: `me_version()` returns
  `v=0.0.1 sha='c0d4bfc-dirty'`. After committing this change, the
  next configure will produce a clean SHA.
- Fallback: not explicitly tested by deleting `.git/` (destructive),
  but the code path is guarded by `find_package(Git)` + `EXISTS .git`
  + `RESULT_VARIABLE` from `execute_process`, all three of which set
  `ME_GIT_SHA` to `""`.

## 影响 / 后续

- `me_version().git_sha` is now load-bearing for bug reports. Host
  integration docs should mention it (INTEGRATION.md update can be
  folded into a future pass; not critical now — the field is
  self-explanatory).
- Once the first release tag lands (post-M1), revisit the `rev-parse`
  → `describe --tags --dirty` swap; one-line template change.
