/*
 * Minimal Kotlin/Native example that links against libmedia_engine_kn
 * — the SHARED dylib wrapper built by
 * `bindings/kotlin-native/CMakeLists.txt`.
 *
 * Build order:
 *   1. From repo root: `cmake -B build -S . && cmake --build build`
 *      — produces build/bindings/kotlin-native/libmedia_engine_kn.dylib
 *      (or .so on Linux).
 *   2. From this dir:  `gradle runDebugExecutableNative`
 *      — the gradle invocation runs cinterop, compiles Main.kt,
 *        and executes the resulting binary; stdout prints the
 *        engine version and a round-tripped engine handle.
 *
 * Paths walk up three levels (example/ → kotlin-native/ → bindings/
 * → repo root) to locate include/ and build/. Override by setting
 * `-PmediaEngineRoot=/absolute/path` on the gradle command line.
 * The K/N CMakeLists.txt's ctest also passes
 * `-PmediaEngineKnLib=$<TARGET_FILE_DIR:media_engine_kn>` so the
 * lib path resolves cleanly for out-of-tree builds.
 */
import org.jetbrains.kotlin.konan.target.HostManager

plugins {
    /* Pinned to 2.1.20: this is the first stable Kotlin/Native
     * multiplatform plugin compatible with Gradle 9 (the 2.0.x
     * line depended on DefaultArtifactPublicationSet which Gradle
     * 9 removed). Bumping the pin also forced the Main.kt cinterop
     * migration documented at the top of the source file —
     * me_engine_tVar typedef stayed but the opaque-handle
     * out-param idiom in K/N 2.1+ uses allocPointerTo<me_engine>()
     * directly, which is more uniform across other handle types. */
    kotlin("multiplatform") version "2.1.20"
}

repositories {
    mavenCentral()
}

val mediaEngineRoot: String =
    (findProperty("mediaEngineRoot") as String?)
        ?: project.rootDir.resolve("../../..").canonicalPath

val mediaEngineInclude = "$mediaEngineRoot/include"
/* SHARED dylib wrapper produced by
 * `bindings/kotlin-native/CMakeLists.txt`. The wrapper exists so
 * cinterop sees a single library with every engine transitive dep
 * (FFmpeg / OCIO / libass / Skia / SoundTouch / bgfx, whichever
 * ME_WITH_* are configured) already resolved by CMake — without
 * it the gradle build would have to enumerate every engine-
 * internal dep, drifting whenever a new ME_WITH_* lands.
 *
 * `mediaEngineKnLib` defaults to the dev-box convention
 * (build dir = repo-root/build), but the K/N CMakeLists.txt
 * passes `-PmediaEngineKnLib=$<TARGET_FILE_DIR:media_engine_kn>`
 * for the ctest run, which works for any out-of-tree build dir. */
val mediaEngineKnLib: String =
    (findProperty("mediaEngineKnLib") as String?)
        ?: "$mediaEngineRoot/build/bindings/kotlin-native"

kotlin {
    val nativeTarget = when {
        HostManager.hostIsMac && System.getProperty("os.arch") == "aarch64" -> macosArm64("native")
        HostManager.hostIsMac                                               -> macosX64("native")
        HostManager.hostIsLinux                                             -> linuxX64("native")
        else -> error("Unsupported host for this example: ${HostManager.host.name}")
    }

    nativeTarget.apply {
        compilations.getByName("main") {
            cinterops {
                val mediaEngine by creating {
                    defFile(project.file("../media_engine.def"))
                    packageName("io.mediaengine.cinterop")
                    compilerOpts("-I$mediaEngineInclude")
                    extraOpts("-libraryPath", mediaEngineKnLib)
                }
            }
        }
        binaries {
            executable {
                entryPoint = "io.mediaengine.example.main"
                linkerOpts("-L$mediaEngineKnLib", "-lmedia_engine_kn")
            }
        }
    }

    sourceSets {
        val nativeMain by getting
    }
}
