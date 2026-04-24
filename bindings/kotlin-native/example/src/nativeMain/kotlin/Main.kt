/*
 * Smoke test — prove the cinterop binding resolves symbols and the
 * handle round-trip works. Not a full wrapper; real hosts (e.g.
 * talevia platform-impls) layer idiomatic Kotlin on top.
 *
 * Covers the three M7 exit-criterion surfaces:
 *   - me_version()          (value-typed struct return)
 *   - me_engine_create()    (opaque handle out-param)
 *   - me_engine_destroy()   (ownership transfer back to the engine)
 */
package io.mediaengine.example

import io.mediaengine.cinterop.ME_OK
import io.mediaengine.cinterop.me_engine_create
import io.mediaengine.cinterop.me_engine_destroy
import io.mediaengine.cinterop.me_engine_tVar
import io.mediaengine.cinterop.me_version
import kotlinx.cinterop.ExperimentalForeignApi
import kotlinx.cinterop.alloc
import kotlinx.cinterop.memScoped
import kotlinx.cinterop.ptr
import kotlinx.cinterop.toKString
import kotlinx.cinterop.useContents

@OptIn(ExperimentalForeignApi::class)
fun main() {
    me_version().useContents {
        val sha = git_sha?.toKString().orEmpty().ifEmpty { "<unknown>" }
        println("media-engine $major.$minor.$patch ($sha)")
    }

    memScoped {
        val slot = alloc<me_engine_tVar>()
        val rc = me_engine_create(null, slot.ptr)
        check(rc == ME_OK) { "me_engine_create failed: rc=$rc" }
        val handle = slot.value
        check(handle != null) { "me_engine_create returned ME_OK but wrote NULL" }
        println("engine created: $handle")
        me_engine_destroy(handle)
        println("engine destroyed")
    }
}
