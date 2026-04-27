#!/usr/bin/env bash
# check_lib_size.sh — VISION §5.7-5 guard.
#
# Reads the size of a built library file and asserts it is ≤ the
# per-config ceiling recorded in tools/lib_size_budget.txt.
#
# Usage:
#   check_lib_size.sh <lib-path> <config> <budget-file>
#
# <lib-path>     absolute path to the built archive/dylib (cmake
#                generator expression $<TARGET_FILE:media_engine>)
# <config>       multi-config CMake config name (Debug/Release/etc.)
#                or empty for single-config generators
# <budget-file>  path to lib_size_budget.txt
#
# Exit code:
#   0  size within budget (PASS) OR config not listed (LENIENT skip
#      with stderr warning — favors not breaking unconfigured builds
#      over hiding a regression; see comment in budget file)
#   1  size exceeds the recorded budget for this config (FAIL)
#   2  argument / file errors

set -euo pipefail

if [[ $# -lt 3 ]]; then
  echo "usage: $0 <lib-path> <config> <budget-file>" >&2
  exit 2
fi

lib_path="$1"
config="$2"
budget_file="$3"

if [[ ! -f "$lib_path" ]]; then
  echo "check_lib_size: library not found: $lib_path" >&2
  exit 2
fi
if [[ ! -f "$budget_file" ]]; then
  echo "check_lib_size: budget file not found: $budget_file" >&2
  exit 2
fi

# Empty config (single-config generator like Make/Ninja w/o
# CMAKE_BUILD_TYPE) defaults to Debug for budget purposes — this is
# what `cmake -B build -S .` without -DCMAKE_BUILD_TYPE produces on
# most platforms.
if [[ -z "$config" ]]; then
  config="Debug"
fi

# Cross-platform stat: BSD (macOS) uses -f "%z", GNU uses -c "%s".
# Try BSD first; fall back to GNU.
if size_bytes=$(stat -f "%z" "$lib_path" 2>/dev/null); then
  :
elif size_bytes=$(stat -c "%s" "$lib_path" 2>/dev/null); then
  :
else
  echo "check_lib_size: could not stat $lib_path" >&2
  exit 2
fi

# Look up per-config budget. Lines like `Debug=4194304`.
budget=$(awk -F= -v cfg="$config" '
  $1 == cfg { print $2; exit }
' "$budget_file")

if [[ -z "${budget:-}" ]]; then
  echo "check_lib_size: no budget entry for config=$config in $budget_file — " \
       "skipping (LENIENT). Add a row to enforce this config." >&2
  exit 0
fi

printf 'check_lib_size: %s config=%s size=%d B budget=%d B\n' \
  "$lib_path" "$config" "$size_bytes" "$budget"

if (( size_bytes > budget )); then
  echo "check_lib_size: SIZE REGRESSION — $size_bytes B > $budget B " \
       "(config=$config). Either fix the bloat or bump $budget_file " \
       "with a commit body explaining the cause." >&2
  exit 1
fi

echo "check_lib_size: PASS"
exit 0
