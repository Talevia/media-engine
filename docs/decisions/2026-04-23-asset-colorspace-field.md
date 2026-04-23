# 2026-04-23 — asset-colorspace-field

## Gap

`docs/TIMELINE_SCHEMA.md` §Asset documents an optional `"colorSpace":{...}`
override ({primaries, transfer, matrix, range}), but `me::Asset` carried
only `uri` + `content_hash`. The loader silently dropped the field. M2
OCIO integration needs a place on the IR to hang the four enums — without
it, the pipeline has no way to know when to trust container metadata
versus a per-asset override.

## 决策

Add typed enum-per-axis `me::ColorSpace` struct + `std::optional<ColorSpace>
color_space` field on `me::Asset`. Wire the loader to parse
`"colorSpace":{...}` when present; absence leaves `color_space ==
std::nullopt` (= "trust container"). Public C API unchanged — this is a
scaffold for M2, not a consumer yet.

1. `src/timeline/timeline_impl.hpp`:
   - new `struct ColorSpace { Primaries primaries; Transfer transfer;
     Matrix matrix; Range range; }`; each enum has `Unspecified = 0` as
     default so partial JSON (e.g. only `{"primaries":"bt709"}`) is valid
     and other axes keep "trust container".
   - `Asset::color_space` is `std::optional<ColorSpace>`. An absent
     `"colorSpace"` key → `std::nullopt`; an empty `"colorSpace":{}` →
     engaged optional with all axes `Unspecified`. The distinction
     matters for M2 when `{}` could be a signal "explicitly opt out of
     container auto-detect".

2. `src/timeline/timeline_loader.cpp`:
   - four small `to_primaries / to_transfer / to_matrix / to_range`
     string→enum helpers mapping the schema's string tables to the enum
     values. Unknown string → `LoadError{ME_E_PARSE, ...}` with the
     offending field name in the message.
   - `parse_color_space(json, where)` reads any subset of the four keys;
     keys present must parse, keys absent default to `Unspecified`.
   - Asset construction now includes `std::move(cs)` as the third field.

3. `tests/timeline_builder.hpp`: new `AssetSpec::color_space_json` —
   empty = omit, non-empty = emitted verbatim as `,"colorSpace":<body>`.
   Keeps the raw-string-fragment convention the builder already uses for
   `extra`. No JSON library dependency in the builder.

4. `tests/CMakeLists.txt`: extend the private src/ include override to
   `test_timeline_schema` (matches the existing `test_content_hash`
   carve-out), so the new tests can reach into `tl->tl.assets` to
   verify the parsed enum values.

5. `tests/test_timeline_schema.cpp`: three new cases —
   - BT.2020 / PQ / BT.2020 NC / full-range round-trip
   - missing colorSpace → `color_space` is `std::nullopt`
   - unknown primaries string → `ME_E_PARSE` + last_error mentions
     "primaries"

## 被拒的替代方案

1. **Store 4 optional enums directly on `Asset` (no nested struct)** —
   rejected: M2 OCIO will want to pass the whole color space around as
   one value (transform lookup, caching key, logging). Keeping them
   grouped matches how they're consumed.

2. **Represent axes as strings (`std::string primaries;` etc.)** —
   rejected: VISION §3.2 "typed params" spirit. Strings push validation
   to every consumer; enums validate once at load time. The scale is
   bounded (~20 total values) and unlikely to grow fast enough to
   justify a string bag.

3. **Fixed-struct + `bool is_set` bits instead of `std::optional`** —
   the backlog bullet offered both as equivalent. `std::optional` is
   idiomatic C++17 and already present in the codebase pattern
   (`me::ColorSpace` itself uses per-axis `Unspecified`, so the
   structured absence is captured at two levels: "asset didn't declare"
   vs "asset declared but axis X not pinned"). A flat struct with bits
   collapses those two signals.

4. **Add `colorSpace` accessor to the public C API now** — rejected:
   no consumer in M1. An accessor would need its own ABI decision
   (what enum IDs? stable numeric values? string API?). Defer until
   M2 OCIO is the caller; decide the C shape from actual use.

5. **Parse `colorSpace` at the Clip level too** — the schema mentions
   per-asset only at §Asset; clip-level color overrides aren't in the
   schema. Not in scope.

## 自查（§3a 设计约束）

- 类型化 effect 参数：N/A — this is per-asset metadata, not effect params.
- 浮点时间：N/A.
- 公共头：**unchanged** — `include/media_engine/*.h` untouched. The new
  enums live in `src/timeline/timeline_impl.hpp` (internal).
- C API 异常 / STL 泄露：loader still catches `LoadError` / `json::exception`
  inside `extern "C" me_timeline_load_json`. New code paths only throw
  the existing `LoadError`.
- GPL：no new dep.
- 确定性：`std::optional` / enum-class / `std::unordered_map` layout
  changes are ABI-only inside `me::Timeline` (internal). Output bytes of
  `01_passthrough` on the existing `sample.timeline.json` (which has no
  `colorSpace`) are **byte-identical** (MD5 match with cycle 21 output:
  `f47b2e54adaa1e1f0bfb93c2d60ef9c7`). `test_determinism` re-ran and
  passed.
- Stub 净增：none; `check_stubs.sh` still 3.
- OpenGL：N/A.
- Schema 兼容：`"colorSpace"` was already optional in schema v1 per
  `docs/TIMELINE_SCHEMA.md`. Existing JSONs lacking the field continue
  to load (absence → nullopt). Backward-compatible; no `schemaVersion`
  bump needed.
- ABI：public struct layout unchanged; no new public symbol.

## 验证

- `cmake --build build` + `ctest` → 6/6 green, incl. 3 new cases in
  `test_timeline_schema` (BT.2020+PQ round-trip, nullopt when absent,
  unknown enum rejection).
- Release `-Werror` (`cmake -B build-rel -DCMAKE_BUILD_TYPE=Release
  -DME_WERROR=ON -DME_BUILD_TESTS=ON`) clean; `ctest --test-dir
  build-rel` → 6/6.
- `01_passthrough` on the pre-existing `sample.timeline.json`: output
  byte-identical to cycle 21's output (same MD5).

## 影响 / 后续

- `me::Asset::color_space` now available for M2 OCIO:
  `asset.color_space.has_value()` → use the override; else → fall back
  to container metadata from demux. Per-axis `Unspecified` lets host
  JSONs partial-override without ceremony.
- When M2 lands the public accessor (future bullet), pick either an
  opaque `me_color_space_t` handle or four POD enum fields appended to
  a `me_asset_info_t`. Both fit; no decision here.
