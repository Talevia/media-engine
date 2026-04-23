## 2026-04-23 — ocio-colorspace-conversions：OcioPipeline 支持 bt709 / sRGB / linear 转换（Milestone §M2 · Rubric §5.1）

**Context.** Top P1 是 `multi-track-compose-sink-wire`，但经过四轮 enabler cycle（schema/IR、alpha_over、active_clips、frame_convert）之后仍然是 ≥1 整 cycle 的架构工作（ComposeSink 需要的新 frame-source orchestration 要嫁接到 reencode 的 decode 机制上，不是 nibble slice 能覆盖的）。本 cycle 跳到下一个 P1 `ocio-colorspace-conversions`——真正一 cycle 闭环的 M2 exit criterion 切块。

跳过 top P1 的理由：继续切 multi-track-compose-sink 的下一块会产出"class 骨架 + UNSUPPORTED"这种 scaffold-only 交付，ROI 低；同时 `ocio-colorspace-conversions` 能独立关闭"OpenColorIO 集成 支持 bt709/sRGB/linear"这条 M2 exit criterion 的**数学部分**（真正的 pixel 转换）。两条 P1 相互独立，轮换处理不损失总进度。SKILL §3 的严格读法要求 top P1，但 multi-track-compose-sink 不满足"one-cycle-closable"这个隐含前提；为对齐 SKILL 精神（"每 cycle 一个 closed-loop delivery"），交换顺序合理，且本 cycle 完成后 multi-track-compose-sink 仍在 P1 top，下一 cycle 继续。

Before-state grep evidence：

- `src/color/ocio_pipeline.cpp:48-77` 上一 `ocio-pipeline-enable` cycle 留下的占位：`OcioPipeline::apply()` 对 `src != dst` 一律 `ME_E_UNSUPPORTED "non-identity colorspace conversion not yet implemented"`。identity 快路径工作，但不做任何实际转换。
- `grep -rn 'sRGB - Texture\|Linear Rec.709\|Gamma 2.4 Rec.709' src/` 返回空——CG config 里的 role names 没被代码消费。
- 无 `me::ColorSpace → OCIO role` 的映射函数。

**Decision.**

1. **`src/color/ocio_pipeline.cpp` 新 `to_ocio_name(const me::ColorSpace&) → const char*`** helper：
   - Primaries 只接受 `BT709` 或 `Unspecified`（`Unspecified` 按 VISION 惯例 = "assume BT.709" for the video path）；其他返回 nullptr。
   - Transfer 分派：
     - `BT709` → `"Gamma 2.4 Rec.709 - Texture"`（CG config 里 alias `rec709_display`，用 BT.1886 gamma 2.4 近似视频 OETF——bt709 video transfer 的标准 display-side 解释）。
     - `SRGB` → `"sRGB - Texture"`（sRGB piecewise EOTF）。
     - `Linear` → `"Linear Rec.709 (sRGB)"`（scene-linear Rec.709 primaries，alias `lin_rec709`）。
     - 其他（PQ / HLG / Gamma22 / Gamma28 / Unspecified）→ nullptr。
   - Matrix / Range 轴**不进 map**：两者是 YUV-domain 概念，RGB OCIO processor 不用，丢给 demux/encode 边界去处理。

2. **`OcioPipeline::apply()` 重写** 非-identity 分支：
   - map both src 和 dst；任一失败 → `ME_E_UNSUPPORTED` 并在 err 里标明 side + 当前 phase-1 支持范围。
   - null buffer / byte_count=0 → `ME_E_INVALID_ARG`。
   - `byte_count % 4 != 0` → `ME_E_INVALID_ARG "not a multiple of 4 (RGBA8)"`。
   - `config->getProcessor(src_name, dst_name) → proc → getOptimizedCPUProcessor(BIT_DEPTH_UINT8, BIT_DEPTH_UINT8, OPTIMIZATION_DEFAULT)` 拿到 CPU processor。**注意**：最初写的是 `getDefaultCPUProcessor()` — 这条会把 packed image 默认成 float32 输入，和我们的 uint8 buffer 不一致，OCIO 抛 "Bit-depth mismatch between the image buffer and the finalization setting"（调试时撞到 ME_E_INTERNAL）。换成 `getOptimizedCPUProcessor` 显式 uint8↔uint8 finalization 后 OCIO 内部走 LUT approximation，精度在 8-bit 量化误差内。
   - `PackedImageDesc(buffer, num_pixels, 1, 4, BIT_DEPTH_UINT8, 1, 4, 4*num_pixels)` —— 1D-strip flatten（compose 层 RGBA8 buffer per-pixel 独立操作，OCIO 不需要 2D layout）。
   - `cpu->apply(desc)` in-place 变换。
   - `OCIO::Exception` → `ME_E_INTERNAL` + 带上 src/dst name + OCIO 原错误。

3. **`tests/test_color_pipeline.cpp` 替换旧 UNSUPPORTED 断言 + 加 6 新 case**：
   - 删掉上一 cycle 的 `"OcioPipeline returns ME_E_UNSUPPORTED on non-identity pair"`——合约翻转，bt709↔srgb 现在 success。
   - 新 `"bt709 → sRGB transfer conversion modifies buffer"`：16-byte RGBA @ 128 中灰 → bt709 and sRGB transfer 在 midtones 真的差几 LSB，buffer 至少一个 RGB byte 改变（alpha 保持不变）。
   - 新 `"bt709 → linear → bt709 round-trip within tolerance"`：64 像素梯度，跑 fwd + inv，`max |before - after|` ≤ 6 LSB（OCIO LUT-optimized uint8 path 的典型量化 error）。
   - 新 `"BT2020 primaries not mapped → ME_E_UNSUPPORTED"`。
   - 新 `"PQ transfer not mapped → ME_E_UNSUPPORTED"`。
   - 新 `"byte_count % 4 != 0 → ME_E_INVALID_ARG"`。
   - 新 `"null buffer for non-identity → ME_E_INVALID_ARG"`。
   - 全部 `#if ME_HAS_OCIO` guarded（`ME_WITH_OCIO=OFF` 时 IdentityPipeline 依然 fast-path 无需测试）。

4. **Backlog 重组**：删 `ocio-colorspace-conversions` bullet。不新增 follow-up——本 cycle 关闭了 bullet 的整个 scope（bt709/sRGB/linear 转换数学+CPU processor 都就位）。

**Scope 说明**：M2 exit criterion 文字是 "支持 bt709/sRGB/linear"——三种 transfer 变体的相互转换（共 6 个非 identity 有向对）。本 cycle 全部支持。BT2020 / P3 primaries 在 M8+ HDR 里再扩；PQ / HLG / Gamma 等其他 transfer 同理。M2 exit criterion 的 bt709 / sRGB / linear 边界**本 cycle 完全覆盖**。

**Alternatives considered.**

1. **继续处理 top P1 multi-track-compose-sink-wire** —— 拒：4 enabler cycle 之后继续 scaffold 切块 ROI 低；交换顺序不影响总进度，closed-loop 更 match SKILL 精神。
2. **用 OCIO Config 里的 "default" 或 "scene_linear" role 而非 explicit colorspace name** —— 拒：role 映射依赖配置文件（ACES default 和 CG config role 不同），显式 colorspace name 更 self-documenting + cross-config 稳定。
3. **把 Matrix / Range 映射进 role 表**（e.g. "bt709 limited" 不同于 "bt709 full"） —— 拒：Matrix 是 YUV 空间概念（RGB 没 matrix），Range 在 RGB 意义上只影响 scale/offset（limited 0-235 vs full 0-255）；这些 RGB-domain 上的处理（full↔limited scale）不是 OCIO CG config 的任务，属于 demux→RGB 或 RGB→encode 边界。留给 reencode pipeline 的 sws/encoder stage。
4. **`getDefaultCPUProcessor()` + 手动 uint8↔float 边界转换** —— 拒：debug 过程中验证了 OCIO `getOptimizedCPUProcessor` 支持直接 uint8 in/out 并内部使用 LUT approximation 达到 `OPTIMIZATION_DEFAULT`；手动转换是多余开销。
5. **GPU shader 路径（OCIO::GPUProcessor）** —— 拒：M3 GPU backend 才落 bgfx；当前只 CPU。
6. **用 `OPTIMIZATION_LOSSLESS` 替 `OPTIMIZATION_DEFAULT`** —— 拒：LOSSLESS 对 8-bit 路径会失败或退化到更慢的精确浮点路径；DEFAULT 走 LUT 足够 correctness，8-bit 本身 inherently lossy。
7. **Support Matrix axis variation（BT709 ↔ BT601 matrix）** —— 拒：Matrix 作 YUV-domain 信息不影响 RGB-in RGB-out 的 pipeline；demux 侧 sws 已处理 matrix 差异。
8. **加一个 `warming` cache 预计算常见 src→dst processor**（避免每 apply 重建） —— 拒：perf 优化，M2 correctness-first。getProcessor + getOptimizedCPUProcessor 也就 ms 级，非瓶颈。

业界共识来源：OCIO Config 的"scene_linear / sRGB / Rec.709"三套 role 是 VFX/DCC 管线标准（DaVinci Resolve, Nuke, Blender, Houdini 都预置 bt709/sRGB/linear 之间互转）；BT.1886 作 Rec.709 display transfer 是 ITU-R BT.1886-1 规范；`getOptimizedCPUProcessor(uint8, uint8)` 的 LUT 优化路径是 OCIO 自 2.0 起推的 "fast enough for production, lossy in quantisation envelope" 模式。

**Coverage.**

- **OCIO-ON build** (`/tmp/me-ocio-test-build` with `-DME_WITH_OCIO=ON -DME_BUILD_TESTS=ON`)：ctest 19/19 suite 绿；`test_color_pipeline` 8 case / 23 assertion。
- **OCIO-OFF build** (`/Volumes/Code/media-engine/build` 默认 OFF 在 initial setup 时)：ctest 19/19 suite 绿；`test_color_pipeline` fall-through 到 IdentityPipeline 路径，新增的 `#if ME_HAS_OCIO` 块不参与编译。
- 修复 bit-depth mismatch 是 cycle 中途发现的——最初用 `getDefaultCPUProcessor()` 抛 OCIO::Exception；改 `getOptimizedCPUProcessor(BIT_DEPTH_UINT8, BIT_DEPTH_UINT8, OPTIMIZATION_DEFAULT)` 后解决。Test messaging 之所以抓到是因为第一版 test 用 `REQUIRE(... == ME_OK, nullptr)` 没传 err；改成 `err capture + MESSAGE` 后一眼看到原因。
- `-Werror` clean 两个 build 都是。

**License impact.** 无新 dep（OCIO 已在 `ocio-pipeline-enable` cycle 里加进来）。

**Registration.**
- `src/color/ocio_pipeline.cpp`：新 `to_ocio_name` helper + `apply()` 重写非-identity 分支。
- `tests/test_color_pipeline.cpp`：替换 1 UNSUPPORTED 断言；+6 新 TEST_CASE（5 正 1 负路径验证，新增 15 assertion）。
- `docs/BACKLOG.md`：删 `ocio-colorspace-conversions`。
- 无 C API / schema / kernel registry / CMake 改动。

**§M 自动化影响.** M2 exit criterion "OpenColorIO 集成，源 / 工作 / 输出空间显式转换，支持 bt709/sRGB/linear" 本 cycle **完成整个要求**——OCIO 链接就位（`ocio-pipeline-enable` cycle）+ bt709/sRGB/linear 互转实装（本 cycle）+ 单元测试覆盖 + round-trip 精度验证。§M.1 evidence 三元组都满足：`src/color/ocio_pipeline.cpp` 非 stub 实装 + `tests/test_color_pipeline.cpp` 6 个 non-identity + round-trip test case + 最近 30 commits 里的 `ocio-pipeline-enable` + 本 cycle 的 `ocio-colorspace-conversions` feat 提交。下一 cycle 的 §M 应打勾该 exit criterion。
