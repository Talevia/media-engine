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

import cnames.structs.me_engine
import io.mediaengine.cinterop.ME_OK
import io.mediaengine.cinterop.me_engine_create
import io.mediaengine.cinterop.me_engine_destroy
import io.mediaengine.cinterop.me_version
import kotlinx.cinterop.ExperimentalForeignApi
import kotlinx.cinterop.allocPointerTo
import kotlinx.cinterop.memScoped
import kotlinx.cinterop.ptr
import kotlinx.cinterop.toKString
import kotlinx.cinterop.useContents
import kotlinx.cinterop.value

/* K/N 2.1+ cinterop note: opaque-handle out-params (here
 * `me_engine_t**`) no longer surface as a `me_engine_tVar`
 * typedef on the Kotlin side. The replacement is
 * `allocPointerTo<me_engine>()`, which yields a
 * `CPointerVar<me_engine>` — `slot.ptr` is the address-of, and
 * `slot.value` reads back the engine handle as a
 * `CPointer<me_engine>?`. The C side still sees `me_engine_t**`
 * unchanged; this is purely a binding-side migration. */
@OptIn(ExperimentalForeignApi::class)
fun main() {
    me_version().useContents {
        val sha = git_sha?.toKString().orEmpty().ifEmpty { "<unknown>" }
        println("media-engine $major.$minor.$patch ($sha)")
    }

    memScoped {
        val slot = allocPointerTo<me_engine>()
        val rc = me_engine_create(null, slot.ptr)
        check(rc == ME_OK) { "me_engine_create failed: rc=$rc" }
        val handle = slot.value
        check(handle != null) { "me_engine_create returned ME_OK but wrote NULL" }
        println("engine created: $handle")
        me_engine_destroy(handle)
        println("engine destroyed")
    }
}
