## 2026-04-23 — multi-track-compose-kernel：alpha-over 内核 + 3 blend mode 先行，sink wiring 再拆（Milestone §M2 · Rubric §5.1）

**Context.** `multi-track-compose-kernel` bullet 自己标 "最大 follow-up bullet，估计 3-5 cycle"。完整工作拆 4 块：

1. 像素级 alpha-over 内核（Porter-Duff src-over + blend modes）
2. ComposeSink / H264AacSink 多 demux 支持（或新 sink 类型）
3. 多 track 在时间轴上的合成调度（每 frame 从哪些 track 抽 clip 源帧）
4. e2e 测试 + 确定性回归

本 cycle 做 (1)——pure function + 自测，零架构 churn。后续 cycle 再接 sink / 调度 / e2e。这个顺序原因：(1) 是其他三者的 strict dep；且 (1) 是最容易 unit-test 的部分（数学正确性 vs 集成正确性），先把"算得对"钉死再做"接得上"。

Before-state grep evidence：

- `grep -rn 'alpha_over\|compose/\|BlendMode' src/` 返回空（有 `src/color/` 但无 `src/compose/` 子目录）——没任何合成代码。
- `src/orchestrator/exporter.cpp:60` 的 multi-track gate 直接 `ME_E_UNSUPPORTED "multi-track compose not yet implemented"`——仍然是本 cycle 开始时的状态。
- 无 per-pixel RGBA blending utility；reencode 路径 sws-scale 后直接 encode，从不 composite 多源。

**Decision.** 新 `src/compose/` 模块，两个文件 + 一个 test suite：

1. **`src/compose/alpha_over.hpp`**：
   - `enum class BlendMode : uint8_t { Normal = 0, Multiply = 1, Screen = 2 }`。覆盖 M2 direction line 明示的三种模式。append-only enum（其他模式 lighten / darken / overlay ... 留给 M3+ 按需加）。
   - `void alpha_over(uint8_t* dst, const uint8_t* src, int w, int h, size_t stride_bytes, float opacity, BlendMode mode)`。in-place composite；RGBA8 straight-alpha；无返回值（纯 byte 运算，无失败模式；null / OOB 是调用方责任）。

2. **`src/compose/alpha_over.cpp`** impl：
   - 双 for-loop（no SIMD），每像素：`src_rgb / dst_rgb → [0,1] float32 → BlendMode apply on src → Porter-Duff src-over with scaled alpha (src_a * opacity) → u8 round`.
   - `lroundf()` 用 IEEE-754 round-to-nearest（half-away-from-zero），跨 host 稳定；避 `static_cast<int>(x+0.5f)` 的 banker's-rounding 漂移。
   - `clamp01` 边界保护（opacity / 过量 blend 超 1.0 都 clamp 回）。
   - Blend mode 实装：Normal = src；Multiply = src*dst per channel；Screen = 1 - (1-src)*(1-dst) per channel。全部在 straight-alpha 空间；alpha composite 靠后 Porter-Duff 接管。

3. **`tests/test_compose_alpha_over.cpp`**（11 TEST_CASE / 37 assertion）：
   - Normal: 全不透明 src 替换 dst; 全透明 src 保留 dst; 50% alpha 混合精确（200 * 128/255 = 100.4 → 100 ✓）; opacity 参数缩放 src_a（0.5 opacity + 255 src → effective 128）; opacity=0 no-op on dst。
   - Multiply: 白 src (255,255,255) 保留 dst（identity）; 黑 src (0,0,0) 产出黑（零化）。
   - Screen: 黑 src 保留 dst（identity）; 白 src 产出白（max）。
   - **Determinism**：4×4 buffer 跑 3 mode 各两次 → 每 mode 两次 byte-identical；且三 mode 互不相等（blend_mode_apply 确实在切换）。
   - 0-size buffer (w=0 或 h=0) no-op（no crash）。

4. **`src/CMakeLists.txt`** media_engine sources 追加 `compose/alpha_over.cpp`；`tests/CMakeLists.txt` `_test_suites` 追加 `test_compose_alpha_over` + `target_include_directories PRIVATE src/` 让 test 看见 internal header。

**Not done（留给 follow-up cycle）**。下一个 cycle 的明确 scope 是 `multi-track-compose-sink-wire`（新 bullet）：

- 新 `ComposeSink`（或扩展 `H264AacSink`）接受 `vector<shared_ptr<DemuxContext>>` 代替单 demux——当前 reencode path 内部迭代 per-segment 单 demux，多轨 compose 要改成 per-frame 抽所有 active track 的对应源帧。
- 输出 encoder 接 composited RGBA 帧 —— 现在 reencode_pipeline 直接从 AVFrame (YUV) 过 sws 到 VideoToolbox，compose 需要插一个 RGBA8 中间 buffer。
- Exporter 的 `if (tracks.size() > 1)` gate 翻成调用 ComposeSink。
- e2e 测试：2-track 叠加 timeline → byte-identical 输出两次。

**Alternatives considered.**

1. **Premultiplied alpha** 而非 straight —— 拒：straight alpha 的 math 更贴用户意图（opacity 属性直接等价于 src_a 的 scaling），premultiply 在边界时容易失精度（src_a=0 且 premultiplied，颜色已被归零就没法反向）。大多数 NLE（Premiere, AE, FCP）内部走 straight for 合成，premul 仅在专门 VFX / GPU path 用。
2. **SIMD (AVX2 / NEON) 加速** —— 拒：当前 M2 exit criterion 不 require perf，加 SIMD 撑大该 cycle 的 scope 并引入跨 arch 确定性成本。未来 `compose-simd-optim` 单独 bullet 做。
3. **用 libpixman / cairo 的 compositor 替 own impl** —— 拒：引入新 dep 为了 ~50 行的 blend math 不值；且 pixman license 审查（LGPL）与直接用 MIT-compat 的 FFmpeg/OCIO 风格不符。自己写 + unit test 更可控。
4. **Bake Porter-Duff over 进 blend mode 一块**（变成 9 mode 而非"3 blend + over 标配"） —— 拒：blend mode 和合成规则是正交概念（Premiere UI 也分开：blend dropdown 独立于 opacity slider）。解耦。
5. **Premultiplied 内部 + straight 外部** 的混合模式 —— 拒：增加 API surface 复杂度；straight-only 已够 M2。
6. **浮点输出 buffer (RGBA32F)** 允许高 dynamic range —— 拒：对接 h264/aac encoder path 需要 YUV8 最终，HDR (M8+) 再引入。目前 RGBA8 覆盖主用例。
7. **不做 opacity 参数，让 caller 预 scale src.a** —— 拒：用户 semantic 里 opacity 是 layer 级属性（Track 或 Clip.transform.opacity），不是 alpha channel 的值。API 分开 alpha 和 opacity 两个入参更 match 使用模型。
8. **测试用 fuzz / property-based 而非 known-value** —— 拒：known-value + 业界 canonical fixture（white multiply identity / black screen identity）更有回归抓力；numerical property 测试属于专门 `compose-numerical-robustness` cycle。
9. **在 alpha_over 签名里加 ME_E_INVALID_ARG 返回 for null** —— 拒：增加 check 路径破坏 "pure math, zero side effects" 的简洁性。null / OOB 是 caller UB，与 Porter-Duff 业界约定（pixman, skia）一致。

业界共识来源：Porter-Duff 1984 "Compositing Digital Images" paper 的 src-over 公式是业界标准；Adobe Premiere / After Effects / Photoshop 的 blend mode 数学（`docs/compose_blend_modes.md` 若存在，则以此对齐）；cairo_operator_t 枚举值顺序；Skia SkBlendMode。本 impl 的 3 种 mode 在所有这些系统里的定义都 identical（channel-wise algebra）。

**Coverage.**

- `cmake --build build`（OCIO=OFF）+ `-Werror` clean。
- `ctest --test-dir build` 17/17 suite 绿（新 `test_compose_alpha_over` 是第 17 个）。
- `build/tests/test_compose_alpha_over` 11 case / 37 assertion / 0 fail。
- 其他 16 suite 全绿，无 side-effect（compose/ 模块独立 TU，只新增 symbol）。
- `libmedia_engine.a` 多一个 TU；binary size +几 KB。

**License impact.** 无新 dep。`src/compose/` 是原生 C++20 math 代码。

**Registration.**
- `src/compose/alpha_over.{hpp,cpp}` 新 TU。
- `src/CMakeLists.txt` `media_engine` source list 追加 `compose/alpha_over.cpp`。
- `tests/test_compose_alpha_over.cpp` 新 suite。
- `tests/CMakeLists.txt` `_test_suites` 追加 `test_compose_alpha_over` + 对应 `target_include_directories PRIVATE src/` block。
- `docs/BACKLOG.md`：删 `multi-track-compose-kernel`，P1 末尾加 `multi-track-compose-sink-wire`（sink + 调度 + e2e 承接）。
- 无 C API / schema / kernel registry 改动。

**§M 自动化影响.** M2 exit criterion "2+ video tracks 叠加, alpha / blend mode" 本 cycle **未完成**——像素 math 就位但端到端仍走不通（Exporter gate 依然拒多 track）。§M.1 evidence check：`src/orchestrator/exporter.cpp` 的多轨 gate `ME_E_UNSUPPORTED` 仍在；本 exit criterion 保留未打勾。下一 cycle 的 `multi-track-compose-sink-wire` 继续推进。
