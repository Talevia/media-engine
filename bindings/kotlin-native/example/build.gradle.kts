/*
 * Minimal Kotlin/Native example that links against libmedia_engine
 * built from this repo.
 *
 * Build order:
 *   1. From repo root: `cmake -B build -S . && cmake --build build`
 *      — produces build/libmedia_engine.{a,dylib,so}.
 *   2. From this dir:  `./gradlew runDebugExecutableNative`
 *      — the gradle invocation runs cinterop, compiles Main.kt,
 *        and executes the resulting binary; stdout prints the
 *        engine version and a round-tripped engine handle.
 *
 * Paths walk up three levels (example/ → kotlin-native/ → bindings/
 * → repo root) to locate include/ and build/. Override by setting
 * `-PmediaEngineRoot=/absolute/path` on the gradle command line.
 */
import org.jetbrains.kotlin.konan.target.HostManager

plugins {
    kotlin("multiplatform") version "2.0.20"
}

repositories {
    mavenCentral()
}

val mediaEngineRoot: String =
    (findProperty("mediaEngineRoot") as String?)
        ?: project.rootDir.resolve("../../..").canonicalPath

val mediaEngineInclude = "$mediaEngineRoot/include"
val mediaEngineLib     = "$mediaEngineRoot/build"

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
                    extraOpts("-libraryPath", mediaEngineLib)
                }
            }
        }
        binaries {
            executable {
                entryPoint = "io.mediaengine.example.main"
                linkerOpts("-L$mediaEngineLib", "-lmedia_engine")
            }
        }
    }

    sourceSets {
        val nativeMain by getting
    }
}
