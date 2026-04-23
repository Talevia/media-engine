## 2026-04-23 — debt-render-bitexact-flags：reencode 路径确定性守护（Milestone §M1-debt · Rubric §5.3）

**Context.** `test_determinism` 只覆盖 passthrough（stream-copy），reencode 路径（h264_videotoolbox + libavcodec aac）没有确定性 tripwire。Backlog bullet 断言当时 reencode 产物 mvhd 带 wallclock → 两次跑不 byte-identical。**实测发现**假设错了一半：现在 reencode **经验上是** byte-deterministic 的（同 binary 跑两次 `05_reencode examples/01_passthrough/sample.timeline.json` 产 SHA1 一致的 406,877 字节 MP4）。FFmpeg ≥ 5.x 的 mov muxer 在 `creation_time` 不被 metadata 显式指定且 `AVFMT_FLAG_BITEXACT` 不 set 时写 `0`（而不是 wallclock）——这是默认行为改过，backlog bullet 的证据基于老版 libav。

但"偶然成立的不变量" 不是"承诺的不变量"——libav 的下游版本升级、或某个 encoder option 改默认、或 HW encoder 行为漂移，都可能让这条偶然性消失。所以本轮**还是按原计划**把 BITEXACT flags set 下去，把非承诺变成承诺，同时加回归测试作为 tripwire。

**Decision.** 三处 code + 一处测试：

1. `src/io/mux_context.cpp` 的 `MuxContext::open()` 在 `avformat_alloc_output_context2` 后 `fmt->flags |= AVFMT_FLAG_BITEXACT`。这个 flag 让 mov muxer 把 `mvhd/tkhd.creation_time/modification_time` 写 `0`，不写 "encoder=Lavf…" / "major_brand_version" 等随版本漂移的字符串。覆盖所有用 MuxContext 的路径（passthrough + reencode 都吃这一条）。
2. `src/orchestrator/reencode_video.cpp` 的 `open_video_encoder` set `AV_CODEC_FLAG_BITEXACT` 在 `avcodec_open2` 之前。h264_videotoolbox 是 HW encoder，这个 flag 是**建议性的**（VT 可能忽略），但未来如果切 x264（GPL) 或 OpenH264 (LGPL) 等软件 encoder 这条 flag 直接生效。现在 set 下去不花成本、面向未来。
3. `src/orchestrator/reencode_audio.cpp` 的 `open_audio_encoder` 同样 set `AV_CODEC_FLAG_BITEXACT`。libavcodec 内置 AAC encoder **明确** 读这个 flag：打开会影响 extradata 里的 MPEG-4 AudioSpecificConfig padding 和 objectTypeIndication 的"encoder identification" 区段。
4. `tests/test_determinism.cpp` 新 `TEST_CASE("h264/aac reencode is byte-deterministic across two independent renders")`：同 fixture 走 `render_with_spec(…, "h264", "aac")` 两次，slurp + byte-equality + fail-with-offset 格式。`render_with_spec` 是把原 `render_passthrough` 提取成可参数化的 codec 选择 helper；`render_passthrough` 现在是 thin wrapper。

**Fixture 尺寸调整.** 原 fixture 320×240 @ 10 fps (MPEG-4 Part 2 via libav 的 gen_fixture)。测 h264_videotoolbox 发现它在 320×240 或 10 fps 下直接 `avcodec_open2` 返回 `Generic error in an external library` / `Invalid argument`——macOS VideoToolbox 的 h264 encoder 有实际的最小尺寸和帧率下限（实测 480×270、15 fps 以上才开得了）。为了让 reencode determinism case 能**真跑**（而不是 skip），把 fixture 调到 640×480 @ 25 fps / 25 帧仍是 1 秒：

- `gen_fixture.cpp` 里 `kWidth/kHeight/kFrameRate/kNumFrames` 从 320/240/10/10 改到 640/480/25/25。注释里说明"VT 最小值"是改这个尺寸的原因。
- `test_determinism.cpp` 两处 `TimelineBuilder().frame_rate(...).resolution(...)` 跟着改到 640×480 / 25fps / 25-denominator 的 clip duration。

passthrough 确定性不受影响（本来就和 fixture 内容无关，只和处理路径稳定性有关）。

**Reencode case 的 fallback.** 上面新 TEST_CASE 有个显式 skip 分支：如果 `render_with_spec(..."h264","aac")` 首次调用返回 `ME_E_UNSUPPORTED` 或 `ME_E_ENCODE`，就 `MESSAGE(...)` 跳过——Linux CI 没 VideoToolbox 时这条才 trip，mac dev 机上总是跑真 case。不让 Linux CI 红，也不让本 case 变"空 skip 占位"——mac 机严格覆盖。

**Alternatives considered.**

1. **不 set BITEXACT，只加测试**——拒：测试偶然通过在现 libav 版本上，升 libav 之后可能无声劣化。既然要加回归 tripwire，就把 flag 也 set 下去，让 tripwire 有确定的语义保证后盾。
2. **用 container-level metadata `-metadata creation_time=0` 手工写**——拒：FFmpeg CLI 的做法不移植到 libav API。`AVFMT_FLAG_BITEXACT` 是 libav 原生 knob，一行搞定。
3. **保持 320×240 fixture + reencode case 总是 skip** —— 拒：VT 不可用的场合已经是"skip"出口；可用场合不应白费。调整 fixture 尺寸让 mac 机走真路径覆盖率划算。
4. **单独生成一个 VT-friendly fixture，保留 320×240 给 passthrough**——拒：两个 fixture 意味着 gen_fixture 要吃参数 + CMakeLists 要 run 两次。统一 fixture 只多 ~200 KB 而保持 tests 简单。
5. **切 x264 encoder**（软件 h264）让 BITEXACT 强制生效—— 拒：libx264 是 GPL（CLAUDE.md 反需求红线）。VT 在 Mac 是 LGPL-clean 的硬件路径；Linux 打算上 OpenH264 / VA-API，也都不是 x264。

业界共识来源：FFmpeg 自己的 `fate` regression suite 就是靠 `-fflags +bitexact -flags +bitexact` 走 byte-identical 对比，这是 libav 生态里"确定性回归"的标准工具链。Apple 的 VideoToolbox 尺寸/帧率最小值是多年来 forum 里反复出现的 gotcha（Apple Developer Forums 上"VideoToolbox h264 encoder return -12902 / Invalid argument" 的典型答复就是提升尺寸和帧率）——不是 bug，是 API contract。

**Coverage.**

- `cmake --build build` 与 `-Werror` 通过。
- `ctest --test-dir build` 7/7 suite 绿；`test_determinism` 从 2 case / 10 assertion 升到 3 case / 16 assertion。
- `build/tests/test_determinism -s` 显示新 reencode case 跑出 242,398 bytes 两份 byte-identical，不 skip。
- Passthrough 两 case 仍绿（fixture 改尺寸不改稳定性）。
- `01_passthrough` 手测仍产 2s MP4（passthrough 路径 sanity）。

**License impact.** 无新依赖。`AVFMT_FLAG_BITEXACT` / `AV_CODEC_FLAG_BITEXACT` 是 libav 既有 flag，FFmpeg ≥ 3.x。

**Registration.** 无注册点变更：
- 无 C API 改动。
- 无 schema / kernel / CodecPool / Exporter factory / CMake target 改动。
- Fixture 规格（尺寸 / 帧率）算"internal test fixture"，没有外部契约。
