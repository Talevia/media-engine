# bindings/jni — JVM hosts (Java, Kotlin/JVM, Scala)

Thin JNI wrapper around the public C API. Produces
`libmedia_engine_jni.{dylib,so,dll}`; Java loads it via
`System.loadLibrary("media_engine_jni")`.

## When to use this binding

- JVM hosts (talevia, Android Studio plugins, Kotlin/JVM tools).
- Single-process, in-VM access to the engine — no IPC overhead.
- Hosts already on a JVM build system (Gradle / Maven / Bazel).

For Kotlin/Native or Swift, see `bindings/kotlin-native/` (cinterop)
or hand-roll a Swift package against `include/media_engine.h`.

## Prerequisites

| Requirement | Tested on this repo |
|---|---|
| JDK | 17 or newer (dev box: javac 21.0.10) |
| CMake | ≥ 3.24 (same as engine root) |
| Engine build | `-DME_BUILD_JNI=ON` at CMake configure |

The CMake build calls `find_package(JNI REQUIRED)`. On macOS the
system JDK headers are usually picked up via `/usr/libexec/java_home`;
on Linux set `JAVA_HOME` if `find_package` can't locate them.

## Build

From repo root:

```sh
cmake -B build -S . -DME_BUILD_JNI=ON
cmake --build build --target media_engine_jni
# → build/bindings/jni/libmedia_engine_jni.dylib (or .so / .dll)
```

The shared lib is **not** self-contained — it links the static
`libmedia_engine.a` + FFmpeg / Skia / OCIO / libass dynamic libs
that the engine pulls in. Hosts must keep those reachable from
`java.library.path` (or bundle them into a JAR resources tree
that the host extracts at startup).

## Smoke run

```sh
javac -d build/bindings/jni/classes \
      bindings/jni/src/io/mediaengine/MediaEngine.java
java -Djava.library.path=build/bindings/jni \
     -cp build/bindings/jni/classes \
     io.mediaengine.MediaEngine
```

Output: engine version + a round-tripped engine handle. With
`<timeline.json> <output.mp4>` args, runs a passthrough render.

The same flow runs as a `ctest` target (`jni_load_smoke`) when
`-DME_BUILD_TESTS=ON` is set and a Java toolchain is on `PATH`.

## API surface

`MediaEngine.java` mirrors the C surface hosts need for a
smoke-level integration:

| Java | C |
|---|---|
| `MediaEngine()` | `me_engine_create()` |
| `MediaEngine.close()` | `me_engine_destroy()` |
| `loadTimeline(json)` | `me_timeline_load_json()` |
| `Timeline.close()` | `me_timeline_destroy()` |
| `renderStart(tl, spec, listener)` | `me_render_start()` |
| `RenderJob.waitFor()` | `me_render_wait()` |
| `RenderJob.cancel()` | `me_render_cancel()` |
| `RenderJob.close()` | `me_render_job_destroy()` |
| `lastError()` | `me_engine_last_error()` |
| `version()` | `me_engine_version()` |

Progress callbacks deliver `kind / ratio / message` to a Java
`ProgressListener`. Native side runs on an engine-owned worker
thread; the trampoline in `me_jni.cpp` attaches via
`AttachCurrentThread` and detaches before returning.

Not exposed yet: `me_render_frame` (frame server), `me_thumbnail_*`
(scrub-row API). See backlog `examples-jni-thumbnail-jvm-demo` for
the planned thumbnail JVM bridge.

## FFmpeg licensing at deploy time

The dev box's FFmpeg is GPL-built (`--enable-gpl --enable-libx264`).
JNI hosts shipping production builds **must** link against an LGPL
FFmpeg — see `docs/INTEGRATION.md` → "FFmpeg licensing at deploy
time". The JNI shim itself is fine; the constraint comes from the
engine's transitive dependency.

## Talevia integration

This wrapper is the source-of-truth for talevia's
`platform-impls/video-media-engine-jvm`. The integration steps:

1. Copy `MediaEngine.java` (and any future siblings) into talevia's
   JVM module under `io.mediaengine`.
2. Bundle `libmedia_engine_jni.dylib` + the LGPL FFmpeg libs into
   talevia's resources tree; load via the existing
   `System.loadLibrary` shim.
3. Replace shell-out FFmpeg passthrough callers with
   `MediaEngine.renderStart`.

See `docs/INTEGRATION.md` for the broader integration contract.
