# OpenFX host integration

M13 bootstrap target. The host-side path that lets media-engine
load OFX plugins (the cross-DCC effect plugin standard used by
Resolve, Nuke, Fusion, Vegas).

## Status

What landed (cycle `m13-openfx-host-bootstrap`):

- This document, scoping the planned src/openfx/ module + the
  four follow-up bullets that implement it in vertical slices.

What's NOT done yet (split out as P1 follow-ups in BACKLOG):

- `m13-openfx-sdk-vendor` — vendor the OFX 1.4 SDK headers
  (BSD-licensed) into `third_party/openfx/` or set up
  FetchContent. Headers ship the C ABI types + protocol
  constants that any host implementation needs.
- `m13-openfx-plugin-cache-impl` — implement PluginCache in
  `src/openfx/plugin_cache.{hpp,cpp}`: scan a directory,
  dlopen each `*.ofx` bundle, call `OfxGetNumberOfPlugins`
  + `OfxGetPlugin`, populate plugin descriptor table.
- `m13-openfx-image-effect-suite-impl` — implement
  `kOfxImageEffectSuite`'s 30+ function pointers in
  `src/openfx/image_effect_suite.{hpp,cpp}`. Minimal first
  pass: just the lifecycle calls (Load / Describe /
  DescribeInContext / Render) + memory + property
  primitives.
- `m13-openfx-render-smoke` — load one OFX plugin from disk
  via the cache, run kOfxImageEffectActionRender on a
  synthetic input frame, verify the output frame is
  populated. Env-gated on `ME_TEST_OFX_PLUGIN_PATH`
  (same pattern as test_blazeface_e2e + test_inference_*).

## Why OpenFX

OFX is the cross-DCC plugin standard:
- Resolve, Nuke, Fusion, Vegas, Premiere all consume OFX.
- ~hundreds of commercial plugins (RE:Vision Effects, BCC,
  Sapphire) ship as `.ofx` bundles.
- Being able to load OFX plugins means media-engine can
  serve as a timeline back-end for those tools without
  re-implementing each effect.
- Conversely: hosts with their own effects (an in-house
  styler, a research effect) can ship them as `.ofx` and
  drop them into a media-engine-driven pipeline.

## Design summary

### File layout (planned)

```
src/openfx/
├── README.md                  — module-internal design notes
├── plugin_cache.{hpp,cpp}     — discovery + dlopen
├── image_effect_suite.{hpp,cpp} — kOfxImageEffectSuite host impl
├── property_suite.{hpp,cpp}   — kOfxPropertySuite host impl
├── parameter_suite.{hpp,cpp}  — kOfxParameterSuite host impl
├── memory_suite.{hpp,cpp}     — kOfxMemorySuite host impl
├── render_dispatch.{hpp,cpp}  — kOfxImageEffectActionRender
└── ofx_runtime.hpp            — opaque host context object
                                  threaded into all suite calls

third_party/openfx/             — vendored SDK headers (~30 .h)
include/ofxCore.h               — re-export for plugin authors
                                  (optional — only if hosting
                                  in-tree OFX plugins)
```

### Host suite responsibilities

OFX delegates almost everything to the host via "suites" — C
ABI tables of function pointers. Host implements each suite;
plugins call into it via `OfxHost::fetchSuite(name, version)`.

Minimum viable host:
- `kOfxImageEffectSuite` (v3 or v4): clip image fetch +
  release, render begin/end, info queries, action redirects.
- `kOfxPropertySuite` (v1 or v2): typed property get/set on
  the OfxImageEffectHandle / OfxClipHandle / OfxPropertySet.
- `kOfxParameterSuite` (v1): instance + descriptor parameter
  ops. (Initial render-only path can stub these to ME_E_UNSUPPORTED
  for non-essential param types.)
- `kOfxMemorySuite` (v1): memoryAlloc / memoryFree (forwarded
  to the engine's FramePool / std::malloc).

### Plugin lifecycle

```
host                                    plugin
├─ dlopen("Effect.ofx")                  …
├─ OfxGetNumberOfPlugins()               returns N
├─ OfxGetPlugin(0)                       returns OfxPlugin*
├─ plugin->setHost(&host)                stores host
├─ plugin->mainEntry(kOfxActionLoad)     plugin init
├─ for each plugin instance:
│  ├─ mainEntry(kOfxImageEffectActionDescribe)
│  ├─ mainEntry(kOfxImageEffectActionDescribeInContext)
│  ├─ create instance handle
│  └─ at render time:
│     └─ mainEntry(kOfxImageEffectActionRender)
└─ at unload: mainEntry(kOfxActionUnload) + dlclose
```

The render path takes the host-prepared input clip (with the
input frame's RGBA bytes attached as a Property), invokes the
plugin's render entry, and reads the output clip's bytes.

### Determinism + license tagging

- VISION §3.1 (deterministic software path) does NOT extend
  to OFX plugins — third-party plugin behavior is opaque.
  Host wrapping uses "non-deterministic-effect" labeling on
  outputs of OFX-driven nodes, same as the M11 ML inference
  carve-out (§3.4).
- Per-plugin license tagging at load time. The host queries
  the plugin's `kOfxPropPluginDescription` + the bundle's
  `Info.plist`-equivalent metadata (when present) to record
  the license. Plugin licenses VARY (commercial / GPL /
  proprietary) — host SHOULD NOT load GPL plugins into a
  closed-source app since linking-style dlopen is GPL-
  contagious in some interpretations. Per-plugin license
  whitelist mirrors the M11 ML model whitelist (Apache /
  MIT / BSD / explicit commercial license string).

### Threading

OFX plugins assume single-threaded entry by default
(kOfxImageEffectInstancePropSequentialRender = 0). Host
serializes calls per-instance unless the plugin advertises
parallel-render support. Engine-side: each OFX node in the
compose graph is its own task::TaskKindId
(`RenderOfxPlugin`); the scheduler's existing per-node
serialization is sufficient.

## License compliance

- OFX standard: BSD (vendored SDK headers ship cleanly).
- Per-plugin licenses: VARY. Host enforces a license-string
  whitelist at load (parallel to M11 model-license whitelist).
- Engine code (host-side, not plugin code): ME_WITH_OFX
  CMake gate; OFF by default until plugin support is wanted.

## Out of scope

- Multi-clip OFX inputs (e.g. mask input + main input).
  Initial render path supports single-clip plugins only.
- Animated parameters. Static + bake-at-load.
- Resolve-style OFX overlay parameters (drag handles in the
  preview). Host-side UI is not in media-engine's scope.
- Time-varying plugins (e.g. plugins that ask for input from
  a different timeline time). Single-frame at a time only.

## Reference material

- OpenFX 1.4 spec: <https://openfx.readthedocs.io/en/master/Reference/index.html>
- OFX SDK (BSD): <https://github.com/AcademySoftwareFoundation/openfx>
- Sample plugins for testing: the SDK bundles `Basic`,
  `Custom`, `DepthDescriptor` examples that build with `make`
  on macOS / Linux.
