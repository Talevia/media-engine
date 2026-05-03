# cmake/ios.toolchain.cmake — minimal iOS / iOS-Simulator
# toolchain for media-engine.
#
# Purpose: give callers a single, project-versioned toolchain
# entry point for cross-compiling the engine for iOS device,
# iOS simulator, and macOS Catalyst targets, then bundling the
# results as an `.xcframework`.
#
# Why we write our own (vs the Android pattern of delegating
# to the NDK's toolchain). Apple does NOT ship a CMake
# toolchain file for iOS — projects pin to community
# toolchains (e.g. leetal/ios-cmake) or roll their own. Pulling
# in a community toolchain adds a dependency the engine
# doesn't otherwise need; this file is small enough (~80
# lines) to vendor directly.
#
# Usage:
#   # Device (arm64 only):
#   cmake -B build-ios-device -S . \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/ios.toolchain.cmake \
#     -DIOS_PLATFORM=OS
#
#   # Simulator (arm64 + x86_64 universal):
#   cmake -B build-ios-sim -S . \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/ios.toolchain.cmake \
#     -DIOS_PLATFORM=SIMULATOR
#
# Then `xcodebuild -create-xcframework -library
# build-ios-device/src/libmedia_engine.a -library
# build-ios-sim/src/libmedia_engine.a -output
# media_engine.xcframework`.
#
# Configurable variables (cache-set, host overridable via
# `-D...=...` BEFORE -DCMAKE_TOOLCHAIN_FILE=...):
#   IOS_PLATFORM   — OS (device) | SIMULATOR | MACCATALYST
#                    (default: OS)
#   IOS_DEPLOYMENT_TARGET — minimum iOS version
#                          (default: 14.0)
#   IOS_ARCH       — explicit arch override; auto-derived
#                    from IOS_PLATFORM if unset.
#
# License compliance reminder. Per docs/INTEGRATION.md "FFmpeg
# licensing at deploy time", ship builds MUST link a LGPL-
# built FFmpeg. The Mac dev-box homebrew FFmpeg is GPL-built
# (--enable-gpl --enable-libx264) and CANNOT be shipped in an
# App Store binary. iOS bundles MUST dynamic-link FFmpeg
# (LGPL re-link compatibility) — static link with LGPL
# requires distributing the unmodified FFmpeg sources or a
# re-link mechanism, which is hard for a closed-source app.
# Dynamic linking via .framework (Embed & Sign in Xcode) is
# the standard path.

# --- iOS platform selection -----------------------------------
if(NOT DEFINED IOS_PLATFORM)
    set(IOS_PLATFORM "OS" CACHE STRING
        "iOS target platform: OS (device) | SIMULATOR | MACCATALYST")
endif()

if(NOT DEFINED IOS_DEPLOYMENT_TARGET)
    set(IOS_DEPLOYMENT_TARGET "14.0" CACHE STRING
        "Minimum iOS deployment target")
endif()

# Validate IOS_PLATFORM and derive sysroot + arch.
if(IOS_PLATFORM STREQUAL "OS")
    set(_ios_sysroot "iphoneos")
    set(_ios_default_arch "arm64")
elseif(IOS_PLATFORM STREQUAL "SIMULATOR")
    set(_ios_sysroot "iphonesimulator")
    # Universal sim arch list — modern Macs use arm64
    # simulators, older Macs use x86_64.
    set(_ios_default_arch "arm64;x86_64")
elseif(IOS_PLATFORM STREQUAL "MACCATALYST")
    set(_ios_sysroot "macosx")
    set(_ios_default_arch "arm64;x86_64")
else()
    message(FATAL_ERROR
        "cmake/ios.toolchain.cmake: IOS_PLATFORM='${IOS_PLATFORM}' not "
        "supported. Expected OS / SIMULATOR / MACCATALYST.")
endif()

if(NOT DEFINED IOS_ARCH)
    set(IOS_ARCH "${_ios_default_arch}")
endif()

# --- CMake target system + compiler ---------------------------
set(CMAKE_SYSTEM_NAME       Darwin)
set(CMAKE_SYSTEM_PROCESSOR  arm64)
set(CMAKE_OSX_SYSROOT       "${_ios_sysroot}")
set(CMAKE_OSX_ARCHITECTURES "${IOS_ARCH}")
set(CMAKE_OSX_DEPLOYMENT_TARGET "${IOS_DEPLOYMENT_TARGET}")

if(IOS_PLATFORM STREQUAL "MACCATALYST")
    # Catalyst uses iOS-style frameworks linked against macOS
    # SDK. Set the iOSMac variant flag.
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.1")  # Catalyst minimum
    add_compile_options("-target" "${IOS_ARCH}-apple-ios14.0-macabi")
    add_link_options(   "-target" "${IOS_ARCH}-apple-ios14.0-macabi")
endif()

# Find the Xcode-bundled clang via xcrun.
execute_process(
    COMMAND xcrun --sdk "${_ios_sysroot}" --find clang
    OUTPUT_VARIABLE _ios_cc
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _ios_cc_rc)
if(NOT _ios_cc_rc EQUAL 0)
    message(FATAL_ERROR
        "cmake/ios.toolchain.cmake: xcrun --sdk ${_ios_sysroot} --find "
        "clang failed (rc=${_ios_cc_rc}). Is Xcode + Command Line Tools "
        "installed?")
endif()
set(CMAKE_C_COMPILER   "${_ios_cc}")
set(CMAKE_CXX_COMPILER "${_ios_cc}")

# Static archives only (not bundles) for engine link;
# .xcframework wraps these.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
