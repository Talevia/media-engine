# Host integration

How hosts consume media-engine's C API on each target. Canonical host is **talevia** (Kotlin Multiplatform, four targets). This doc is target-neutral but calls out talevia-specific wiring where relevant.

## Target matrix

| Host target | Runtime | Binding technology | Library form |
|---|---|---|---|
| Desktop JVM (Compose) | JVM | **JNI** | `libmedia_engine.dylib` / `.so` / `.dll` |
| Server (Ktor on JVM)  | JVM | **JNI** | same as desktop |
| Android               | ART (JVM) | **JNI via NDK** | `libmedia_engine.so` per ABI |
| iOS                   | native     | **Kotlin/Native cinterop** (+ Swift C interop) | `libmedia_engine.a` |
| Native C / C++ host   | any        | direct                          | either |

## Asset resolution

media-engine consumes **URIs**, not abstract asset IDs. The host owns the mapping.

In talevia: `MediaPathResolver` → URI happens in the JNI / cinterop adapter layer before building the timeline JSON. Engine never sees an `AssetId`.

```
talevia Timeline (AssetId-based)
        │   [adapter]
        │     AssetId → MediaPathResolver.resolve() → "file:///..."
        ▼
media-engine timeline JSON (URI-based)
```

Rationale: keeps the engine simple and testable, and avoids a round-trip callback every time the engine opens a file.

## JNI (JVM, Android)

### Build

- Package the native library as a JAR resource per platform pair (e.g., `media-engine-macos-arm64.jar`, `media-engine-linux-x86_64.jar`, `media-engine-android-arm64-v8a.so` in the APK).
- On load, extract the library for the current platform to a temp dir and `System.load(path)`.
- Android: put `.so` in `src/main/jniLibs/<abi>/` and use `System.loadLibrary("media_engine")`.

### JNI glue (C side)

Write a thin `jni/media_engine_jni.cpp` that:
- Implements `Java_io_talevia_mediaengine_NativeEngine_create`, `_destroy`, `_loadTimeline`, etc.
- Handles pointer ↔ `jlong` conversion.
- Catches any C++ exceptions and throws `MediaEngineException`.
- Marshals `me_progress_cb` to a Java callback via a global ref to a `Consumer<ProgressEvent>`.

Sketch:

```cpp
extern "C" JNIEXPORT jlong JNICALL
Java_io_talevia_mediaengine_NativeEngine_nativeCreate(JNIEnv*, jclass) {
    me_engine_t* e = nullptr;
    if (me_engine_create(nullptr, &e) != ME_OK) return 0;
    return reinterpret_cast<jlong>(e);
}

extern "C" JNIEXPORT void JNICALL
Java_io_talevia_mediaengine_NativeEngine_nativeDestroy(JNIEnv*, jclass, jlong h) {
    me_engine_destroy(reinterpret_cast<me_engine_t*>(h));
}
```

### Kotlin wrapper (JVM)

```kotlin
class NativeEngine : AutoCloseable {
    private val handle: Long = nativeCreate()
    override fun close() { nativeDestroy(handle) }
    fun loadTimeline(json: String): NativeTimeline { /* ... */ }
    // ...
    companion object {
        init { System.loadLibrary("media_engine") }
        @JvmStatic private external fun nativeCreate(): Long
        @JvmStatic private external fun nativeDestroy(handle: Long)
    }
}
```

### Progress callbacks → Kotlin Flow

Wrap the C callback in a `callbackFlow`:

```kotlin
fun render(tl: NativeTimeline, spec: OutputSpec): Flow<RenderProgress> = callbackFlow {
    val cb = ProgressCallback { ev ->
        trySend(ev.toRenderProgress()).isSuccess
        if (ev.isTerminal) close()
    }
    val job = nativeRenderStart(handle, tl.handle, spec, cb)
    awaitClose { nativeRenderCancel(job) ; nativeRenderJobDestroy(job) }
}
```

ProgressCallback is a Java SAM that the JNI glue invokes.

## Kotlin/Native cinterop (iOS)

Much cleaner than JNI — Kotlin/Native has direct C interop.

**Canonical in-repo example: `bindings/kotlin-native/`.** `media_engine.def` + `example/` is a runnable Gradle project that links against the CMake build output and exercises `me_version` / `me_engine_create` / `me_engine_destroy`. Host projects can copy the `.def` verbatim and adapt `example/build.gradle.kts`'s cinterop block; path conventions are documented at the top of that file.

### `.def` file

```
# nativeBindings/media_engine.def
headers = media_engine.h
headerFilter = media_engine.h media_engine/*
package = io.mediaengine.cinterop
linkerOpts.osx = -lmedia_engine
linkerOpts.ios_arm64 = -lmedia_engine
linkerOpts.ios_simulator_arm64 = -lmedia_engine
```

Include / library search paths come from the host Gradle build via `cinterops { compilerOpts("-I…") ; extraOpts("-libraryPath", "…") }`, not the `.def` itself — keeps the binding portable across monorepo layouts.

### Gradle wiring (KMP)

```kotlin
kotlin {
    val iosArm64 = iosArm64()
    val iosSimArm64 = iosSimulatorArm64()
    listOf(iosArm64, iosSimArm64).forEach { t ->
        t.compilations["main"].cinterops {
            val mediaEngine by creating {
                defFile(project.file("src/nativeInterop/cinterop/media_engine.def"))
                packageName("io.talevia.mediaengine.native")
            }
        }
    }
}
```

Kotlin code then uses `io.talevia.mediaengine.native.me_engine_create`, `io.talevia.mediaengine.native.me_render_start` directly, wrapped in idiomatic Kotlin classes.

### Swift (from the iOS app side)

Usually not needed — Kotlin wrappers are called from Swift via SKIE or generated Objective-C headers. If you want to call the C API directly from Swift:

```
// media_engine.modulemap
module MediaEngine {
    umbrella header "media_engine.h"
    export *
}
```

Swift:
```swift
import MediaEngine
var eng: OpaquePointer?
me_engine_create(nil, &eng)
```

## FFmpeg licensing at deploy time

**Critical**: the engine links FFmpeg dynamically (LGPL requires either dynamic linking or source-level replaceability). Ship builds MUST use an FFmpeg build **without** `--enable-gpl` and **without** `libx264` / `libx265`.

The system FFmpeg from Homebrew (`brew install ffmpeg`) is GPL-enabled and **not suitable for production**. Use it for development only.

Production build recipe (summary):
```
./configure \
  --disable-gpl --disable-nonfree \
  --enable-shared \
  --disable-libx264 --disable-libx265 \
  --enable-videotoolbox   # mac / iOS
  # --enable-mediacodec   # Android
  # --enable-nvenc        # Windows / Linux NVIDIA
  # --enable-amf          # Windows AMD
```

Attribution: LGPL requires bundling FFmpeg's LICENSE/COPYING and source availability (or offer). Put the compliance bundle in every shipped artifact — see `LICENSE_COMPLIANCE.md` (TODO).

## Android-specific

- NDK r26+ (C++20 needs it).
- Build one `.so` per ABI: `arm64-v8a`, `armeabi-v7a` (legacy), `x86_64` (emulator). Skip 32-bit x86.
- Use `android_app.cmake` / `external_native_build` blocks in `build.gradle.kts`.
- HW encoding via `--enable-mediacodec` in FFmpeg.

## iOS-specific

- Xcode 15+.
- Build for `iphoneos`, `iphonesimulator`, `macosx`. XCFramework bundles all three.
- HW encoding via `--enable-videotoolbox`.
- File I/O: iOS sandbox — host resolves to sandbox paths before passing URIs.

## talevia integration path

Phased, not all at once:

1. **Phase A** (when media-engine Phase 1 ships): JNI binding + new module `platform-impls/video-media-engine-jvm`. Runs in Desktop and Server, replacing `video-ffmpeg-jvm`'s shell-out. Implements the `VideoEngine` contract by translating talevia Timeline → media-engine JSON and forwarding.
2. **Phase B** (when media-engine Phase 4 ships): Android `.so` packaging + wire into `Media3VideoEngine` as the filter/transition path; Media3 continues to own the HW decode/encode fast path.
3. **Phase C**: iOS cinterop + replace / supplement `AVFoundationVideoEngine`.

Each phase is independent; talevia's `VideoEngine` contract absorbs the changes.
