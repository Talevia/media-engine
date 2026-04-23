## 2026-04-23 — me-asset-color-space-thread-to-encoder：per-clip `me::Asset::color_space` threaded 到 reencode 的 `Pipeline::apply()`（Milestone §M2-prep · Rubric §5.1）

**Context.** 昨天的 `ocio-pipeline-wire-first-consumer` cycle 把 `me::color::make_pipeline()` 接进 `SharedEncState` 并 per-video-frame call `apply()`——但 apply 的 (src, dst) ColorSpace 参数是 `src/orchestrator/reencode_segment.cpp:210` 的 `const me::ColorSpace dummy{}` 硬编码空值。`asset-colorspace-field`（65036a5）已经让 loader 把 `me::Asset::color_space`（std::optional<me::ColorSpace>）从 JSON 填上——但这条路径**没接通**到 reencode_segment。结果 ME_WITH_OCIO 切 ON 那天真 OcioPipeline 只能看到 UNSPECIFIED→UNSPECIFIED 的 identity，等于没接入。

**Decision.** 按 Exporter → OutputSink → ReencodeOptions → SharedEncState → process_segment 的层级往下 thread per-clip source + timeline-level target color space。具体改动：

1. `src/orchestrator/reencode_pipeline.hpp`：
   - `ReencodeSegment` 加 `me::ColorSpace source_color_space { }`（default UNSPECIFIED）。
   - `ReencodeOptions` 加 `me::ColorSpace target_color_space { }`。
   - 新 include `timeline/timeline_impl.hpp`。
2. `src/orchestrator/reencode_segment.hpp`：
   - `SharedEncState` 加 `me::ColorSpace target_color_space { }`。
3. `src/orchestrator/reencode_pipeline.cpp`：
   - `reencode_mux` 在构造 `shared` 时 copy `opts.target_color_space` → `shared.target_color_space`。
4. `src/orchestrator/reencode_segment.cpp`：
   - `push_video_frame` lambda 的 `apply(...)` call 把 `dummy` 换成 `seg.source_color_space, shared.target_color_space`。
5. `src/orchestrator/output_sink.hpp`：
   - `ClipTimeRange` 加 `me::ColorSpace source_color_space { }`。
   - `SinkCommon` 加 `me::ColorSpace target_color_space { }`。
   - 新 include `timeline/timeline_impl.hpp`。
6. `src/orchestrator/output_sink.cpp`：
   - `H264AacSink::process` 构造 `ReencodeSegment` 时传递 `ranges_[i].source_color_space`；构造 `ReencodeOptions` 时 set `opts.target_color_space = common_.target_color_space`。`PassthroughSink` 不 consume color space（stream copy 不变换 color），不动。
7. `src/orchestrator/exporter.cpp`：
   - 构造 `ClipTimeRange` 时读 `tl_->assets.at(clip.asset_id).color_space`，有值就用，否则 default-constructed。
   - `SinkCommon::target_color_space` 今天**保持 default-UNSPECIFIED**（见下节）。

**Target color space 今天留空的原因.** `me::Timeline` struct（`src/timeline/timeline_impl.hpp:77`）**没有** top-level `color_space` 字段——只有 per-Asset。loader 也只在 asset 内部 parse `colorSpace`（`timeline_loader.cpp:148`）。JSON schema 里 `"colorSpace": {...}` 顶层字段是**被 loader 忽略**的——这条是 pre-existing discrepancy，不在本 cycle scope。要把 target 串起来需要：(a) `Timeline::color_space` 字段，(b) loader parse timeline-level `colorSpace`，(c) TIMELINE_SCHEMA.md 写明。那是独立 "`me-timeline-top-level-color-space`" cycle。本 cycle 的 scope 明写"per-clip source 接通"——target 先留 UNSPECIFIED，`IdentityPipeline` 反正忽略，未来接 OcioPipeline 时那边会 fallback 到"假设 scene-referred"的保守行为，不崩不错。

**为什么先做 per-clip source 而不是 target.**

- source 已经有 impl 支持（`asset-colorspace-field` cycle 把 loader 搞完了，只差 thread），target 没有（要先改 schema + loader）。
- 颜色管理的"主要功能差异"在 source——两个 asset 不同 source color space 输到同一 output 时才需要 pipeline 真工作；target 相同、source 不同的 case 是最常见的。一个 asset 一个 output，没 pipeline 需求；多个不同 asset 一个 output，source 差异驱动 apply()。
- 先让 asset.color_space 有 **exit point**（不是死数据）——downstream（future OcioPipeline）有实际 consumer 可以看到。

**Alternatives considered.**

1. **顺带加 Timeline::color_space 字段 + loader parse**——拒：schema change + loader change + TimelineBuilder change + test 更新，cycle 会膨胀 2x。分离到下一 bullet。
2. **把 color_space 放进 `me::Clip` struct 直接**（不走 Asset）——拒：两 clip 引用同一 asset 时 color_space 应该一致（source 来自同一 material），放 Asset 才是 canonical。`asset-colorspace-field` 决定早就把 Asset 选成 source of truth。
3. **让 Pipeline::apply 签名不吃 ColorSpace 参数**（用 "pre-bound" pipeline 模式）——拒：factory 每次只 build 一个 pipeline，per-clip source 变化意味着 per-call ColorSpace 参数。预绑定需要 per-clip pipeline instance，对 IdentityPipeline 是浪费。
4. **把 per-clip source 放 `SharedEncState` 而非 `ReencodeSegment`**——拒：segment 之间 source 可能不同（不同 asset 不同 color_space）。segment-local 正确，shared 全局错。

业界共识来源：DaVinci Resolve / Adobe Premiere 的 "source color space" 元数据存放在 clip-的-asset（media pool item）上，而 "timeline working color space" 是 timeline-global——正是本仓库的划分。per-frame render 时 decoder 拿 asset 的 source，compose/export 拿 timeline working 的 target，pipeline 做 (source → target) 转换。

**Coverage.**

- `cmake --build build` 与 `-Werror` clean（7 files touched：4 headers + 3 cpp）。
- `ctest --test-dir build` 12/12 suite 绿。
- **Determinism 保持**：`test_determinism` 的 4 个 reencode/passthrough case 全过——`IdentityPipeline.apply` 仍是 no-op，即使 source_color_space 现在是实际值（而非 dummy），apply 不读它，所以 bytes 不动。
- **Asset-reuse 保持**：`test_asset_reuse` 的 3 case 全过——asset map dedup 合约未改，只是 color_space 字段多一次读取。
- **test_output_sink 继续绿**——`ClipTimeRange` 构造器是 aggregate + default，旧测试 init 两字段仍然合法（新字段用 default）。

**License impact.** 无新依赖。仅 `me::ColorSpace` 跨 TU 引用（header 互相 include）。

**Registration.** 无 C API / schema / kernel 变更（`me::ColorSpace` 是 internal type；没有 public header 新增）。
- `src/orchestrator/reencode_pipeline.hpp` / `reencode_pipeline.cpp`
- `src/orchestrator/reencode_segment.hpp` / `reencode_segment.cpp`
- `src/orchestrator/output_sink.hpp` / `output_sink.cpp`
- `src/orchestrator/exporter.cpp`
