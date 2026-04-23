## 2026-04-23 — debt-test-multi-track-asset-reuse：asset-map dedup 合约上 tripwire（Milestone §M1-debt · Rubric §5.2）

**Context.** `timeline-asset-map` 决定（2026-04-23）把 `me::Asset` 提成 `me::Timeline::assets` map 里的一等 IR 节点，承诺"多 clip 引用同一 assetId 时 asset 存**一份** URI + content_hash，不重复"。这个契约直接观察于 `me_cache_stats.entry_count`——N 个 clip 引用 1 个 asset 时 entry_count 应该 = 1，不是 N。然而 `tests/` 一条 doctest 都没直接断言它。未来谁在 `Exporter::export_to` 里反手从 `clips[].asset_id` 自己重建 asset map（退回 decision 前的 shape）会**无声破坏**承诺。

**Decision.** 新 `tests/test_asset_reuse.cpp`，3 个 test case / 12 assertion：

1. **2 clip 同 asset → 1 cache entry**：`TimelineBuilder` 造 2 clip 都引用 `a1`，每个 1s（30/30）背靠背，assetSpec 带显式 `contentHash`。load 后 `me_cache_stats.entry_count == 1`。
2. **2 clip 两 asset → 2 cache entry**：对照组。两个不同 assetId + contentHash。load 后 `entry_count == 2`。两 case 对比把 dedup 合约显式成"asset 数 = entry 数"的等式，而不是"entry 数 ≤ clip 数"的单边界。
3. **load 阶段不开 decoder**：上述同-asset timeline load 后 `me_cache_stats.codec_ctx_count == 0`。未来谁要是往 loader 里塞 "pre-open decoder for seek" 的优化就会在这条断言上挂掉。

所有断言都只走 `me_timeline_load_json` + `me_cache_stats`——纯 public C API，不碰 `me::Timeline` 的 internal struct，test 不需要 `target_include_directories(... PRIVATE src/)`。

**Loader phase-1 约束意外踩坑.** 第一版实现直接抄 `TimelineBuilder` 的 docstring 例子（`add_clip({.clip_id="c2", .time_start_num=60}`)），被 loader 的 `timeRange.duration == sourceRange.duration` 要求拒绝（ME_E_PARSE, -7）——`ClipSpec` 默认 `time_dur_num=60` 但 `source_dur_num=60` 也是 60，加上我覆盖 `time_dur_num=30` 不覆盖 `source_dur_num` 就 mismatch。新版本把 `time_*` 和 `source_*` 四元组一起显式写在每个 `.add_clip(...)` 里，防止重入这个坑。

**Alternatives considered.**

1. **直接端到端跑 Exporter 到 passthrough 再断言 `engine.codecs.live_count()`**——拒：passthrough 不开 decoder (`live_count()` 永远 0)，断言不成立；reencode 会真开 decoder 但那是跟 `reencode-multi-clip` 已覆盖的路径重合，且对断言"asset 层 dedup"没增值。load + stats 就足以 pin 住 IR 层的承诺。
2. **直接 `#include "timeline/timeline_impl.hpp"` 查 `tl.assets.size()`**——拒：test 通过公开 C API 断言是更稳定的契约保证；`me::Timeline` 的 internal shape 未来可能重命名 `assets` 字段，而 `me_cache_stats` 是 ABI，不会变。
3. **合并进 `test_cache.cpp`**——拒：test_cache 覆盖 cache lifecycle（insert / invalidate / clear），`test_asset_reuse` 覆盖的是 timeline IR 层的 dedup 合约。独立 suite 让 "cache 行为 regression" 和 "IR dedup regression" 在 ctest 输出里分得开。
4. **不要第 3 个 case（codec_ctx_count == 0）**——拒：load 的零副作用是隐含契约，未来改 loader 的人最容易无声破坏，值得显式 pin。

业界共识来源：Google Test 和 doctest 社区的 "asymmetric contract test" 模式（"X without Y" vs "X with Y"）就是这种 "1 asset / 2 entries、2 assets / 2 entries" 的对照对——比单一 "≤ N" 断言强得多。

**Coverage.**

- `cmake --build build` 与 `-Werror` clean。
- `ctest --test-dir build` 10/10 suite 绿（新 `test_asset_reuse` 是第 10 个）。
- `build/tests/test_asset_reuse -s` 3 case / 12 assertion / 0 skip / 0 fail。
- 不动 src/，其他 9 个 suite 继续绿。

**License impact.** 无依赖变更。纯 doctest 新 suite。

**Registration.** 无 C API / schema / kernel / CMake target 变更。仅 `tests/CMakeLists.txt` 的 `_test_suites` 列表末尾加 `test_asset_reuse`。
