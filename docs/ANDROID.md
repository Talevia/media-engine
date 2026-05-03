# Building media-engine for Android

M13 bootstrap target. This document is the entry point for
hosts wanting to ship media-engine as an Android library
(.aar / .so).

## Status

What landed (cycle `m13-android-package-bootstrap`):

- `cmake/android.toolchain.cmake` — thin wrapper over the
  NDK's standard toolchain file with sane defaults
  (arm64-v8a + android-26).
- `bindings/jni/build.gradle.kts.example` — minimal Android
  Gradle Module template that drives the engine's CMake
  build through the NDK toolchain.
- This document, with the build invocation + LGPL FFmpeg
  staging notes.

What's NOT done yet (follow-up bullets):

- `m13-android-emulator-smoke` — instrumented test that
  confirms the .so loads in an Android emulator and
  exercises one C-API call (`me_engine_create` /
  `me_version`). Needs an emulator-present CI runner; left
  for a host with a real Android lab to wire up.
- Vendoring or auto-fetching LGPL FFmpeg `.so` builds for
  Android. Today the host stages them externally per the
  gradle template's `jniLibs.srcDirs` comment.

## Build invocation (CMake-only, no gradle)

```sh
cmake -B build-android -S . \
    -DCMAKE_TOOLCHAIN_FILE=cmake/android.toolchain.cmake \
    -DANDROID_NDK=$ANDROID_HOME/ndk/26.1.10909125 \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-26 \
    -DME_BUILD_TESTS=OFF \
    -DME_WITH_INFERENCE=OFF \
    -DME_WITH_KVAZAAR=OFF
cmake --build build-android --target media_engine
```

### ABI overrides

Default is `arm64-v8a` (covers all modern Android devices
since 2019). Pass `-DANDROID_ABI=x86_64` for the emulator,
or omit and let the gradle plugin sweep all
`abiFilters`-listed ABIs.

### API level

Default is `android-26` (Android 8.0 / API 26). The engine
uses no APIs that need a higher level today; lower levels
(e.g. android-21) may work but aren't tested.

## Build invocation (gradle, recommended for app packaging)

1. Copy `bindings/jni/build.gradle.kts.example` to
   `bindings/jni/build.gradle.kts` (or to your host app's
   module dir).
2. Adjust `namespace` + `compileSdk` to your host
   conventions.
3. Stage LGPL FFmpeg `.so` files (see below).
4. Add the module to your settings.gradle.kts and run:
   ```sh
   ./gradlew :bindings-jni:assembleRelease
   ```

## LGPL FFmpeg staging — required for ship

VISION §3.4 + docs/INTEGRATION.md "FFmpeg licensing at deploy
time" require a **LGPL-built** FFmpeg in any distributable
artifact. The Mac dev-box's homebrew FFmpeg is built with
`--enable-gpl --enable-libx264` and **MUST NOT** be shipped
in an Android APK — doing so re-licenses the engine to GPL,
which violates VISION §3.4.

For Android, the host has three choices:

1. **Pre-built LGPL FFmpeg .so per ABI.** Stage them under
   `bindings/jni/src/main/jniLibs/<ABI>/lib*.so`. The gradle
   plugin packages them into the APK / AAR automatically.
   Source candidates: <https://github.com/javiersantos/ffmpeg-android>
   or build from source per
   <https://trac.ffmpeg.org/wiki/CompilationGuide/Android>.
2. **Build LGPL FFmpeg from source via the NDK.** Slower
   but auditable. Wrap as an `ExternalProject_Add` in
   `cmake/android.toolchain.cmake` (deferred — not done in
   this bootstrap cycle).
3. **Static-link via FFmpeg's --enable-static.** APK size
   grows; LGPL re-linking compatibility requirements still
   apply (host must distribute the unmodified FFmpeg
   sources or a re-link mechanism).

## Mac-host gates already in place

The engine's macOS-specific paths (CoreML, VideoToolbox,
Skia/SDK frameworks) are gated on `if(APPLE)` in
`src/CMakeLists.txt`. Cross-compiling for Android skips
them automatically — no Android-side ifdefs needed in the
engine's C++ source.

What's still manually deselected for Android:

- `ME_WITH_INFERENCE=OFF` until an Android-targeted ONNX
  runtime build is wired in. CoreML is APPLE-only.
- `ME_WITH_KVAZAAR=OFF` unless the host stages a
  libkvazaar.a built for the target ABI.

## Troubleshooting

- "ANDROID_NDK must be set to a valid NDK path": pass
  `-DANDROID_NDK=$ANDROID_HOME/ndk/<version>` on the
  cmake invocation.
- "CMAKE_C_COMPILER not set": ensure
  `-DCMAKE_TOOLCHAIN_FILE=cmake/android.toolchain.cmake`
  comes before `-S .` on the cmake line.
- "ld: cannot find -lavformat": LGPL FFmpeg `.so` files
  aren't in the linker path. Either set
  `-DCMAKE_LIBRARY_PATH=/path/to/ffmpeg/<ABI>/lib` or
  configure pkg-config to find them.
