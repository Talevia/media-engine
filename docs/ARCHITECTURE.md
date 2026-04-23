# Architecture

Snapshot of the current repository layout, build system, and C API design principles. Read `VISION.md` first — that's where the why lives. This file is the how.

## Module layout

```
include/media_engine.h          Master include (pulls in all public headers)
include/media_engine/           Public C API — ABI-stable surface
  types.h                       Handles, status codes, version, me_rational_t
  engine.h                      Engine lifecycle (create / destroy / last_error)
  timeline.h                    Timeline load + queries
  render.h                      Batch render + frame server
  probe.h                       Media probe
  thumbnail.h                   Single-frame PNG
  cache.h                       Cache observability + invalidation

src/                            Internal C++17/20 implementation (private)
  core/                         Shared types, engine impl struct, version
  api/                          Thin C→C++ adapters (one .cpp per public header)
  timeline/                     JSON loader + internal IR + segmentation (see docs/ARCHITECTURE_GRAPH.md)
  io/                           FFmpeg wrappers (current: remux; refactoring into io::demux kernel)
  graph/                        (scaffolded) Node / Graph / Builder / compiler — 见 ARCHITECTURE_GRAPH.md
  task/                         (scaffolded) Kernel registry + TaskContext + schema
  scheduler/                    (scaffolded) Task runtime + heterogeneous pools + evaluate_port
  resource/                     (scaffolded) FramePool / CodecPool / GpuCtx / Budget
  orchestrator/                 (scaffolded) Previewer / Exporter / Thumbnailer — 持 Timeline，按段驱动

docs/                           Design docs — VISION, this file, API.md, etc.
cmake/                          Build helpers
examples/                       End-to-end samples (added in Phase 1 demo commit)
```

Rule: **nothing in `src/` is exported**. Only `include/` is part of the ABI. Internal headers (`.hpp`) live in `src/` and are never installed.

## C API design principles

These exist to make the ABI survivable across C++ toolchain changes and to keep Kotlin/Native cinterop, JNI, Swift, and Python bindings trivially generatable.

1. **Headers are C, not C++.** All public headers are `extern "C"`-guarded and use only C types. No `std::string`, no templates, no inheritance — opaque handles + POD structs + function pointers.
2. **Handles are opaque.** Struct bodies of `me_engine_t` etc. live in `src/`; callers only hold pointers. This lets us evolve internals without breaking the ABI.
3. **Ownership is stated, not inferred.** Every function that returns a pointer/handle documents who frees it (caller via `me_*_destroy` / `me_buffer_free`, or engine-owned valid-until-next-call).
4. **Errors return `me_status_t`.** Out-params via `T**`. Detailed messages via `me_engine_last_error(engine)`, thread-local so concurrent callers don't clobber each other.
5. **Threading contract is explicit.** Engine-level calls are thread-safe; per-handle calls (on the same timeline or render job) are not. Callbacks fire on engine-owned threads — callers are responsible for synchronizing onto their own threads.
6. **No exceptions cross the ABI.** Internal C++ may throw; `extern "C"` boundaries catch and translate to status codes. (To be implemented in the boundary wrapper once real impl lands.)
7. **No global state.** All state lives under an engine handle. This enables multi-engine hosts (test fixtures, sandboxed render pools) without surprise.

See `API.md` for the full surface explained call-by-call; this file is about principles.

## Build system

CMake ≥ 3.24, C++20, C11.

```
cmake -B build -S .
cmake --build build
```

Produces `libmedia_engine.a` (static by default) with the public headers in `include/`.

Options:

| Option | Default | Purpose |
|---|---|---|
| `ME_BUILD_EXAMPLES` | `ON`  | Build `examples/` if present |
| `ME_BUILD_TESTS`    | `OFF` | Enable CTest + unit test targets |
| `ME_WERROR`         | `OFF` | Treat warnings as errors (on in CI) |

No external dependencies are wired in at this commit — the skeleton stands alone. FFmpeg, bgfx, Skia, etc. are introduced by the commits that first need them.

## Dependency policy

VISION §3.4 locks the supply chain at LGPL-clean. Enforcement happens here:

| Allowed | License | When added |
|---|---|---|
| FFmpeg (LGPL build only, no `--enable-gpl`) | LGPL-2.1+ | Phase 1 (I/O) |
| bgfx     | BSD-2  | Phase 3 (GPU backend) |
| Skia     | BSD-3  | Phase 5 (text / vector) |
| OpenColorIO | BSD-3 | Phase 2 (color mgmt) |
| libass   | ISC    | Phase 5 (subtitles) |
| miniaudio | MIT / public domain | Phase 4 (audio I/O) |
| SoundTouch | LGPL-2.1 | Phase 4 (time-stretch) |
| spdlog   | MIT    | any time logging is wired |
| nlohmann::json | MIT | Phase 1 (JSON) |
| doctest  | MIT    | when tests are enabled |
| Taskflow | MIT    | CPU work-stealing task DAG (scheduler 核心) |

**Rejected / never-add list**:
- Any GPL / AGPL library in the direct link graph
- `libx264`, `libx265` direct linkage (GPL) — encoding goes through HW encoders (VideoToolbox, NVENC, QSV, VAAPI, AMF)
- Rubberband default mode (GPL) — SoundTouch is the default; Rubberband can be a paid-license optional module
- OpenGL as the primary GPU backend (deprecated on macOS) — fallback-only

New dependencies must be added to this table in the same PR that introduces them, with a one-line justification in the PR description.

## Current implementation state

Organized by the five execution modules defined in `docs/ARCHITECTURE_GRAPH.md`. "Scaffolded" = directory + README exist, no code; backlog bullet tracks impl.

| Module | Status | Notes |
|---|---|---|
| Public C API headers | **Shipped** | 7 sub-headers, ABI stable; no changes planned for this architecture phase |
| Engine create/destroy, `me_version`, `me_status_str` | **Shipped** | |
| `timeline/` | **Partial** | JSON loader shipped (single-clip subset); `segmentation` pending (backlog `timeline-segmentation`) |
| `graph/` | **Scaffolded** | See `src/graph/README.md`; impl by backlog `graph-task-bootstrap` |
| `task/` | **Scaffolded** | Kernel registry + TaskContext + schema; impl by `graph-task-bootstrap` |
| `scheduler/` | **Scaffolded** | Evaluate_port entry + EvalInstance + heterogeneous pools; impl by `graph-task-bootstrap` + `taskflow-integration` |
| `resource/` | **Scaffolded** | FramePool / CodecPool / GpuCtx / Budget; impl by `engine-owns-resources` |
| `orchestrator/` | **Scaffolded** | Previewer / Exporter / Thumbnailer; impl by `orchestrator-bootstrap` |
| `me_probe` / `me_thumbnail_png` | Stub → `ME_E_UNSUPPORTED` | Backlog: `probe-impl`, `thumbnail-impl` |
| `me_render_start` (passthrough) | **Shipped** | Current single-thread direct FFmpeg remux; migrating to `io::demux` kernel + Exporter specialization via `refactor-passthrough-into-graph-exporter` |
| `me_render_start` (re-encode) | Not yet | Backlog: `reencode-h264-videotoolbox` (first LGPL-clean encode path) |
| `me_render_frame` (frame server) | Stub → `ME_E_UNSUPPORTED` | Arrives with M6; needs Previewer + cache layer |
| Cache | Partially shipped | Stats returns zeroed-but-valid struct; real cache arrives with M6 |

## Testing philosophy (aspirational; no tests yet)

- **C API smoke tests** for every public function — runs under a C-only compiler (not just C++) to catch `extern "C"` regressions.
- **Determinism tests**: render the same timeline twice, diff bytes. Any non-determinism is a bug to isolate and document.
- **ABI golden files**: dump exported symbols, diff against a committed `.abi` file. Intentional ABI changes require updating the golden + a CHANGELOG entry.
- **Per-platform CI**: macOS (primary), Linux, Windows. iOS / Android cross-compile jobs added when their build recipes exist.
