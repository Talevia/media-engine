/*
 * Minimal Kotlin/Native example that links against libmedia_engine
 * built from this repo.
 *
 * Build order:
 *   1. From repo root: `cmake -B build -S . && cmake --build build`
 *      — produces build/src/libmedia_engine.a.
 *   2. From this dir:  `gradle compileKotlinNative` (smoke test:
 *      cinterop generation + Main.kt compile) or
 *      `gradle runDebugExecutableNative` (full executable build —
 *      currently blocked on the visibility issue tracked by
 *      `debt-bindings-kn-link-visibility` in the BACKLOG).
 *
 * Paths walk up three levels (example/ → kotlin-native/ → bindings/
 * → repo root) to locate include/ and build/. Override by setting
 * `-PmediaEngineRoot=/absolute/path` on the gradle command line.
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
/* CMake puts the static archive at build/src/libmedia_engine.a
 * (from src/CMakeLists.txt:258 add_library STATIC). The cinterop
 * tool's `-libraryPath` is for header / symbol resolution, not
 * the final executable link — the gradle ctest target runs only
 * `compileKotlinNative` (cinterop generation + Main.kt compile),
 * which needs the headers but not a fully resolved link line.
 *
 * The full executable link
 * (`linkDebugExecutableNative` → `runDebugExecutableNative`)
 * is currently blocked: media_engine sets
 * `CXX_VISIBILITY_PRESET=hidden`, so when wrapped into any shared
 * library the `me_*` C API symbols stay hidden. Resolving that
 * cleanly needs an `ME_API` annotation pass on every public
 * header — distinct ABI-scope decision deferred to a follow-up
 * bullet (`debt-bindings-kn-link-visibility`). Until then,
 * `runDebugExecutableNative` will fail at the link step on this
 * dev box; `compileKotlinNative` is what the ctest exercises. */
val mediaEngineLib     = "$mediaEngineRoot/build/src"

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
