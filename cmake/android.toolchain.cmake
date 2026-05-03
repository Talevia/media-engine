# cmake/android.toolchain.cmake — thin wrapper over the NDK's
# own android.toolchain.cmake.
#
# Purpose: give callers a single, project-versioned toolchain
# entry point that documents the engine's required NDK + ABI
# defaults without forcing them to remember every flag. The NDK
# already ships a comprehensive toolchain at
# `${ANDROID_NDK}/build/cmake/android.toolchain.cmake`; this
# file is a documented thin wrapper that includes it and sets
# sane defaults for engine-relevant variables.
#
# Usage:
#   cmake -B build-android -S . \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/android.toolchain.cmake \
#     -DANDROID_NDK=$ANDROID_HOME/ndk/26.1.10909125    \
#     # ABI / API overrides are optional; defaults below apply.
#
# The wrapper:
#   1. Validates `ANDROID_NDK` is set and points at a real NDK.
#   2. Sets `ANDROID_ABI=arm64-v8a` if not already set (the modern
#      Android default; covers ~all post-2019 devices).
#   3. Sets `ANDROID_PLATFORM=android-26` if not already set
#      (Android 8.0 — the engine's minimum supported API for
#      modern AV codec features without per-call shims).
#   4. Includes the NDK's own toolchain so all the
#      cross-compile machinery happens there, not here.
#
# To override the defaults, pass `-DANDROID_ABI=...` and/or
# `-DANDROID_PLATFORM=android-N` on the cmake invocation BEFORE
# the toolchain file is loaded — the wrapper uses
# `if(NOT DEFINED ...)` so caller values win.
#
# License compliance reminder. Per docs/INTEGRATION.md "FFmpeg
# licensing at deploy time", the Android build MUST link an
# LGPL-built FFmpeg (NOT the homebrew --enable-gpl bottle that
# the Mac dev box uses). The host is responsible for staging
# LGPL FFmpeg .so files in their NDK sysroot or via gradle's
# native-libs path.

if(NOT DEFINED ANDROID_NDK OR NOT EXISTS "${ANDROID_NDK}")
    message(FATAL_ERROR
        "cmake/android.toolchain.cmake: ANDROID_NDK must be set to a "
        "valid NDK path (got: '${ANDROID_NDK}'). Example: "
        "-DANDROID_NDK=$ANDROID_HOME/ndk/26.1.10909125. The NDK ships "
        "its own android.toolchain.cmake which this wrapper includes.")
endif()

if(NOT DEFINED ANDROID_ABI)
    set(ANDROID_ABI "arm64-v8a" CACHE STRING
        "Android ABI (default arm64-v8a; modern devices 2019+)")
endif()

if(NOT DEFINED ANDROID_PLATFORM)
    set(ANDROID_PLATFORM "android-26" CACHE STRING
        "Android platform/API level (default android-26 = Android 8.0)")
endif()

# Delegate to the NDK's toolchain — it does the actual heavy lifting
# (sysroot, compiler, sysroot include paths, ABI-specific flags, etc).
include("${ANDROID_NDK}/build/cmake/android.toolchain.cmake")

# Engine-side gates. CoreML is APPLE-only (already gated in
# src/CMakeLists.txt); VideoToolbox same. The kvazaar path
# checks `pkg_check_modules` so it gracefully skips on Android
# unless the host provides a libkvazaar built for the target ABI.
# Nothing extra needed here.
