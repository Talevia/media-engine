# Building media-engine for iOS

M13 bootstrap target. Hosts wanting to ship media-engine in
an iOS app, or via SwiftPM / CocoaPods, start here.

## Status

What landed (cycle `m13-ios-package-bootstrap`):

- `cmake/ios.toolchain.cmake` — minimal iOS toolchain
  supporting device (OS), simulator, and Mac Catalyst
  builds. Defaults: arm64 device, universal arm64+x86_64
  simulator, iOS 14.0 minimum.
- This document, with build invocation + xcframework
  packaging steps + LGPL FFmpeg notes.

What's NOT done yet (follow-up bullets):

- `m13-ios-xcframework-pipeline` — automated
  xcframework build that runs `xcodebuild
  -create-xcframework` over the device + simulator outputs
  and produces a single `media_engine.xcframework` ready for
  drop-in. Today the host runs the xcodebuild manually per
  the doc below.
- `m13-ios-swiftpm-package` — Package.swift exposing
  the xcframework as a SwiftPM target. Today the host
  vendors the xcframework + the public C-API headers
  manually.
- `m13-ios-swift-smoke-test` — tiny Swift sample that
  calls `me_engine_create` to confirm the .xcframework
  links and the C-API is reachable from Swift.

## Build invocation

### iOS device (arm64 only)

```sh
cmake -B build-ios-device -S . \
    -DCMAKE_TOOLCHAIN_FILE=cmake/ios.toolchain.cmake \
    -DIOS_PLATFORM=OS \
    -DIOS_DEPLOYMENT_TARGET=14.0 \
    -DME_BUILD_TESTS=OFF \
    -DME_WITH_INFERENCE=OFF \
    -DME_WITH_KVAZAAR=OFF
cmake --build build-ios-device --target media_engine
```

### iOS simulator (universal arm64 + x86_64)

```sh
cmake -B build-ios-sim -S . \
    -DCMAKE_TOOLCHAIN_FILE=cmake/ios.toolchain.cmake \
    -DIOS_PLATFORM=SIMULATOR \
    -DME_BUILD_TESTS=OFF
cmake --build build-ios-sim --target media_engine
```

### Mac Catalyst (run iOS app on macOS)

```sh
cmake -B build-catalyst -S . \
    -DCMAKE_TOOLCHAIN_FILE=cmake/ios.toolchain.cmake \
    -DIOS_PLATFORM=MACCATALYST \
    -DME_BUILD_TESTS=OFF
cmake --build build-catalyst --target media_engine
```

## Combining into an `.xcframework`

Once the device + simulator (and optionally Catalyst)
builds are produced, wrap them with `xcodebuild`:

```sh
xcodebuild -create-xcframework \
    -library build-ios-device/src/libmedia_engine.a \
        -headers include/media_engine \
    -library build-ios-sim/src/libmedia_engine.a \
        -headers include/media_engine \
    -output build/media_engine.xcframework
```

The resulting `media_engine.xcframework` directory drops
into Xcode's "Frameworks, Libraries, and Embedded Content"
list and gets built into the host app automatically.

## SwiftPM consumption (once the follow-up bullets land)

```swift
// In your Package.swift:
.binaryTarget(
    name: "MediaEngine",
    path: "third_party/media_engine.xcframework"
)
```

Today this requires manually updating the binaryTarget URL
on every release; once `m13-ios-swiftpm-package` lands,
the engine repo will host a Package.swift with a remote
binaryTarget URL pointing at GitHub release artifacts.

## CocoaPods consumption

Vendor the `.xcframework` in your podspec's
`vendored_frameworks`:

```ruby
spec.vendored_frameworks = 'media_engine.xcframework'
spec.public_header_files = 'include/media_engine/*.h'
```

## LGPL FFmpeg requirement — App Store ship

Per VISION §3.4 + docs/INTEGRATION.md, ship builds MUST
link a **LGPL-built** FFmpeg (NOT the homebrew
`--enable-gpl --enable-libx264` build the Mac dev-box uses).
The dev-box build is for local examples / development
ONLY — distributing it via App Store violates VISION §3.4.

For iOS, the host has two viable strategies:

1. **Dynamic-link via FFmpeg.framework.** The standard
   App Store path: build LGPL FFmpeg as `.framework`
   bundles, embed-and-sign in Xcode under "Frameworks,
   Libraries, and Embedded Content" with "Embed & Sign".
   The framework is dynamically linked, satisfying LGPL's
   re-link clause without distributing FFmpeg sources.
2. **Build LGPL FFmpeg from source via the iOS SDK.**
   Slower but auditable. Wrap as an `ExternalProject_Add`
   in the iOS toolchain (deferred — not done in this
   bootstrap cycle).

Static-linking LGPL FFmpeg is technically possible but
requires shipping unmodified FFmpeg sources alongside the
app, which is hard to justify for a closed-source App
Store app.

## Mac-host gates already in place

The engine's `if(APPLE)`-gated paths (CoreML,
VideoToolbox, Skia frameworks) work on iOS without
modification — the iOS toolchain sets `CMAKE_SYSTEM_NAME`
to `Darwin` so these paths activate as expected.

What's still manually deselected for iOS bootstrap builds:

- `ME_WITH_INFERENCE=OFF` recommended for initial
  bootstrapping; CoreML works on iOS but the engine's
  ONNX paths need ONNX runtime built for iOS (not done).
- `ME_WITH_KVAZAAR=OFF` (kvazaar isn't built for iOS by
  homebrew; would need source build).
- `ME_BUILD_TESTS=OFF` — the test suite assumes desktop
  filesystem layout + spawning subprocesses, neither of
  which fit iOS sandbox.

## Troubleshooting

- "xcrun --find clang failed": Xcode Command Line Tools
  not installed. Run `xcode-select --install`.
- "ld: symbol not found for architecture arm64":
  IOS_ARCH and CMAKE_OSX_ARCHITECTURES don't agree.
  Pass `-DIOS_ARCH=arm64` explicitly.
- "duplicate output file" when running
  `xcodebuild -create-xcframework`: the device build was
  produced for the simulator sysroot or vice versa. Verify
  `IOS_PLATFORM=OS` for device and `IOS_PLATFORM=SIMULATOR`
  for simulator.
