# bindings/kotlin-native — Kotlin/Native via cinterop

`media_engine.def` is a Kotlin/Native cinterop definition. Gradle
runs cinterop, generates Kotlin bindings under
`io.mediaengine.cinterop`, and links the resulting binary against
`libmedia_engine`.

## When to use this binding

- iOS / macOS / Linux / Windows hosts written in Kotlin/Native.
- Single-process, native access — no JVM startup cost.
- KMP (Kotlin Multiplatform) projects whose iOS / native targets
  call the engine directly while their JVM target uses
  `bindings/jni/`.

## Prerequisites

| Requirement | Tested on this repo |
|---|---|
| JDK | 17 or newer (Gradle bootstrap) |
| Gradle | 8.x (wrapper recommended; example uses 8.x) |
| Kotlin | 2.0.20 (declared in `example/build.gradle.kts`) |
| Engine build | `cmake --build build` — produces `build/libmedia_engine.{a,dylib}` |

The example walks up from `bindings/kotlin-native/example/` to the
repo root, expecting `include/` and `build/` to be in the
canonical layout. Override with `-PmediaEngineRoot=/abs/path` on
the gradle command line.

## Build

From repo root:

```sh
cmake -B build -S .
cmake --build build               # → build/libmedia_engine.{a,dylib}
```

Then from `bindings/kotlin-native/example/`:

```sh
./gradlew runDebugExecutableNative
```

Gradle invokes cinterop, compiles `Main.kt`, and runs the
resulting native binary. Output: engine version + a round-tripped
engine handle.

The example does **not** ship a Gradle wrapper to keep the repo
tree small — initialize one with `gradle wrapper --gradle-version 8.10`
on first run, or invoke a system Gradle (≥ 8.x).

## .def contract

`media_engine.def` declares the headers + link line:

```
headers = media_engine.h
headerFilter = media_engine.h media_engine/*
package = io.mediaengine.cinterop
linkerOpts.osx = -lmedia_engine
…
```

Paths are *not* hardcoded — consumers supply `-I<include>` and
`-L<lib>` via Gradle `compilerOpts` / `extraOpts("-libraryPath",
…)`. See `example/build.gradle.kts` for the canonical wiring.

## API surface

The cinterop tool emits Kotlin bindings for the entire public C
API (everything reachable from `media_engine.h`):

- `me_engine_create` / `me_engine_destroy`
- `me_timeline_load_json` / `me_timeline_destroy`
- `me_render_start` / `me_render_wait` / `me_render_cancel`
- `me_render_frame`
- `me_thumbnail_png` / `me_thumbnail_pixels`
- `me_probe_*`
- `me_cache_stats`
- `me_engine_version` (struct return)

Opaque handles surface as `me_engine_tVar` etc. Callers use
`memScoped { alloc<…>() }` to manage out-pointer lifetimes.

## FFmpeg licensing at deploy time

Same constraint as `bindings/jni/`: the dev box's FFmpeg is
GPL-built; production hosts must link against an LGPL FFmpeg. See
`docs/INTEGRATION.md` → "FFmpeg licensing at deploy time".

## Talevia / KMP integration

For KMP projects mixing JVM + native:

- `commonMain` declares the cross-platform engine wrapper
  interface.
- `jvmMain` implements via `bindings/jni/`.
- `nativeMain` (iOS / macOS) implements via this cinterop binding.

Both implementations target the same `io.mediaengine` namespace
shape so call sites stay platform-agnostic.

## Known limitations

- No automated ctest yet — the example runs only when developers
  invoke Gradle by hand. A future cycle adds a CMake-driven smoke
  that wraps the gradle invocation.
- The example walks `../../..` paths; symlink-heavy checkouts
  (e.g. nested git worktrees) may need `-PmediaEngineRoot`.
