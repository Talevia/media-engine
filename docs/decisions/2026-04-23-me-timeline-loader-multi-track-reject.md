## 2026-04-23 — me-timeline-loader-multi-track-reject：补 multi-track 拒绝的测试覆盖（Milestone §M1-debt · Rubric §5.2）

**Context.** Backlog bullet（这轮 repopulate 写的）断言 "timeline loader 解析 JSON 时接受任意长度的 tracks 数组，但 Exporter 只遍历第一条——第二条被**静默丢掉**，方向是 loader 侧显式 reject 并给明确 err"。

拉开 `src/timeline/timeline_loader.cpp` 一看，**已经有了**：第 171 行就是

```cpp
const auto& tracks = comp->at("tracks");
require(tracks.size() == 1, ME_E_UNSUPPORTED,
        "phase-1: exactly one track supported");
```

Bullet 的代码侧 direction 是对不存在的缺失动作的描述——这条路径在 `multi-clip-single-track` cycle 就顺手加进去了。

**但** 翻 `tests/test_timeline_schema.cpp`，发现这条 rejection **没有任何 doctest 覆盖**——test 里最近的邻居是 `multi-clip single-track`（positive case），没有"multi-track → rejected"的 negative case。那才是真正残留的 gap。

**Decision.** 不改 loader（code 已经对了）。只补 `tests/test_timeline_schema.cpp` 的 negative case：`phase-1 rejects multi-track timeline as ME_E_UNSUPPORTED`。JSON 内联在 test case 里（TimelineBuilder 单轨设计，为一条 negative case 扩 builder 多轨 API 是 over-engineering），断言：

- `load(2-track JSON) == ME_E_UNSUPPORTED`
- `tl == nullptr`
- `me_engine_last_error(eng)` 包含子串 `"exactly one track"`

这把 "多轨 JSON 会 fail-fast" 这条已存在的行为契约 pin 在 CI 里；未来 `multi-track-video-compose` cycle 解除这条 rejection 时必须**同时**更新这个 test（或删它），强制开发者意识到 "原 contract 在变"。

**为什么 bullet premise 和代码不一致.** Repopulate 那次我写 bullet 时没 grep 确认 loader 有没有 require。这是 repopulate 的一个常见失误模式："猜测当前状态 → 生成 bullet"。下轮 repopulate 的改进点：对每个疑似"功能缺失"的 bullet，先跑一遍 `grep -rn '<relevant_symbol>' src` 验证状态，写进 bullet 的 "Gap" 部分引用具体行号而不是"我以为是这样"的转述。本 cycle 不改 repopulate 流程（不在 scope）；decision 记一笔。

**Alternatives considered.**

1. **跳过 bullet，leave it in backlog for user review**——拒：bullet 对应的**真实 gap 存在**（coverage 缺失），只是描述的 direction 不对。删 bullet + 记在 decision 里解释 premise-vs-reality，比留着误导未来读者更好。
2. **改 loader err msg 让它更 verbose（加"see `multi-track-video-compose` backlog"）**——拒：loader 错误消息已经说 "phase-1: exactly one track supported"，已经够信息量；把 backlog slug 硬编码到 runtime error 是反模式（bullet slug 变了 err 就过时）。
3. **扩 TimelineBuilder 支持多轨再用 builder 写 test**——拒：TimelineBuilder 设计就是 single-track 的，为一个 negative case 扩 multi-track API 得同时扩 `AssetSpec` → `TrackSpec` → `ClipSpec` 的三层结构，性价比极低。Raw JSON 就 20 行 R-string，等 `multi-track-video-compose` 真上线时用实际需要的 shape 扩 builder。
4. **把 test 塞进 test_asset_reuse.cpp**——拒：`test_asset_reuse` 是 asset-map dedup 合约测试，"多轨被拒"是 schema 层的 phase-1 enforcement，属于 test_timeline_schema.cpp 的 concern。

**Coverage.**

- `cmake --build build` 与 `-Werror` clean。
- `ctest --test-dir build` 10/10 suite 绿（test_timeline_schema 从 N 个 case 升到 N+1）。
- 新 test case 按 substring 断言 err 消息 "exactly one track"——未来改 wording 只能换相同语义 token，不能静默删。
- 不动 src/，其他 9 个 suite 继续绿。

**License impact.** 无依赖变更。

**Registration.** 无 C API / schema / kernel 变更。仅 `tests/test_timeline_schema.cpp` 新 1 个 test case。
