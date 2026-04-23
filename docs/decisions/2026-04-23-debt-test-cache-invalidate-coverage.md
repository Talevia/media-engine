## 2026-04-23 — debt-test-cache-invalidate-coverage：null-arg + single-asset round-trip 补齐（Milestone §M1-debt · Rubric §5.2）

**Context.** Bullet 的描述："`tests/test_cache.cpp` 断言 `me_cache_stats` 随 asset 插入递增，但没断言 `me_cache_invalidate_asset` 的反向语义（invalidate → stats 回退）。invalidate 现在是 impl 完了的（`a4a1c1c`），应该覆盖"。

读 `tests/test_cache.cpp` 发现：**这条断言已经存在** — TEST_CASE `me_cache_invalidate_asset removes entries matching a content hash`（line 116 起）已经覆盖：
- Load 2 个 timeline，每个带独立 contentHash
- `entry_count == 2`
- `invalidate_asset(sha256:abc)` → `entry_count == 1`
- `invalidate_asset(bare_hex)` → `entry_count == 0`

**第三次** 碰到 bullet premise 和代码 state 不一致（前两次：`debt-render-bitexact-flags`、`me-timeline-loader-multi-track-reject`）。Repopulate 时 bullet 作者没 `grep -rn 'me_cache_invalidate_asset' tests/` 验证当前状态。同样的 repopulate 失误。

**Decision.** 不重复已覆盖的主路径断言，只补两个**真正的 coverage 缺口**：

1. **`me_cache_invalidate_asset` null-arg 拒绝**：`null engine / null content_hash → ME_E_INVALID_ARG`。加上"invalidate-miss（哈希没对应 entry）不算 error"的显式断言——`me_cache_invalidate_asset(eng, bogus_hash) == ME_OK` 且 `entry_count` 不变。这条契约未写入 cache.h 注释，也无 test 断言，未来可能被重构成"miss → `ME_E_NOT_FOUND`" 之类的 breaking change；tripwire 在此。
2. **单 asset round-trip**：单 timeline（单 asset）load → stats.entry_count==1 → invalidate → stats.entry_count==0。bullet 字面上要求的 "seed → stats 递增 → invalidate → stats 回退" round-trip，主路径测用 2 asset 验证**差分** 语义（invalidate 只掉一个，留一个）；新 case 验证**绝对**语义（单 asset 完整 round-trip）。对 host UI "freshness"（删 asset 后 stats 立刻归零）是主要用户可见语义，独立断言更稳。

`test_cache.cpp` 从 5 个 TEST_CASE / ~22 assertion 升到 7 case / 41 assertion。

**Alternatives considered.**

1. **直接 SKIP 整个 bullet（已覆盖）**——拒：bullet 里的"只覆盖主路径 2-asset 差分不算完备"这个观察其实有道理，补 null-arg + 单 asset round-trip 是真正的价值添加。Skip 会让 bullet 留在 backlog，下次 repopulate 又要重估。
2. **删 bullet 不加任何 test**——拒（同 1）。
3. **把 `null content_hash 不崩`改成 loader 级 panic**（让 null 成为硬错误而非 `ME_E_INVALID_ARG`）——拒：这是 API policy 决定，不在 test cycle scope。Bullet 要求 coverage，本 cycle 就给 coverage。
4. **测 `me_cache_invalidate_asset` 对 FramePool 的 side effect**——FramePool 没按 content_hash 索引；invalidate 对 FramePool 是 no-op（见 `src/api/cache.cpp` 注释）。没有 behavior 可断言。等 M6 frame cache 真 keyed by hash 时再补。

**Repopulate 失误模式（第 3 次）.** 三个 cycle：
- `debt-render-bitexact-flags`（cycle N）：bullet 说 reencode 产物非 byte-deterministic，实测已经 deterministic（FFmpeg ≥ 5.x mov muxer 默认 creation_time=0）；做的是 defensive flag + tripwire test。
- `me-timeline-loader-multi-track-reject`（cycle N+4）：bullet 说 loader 静默接受 multi-track，实测 loader:171 已经 require 单 track；做的是补 reject 路径的 test 覆盖。
- **本 cycle**：bullet 说 invalidate 反向语义没覆盖，实测已覆盖；做的是 null-arg + 单 asset round-trip 补边角。

模式很一致：repopulate 时 bullet 作者（我）在不 grep 代码的前提下凭印象写 Gap description。每次都"bullet 描述的是已做的工作，真正的 gap 在临近 / 补足那个 contract 的边角"。**下一 repopulate 前要强制 grep** — 或者更好地，让 bullet 的 Gap 部分直接引用 grep 命令 + 行号。以此 decision 做一个持续观察点，下次 repopulate 时 honest 地审视 bullet 质量。

**Coverage.**

- `cmake --build build` 与 `-Werror` clean。
- `ctest --test-dir build` 12/12 suite 绿。
- `build/tests/test_cache -s` 7 case / 41 assertion / 0 skip（从 5/22 升）。
- 不动 src/，其他 11 个 suite 继续绿。

**License impact.** 无。

**Registration.** 无 C API / schema / kernel 变更。仅 `tests/test_cache.cpp` 加 2 个 TEST_CASE。
