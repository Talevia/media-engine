## 2026-04-23 — debt-update-architecture-md (Milestone §M1 · Rubric §5.2)

**Context.** `docs/ARCHITECTURE.md` documented the repo as of the
scaffolding phase: five modules tagged "scaffolded" with pending
`*-bootstrap` backlog references, `me_probe` / `me_thumbnail_png` as
stubs, `me_render_start` (re-encode) as "not yet", and a "Testing
philosophy (aspirational; no tests yet)" section. Ten iterate-gap
cycles later, all those states are stale: probe / thumbnail /
reencode-h264 / multi-clip concat / content-hash / doctest scaffold /
determinism regression / stub inventory / thread-local last-error are
all shipped. Someone reading ARCHITECTURE.md cold would get a picture
ten cycles out of date. This cycle rewrites it to match reality.

**Decision.**
- **Module layout block**: strip every `(scaffolded)` tag; add one-line
  descriptions of what actually lives in each directory today (e.g.
  `graph/` calls out Node / Graph / Builder + content-hash;
  `orchestrator/` names Exporter's two specializations and flags the
  Previewer/Thumbnailer stubs explicitly). New entries for `tests/`
  and `tools/` that didn't exist when the doc was first written.
  Examples list updated with the current six example binaries.
- **Current implementation state table**: split into two sections —
  "Modules" (by the five-module architecture from
  `ARCHITECTURE_GRAPH.md` plus `src/core/`, `src/api/`, `src/timeline/`,
  `src/io/`) and "Feature paths (cross-cutting)" (probe / thumbnail /
  render-passthrough / render-reencode / frame-server / cache /
  determinism). This resolves the old table's category mixing ("graph/"
  and "me_probe" were rows at the same level, which made "which module
  does this belong to" unreadable). Each row gets a status
  (Shipped / Partial / Stub) with scope in parentheses.
- Every **Stub** row names the slug from `tools/check_stubs.sh` so a
  reader can jump from ARCHITECTURE.md → BACKLOG.md → code by slug.
- **Testing section**: replace "aspirational; no tests yet" with a
  table of the five suites that actually exist today + what each
  covers. Keep the aspirational items (ABI golden files, C-only CI
  compile, per-platform CI) but under a clearly-labeled "still pending"
  header so the doc doesn't overstate what's done.
- **Paragraph on `check_stubs.sh`**: a one-liner pointer to the tool so
  the "Current implementation state" table doesn't need to be edited
  every time a stub flips to shipped — the tool is the authoritative
  current snapshot.

**Alternatives considered.**
- **Re-derive the table from source on every build** (generate
  markdown via a script that scans `STUB:` markers + CMakeLists
  targets): more robust than hand-maintained prose, but adds CI
  surface and the hand-written descriptions are where the value is
  (reader wants "what this module does" more than "these files
  exist"). Rejected — keep it hand-written, run `check_stubs.sh` for
  the fresh slice.
- **Split ARCHITECTURE.md into ARCHITECTURE.md + CURRENT_STATE.md**:
  separate the stable principles from the drifting state table.
  Clean in theory but doubles the indexing cost for readers, and
  stable vs. state is already implicitly delineated by section
  ordering (principles first, state last). Rejected for now; revisit
  if the state table starts drifting within a single cycle.
- **Just delete the status table entirely** and point readers at
  BACKLOG.md + `check_stubs.sh`: underinvests in the "skim" use case.
  A reader landing on ARCHITECTURE.md for the first time wants one
  place to see "what's in flight / shipped / stubbed"; forcing three
  separate reads is friction. Rejected.
- **Use green/yellow/red emoji badges** for Shipped / Partial / Stub:
  CLAUDE.md says "Only use emojis if the user explicitly requests".
  Rejected. The text label is fine.

**Coverage.**
- `cmake --build build` + `ctest --test-dir build` — 5/5 suites pass
  (no code changed, expected).
- Manual read-through: every claim in the new tables cross-checked
  against `src/` contents, `docs/BACKLOG.md` (nothing claims shipped
  that's still in BACKLOG), and `tools/check_stubs.sh` output
  (every `Stub` row corresponds to a real marker).

**License impact.** Doc-only; dependency table unchanged.

**Registration.** Documentation-only; no code / CMake / ABI / schema
changes this cycle.
