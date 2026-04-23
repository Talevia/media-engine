#!/usr/bin/env bash
# scan-debt.sh — mechanized snapshot of the 8 debt signals described in
# `.claude/skills/iterate-gap/SKILL.md` §R.5.
#
# Run at the start of a `docs(backlog)` repopulate cycle; paste output into
# the repopulate commit body. Numbers in successive repopulates form the
# debt-regression curve that `git log` naturally preserves.
#
# Output: one markdown section per signal, always printed in the same order
# so `diff`ing two repopulate commits is meaningful.
#
# Exit codes:
#   0  → scan completed (debt may or may not exist; this tool only reports)
#   1  → ran outside a media-engine checkout (missing docs/ or src/)
set -uo pipefail

cd "${CLAUDE_PROJECT_DIR:-.}" || exit 1
if [ ! -d docs ] || [ ! -d src ]; then
  echo "scan-debt.sh: not in media-engine checkout (docs/ or src/ missing)" >&2
  exit 1
fi

printf '# Debt scan — %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
printf '_commit: %s_\n\n' "$(git rev-parse --short HEAD 2>/dev/null || echo 'unknown')"

# 1. Long files (.cpp / .hpp / .h in src, include)
printf '## 1. Long files (> 400 lines)\n\n'
long=$(find src include -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) 2>/dev/null \
  | xargs wc -l 2>/dev/null \
  | awk '$1 > 400 && $2 != "total" { printf "- %s (%d lines)\n", $2, $1 }' \
  | sort -t'(' -k2 -rn)
if [ -n "$long" ]; then
  echo "$long"
else
  echo '- _none_'
fi
echo

# 2. C API stubs — authoritative count via `STUB:` markers
#
# History: this section used to report `grep -c 'return ME_E_UNSUPPORTED'`
# as if it were a stub count. But most ME_E_UNSUPPORTED returns are
# runtime-reject paths ("no video stream", "unknown codec spec",
# "encoder h264_videotoolbox not available") — legitimate error handling,
# not unimplemented code. tools/check_stubs.sh already formalised the
# distinction: a stub carries an explicit `/* STUB: <slug> — ... */`
# comment; everything else is a reject. The authoritative count lives on
# the marker.
#
# Output shape: the marked count is the number to track across cycles
# (goal: monotonically decreases per milestone). Raw ME_E_UNSUPPORTED is
# still printed but labelled explicitly so future readers don't mistake
# it for the stub metric.
printf '## 2. C API stubs (via `STUB:` markers)\n\n'
marked_count=$(grep -rEn 'STUB:[[:space:]]+[A-Za-z0-9_-]+' src 2>/dev/null | wc -l | tr -d ' ')
printf 'Marked stubs (authoritative): %s\n\n' "$marked_count"
grep -rEn 'STUB:[[:space:]]+[A-Za-z0-9_-]+' src 2>/dev/null \
  | awk -F: '{
      path = $1; line = $2;
      body = "";
      for (i = 3; i <= NF; ++i) body = body (i > 3 ? ":" : "") $i;
      if (match(body, /STUB:[[:space:]]+/)) body = substr(body, RSTART + RLENGTH);
      slug = body;
      if (match(slug, /[[:space:]]/)) slug = substr(slug, 1, RSTART - 1);
      printf "- %s:%s (STUB: %s)\n", path, line, slug;
    }' \
  | sort

raw_count=$(grep -rn 'return ME_E_UNSUPPORTED' src include 2>/dev/null | wc -l | tr -d ' ')
runtime_reject_count=$((raw_count - marked_count))
printf '\nRaw `return ME_E_UNSUPPORTED` returns: %s\n' "$raw_count"
printf 'Runtime-reject returns (raw − marked stubs; informational): %s\n\n' "$runtime_reject_count"

if [ "$runtime_reject_count" -lt 0 ]; then
  printf '_inconsistency: STUB markers outnumber raw rejects — investigate_\n\n'
fi

printf 'Raw ME_E_UNSUPPORTED by file:\n'
grep -rn 'return ME_E_UNSUPPORTED' src include 2>/dev/null \
  | awk -F: '{ print $1 }' | sort | uniq -c | sort -rn \
  | awk '{ printf "- %s: %d\n", $2, $1 }'
echo

# 3. TODO / FIXME / HACK / XXX
printf '## 3. TODO / FIXME / HACK / XXX\n\n'
todo_count=$(grep -rnE 'TODO|FIXME|HACK|XXX' src include cmake 2>/dev/null | wc -l | tr -d ' ')
printf 'Total: %s\n\n' "$todo_count"

# 4. Skipped / disabled tests
printf '## 4. Skipped / disabled tests\n\n'
if [ -d tests ]; then
  skips=$(grep -rnE 'SKIP|DISABLED|GTEST_SKIP|DOCTEST_SKIP' tests 2>/dev/null)
  if [ -n "$skips" ]; then
    echo "$skips" | awk '{ print "- " $0 }'
  else
    echo '- _none_'
  fi
else
  echo '- _no tests/ dir yet_'
fi
echo

# 5. Dependency fetches vs architecture whitelist
printf '## 5. FetchContent / find_package vs ARCHITECTURE.md whitelist\n\n'
# FetchContent_Declare is commonly written across two lines (paren, then name).
# `find_package_handle_standard_args` is filtered via the `(` anchor — the
# handle variant has `_handle_` not `(` immediately after `find_package`.
cmake_files=$(find . -type f \( -name 'CMakeLists.txt' -o -name '*.cmake' \) \
  -not -path './build*' -not -path './.git/*' 2>/dev/null | sort)

fp=$(printf '%s\n' $cmake_files \
  | xargs grep -hE 'find_package[[:space:]]*\([[:space:]]*[A-Za-z0-9_]+' 2>/dev/null \
  | sed -E 's/.*find_package[[:space:]]*\([[:space:]]*([A-Za-z0-9_]+).*/- find_package: \1/' \
  | sort -u)

# Same-line form: FetchContent_Declare(name ...)
fc_inline=$(printf '%s\n' $cmake_files \
  | xargs grep -hE 'FetchContent_Declare[[:space:]]*\([[:space:]]*[A-Za-z0-9_]+' 2>/dev/null \
  | sed -E 's/.*FetchContent_Declare[[:space:]]*\([[:space:]]*([A-Za-z0-9_]+).*/- FetchContent_Declare: \1/' \
  | sort -u)

# Multi-line form: FetchContent_Declare( on one line, identifier on the next
fc_multi=$(printf '%s\n' $cmake_files \
  | xargs grep -hA1 'FetchContent_Declare[[:space:]]*([[:space:]]*$' 2>/dev/null \
  | awk 'NR%3==2 { gsub(/^[[:space:]]+|[[:space:]]+$/, ""); if ($0 ~ /^[A-Za-z0-9_]+$/) print "- FetchContent_Declare: " $0 }' \
  | sort -u)

all=$(printf '%s\n%s\n%s\n' "$fp" "$fc_inline" "$fc_multi" | grep . | sort -u)
total=$(printf '%s' "$all" | grep -c '^- ' 2>/dev/null || echo 0)
printf 'Declared in CMake: %s\n\n' "$total"
if [ -n "$all" ]; then
  echo "$all"
else
  echo '- _none_'
fi
echo

# 6. Deprecated markers
printf '## 6. Deprecated markers\n\n'
dep=$(grep -rn -E '\[\[deprecated|DEPRECATED' include src 2>/dev/null)
if [ -n "$dep" ]; then
  echo "$dep" | awk '{ print "- " $0 }'
else
  echo '- _none_'
fi
echo

# 7. Compile flag redeclarations
printf '## 7. Repeated add_compile_options / target_compile_options\n\n'
grep -rnE 'add_compile_options|target_compile_options' CMakeLists.txt src cmake 2>/dev/null \
  | awk '{ print "- " $0 }'
echo

# 8. Public header dependency leakage
printf '## 8. Public header includes (`include/`)\n\n'
printf 'Only `<stddef.h>`, `<stdint.h>`, `<stdbool.h>`, `<media_engine/*.h>`, and sibling `"*.h"` are allowed.\n\n'
suspect=$(grep -rn '#include' include 2>/dev/null \
  | grep -vE '#include[[:space:]]*<(stddef|stdint|stdbool)\.h>' \
  | grep -vE '#include[[:space:]]*[<"]media_engine/[^>"]+[>"]' \
  | grep -vE '#include[[:space:]]*"[A-Za-z0-9_]+\.h"')
if [ -n "$suspect" ]; then
  echo "$suspect" | awk '{ print "- " $0 }'
else
  echo '- _clean_'
fi
echo

exit 0
