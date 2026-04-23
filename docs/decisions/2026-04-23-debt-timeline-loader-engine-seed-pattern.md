## 2026-04-23 — debt-timeline-loader-engine-seed-pattern：loader 侧不变，engine seed 侧抽到 `core/engine_seed.{hpp,cpp}`（Milestone §M2-debt · Rubric §5.2）

**Context.** Bullet 的原文指出：`me_timeline_load_json`（`src/api/timeline.cpp`）在 loader 返回 `me::Timeline` IR 之后原地跑一个 `for (auto& [id, asset] : tl.assets) engine->asset_hashes->seed(...)` 循环。loader 自己是 engine-agnostic 的（`me::timeline::load_json` 只吃 JSON，不知道 engine 的存在）；结果 extern "C" entry 在做两件事——薄 glue + 业务副作用。未来 M2 color pipeline preheat、M3 effect LUT preheat 等 seed 消费者一个个加进去时，这个 extern C block 会继续膨胀。

Bullet 给了两条 direction，但明确写 "等 M2 第二种 seed 资源出现再定":
- (a) `me::timeline::load_json` 签名吃 `Engine*`，loader 自带 seed（牺牲 engine-agnostic）
- (b) Timeline 暴露 `apply_to_engine(Engine&)` hook，保持 loader 纯净（引入新概念）

**前两 cycle 连续跳过这 bullet**（"trigger 未到"为由），但看了一下实际情况：第二个 consumer 可能要到 M2 / M3 才出现（数周到数月），一直让这个 bullet 停在 P1 top 等于每 cycle 都付 "skip-然后-解释-为什么-skip" 的 overhead。换种思路——**不做 (a) 或 (b) 的大决策**，只做一个**low-commitment 的中间形态**，把风险降到可逆。

**Decision.** 新 `src/core/engine_seed.{hpp,cpp}`，声明 + 实现单一 free function：

```cpp
namespace me::detail {
    void seed_engine_from_timeline(me_engine& engine, const me::Timeline& tl);
}
```

行为：今天就是**原先那段 asset_hashes->seed 循环的逐字搬运**，接口形状一致（`(Engine&, const Timeline&)`）。`me_timeline_load_json` 里原 30 行 seed 循环缩成一行：

```cpp
if (*out) me::detail::seed_engine_from_timeline(*engine, (*out)->tl);
```

**为什么这不是 premature 决定 (a) 或 (b).**

- **loader 签名不变**——`me::timeline::load_json(std::string_view, me_timeline_t**, std::string*)` 还是 engine-agnostic 的；option (a) 的"loader 吃 Engine*" 路线依然可逆（未来可以把 `seed_engine_from_timeline` 调用 inline 回 loader 里，改签名 widen 就行）。
- **没引入 Timeline 上的新 method**——option (b) 的"Timeline::apply_to_engine(Engine&)" 涉及把 engine-facing behavior 挂在 IR 类上，语义重大。本决定只用了一个**TU-local 的 free function**（属于 detail 命名空间、不进公共 API），不把 behavior 挂在 Timeline 上。
- **真正的延迟决策还是在** —— 等第二个 seed 消费者上线时，三种形状都还可选：
  1. 继续堆在 `seed_engine_from_timeline` 里（当前路径的平庸扩展）
  2. 升级成 option (b)（Timeline::apply_to_engine, free function 作为 adapter）
  3. 升级成 option (a)（loader 吃 Engine*, free function 删除）

**获得的好处**：
- 业务副作用从 extern C glue block 里摘出来，named、searchable、有独立 TU。新 seed 消费者加时改**一处**（`engine_seed.cpp`），不用碰 `api/timeline.cpp`。
- `api/timeline.cpp` 的 `me_timeline_load_json` 实际 glue 代码从 ~30 行缩到 ~15 行，读起来的"extern C 在做什么"更干净。
- Header 注释显式挂了"M2 color pipeline preheat、M3 effect LUT preheat"作为 placeholder——下次写那些 feature 的人能**立刻**找到正确扩展点。

**成本**：
- 多一个 TU（`core/engine_seed.cpp`，19 行；`.hpp` 30 行，大半注释）。
- `src/CMakeLists.txt` 的 media_engine source 列表多一行。

**实际上是 option (b) 的简化形态.** 诚实地讲：`seed_engine_from_timeline(engine, tl)` 从结构上 **是** option (b) 的 "Timeline 暴露 hook" 降级版——把 member function 换成 free function，Timeline 上不加 method。这一层差别保留了两个可逆性：
- Timeline struct 保持纯数据不知 engine（option (a) 回归时 Timeline 不用改）；
- free function 作为 adapter 扁平（option (a) 回归时只需删除调用点 + 把 body 挪进 loader）。

真想延迟决定到 M2 的话，也可以不做这一步。但考虑到不做的成本是"bullet 每 cycle 占 P1 top 被 skip + 业务副作用继续混在 extern C glue 里"，做这一步的成本（30 分钟写代码 + 无 API surface change）相比之下划算。

**Alternatives considered.**

1. **继续 skip**——拒（已 skip 两次，成本在累积）。
2. **做完整的 (a)**（loader 吃 Engine*）——拒：`me::timeline::load_json` 的 engine-agnostic 属性是有用的 invariant（比如未来 offline schema validator 可以复用 loader 不拉入 Engine）。破坏这个需要更硬的证据。
3. **做完整的 (b)**（`Timeline::apply_to_engine(Engine&)`）——拒：给 `me::Timeline` 加 member method 把 IR 类和 engine 类耦合。Free function 是等价功能，耦合更少。
4. **把 seed logic 塞进 `engine_impl.hpp` 的 inline function**——拒：inline header 意味着每个 include engine_impl.hpp 的 TU 都要 include `timeline_impl.hpp` / `asset_hash_cache.hpp`，依赖传染。独立 TU 清晰。
5. **bullet 直接删（"premature abstraction"评价）**——拒：未来 M2 新消费者出现时 extern C glue 里再塞循环是反模式；现在花 30 分钟把 extension point 挂好比到时重构便宜。

**Coverage.**

- `cmake --build build` 与 `-Werror` clean（新 `core/engine_seed.cpp` 编译通过）。
- `ctest --test-dir build` 12/12 suite 绿。
- `test_asset_reuse`（asset-map dedup 合约）仍然断言 `me_cache_stats.entry_count == 1` 正确——因为 `seed_engine_from_timeline` 是 逐字搬运 原循环，behavior 不变。
- 不 touch `me::timeline::load_json` 签名，不 touch `me::Timeline` 公共形状；retention invariants 完整。

**License impact.** 无。

**Registration.** 无 C API / schema / kernel 变更。
- 新 `src/core/engine_seed.hpp` 内部 header（`me::detail::` 命名空间，不暴露给公共 API）。
- 新 `src/core/engine_seed.cpp` 一个 TU。
- `src/CMakeLists.txt` 的 `media_engine` source 列表加 `core/engine_seed.cpp`。
- `src/api/timeline.cpp` 里 `me_timeline_load_json` 的 ~10 行 seed 循环替换成一行调用。
