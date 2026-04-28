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
  orchestrator/                 Player (playback session) / Exporter (passthrough+h264 re-encode) /
                                compose_frame.{hpp,cpp} (per-frame free functions for me_render_frame
                                + composition PNG thumbnail; C API asset thumbnail lives in api/)

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
| ONNX Runtime (system pkg-config: `libonnxruntime` ≥ 1.16) | MIT | Phase M11 (ML inference cross-platform CPU FP32 reference) — CMake auto-detection behind `ME_WITH_INFERENCE` via `pkg_check_modules(libonnxruntime)`. Linked dynamically (`libonnxruntime.dylib` / `.so`); host installs via `brew install onnxruntime` (macOS) or distro package (Linux). When pkg-config doesn't find it the `OnnxRuntime` backend isn't built — the engine falls back to `CoreMlRuntime` on Apple, or fails to instantiate any runtime on non-Apple (acceptable: ML-effect kernels return ME_E_UNSUPPORTED with a backlog-tracked slug). `Ort::Session` runs single-threaded with `ORT_ENABLE_BASIC` graph optimization; ML inference is in VISION §3.4's non-deterministic carve-out. |
| Kvazaar (system pkg-config: `kvazaar` ≥ 2.3) | BSD-3 | Phase M10 (SW HEVC fallback for hosts lacking `hevc_videotoolbox`) — CMake opt-in behind `ME_WITH_KVAZAAR` (default OFF, since the binary-size hit is real and most hosts have HW HEVC). When `ME_WITH_KVAZAAR=ON` and `pkg_check_modules(kvazaar)` succeeds, `me::io::KvazaarHevcEncoder` (`src/io/kvazaar_hevc_encoder.{hpp,cpp}`) wraps `kvz_api_get(8)` + `encoder_open` + `encoder_encode` to produce HEVC Main 8-bit Annex-B bitstreams. SW path is 1080p-ceiling (`create()` rejects > 1920×1080 with named diag) per VISION §3.4. Single-threaded encode (`--threads=0 --owf=0`) for byte-stable output within a single Kvazaar build + host architecture; cross-arch determinism not promised (per VISION §3.4 SW HEVC carve-out). Main 10 (10-bit HDR) requires a `KVZ_BIT_DEPTH=10` Kvazaar build — tracked as follow-up `encode-hevc-main10-kvazaar-source-build`. Replaces the never-suitable libx264 / libx265 GPL options. |
| CoreML.framework (Apple SDK) | Apple SDK | Phase M11 (ML inference Apple HW path) — system framework, no FetchContent. Linked as `-framework CoreML -framework Foundation` only when `APPLE AND ME_WITH_INFERENCE`. `MLModel.compileModelAtURL:` lazy-loads the host-supplied blob; `predictionFromFeatures:` runs inference. |

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
| `src/io/` | **Shipped** | `io::demux` kernel + `io::decode_video` kernel + `io::DemuxContext` RAII wrapper. Exporter consumes demux directly; the per-frame path (`compose_frame_at` / Player) consumes the demux + decode chain through the scheduler. |
| `src/graph/` | **Shipped** | `Node` / `Graph` / `Graph::Builder` — immutable pure-data model with recursive `content_hash`. Each `Node` carries the `time_invariant` and `cacheable` flags mirrored from its `KindInfo`. |
| `src/task/` | **Shipped** | `TaskKindId` registry, `TaskContext` (resource injection at dispatch), `KindInfo` with typed input/output/param schema + `cacheable` flag. Three production kernels registered: `IoDemux`, `IoDecodeVideo`, `RenderConvertRgba8`. |
| `src/scheduler/` | **Shipped** | Taskflow-backed CPU scheduler, `evaluate_port<T>` entry, `EvalInstance`, heterogeneous pool routing by `Affinity`. `OutputCache` peek-before-dispatch on `(content_hash, time, port_idx)` per ARCHITECTURE_GRAPH §缓存集成; gated by `KindInfo::cacheable` so stateful-handle outputs (e.g. `IoDemux`'s AVFormatContext) bypass the cache. |
| `src/resource/` | **Shipped (bootstrap)** | `FramePool` memory budget, `CodecPool` (stub body; actual codec caching lands with M4), `AssetHashCache` (URI → sha256 via libavutil), `content_hash` helper. |
| `src/orchestrator/` | **Shipped (phase-1 scope)** | `Exporter` with passthrough concat (multi-clip) + h264/AAC re-encode (single-clip). `compose_frame.{hpp,cpp}` is the framework's first per-frame client — `compile_frame_graph` builds the `IoDemux + IoDecodeVideo + RenderConvertRgba8` graph that `me_render_frame` / `Player::producer_loop` / `compose_png_at` evaluate via the scheduler. Multi-track compose is the follow-up bullet (`per-frame-multi-track-compose-graph`). `compose_png_at` reuses `compose_frame_at` + scales + encodes PNG; the asset-level `me_thumbnail_png` in `src/api/` is fully implemented and bypasses by design. |

### Feature paths (cross-cutting)

| Path | Status | Notes |
|---|---|---|
| Public C API headers (7) | **Shipped** | ABI stable; no ABI-breaking change landed since M1 started. |
| `me_probe` | **Shipped** | libavformat-backed; fills container / duration / stream metadata. |
| `me_thumbnail_png` (asset-level) | **Shipped** | seek → decode → sws_scale RGB24 → libavcodec PNG. |
| `me_render_start` passthrough | **Shipped** | Multi-clip single-track concat with DTS-continuity stitching; graph-driven demux per clip. |
| `me_render_start` re-encode | **Shipped (single-clip, Mac)** | video=h264 via `h264_videotoolbox`, audio=aac (libavcodec built-in). Multi-clip path is a tracked backlog item (`reencode-multi-clip`). |
| `me_render_frame` (frame server) | **Shipped (single-track)** | `src/api/render.cpp` calls `resolve_active_clip_at` + DiskCache peek + `compose_frame_at`, which runs the per-frame `IoDemux + IoDecodeVideo + RenderConvertRgba8` graph through the scheduler. Cross-process repeats hit the `DiskCache` layer, in-process repeats hit the scheduler's `OutputCache`. Multi-track compose follow-up is `per-frame-multi-track-compose-graph`. |
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
