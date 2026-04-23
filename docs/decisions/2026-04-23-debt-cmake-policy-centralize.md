## 2026-04-23 — debt-cmake-policy-centralize (Milestone §M1-debt · Rubric §5.2)

**Context.** `tests/CMakeLists.txt` carried a `set(CMAKE_POLICY_VERSION_MINIMUM
3.5)` line before `FetchContent_MakeAvailable(doctest)` because doctest
v2.4.11 declares `cmake_minimum_required(VERSION 2.8)` in its own
CMakeLists, which CMake 4.x refuses outright. PAIN_POINTS 2026-04-23
flagged this: "nlohmann/json, taskflow 未来任一依赖踩同样的地板，本项目
就会反复在每次 FetchContent 处粘一坨 policy 设置". Two sites today, a
third at the next ship-date bump — consolidate now.

**Decision.**
- New `cmake/fetchcontent_policy.cmake` — a one-line helper:
  `set(CMAKE_POLICY_VERSION_MINIMUM 3.5)`. The comment block above the
  set call documents the CMake 4.x floor issue and the scoping semantics
  (directory-level, not global — see "Alternatives" below for why).
- `tests/CMakeLists.txt` — replaces its inline `set(...)` with
  `include(fetchcontent_policy)`. Behavioral no-op; the value is the
  same.
- `src/CMakeLists.txt` — adds `include(fetchcontent_policy)` before its
  FetchContent block. Today nlohmann_json and taskflow both declare
  modern CMake minimums, so the setting is a no-op for them — the
  include is defensive, sparing a future-self cycle when either dep
  (or a new one) bumps below the 3.5 floor.
- `cmake/` is already on `CMAKE_MODULE_PATH` (set in top-level
  `CMakeLists.txt` line 15), so `include(fetchcontent_policy)` just
  works without a path.

**Alternatives considered.**
- **Set `CMAKE_POLICY_VERSION_MINIMUM` as a CACHE variable at top-level
  `CMakeLists.txt`**: one-stop shop, propagates everywhere. Rejected
  because cache entries persist across configure runs and are visible
  to everything that includes this build tree — polluting global config
  for a one-off workaround reads as hiding the issue. Per-directory
  `include()` keeps the workaround visible at each call site.
- **Inline policy setter in the helper as a `macro()` instead of bare
  `set()`**: same runtime behavior but adds a symbolic name
  (`me_fetchcontent_policy_floor()`). Rejected as overkill — the
  include itself is the name. If multiple knobs ever need to live
  together, promoting to a macro is trivial.
- **Pin doctest to a version that fixed the floor upstream**: no
  such version exists yet (doctest's active maintenance has stalled
  on this). And even when it lands, other deps will hit the same
  floor on their own schedule. Centralization is the durable move.
- **Skip the `src/CMakeLists.txt` include**: nothing in `src/`
  currently needs it. Including defensively means when a future
  dep does need it, the diff-to-fix is "bump to the new dep" not
  "bump the dep AND thread a workaround through CMake". Trivial
  insurance.

**Coverage.**
- `rm -rf build && cmake -B build -S . -DME_BUILD_TESTS=ON` fresh
  configure from scratch — succeeds with the same CMake 4.x warning
  doctest's upstream would produce regardless; build proceeds to
  completion.
- `rm -rf build-rel && cmake -B build-rel ... -DME_WERROR=ON ...`
  fresh Release configure — also clean.
- `ctest` Debug + Release: 6/6 suites pass.
- `01_passthrough` / `05_reencode` / `06_thumbnail` regressions —
  all produce expected output. Thumbnail PNG is byte-for-byte
  identical to previous cycles (7178 bytes) — confirms the CMake-
  only refactor didn't drag a stray behavioral change into the build.

**License impact.** No dependencies added or removed. The policy
setting affects how upstream CMakeLists are processed; it does not
affect link graph or runtime behavior.

**Registration.** Changes this cycle:
- `TaskKindId` / kernel registry — untouched.
- Resource factory / orchestrator factory — untouched.
- Exported C API — untouched.
- CMake — new helper `cmake/fetchcontent_policy.cmake`;
  `src/CMakeLists.txt` and `tests/CMakeLists.txt` both `include()`
  it. No new `FetchContent_Declare` / `find_package`.
- JSON schema — untouched.
