## 2026-04-23 — debt-test-content-hash-seed-collisions：`AssetHashCache::seed` 冲突语义 tripwire（Milestone §M1-debt · Rubric §5.2）

**Context.** `AssetHashCache::seed(uri, hash)`（`src/resource/asset_hash_cache.cpp:46`）实现是 `map_[std::string{uri}] = std::string{hex_hash}`——`operator[]` 是无条件 assign，**last-wins**。对比 `get_or_compute`（同文件 line 38 附近）走 `try_emplace`——**first-wins**。两条 insert 路径的 policy **故意不对称**：

- `get_or_compute` 的 try_emplace 路径只在 race 下插入，first-wins 避免同 URI 重复计算。
- 显式 `seed` 的 operator[] 路径表达"caller 有权威信息"，last-wins 让更新生效。

这条不对称 policy 对 user 真实可观察：host 加载新 timeline（同 URI 不同 `contentHash`）时，`me_timeline_load_json` → `seed_engine_from_timeline` → `AssetHashCache::seed(uri, new_hash)`——如果 policy 不是 last-wins，host 看到的还是 stale hash，下游 cache lookup 全错。

这条 policy 至今**没被 CI pin 住**：`grep -n 'seed' tests/test_content_hash.cpp` 只找到 `seed skips recomputation` 和 `seed ignores empty hashes` 两条 positive case，没测"同 URI 二次 seed 用 which hash"。一次 refactor 把 `operator[]` 换成 `try_emplace` 就静默翻转语义。

**Decision.** `tests/test_content_hash.cpp` 加 2 个 TEST_CASE：

1. **Same-URI different-hash is last-wins**：`seed(uri, "aaa...")` → `peek==aaa` → `seed(uri, "bbb...")` → `peek==bbb && size==1`。加 `seed(uri, "")` → `peek==bbb`（empty-hash noop 语义再验一次，上 case 已覆盖但本 case 补组合性）。
2. **Different URIs with same hash are independent entries**：`seed(uri_a, hash)` + `seed(uri_b, hash)` → `size==2` + `peek(uri_a)==peek(uri_b)==hash`。这条的目的是 pin "cache 以 URI 为 key、以 hash 为 **value**" 不反过来——防止未来某个 "dedupe by hash" 优化破坏 `invalidate_by_hash` 的反向映射（URI 是 primary，hash 是 data）。

两 case 共 7 个 assertion。`test_content_hash` 从 8 case / 20 assertion 升到 10 case / 26 assertion。

**Decision 额外作用.** Policy 现在有 3 处证据：
- impl 在 `asset_hash_cache.cpp:46`（`operator[]` 用 last-wins）。
- test 在 `test_content_hash.cpp`（本 cycle 新 case 直接断言）。
- decision 在 `docs/decisions/` 这份文件（解释**为什么**选 last-wins 而不是 first-wins）。

未来要改 policy 的 PR 必须同时改三处——静默绕是 test 告警。

**Alternatives considered.**

1. **顺手改 policy 到 first-wins 更"安全"**——拒：`me_timeline_load_json` 的 host 场景用例是"更新 content hash"，first-wins 会让 stale hash 粘住 → user-observable 错误。last-wins 是正确选择。
2. **seed 签名改 `void seed(uri, hash, bool overwrite=true)` 让 caller 挑**——拒：over-engineering。单一 policy 简单正确。未来真需要区分时再加 `try_seed()` 或类似。
3. **只加第一 case（same-URI collision），不加第二 case（different-URI shared-hash）**——拒：两 case 覆盖不同 invariant，尺寸小所以一起收。第二 case 是对"dedupe by hash 优化"的预防性 tripwire。
4. **把测试拆到独立 file `test_asset_hash_cache_policy.cpp`**——拒：`test_content_hash.cpp` 已经是 AssetHashCache concerns 的 home，继续塞。

业界共识来源：LRU / LFU / HashMap 这类 pattern 的 test 套路是"先验正常路径，再验 update semantics (last/first-wins)，再验 eviction 交互"。本 cycle 覆盖中间两层，第三层（`invalidate_by_hash` 在两同 hash 不同 URI 的场景下删几个？）已被 test_cache.cpp 的其他 case 覆盖。

**Coverage.**

- `cmake --build build` 与 `-Werror` clean。
- `ctest --test-dir build` 12/12 suite 绿。
- `build/tests/test_content_hash -s` 10 case / 26 assertion / 0 skip（从 8/20 升 → 10/26）。
- 不动 src/，其他 11 个 suite 继续绿。

**License impact.** 无。

**Registration.** 无 C API / schema / kernel 变更。仅 `tests/test_content_hash.cpp` 加 2 个 TEST_CASE。
