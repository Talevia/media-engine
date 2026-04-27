#!/usr/bin/env bash
# check-engine-impl-compile-defs.sh — guard against the cycle-41 trap.
#
# `core/engine_impl.hpp` declares the engine's struct layout, gating
# some fields behind `ME_HAS_X` macros (today: ME_HAS_SOUNDTOUCH +
# ME_HAS_INFERENCE; future ones may follow). Test TUs that include
# the header for white-box scheduler access (test_audio_chunk_graph,
# test_player, etc.) compute `me_engine` field offsets from THEIR
# view of the header. If the macro is set as PRIVATE in
# `src/CMakeLists.txt`, the macro doesn't propagate to test TUs via
# `target_link_libraries(media_engine)`, so the test sees a struct
# WITHOUT the gated field while the engine itself sees one WITH. The
# divergent layouts produce reads of `eng->scheduler` that resolve to
# the wrong address — symptom: random `pthread_mutex_lock` EINVAL
# inside Taskflow's executor (cycle 41 burned 7 cycles diagnosing
# this before the visibility fix landed at commit `71574a2`).
#
# This script enforces: every `ME_HAS_X` referenced in
# `core/engine_impl.hpp` must be defined as PUBLIC or INTERFACE on
# the `media_engine` target in `src/CMakeLists.txt`. PRIVATE is
# forbidden because it doesn't propagate to consumers.
#
# Other ME_HAS_X (OCIO / GPU / LIBASS / SKIA today) are correctly
# PRIVATE because they gate INTERNAL implementation files, not
# struct fields exposed via engine_impl.hpp. The script doesn't
# touch them — only the ones engine_impl.hpp consults.
#
# Exit codes:
#   0  → all gated macros are PUBLIC/INTERFACE (clean)
#   1  → ran outside a media-engine checkout (missing src/)
#   2  → at least one gated macro is PRIVATE → human read message

set -uo pipefail

# Resolve to the repo root: the script lives at <repo>/tools/, so its
# parent's parent is the source root regardless of where ctest / a
# developer invokes it from. Falls back to `pwd` for legacy uses
# (iterate-gap's CLAUDE_PROJECT_DIR, manual `bash tools/check…` from
# the source tree).
script_dir=$(cd "$(dirname "$0")" && pwd)
repo_root=$(cd "$script_dir/.." && pwd)
cd "$repo_root" || exit 1

if [ ! -d src/core ] || [ ! -f src/CMakeLists.txt ]; then
    echo "check-engine-impl-compile-defs.sh: not in media-engine checkout (resolved root: $repo_root)" >&2
    exit 1
fi

ENGINE_IMPL=src/core/engine_impl.hpp
CMAKE_SRC=src/CMakeLists.txt

if [ ! -f "$ENGINE_IMPL" ]; then
    echo "$ENGINE_IMPL not found" >&2
    exit 1
fi

# Extract every ME_HAS_X token engine_impl.hpp consults. Look only at
# preprocessor `#ifdef` / `#ifndef` / `#if defined(...)` directives —
# arbitrary occurrences inside doc comments / strings (e.g. the
# `ME_HAS_X visibility constraint` block at the top of the header) are
# not actual gate uses and would produce false-positive failures
# (caught the first time this script ran via ctest, before this
# refinement). Sorted-unique so the audit order is deterministic.
gated_macros=$(grep -E '^[[:space:]]*#[[:space:]]*(if|ifdef|ifndef|elif)([[:space:]]|[(])' "$ENGINE_IMPL" |
               grep -oE 'ME_HAS_[A-Z_]+' |
               sort -u)
if [ -z "$gated_macros" ]; then
    # No macros gating struct fields — vacuously clean.
    echo "check-engine-impl-compile-defs: no ME_HAS_X gates in $ENGINE_IMPL — OK"
    exit 0
fi

failed=0
for macro in $gated_macros; do
    # Find the target_compile_definitions line for this macro.
    # Format we expect (one of, both correct):
    #   target_compile_definitions(media_engine PUBLIC    ME_HAS_X=1)
    #   target_compile_definitions(media_engine INTERFACE ME_HAS_X=1)
    # PRIVATE is the trap we want to catch.
    line=$(grep -nE "target_compile_definitions\\(media_engine[[:space:]]+(PRIVATE|PUBLIC|INTERFACE)[[:space:]]+${macro}=" \
        "$CMAKE_SRC" 2>/dev/null || true)

    if [ -z "$line" ]; then
        # Macro is referenced in engine_impl.hpp but never defined for
        # media_engine — that's a separate bug (probably a stale
        # `#ifdef` left over from a removed feature). Flag it.
        echo "FAIL: $macro is referenced in $ENGINE_IMPL but no" >&2
        echo "      target_compile_definitions(media_engine ... $macro=...) in $CMAKE_SRC" >&2
        failed=1
        continue
    fi

    # Pull the scope keyword out of the matched line.
    scope=$(echo "$line" |
        grep -oE 'target_compile_definitions\(media_engine[[:space:]]+(PRIVATE|PUBLIC|INTERFACE)' |
        awk '{print $NF}')

    case "$scope" in
        PUBLIC|INTERFACE)
            : # OK; will report below
            ;;
        PRIVATE)
            echo "FAIL: $macro is PRIVATE on media_engine in $CMAKE_SRC" >&2
            echo "      $line" >&2
            echo "      But $ENGINE_IMPL gates a struct field on this macro," >&2
            echo "      so test TUs that include engine_impl.hpp see a different" >&2
            echo "      me_engine layout than libmedia_engine.a, causing field-" >&2
            echo "      offset divergence (cycle 41's mutex-EINVAL trap)." >&2
            echo "      Fix: change PRIVATE → PUBLIC for this macro." >&2
            failed=1
            ;;
        *)
            echo "FAIL: $macro has unrecognized scope '$scope' in $CMAKE_SRC" >&2
            failed=1
            ;;
    esac
done

if [ $failed -ne 0 ]; then
    exit 2
fi

# Pretty success report — names the macros so reviewers can verify
# the audit covers what they expect.
echo "check-engine-impl-compile-defs: all gated macros are PUBLIC/INTERFACE — OK"
for macro in $gated_macros; do
    line=$(grep -nE "target_compile_definitions\\(media_engine[[:space:]]+(PUBLIC|INTERFACE)[[:space:]]+${macro}=" \
        "$CMAKE_SRC")
    echo "  $macro: $line"
done
exit 0
