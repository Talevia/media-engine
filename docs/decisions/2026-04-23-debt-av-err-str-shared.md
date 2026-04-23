## 2026-04-23 — debt-av-err-str-shared (Milestone §M1-debt · Rubric §5.2)

**Context.** Seven TUs each carried an identical 4-line wrapper around
`av_strerror`, six under the name `av_err_str` and one (in probe.cpp)
under `av_err_to_string`. The function is trivial, but the duplication
is the kind of drift vector that PAIN_POINTS warned about: every new
libav-consumer TU picks up another copy, and if the signature ever
needs to change (e.g. to carry structured error context), all seven
sites drift out of sync.

**Decision.**
- New header-only `src/io/av_err.hpp` — declares `me::io::av_err_str(int
  rc) -> std::string` inline. One 4-line function body, one canonical
  name. Header includes just `<libavutil/error.h>` + `<string>`.
- Seven TUs migrated:
  - `src/io/mux_context.cpp` — drops file-local `av_err_str`, includes
    the new header directly.
  - `src/orchestrator/muxer_state.cpp` — drops local def, adds
    `using me::io::av_err_str;` (keeps unqualified call sites).
  - `src/orchestrator/reencode_pipeline.cpp` — same.
  - `src/orchestrator/reencode_video.cpp` — same.
  - `src/orchestrator/reencode_audio.cpp` — same.
  - `src/api/thumbnail.cpp` — same.
  - `src/api/probe.cpp` — drops local `av_err_to_string`, switches all
    2 call sites to the shared `av_err_str` name.
- Each TU's `extern "C"` libav-header block drops
  `#include <libavutil/error.h>` where it was only pulled for
  `av_strerror` — the shared header covers it transitively.
- `src/CMakeLists.txt` unchanged (header-only).

**Alternatives considered.**
- **Out-of-line definition in `src/io/av_err.cpp`**: would keep the
  header minimal and dodge any inline-function ODR concerns. But the
  function body is 4 lines and the inline version is trivially safe
  (no static state, libav linkage is via avutil already linked from
  every caller). Kept inline to avoid adding a new TU to build.
- **Keep both names** (alias `av_err_to_string` → `av_err_str` via
  using-declaration): backward compat inside the repo. Rejected —
  probe.cpp's lone usage is easy to migrate now, and carrying an
  alias forever is exactly the kind of ABI surface debt that
  `debt-*` cycles are supposed to prevent.
- **Richer error type** (`me::io::AvError { code, msg }`): tempting,
  but no current consumer needs the split — every caller does
  `"prefix: " + av_err_str(rc)` and passes through to `std::string`.
  Structured errors are a reasonable M3+ topic; today we're
  consolidating, not expanding.
- **Put in `ffmpeg_raii.hpp` alongside the deleters**: coupling not
  warranted — `av_err_str` doesn't need deleter machinery or vice
  versa. Separate header keeps each one's purpose single-line-
  describable.

**Coverage.**
- `cmake --build build -DME_BUILD_TESTS=ON` + `cmake --build
  build-rel -DCMAKE_BUILD_TYPE=Release -DME_WERROR=ON
  -DME_BUILD_TESTS=ON` — both clean.
- `ctest` Debug + Release: 6/6 suites pass.
- Example regressions: `01_passthrough`, `05_reencode`,
  `06_thumbnail`, `04_probe` (both success and missing-file error
  path) — all produce expected output. `04_probe /tmp/nope.mp4`
  still emits `"avformat_open_input: No such file or directory"`,
  confirming the probe.cpp rename is semantic-preserving.

**License impact.** No dependency changes. `<libavutil/error.h>` was
already pulled in by every TU that had a file-local copy.

**Registration.** Changes this cycle:
- `TaskKindId` / kernel registry — untouched.
- Resource factory / orchestrator factory — untouched.
- Exported C API — untouched.
- CMake — no new sources (header-only consolidation).
- JSON schema — untouched.
- New internal header: `src/io/av_err.hpp`.
