#!/usr/bin/env bash
# check_stubs.sh — inventory of unimplemented C API surface.
#
# Convention: a "stub" is any code path that exists for ABI/API completeness
# but doesn't carry a real implementation yet. Every stub carries an
# explicit comment marker of the form:
#
#     /* STUB: <slug> — <short note> */        (C-style)
#     // STUB: <slug> — <short note>           (C++-style)
#
# The slug is kebab-case and MAY match a backlog `*-impl` bullet so future
# iterate-gap cycles can cross-reference.
#
# Why explicit markers and not a blanket `grep return ME_E_UNSUPPORTED`:
# runtime-reject returns (invalid spec, unsupported codec combo, missing
# encoder at runtime) are *not* stubs — they're legitimate error paths. The
# marker makes "this code hasn't been written" structurally distinct from
# "this input isn't accepted".
#
# Complements tools/scan-debt.sh (which counts raw ME_E_UNSUPPORTED returns
# for the debt-regression curve). This tool gives the function-level view
# the iterate-gap backlog needs to track which impls remain.
#
# Output: markdown table, always sorted by file+line so diffs between two
# runs highlight added / removed stubs cleanly.
#
# Exit: 0 always — reporting tool, not a gate.

set -uo pipefail

cd "${CLAUDE_PROJECT_DIR:-.}" || exit 0
if [ ! -d src ]; then
  echo "check_stubs.sh: src/ missing — run from a media-engine checkout" >&2
  exit 0
fi

# grep prints lines like: path:line:    /* STUB: slug — note */
# Pull out path, line, slug, note via awk. Slug = first whitespace-delimited
# token after STUB:; note = rest of line with trailing `*/` stripped.
rows=$(grep -rEn 'STUB:[[:space:]]+[A-Za-z0-9_-]+' src 2>/dev/null \
  | awk -F: '{
      path = $1;
      line = $2;
      body = "";
      for (i = 3; i <= NF; ++i) body = body (i > 3 ? ":" : "") $i;
      # trim leading whitespace
      sub(/^[[:space:]]+/, "", body);
      # isolate text after "STUB:"
      if (match(body, /STUB:[[:space:]]+/)) {
        body = substr(body, RSTART + RLENGTH);
      }
      # slug is the first word
      slug = body;
      if (match(slug, /[[:space:]]/)) slug = substr(slug, 1, RSTART - 1);
      # note: rest, minus trailing comment close
      rest = body;
      sub(/^[^[:space:]]+[[:space:]]*/, "", rest);
      sub(/—[[:space:]]*/, "", rest);
      sub(/-[[:space:]]+/, "", rest);      # ASCII dash variant
      sub(/[[:space:]]*\*\/[[:space:]]*$/, "", rest);
      sub(/[[:space:]]+$/, "", rest);
      printf "%s\t%s\t%s\t%s\n", path, line, slug, rest;
    }' \
  | sort -t $'\t' -k1,1 -k2,2n)

count=$(printf '%s\n' "$rows" | grep -c . || true)

printf '# Stub inventory — %s\n\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
if [ "${GIT_COMMIT:-}" ] || command -v git >/dev/null 2>&1; then
  printf '_commit: %s_\n\n' "$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
fi
printf 'Total stubs: %s\n\n' "$count"

printf '| File | Line | Slug | Note |\n'
printf '|------|------|------|------|\n'
if [ -n "$rows" ]; then
  printf '%s\n' "$rows" | awk -F'\t' '{
      printf "| %s | %s | %s | %s |\n", $1, $2, $3, $4;
    }'
else
  printf '| _no STUB markers found_ | | | |\n'
fi

exit 0
