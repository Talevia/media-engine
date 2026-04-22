# media-engine

A cross-platform C++ media engine for AI-driven video creation. Built to serve [talevia](../talevia) as its "compiler" for traditional editing + effect rendering — the AIGC / ML path lives in talevia itself.

**Read first: [`docs/VISION.md`](docs/VISION.md)** — the north star. Everything else in this repo serves it.

Status: **pre-alpha**, scaffolding only. See VISION §7 for the roadmap.

## Core properties

- **Agent-driven**: declarative JSON timeline in, rendered artifacts out. No GUI.
- **Deterministic**: same timeline + same engine version + same inputs → bit-identical output.
- **Commercially licensable**: strict LGPL / BSD / MIT / Apache dependency allowlist. No GPL in the link graph.
- **Modern GPU**: Metal (mac/iOS), Vulkan (Linux/Android), D3D12 (Windows) via bgfx. OpenGL is fallback-only.
- **C API**: stable ABI surface; language bindings (Kotlin/Native, JNI, Swift, Python) are downstream concerns.

## Layout

```
include/media_engine/      Public C API headers
src/                       C++ implementation
  core/                    Types, errors, logging
  timeline/                JSON loader + internal IR
  io/                      FFmpeg wrappers (demux / decode / encode / mux)
  render/                  Frame server, effect graph, GPU backends (bgfx)
  audio/                   Mixer, resampler, effects
  api/                     C API implementation
examples/                  End-to-end samples
docs/                      Design docs — VISION, API, ARCHITECTURE, etc.
cmake/                     Build helpers
```

## Build (early scaffolding)

```sh
cmake -B build -S .
cmake --build build
```

Examples have their own dependencies (FFmpeg) — see `examples/*/README.md`.
