#!/usr/bin/env bash
# Stop hook: incremental build + 01_passthrough smoke when the session
# touched buildable inputs (src/ include/ examples/ cmake/ CMakeLists.txt).
#
# Triggers when ANY of:
#   1. working tree has uncommitted/staged changes under those paths
#   2. HEAD has commits beyond the trunk (origin/main or main) that touch them
#   3. commits within the last hour touch them (catches just-merged case)
#
# Exit codes:
#   0  → no relevant change OR build+smoke passed
#   2  → build or smoke failed; stderr is fed back to the model so the next
#        turn sees what broke before stopping again
set -uo pipefail

cd "${CLAUDE_PROJECT_DIR:-.}" || exit 0

# Skip entirely if the user hasn't run `cmake -B build -S .` yet.
if [ ! -f build/CMakeCache.txt ]; then
  exit 0
fi

paths=(src include examples cmake CMakeLists.txt)

tree_changed=$(git status --porcelain -- "${paths[@]}" 2>/dev/null | head -1)

trunk=""
for candidate in origin/main main; do
  if git rev-parse --verify --quiet "$candidate" >/dev/null 2>&1; then
    trunk="$candidate"
    break
  fi
done
commits_changed=""
if [ -n "$trunk" ]; then
  commits_changed=$(git diff --name-only "$trunk"...HEAD -- "${paths[@]}" 2>/dev/null | head -1)
fi

recent_changed=""
if [ -z "$tree_changed" ] && [ -z "$commits_changed" ]; then
  recent_changed=$(git log --since='1 hour ago' --pretty=format:%H -- "${paths[@]}" 2>/dev/null | head -1)
fi

if [ -z "$tree_changed" ] && [ -z "$commits_changed" ] && [ -z "$recent_changed" ]; then
  exit 0
fi

build_log=$(cmake --build build --target 01_passthrough 2>&1)
status=$?
if [ "$status" -ne 0 ]; then
  {
    echo "cmake --build build --target 01_passthrough FAILED — fix before stopping."
    echo ""
    echo "Tail of build log:"
    echo "$build_log" | tail -30
  } >&2
  exit 2
fi

bin=build/examples/01_passthrough/01_passthrough
timeline=examples/01_passthrough/sample.timeline.json
out=/tmp/me-stop-hook.mp4

if [ ! -x "$bin" ]; then
  echo "$bin not built (target missing after build) — fix before stopping." >&2
  exit 2
fi
if [ ! -f "$timeline" ]; then
  echo "$timeline missing — smoke input not available." >&2
  exit 2
fi

rm -f "$out"
smoke_log=$("$bin" "$timeline" "$out" 2>&1)
status=$?
if [ "$status" -ne 0 ] || [ ! -s "$out" ]; then
  {
    echo "01_passthrough smoke FAILED — fix before stopping."
    echo ""
    echo "Output:"
    echo "$smoke_log" | tail -30
  } >&2
  exit 2
fi

# Optional ffprobe sanity check — skip silently if not available.
if command -v ffprobe >/dev/null 2>&1; then
  if ! ffprobe -v error -show_format "$out" >/dev/null 2>&1; then
    echo "ffprobe rejected $out — output is not a valid container." >&2
    exit 2
  fi
fi

exit 0
