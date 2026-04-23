## 2026-04-23 — debt-docs-api-md-stale-audit：`docs/API.md` 补 frame-server stub 提醒 + probe + cache 示例（Milestone §M1-debt · Rubric §5.2）

**Context.** `docs/API.md` 在 M1 过程中未审：最近几周落了 `me_probe` + 6 个扩 accessor（`me-probe-more-fields`）、`me_thumbnail_png` 实装（`thumbnail-impl`）、`me_cache_stats` 真计数（`cache-stats-impl`）、`me_render_start` h264+aac + multi-clip 支持（`reencode-h264-videotoolbox` + `reencode-multi-clip`）等诸多变化——但 API.md 的 Common patterns 只有 "minimal render" + "frame-server scrub" + "progress callback" 三条，**既缺 probe / cache 的示例，又把 frame-server 写成正常代码路径** 而没标注"M6 之前 return ME_E_UNSUPPORTED"。读者 copy-paste 的体验是 "这段示例跑不过"。

grep 验证：
- `grep -rn 'me_cache_stats\|me_cache_invalidate_asset' docs/API.md` 返回空——cache API 三条 symbol 在 API.md 里完全不出现。
- `grep -rn 'me_probe\|me_media_info_container' docs/API.md` 返回空——probe API 同样不出现。
- `grep -rn 'me_render_frame' docs/API.md` 命中 frame-server scrub 示例——但周围没"stub"标注；读者以为它工作。

**Decision.** 三处最小改动，保持 API.md 既有 scope（只改 Common patterns 小节 + 微调 ownership 表），不扩容文档：

1. **Frame-server scrub 小节加"M6 之前 stub"提醒** —— block-quote 在代码示例前，说明 `me_render_frame` 今天返回 `ME_E_UNSUPPORTED`，the shape 是 committed API surface，host 代码可以 against it 写——但 `if (... == ME_OK)` 分支 M6 之前不触发。读者不会被 `if` 分支欺骗以为 stub 真工作。
2. **新增 "Probe asset metadata" 小节** —— show `me_probe` → `me_media_info_container/codec/width/height/frame_rate` → `me_media_info_destroy` 的完整 lifecycle。注释里点名 extended fields（rotation / color_range / color_primaries / color_transfer / color_space / bit_depth）引读者去 `probe.h` 看全 set。
3. **新增 "Cache observability" 小节** —— show `me_cache_stats` → printf + `me_cache_invalidate_asset` （接受 `sha256:<hex>` 或 bare hex 两种形式）+ `me_cache_clear` 的 teardown 用法。对应 3 条 `cache.h` 的公开函数。
4. **Ownership table 修一处措辞** —— `uint8_t** (PNG, etc.)` 改成 `uint8_t** from me_thumbnail_png` —— "etc." 当时预留给未来 byte-buffer API，但 1 年后还没出现；当前只有 thumbnail，精确更好。

文档从 163 行升到 204 行（+41）。

**不做的事（审后发现不动更 clean）：**

- **不重写 Engine / Timeline / Render job 的 Core concepts 段**——这三段的 ownership / thread-safety 合约没变（M1 各 cycle 都尊重 invariants），不需要改。
- **不加 thumbnail Common pattern**——thumbnail API 已经在 ownership table 提到；加一个 pattern 会让 common-patterns 列表膨胀到 6 段，读者看文档的"抓手"从 3-4 个变 6-7 个 ROI 低。thumbnail 是 ownership + `me_buffer_free` 的典型代表，表里一行足够。Host 真要写 thumbnail 代码直接读 `thumbnail.h`（12 行头文件，自描述）。
- **不改 Threading contract 表**——表准确的。
- **不加 ABI stability 章节新条目**——现有 4 条规则已覆盖 M1 各 cycle 的 ABI 决策（e.g. `me-probe-more-fields` 严格 append-only）。
- **不加 language bindings 具体示例**——那是 `docs/INTEGRATION.md` 的职责。

**Alternatives considered.**

1. **给每个 public function 写 reference doc**（API spec 风格）——拒：`include/media_engine/*.h` 本身是"self-documenting"（签名 + 简洁注释），重复写只会增加漂移风险。API.md 定位是"around the headers" 的 contract（ownership / threading / lifetime），不是 reference。
2. **把 Common patterns 重组成按功能分组的章节（Probe / Render / Thumbnail / Cache 各一节）**——拒：API.md 是 contract doc，不是 tutorial。host 需要具体教程去 `INTEGRATION.md` / 示例仓库。
3. **在 ownership table 展开"all out-param pointers in table 6 行"**——拒：现有 7 行已够。表不是 function enumerator。
4. **加"M1 API 已冻结"的 stability note**——拒：stability note 是 milestone-level 决定，`docs/MILESTONES.md` 头部指针 + CHANGELOG 管；API.md 说"Public headers are C11 / extern C" + ABI 4 条规则足够。

业界共识来源：POSIX 的 C API 文档策略（headers self-document；standards doc 给 contract）、SDL3 migration 文档风格、libcurl 的 CONTRACT 文档分工。本仓库的 API.md 沿袭这个模式是对的，本 cycle 只补 stale / missing 小节不重写结构。

**Coverage.**

- `grep -rn 'me_cache_stats\|me_probe\|me_buffer_free\|ME_E_UNSUPPORTED' docs/API.md` 现在都命中——3 条 shipping API 和 1 条 stub 状态都有显式 narrative anchor。
- 无代码 / 测试 / 构建改动；ctest 不重跑。
- API.md 行数 163 → 204。

**License impact.** 无。

**Registration.** 无代码 / schema / kernel 变更。`docs/API.md` 一处编辑（两个新 "Common patterns" 小节 + 一处 table 措辞修订 + 一个 block-quote stub 提醒）。
