## 2026-04-23 — debt-ffmpeg-raii-shared-header (Milestone §M1-debt · Rubric §5.2)

**Context.** `src/api/thumbnail.cpp` and
`src/orchestrator/reencode_pipeline.cpp` each declared their own
namespace-local copies of `CodecCtxDel / FrameDel / PacketDel / SwsDel`
(plus `SwrDel` in reencode_pipeline), with identical bodies that call
the canonical libav free functions. PAIN_POINTS logged this after the
thumbnail-impl cycle, and the backlog bumped it to P1. With this
cycle's intent to land `debt-io-mux-context-raii` and `codec-pool-impl`
next, consolidating the deleters now prevents a third and fourth copy.

**Decision.**
- New `src/io/ffmpeg_raii.hpp` — header-only, sits under `me::io` next
  to `DemuxContext`. Exports five deleter structs + five
  `std::unique_ptr` aliases:
  - `AvCodecContextPtr` (`avcodec_free_context(&p)`)
  - `AvFramePtr` (`av_frame_free(&p)`)
  - `AvPacketPtr` (`av_packet_free(&p)`)
  - `SwsContextPtr` (`sws_freeContext(p)` — by-value, distinct shape)
  - `SwrContextPtr` (`swr_free(&p)`)
  Each deleter is `noexcept` and null-tolerant (libav functions are
  documented null-tolerant, but the guards make the code read-only
  safe against a future libav tightening).
- `src/api/thumbnail.cpp` and
  `src/orchestrator/reencode_pipeline.cpp` now `#include
  "io/ffmpeg_raii.hpp"` and keep short file-local aliases
  (`using CodecCtxPtr = me::io::AvCodecContextPtr;`) so the dense
  decode/encode bodies don't need to be rewritten. The alias layer
  costs zero — they're pure `using` statements.
- Explicit non-goal: `AVFormatContext` output-side RAII (the other
  duplication noted in PAIN_POINTS). That's tracked by
  `debt-io-mux-context-raii` and needs the `avio_open`/`avio_closep`
  dance captured, which is a wrapper class, not a deleter. Keeping
  this cycle scoped to the trivial unique_ptr deleters keeps the
  commit reviewable.

**Alternatives considered.**
- **Do nothing yet** — two copies is tolerable; wait until a third
  driver lands. Rejected because two upcoming backlog items
  (`debt-io-mux-context-raii` + `codec-pool-impl`) will each want
  these same deleters; consolidating now beats consolidating twice.
- **Put the deleters next to each libav subsystem** (e.g. codec
  deleter in `src/resource/codec_pool.hpp`): scatters ownership
  vocabulary across the tree. The unified header lets any TU that
  touches libav resources `#include` one thing.
- **Use shared_ptr with `void(*)(void*)` deleters** instead of typed
  structs: slower (type-erased), bigger (control block), and loses
  the compile-time type safety. Rejected; unique_ptr + struct is the
  canonical C++ idiom.
- **Rename the file-local aliases to the full
  `me::io::AvCodecContextPtr`** everywhere: uglier in dense loops
  (`AvCodecContextPtr enc(avcodec_alloc_context3(codec))` is already
  long). Keeping short local aliases.

**Coverage.**
- `cmake --build build` + `cmake --build build-rel
  -DCMAKE_BUILD_TYPE=Release -DME_WERROR=ON -DME_BUILD_TESTS=ON` —
  both clean.
- `ctest` Debug + Release: 6/6 suites pass.
- `01_passthrough` → valid 2s MP4 (same as prior cycles).
- `05_reencode` → valid h264+aac MP4.
- `06_thumbnail /tmp/input.mp4 1/1 320 180` → 7178-byte PNG
  (byte-for-byte identical to prior cycles' output — confirms the
  refactor is semantic-preserving: same deleter call graph, just
  centralized).

**License impact.** No new dependencies. Header includes only libav
headers already used by the two consuming TUs.

**Registration.** Changes this cycle:
- `TaskKindId` / kernel registry — untouched.
- Resource factory — untouched.
- Orchestrator factory — untouched; `reencode_pipeline.cpp` body
  unchanged apart from the deleter declarations moving to the shared
  header.
- Exported C API — no new or removed symbols.
- CMake — no new TUs (header-only consolidation; `src/CMakeLists.txt`
  sources list unchanged).
- JSON schema — untouched.
- New internal header: `src/io/ffmpeg_raii.hpp`.
