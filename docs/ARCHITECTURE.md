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

src/                            Internal C++20 implementation (private)
  core/                         engine_impl struct + version/status_str
  api/                          Thin C→C++ adapters (one .cpp per public header)
  timeline/                     JSON loader + internal IR (Timeline, Clip) + segmentation
  io/                           FFmpeg wrappers — io::demux kernel + DemuxContext
  graph/                        Node / Graph / Builder — immutable, content-hashed pure data
  task/                         Kernel registry + TaskContext + TaskKindId + param schema
  scheduler/                    Task runtime — EvalInstance + Scheduler + heterogeneous pools
  resource/                     FramePool / CodecPool (bootstrap) + AssetHashCache + content_hash
  orchestrator/                 Previewer (M6 stub) / Exporter (passthrough+h264 re-encode) /
                                CompositionThumbnailer (M6 stub; C API asset thumbnail lives in api/)

docs/                           Design docs — VISION, this file, API.md, TIMELINE_SCHEMA.md,
                                ARCHITECTURE_GRAPH.md, PAIN_POINTS.md
cmake/                          Build helpers — FindFFMPEG.cmake
examples/                       End-to-end smoke samples (01_passthrough, 04_probe,
                                05_reencode, 06_thumbnail, 02_graph_smoke, 03_timeline_segments)
tests/                          doctest suites — status, engine, timeline schema, content hash,
                                determinism
tools/                          Repo health scripts — scan-debt.sh, check_stubs.sh
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
| bgfx (via `bkaradzic/bgfx.cmake` wrapper, pin `v1.143.9226-530`) | BSD-2 | Phase 3 (GPU backend) — CMake opt-in behind `ME_WITH_GPU` (OFF default; ~5 min first-configure compile cost). `me::gpu::BgfxGpuBackend` does a headless init (0×0 resolution, `platformData.nwh = nullptr`) and picks Metal on macOS / Vulkan on Linux / D3D12 on Windows via auto-select with a Noop retry for drivers that refuse headless. `bgfx::setViewClear` + `bgfx::frame` + `bgfx::shutdown` are exercised per engine lifetime. Real render-target framebuffers are created per compose kernel (future effect-gpu-* cycles). |
| Skia (JetBrains skia-pack `m144-e2e6623374-4` prebuilt) | BSD-3 | Phase 5 (text / vector) — CMake opt-in behind `ME_WITH_SKIA` (ON default as of 2026-04-24 `skia-integration` cycle). Skia's own build system (GN + ninja) doesn't mesh with CMake, so we consume the JetBrains prebuilt binary via `FetchContent` with `URL` + `URL_HASH` per platform (macOS arm64 + x86_64 today; Linux / Windows add parallel branches when needed). The .a bundles transitive deps (FreeType, HarfBuzz, libpng, expat, zlib, ICU). `me::text::SkiaBackend` wraps SkSurface + SkCanvas + SkFontMgr_New_CoreText for macOS text rendering; cross-platform font-manager factories land with follow-up cycles. See `ab08f7b` docs: skia-integration-plan for the integration survey. |
| OpenColorIO (and its transitive deps: Imath / yaml-cpp / pystring / expat / minizip-ng / zlib / sse2neon) | BSD-3 (OCIO, Imath, pystring, sse2neon), MIT (yaml-cpp, expat), Zlib (zlib, minizip-ng) — all LGPL-clean | Phase 2 (color mgmt) — CMake opt-in behind `ME_WITH_OCIO` (ON default as of 2026-04-23 `ocio-pipeline-enable` cycle). Upstream v2.5.1 passes `CMAKE_POLICY_VERSION_MINIMUM=3.5` into its `yaml-cpp_install` ExternalProject, which fixed the nested CMake-4.x floor issue that blocked 2.3.2. `me::color::OcioPipeline` is the current return of `make_pipeline()`; bt709 ↔ sRGB ↔ linear conversion math lands with `ocio-colorspace-conversions`. |
| libass (pkg-config 0.17.4) | ISC | Phase 5 (subtitles) — CMake opt-in behind `ME_WITH_LIBASS` (ON default as of 2026-04-24 `libass-subtitles` cycle). Integration is `find_package(PkgConfig) + pkg_check_modules(libass)` against a system install (dev macOS: `brew install libass`; Linux: `apt install libass-dev`). libass's autoconf build + transitive non-CMake deps (fribidi, HarfBuzz, FreeType) made FetchContent impractical — see libass-subtitles-plan commit 6be4cde for the integration survey. `me::text::SubtitleRenderer` wraps `ass_library_init` / `ass_renderer_init` / `ass_render_frame`; `ME_HAS_LIBASS` compile def gates the codepath and auto-flips OFF when libass isn't found (CMake warning + feature skip). |
| miniaudio | MIT / public domain | Phase 4 (audio I/O) |
| SoundTouch (pin `2.4.0`) | LGPL-2.1 | Phase 4 (time-stretch) — CMake opt-in behind `ME_WITH_SOUNDTOUCH` (ON default as of 2026-04-24 `soundtouch-integration` cycle). `me::audio::TempoStretcher` (`src/audio/tempo.{hpp,cpp}`) wraps SoundTouch's `SoundTouch::setTempo` + `putSamples` / `receiveSamples`. LGPL-2.1 dynamic-link clean; SoundTouch-side `-Werror` is scoped off for 3rd-party code via `target_compile_options(SoundTouch PRIVATE -Wno-error)` — upstream PeakFinder.cpp:139 trips `-Wunused-but-set-variable`. |
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

Organized by the five execution modules defined in `docs/ARCHITECTURE_GRAPH.md`, plus the C
API surface and the feature paths that cut across modules. "Shipped" = wired end-to-end and
under regression; "Partial" = works for phase-1 scope with a narrower input set; "Stub" =
explicit `STUB:` marker in source (see `tools/check_stubs.sh`).

### Modules

| Module | Status | What's in it |
|---|---|---|
| `src/core/` | **Shipped** | `struct me_engine` (FramePool / CodecPool / AssetHashCache / Scheduler owners), `me_version`, `me_status_str`, thread-local last-error plumbing. |
| `src/api/` | **Shipped** | extern "C" adapters for every public header; exception / STL boundary discipline enforced. |
| `src/timeline/` | **Shipped (phase-1 scope)** | JSON loader accepts single-track with N contiguous clips; non-zero sourceRange allowed; `contentHash` propagated. Segmentation scaffolding in place for future multi-segment graph compilation. |
| `src/io/` | **Shipped** | `io::demux` kernel + `io::DemuxContext` RAII wrapper. All exporter paths read through this. |
| `src/graph/` | **Shipped** | `Node` / `Graph` / `Graph::Builder` — immutable pure-data model with recursive `content_hash`. Consumed by orchestrator per-clip. |
| `src/task/` | **Shipped** | `TaskKindId` registry, `TaskContext` (resource injection at dispatch), `KindInfo` with typed input/output/param schema. `IoDemux` is the first registered kernel. |
| `src/scheduler/` | **Shipped** | Taskflow-backed CPU scheduler, `evaluate_port<T>` entry, `EvalInstance`, heterogeneous pool routing by `Affinity`. |
| `src/resource/` | **Shipped (bootstrap)** | `FramePool` memory budget, `CodecPool` (stub body; actual codec caching lands with M4), `AssetHashCache` (URI → sha256 via libavutil), `content_hash` helper. |
| `src/orchestrator/` | **Shipped (phase-1 scope)** | `Exporter` with passthrough concat (multi-clip) + h264/AAC re-encode (single-clip). `Previewer::frame_at` is a tracked stub (`frame-server-impl`). `CompositionThumbnailer::png_at` is a tracked stub (`composition-thumbnail-impl`); the asset-level `me_thumbnail_png` in `src/api/` is fully implemented and bypasses it by design. |

### Feature paths (cross-cutting)

| Path | Status | Notes |
|---|---|---|
| Public C API headers (7) | **Shipped** | ABI stable; no ABI-breaking change landed since M1 started. |
| `me_probe` | **Shipped** | libavformat-backed; fills container / duration / stream metadata. |
| `me_thumbnail_png` (asset-level) | **Shipped** | seek → decode → sws_scale RGB24 → libavcodec PNG. |
| `me_render_start` passthrough | **Shipped** | Multi-clip single-track concat with DTS-continuity stitching; graph-driven demux per clip. |
| `me_render_start` re-encode | **Shipped (single-clip, Mac)** | video=h264 via `h264_videotoolbox`, audio=aac (libavcodec built-in). Multi-clip path is a tracked backlog item (`reencode-multi-clip`). |
| `me_render_frame` (frame server) | Stub → `frame-server-impl` | Arrives with M6; needs Previewer + frame cache. |
| `me_cache_stats` / `me_cache_clear` / `me_cache_invalidate_asset` | Stubs → `cache-stats-impl` / `cache-clear-impl` / `cache-invalidate-impl` | Stats stub returns a valid zeroed struct; invalidation is a no-op. Real cache + observability land with M6 frame server. |
| Determinism regression (passthrough) | **Shipped** | `test_determinism` renders the same timeline twice and byte-compares. |

Run `bash tools/check_stubs.sh` for the machine-readable current stub inventory.

## Testing

doctest suites live under `tests/` and are gated by `-DME_BUILD_TESTS=ON`. Five suites today
(all binding-compatible with CTest), one binary per suite so a crash doesn't take the rest
down with it:

| Suite | What it covers |
|---|---|
| `test_status` | Every `ME_*` enum value → `me_status_str` returns a distinct, non-empty string; `me_version` well-formed. |
| `test_engine` | `me_engine_create`/`destroy` null-safety, config propagation, thread-local per-engine `last_error` isolation across threads. |
| `test_timeline_schema` | Valid single-clip + multi-clip contiguous timelines load; negative paths (schemaVersion mismatch, malformed JSON, non-contiguous clips, effects) return the right status + populate last_error. |
| `test_content_hash` | NIST SHA-256 vectors for in-memory + streaming-from-file; `file://` URI handling; `AssetHashCache::get_or_compute` memoization and `seed` bypass. |
| `test_determinism` | Renders the same JSON timeline twice through `me_render_start` passthrough and asserts byte-identical output. Fixture generated at build time via `ffmpeg` CLI. |

Principles still pending ("aspirational") beyond the current suites:

- **ABI golden files**: dump exported symbols, diff against a committed `.abi` file. Intentional ABI changes require updating the golden + a CHANGELOG entry.
- **C-only compile of public headers**: today validated ad-hoc via `clang -xc -std=c11 -fsyntax-only -Iinclude`; not in CI yet.
- **Per-platform CI**: macOS (primary dev), Linux, Windows. iOS / Android cross-compile jobs added when their build recipes exist.
