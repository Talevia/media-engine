## 2026-04-23 — ocio-pipeline-wire-first-consumer：reencode pipeline 成为 `me::color::make_pipeline()` 的首个消费者（Milestone §M2-prep · Rubric §5.1）

**Context.** `ocio-pipeline-factory` 在 3060977 落地后，`src/color/pipeline.hpp` 里有 `me::color::make_pipeline()` 工厂但**零 consumer**：`grep -rn 'make_pipeline\|IdentityPipeline' src/` 只命中 `src/core/engine_seed.cpp:24` 的 future-extension 注释，不是实际调用。factory body（inline template instantiation）只在 test_color_pipeline 里被 exercise。src 侧 inline header 完全不被 include / call，意味着：（a）未来 ME_WITH_OCIO 切到 ON 时新 consumer 才发现 linkage 问题；（b）factory 的 "per-call 语义" 没被任何 runtime 路径验证。需要一个 src 侧 hook 让 factory 真正进入热路径的 link graph。

**Decision.** Reencode pipeline（`reencode_mux`）作为第一个 consumer：

1. `src/orchestrator/reencode_segment.hpp` 的 `SharedEncState` 加字段 `std::unique_ptr<me::color::Pipeline> color_pipeline`。header 新增 `#include "color/pipeline.hpp"`。
2. `src/orchestrator/reencode_pipeline.cpp` 的 `reencode_mux` 在构造 `shared` 之后 `shared.color_pipeline = me::color::make_pipeline()`——一次 per-job，不 per-segment。make_pipeline() 今天返回 `IdentityPipeline`（ME_HAS_OCIO 未定义）。
3. `src/orchestrator/reencode_segment.cpp` 的 `process_segment()` 里 `push_video_frame` lambda 在 `encode_video_frame()` 之前 call `shared.color_pipeline->apply(f->data[0], y_plane_bytes, dummy, dummy, &err)`——per-video-frame。IdentityPipeline 是 no-op，buffer 不动。
4. 新 `TEST_CASE` 不需要（reencode determinism tripwire 已存在——`test_determinism.cpp` 的 reencode single- + multi-clip 两个 byte-equality case 本身就是"apply 是 no-op" 的验证：有了 apply 调用后 bytes 还是 identical 说明 IdentityPipeline 真的不动 buffer）。

**故意的简化 / 留给下一 cycle 的非承诺.**

- **src/dst ColorSpace 今天传 `me::ColorSpace{}` 空 default**。原因：asset 的 color_space（`me::Asset::color_space`）没被 threaded 到 SharedEncState。`timeline-asset-map` + `asset-colorspace-field` 已经把 ColorSpace 带到 IR 层，但 reencode pipeline 没 consume。传空值对 IdentityPipeline 无意义（apply 不读 src/dst），对真 OcioPipeline 则是 bug——所以这条 wire 是**scaffold**，不是产品级。下一 cycle（大概率 M2 compose 落地时附带）要把 asset.color_space 穿下来。
- **只 apply 到 Y plane**（`f->data[0]`, `linesize[0] * height` bytes）。YUV420P 是 planar，正确的 pipeline 要遍历 data[0..2] 三个 plane，且 OCIO transform 通常要求 interleaved RGB——这整条 buffer-handle 语义是 `me::color::Pipeline::apply` 接口的下一次演进话题，不在本 cycle scope。Y plane 是够"真实 buffer"让 IdentityPipeline 的 apply call path 活跃，不是断言 apply 语义正确。
- **Audio 路径不 apply**。Color pipeline 纯视频 concern，音频路径无 apply 调用。

**为什么这样的简化还算 progress.**

- factory body 现在**实际在 `media_engine` 的 link graph 里**：reencode_pipeline.cpp 的 reencode_mux 会实例化 `make_pipeline()`，进 .a 档。ME_HAS_OCIO 切 ON 那天编译器 / linker 会告诉我们 OcioPipeline 的实际类型有没有 ABI 问题，不用等 M2 发现。
- apply() 的 per-frame 调用路径现在**真的有 runtime call**。即使 body 是 no-op，调用开销为零——但 "call site 存在" 这条是未来 OcioPipeline 替身时的 zero-change hook 保证。
- Determinism tripwire 通过意味着：加 factory call + apply call 对 existing byte output **零影响**。这直接验证 IdentityPipeline 真是 no-op（而不是只测 header-only stub 在 test_color_pipeline.cpp 里的契约）。

**Alternatives considered.**

1. **先把 asset.color_space 穿到 SharedEncState**，再做 wire——拒：两件事（wire factory + 穿 asset color space）bundled 在一起会让本 cycle 膨胀到 >500 lines diff，covering 两个独立决策点。分开做。
2. **Wire 到 thumbnail 或 passthrough 路径**——passthrough 不走 encoder，不是 Pipeline 的自然消费路径；thumbnail 路径也不是 OCIO 的主目标（OCIO 主打 compose + export）。reencode pipeline 是对的选择。
3. **不 call apply()，只 store 到 SharedEncState**——consider 过；factory body 会被实例化（call make_pipeline），但 apply() vtable 不被 runtime 触发——linker 有机会因 `-fvisibility=hidden` 等原因剪掉 IdentityPipeline::apply 符号。call apply() 一次确保 vtable 路径活跃。
4. **给 apply() 传 full RGB buffer 通过 av_frame_make_writable + av_image_copy_to_buffer**——拒：对 IdentityPipeline 是浪费 (no-op 不读)，对 OcioPipeline 也要重设计（OCIO 的 input/output 缓冲区模型不是单一 byte-copy）。留给 Pipeline API 下一次演进。

业界共识来源：factory 头一次 consumer 是 skeleton→integration 的经典过渡。LLVM 的 PassRegistry、bgfx 的 Renderer、Skia 的 GrDirectContext 都走同一 pattern——先 reserve seat，再用 identity 实现守 invariant，最后真实现替换。

**Coverage.**

- `cmake --build build` 与 `-Werror` clean。
- `ctest --test-dir build` 12/12 suite 绿。
- `test_determinism` 的 4 case（2 passthrough + 2 h264/aac）全过——apply() 插入没扰动 byte output。
- `test_color_pipeline` 的 factory smoke 仍过（factory 契约没变）。
- `grep -rn 'make_pipeline\|color_pipeline' src/` 现在返回：pipeline.hpp 声明、reencode_pipeline.cpp 调用、reencode_segment.hpp 字段、reencode_segment.cpp apply 调用。Factory 正式 linked。

**License impact.** 无新依赖。header-only `me::color` + STL。

**Registration.** 无 C API / schema / kernel 变更。
- `src/orchestrator/reencode_segment.hpp` 新 include `color/pipeline.hpp` + `SharedEncState::color_pipeline` 字段。
- `src/orchestrator/reencode_pipeline.cpp` 的 `reencode_mux` 加 `shared.color_pipeline = me::color::make_pipeline()` 一行。
- `src/orchestrator/reencode_segment.cpp` 的 `process_segment()` `push_video_frame` lambda 加 ~12 行 apply() 调用路径。
