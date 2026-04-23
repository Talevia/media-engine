# CLAUDE.md

Operational rules for working in this repo. For *why*, read `docs/VISION.md`. This file is *how*.

## Read order

1. `docs/VISION.md` — north star, non-negotiable
2. `docs/MILESTONES.md` — current milestone, exit criteria
3. `docs/ARCHITECTURE.md` — module layout, dependency policy, ABI principles
4. `docs/ARCHITECTURE_GRAPH.md` — runtime execution model (graph / task / scheduler / resource / orchestrator)
5. `docs/API.md` — C API ownership / threading / error contract
6. `docs/TIMELINE_SCHEMA.md` — JSON schema v1
7. `docs/INTEGRATION.md` — host integration (JNI, cinterop, FFmpeg licensing at deploy)
8. This file
9. Code

For autonomous "find gap → fill gap" loops, use `.claude/skills/iterate-gap/SKILL.md` — reads BACKLOG, works one gap, commits code + decision + bullet-removal in a single commit, pushes.

## Build & run

Dev machine needs `cmake ≥ 3.24`, `clang` (C++20), `ffmpeg` dev libs, internet on first configure for `FetchContent`.

```sh
cmake -B build -S .                    # first run fetches nlohmann/json
cmake --build build
./build/examples/01_passthrough/01_passthrough \
    examples/01_passthrough/sample.timeline.json \
    /tmp/out.mp4
```

| Target | Command |
|---|---|
| Library only | `cmake --build build --target media_engine` |
| All examples | `cmake --build build --target 01_passthrough` (etc.) |
| Unit tests (once scaffolded) | `cmake -B build -S . -DME_BUILD_TESTS=ON && ctest --test-dir build` |
| Release build with `-Werror` | `cmake -B build-rel -DCMAKE_BUILD_TYPE=Release -DME_WERROR=ON` |

## Architecture invariants — enforce these

1. **C API is C, not C++.** All public headers `extern "C"`, POD + opaque handles only, no STL / no exceptions crossing the boundary. Any C-only client must compile against `include/media_engine.h`.
2. **`src/` is private.** Only `include/` ships. Internal headers are `.hpp` and live under `src/`.
3. **Time is rational.** `me_rational_t {num, den}` everywhere. `double seconds`, `float seconds`, `int64_t milliseconds` are all bugs.
4. **Effect parameters are typed.** Each effect kind registers a schema (float/int/bool/string/enum/color/vec2/curve/lut_ref). No `Map<String, Float>` / `std::unordered_map<string, float>`-shaped APIs.
5. **Animated properties are uniform.** Every animatable value is either `{static: v}` or `{keyframes: [...]}` — same JSON shape, same C++ type.
6. **Handles are opaque.** `me_engine_t`, `me_timeline_t`, etc. — never expose struct body to callers. Internal `me_engine` struct lives in `src/core/engine_impl.hpp`.
7. **GPL stays out of the link graph.** New CMake `find_package` / `FetchContent` adds require a license line in `ARCHITECTURE.md` dependency table. See VISION §3.4.
8. **Determinism matters.** Same inputs → same bytes (software path). If you add parallelism or SIMD, prove determinism is preserved or label the path explicitly non-deterministic.
9. **Five-module roles stay unmixed** (see `docs/ARCHITECTURE_GRAPH.md`):
    - `graph/` = pure data (Node/Graph; multi-input/multi-output ports; no function pointers, no vtable)
    - `task/` = kernel registry + TaskContext + schema (kernels are free functions registered by TaskKindId, **never** attached to Node instances)
    - `scheduler/` = Task runtime (Task is a short-lived scheduler-internal object; kernels do NOT capture resources, all injection via TaskContext)
    - `resource/` = FramePool / CodecPool / GpuCtx / Budget (injected into TaskContext at dispatch)
    - `orchestrator/` = Previewer / Exporter / CompositionThumbnailer — hold Timeline, compile per-segment Graphs; they do NOT own Nodes, do NOT define kernels, and they are NOT editors (interactive editing is host-side). The asset-level `me_thumbnail_png` C API lives in `src/api/` and does NOT go through `CompositionThumbnailer` (two roles, two paths — see `docs/PAIN_POINTS.md` 2026-04-23)

## Anti-requirements — don't do these

Red lines. If a task requires any of these, stop and challenge per VISION §"发现不符":

- ❌ Link any GPL / AGPL library (libx264 / libx265 / Rubberband-GPL / Movit etc.)
- ❌ Use `double` or `float` for time or frame rate
- ❌ Return `Map<String, Float>` / `std::map<std::string, float>` from effect parameter APIs
- ❌ Leak C++ types (`std::string`, `std::vector`, exceptions) across `extern "C"` boundary
- ❌ Add OpenGL as the **primary** GPU backend (fallback-only allowed)
- ❌ Add a dependency without updating `ARCHITECTURE.md` dependency table
- ❌ Add an OTIO / AAF / EDL import/export path — VISION §4 forbids non-native interchange formats
- ❌ Ship a GUI / editor / preview-window inside media-engine — hosts do UI
- ❌ Downgrade `-Wall -Wextra -Werror` to placate a warning; fix the warning
- ❌ `git add -A` / `--force` / `--amend` on pushed commits (see iterate-gap hard rules)

## Known incomplete

Visible in code but not wired end-to-end — expected follow-ups from M1, not bugs:

- `me_probe` returns `ME_E_UNSUPPORTED` (backlog: `probe-impl`)
- `me_thumbnail_png` returns `ME_E_UNSUPPORTED` (backlog: `thumbnail-impl`)
- `me_render_frame` returns `ME_E_UNSUPPORTED` — frame server awaits M6
- `me_cache_stats` returns zeroed struct, `me_cache_invalidate_asset` is a no-op — real cache arrives with M4/M6
- `me_render_start` supports only `video_codec="passthrough"` + `audio_codec="passthrough"` — M1 backlog `reencode-h264-videotoolbox` is the first re-encode path
- Timeline loader accepts only single-clip single-track — M1 backlog `multi-clip-single-track` relaxes this
- `engine.last_error` uses mutex, not `thread_local` — M1 backlog `debt-thread-local-last-error`

Touching any of these → expect to wire it up rather than work around it.

## Commit conventions

Follow `git log --oneline` to match style. Current prefixes:

- `feat(<area>):` — user-visible capability (iterate-gap uses this for every backlog task, including the embedded decision file + bullet removal)
- `docs:` — standalone documentation (VISION, ARCHITECTURE, etc.)
- `docs(backlog):` — standalone backlog repopulate (iterate-gap §R)
- `fix:` / `refactor:` / `chore:` — as usual

iterate-gap produces **one** commit per cycle: `feat(<area>): <description>` containing all of:
  1. The code / test changes
  2. A new file at `docs/decisions/<yyyy-mm-dd>-<slug>.md` (never append to or edit existing decision files; do NOT reference the commit hash inside the doc — `git log` suffices)
  3. The corresponding bullet removed from `docs/BACKLOG.md`

Backlog repopulate is the exception: a standalone `docs(backlog): …` commit that only touches `docs/BACKLOG.md`.

## License compliance reminder

Dev machine's Homebrew FFmpeg is **GPL-built** (`--enable-gpl --enable-libx264`). Linking against it for local examples is OK for dev; **ship builds must use LGPL FFmpeg**. See `docs/INTEGRATION.md` → "FFmpeg licensing at deploy time". Any CI job producing a distributable artifact must validate FFmpeg build flags.
