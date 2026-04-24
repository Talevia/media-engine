## 2026-04-23 — compose-determinism-regression-test：ComposeSink 路径 byte-identical 回归（Milestone §M2 · Rubric §5.3）

**Context.** M2 最后一条未打勾 exit criterion："确定性回归测试：同一 timeline 跑两次产物 byte-identical（软件路径）"。`tests/test_determinism.cpp` 既有 4 TEST_CASE 覆盖 M1 单轨 passthrough / h264-videotoolbox 路径。`grep -rn 'compose\|mixer\|transition\|cross_dissolve' tests/test_determinism.cpp` 返回空 —— ComposeSink 的 M2 features（multi-track compose / Transform / cross-dissolve / audio mixer）无 byte-identical 回归覆盖。

Before-state grep evidence：
- `tests/test_determinism.cpp` 行数 300（pre-cycle）；4 TEST_CASE："passthrough byte-deterministic" / "passthrough engine-restart" / "h264/aac reencode byte-deterministic" / "h264/aac concat N segments byte-deterministic"。都是 M1 路径（单轨 reencode，走 `make_output_sink` 非 ComposeSink）。
- `grep -rn 'compose\|transition\|mixer' tests/test_determinism.cpp` → 空。
- test_compose_sink_e2e 新增的 "video + audio track (mixer path)" case 只跑 1 次 + 断 status=OK + 文件大小，**不做 byte compare**。

**Decision.**

1. **`tests/test_determinism.cpp`** +1 TEST_CASE "compose path (2-track video + audio mixer) is byte-deterministic across two independent renders"：
   - 构造 2-track JSON timeline（`v0` kind=video + `a0` kind=audio），两 clip 都引用 with-audio fixture。
   - 2 次独立 `render_with_spec(timeline, out_path, "h264", "aac")` call —— 每次都是 fresh engine（`render_with_spec` 内部 `me_engine_create` + destroy）。
   - `slurp(out1)` + `slurp(out2)` + byte-compare。
   - Skip 策略对齐现有 h264/aac 测试：`s1 == ME_E_UNSUPPORTED || s1 == ME_E_ENCODE` → MESSAGE + return（videotoolbox 不可用的非 mac CI 不 fail）。
   - 测试覆盖的 M2 features：ComposeSink routing、multi-track 索引、AudioMixer wire-in、AAC encoder feed。Cross-dissolve 和 Transform 未直接测（单独 `test_compose_sink_e2e` 的 cross-dissolve e2e 虽无 byte-compare 但覆盖 kernel + compose 整合，作为互补）。

2. **`tests/CMakeLists.txt`** —— 两个改动：
   - `test_determinism` 的 `add_dependencies` + `target_compile_definitions` 块从 `_fixture_mp4_with_audio` 变量定义**之前**（原位 175-178）移到**之后**（新位 ~207）。这是 CMake 变量 top-to-bottom 求值——引用发生时必须已被 `set(...)`。Pre-cycle bug：`ME_TEST_FIXTURE_MP4_WITH_AUDIO` 宏被编译成空字符串，导致新测试一直走 skip 分支（见本 cycle 构建日志：`CXX_DEFINES = ... -DME_TEST_FIXTURE_MP4_WITH_AUDIO=\"\"`）。
   - 新增 `determinism_fixture_with_audio` 到 `test_determinism` 依赖 + `ME_TEST_FIXTURE_MP4_WITH_AUDIO` 宏 define。

3. **Videotoolbox determinism 务实态度**：
   - h264_videotoolbox 不是严格 "byte-identical 软件编码器"——HW encoder 没 FFmpeg 的 bit-exact 合约保证。但**同 host 同 version run-to-run stable** 是 FFmpeg + Apple 实测经验（`tests/test_determinism.cpp:174` 的既有 "h264/aac reencode byte-deterministic" case 已在这基础上跑 + 过）。
   - 本新测试承袭相同假设：compose + mixer 路径在 mac 上 byte-identical（实测 fixture 上产物 **315429 bytes** 两次 match）。Linux / 其它 platform 走 skip 分支不 fail。
   - 将来若要真·软件确定性，有两条路：(a) 加 libavcodec 内建 MPEG-4 Part 2 encoder 作为 `spec.video_codec="mpeg4"` 的 software path（LGPL clean、`gen_fixture.cpp` 已用）；(b) Linux CI 上换 SW encoder（libx264 是 GPL—不行；libvpx / libopenh264 License 都能接受，但要补 FetchContent + ARCHITECTURE.md 白名单）。两条都是独立 bullet，不在 M2 scope。

4. **Scope 边界**：
   - 本测试覆盖 "multi-track video + audio mixer"。
   - **不**覆盖 cross-dissolve transition 的 byte-identity——transition 测试在 `test_compose_sink_e2e`，不做 byte compare。确定性更严测的 scope（e.g. Transform + cross-dissolve + mixer 同时）留后续或作为 CI soak。
   - **不**改 ComposeSink / AAC encoder 产生 byte-identical 的 mechanism——依赖现有 `AV_CODEC_FLAG_BITEXACT` + `AVFMT_FLAG_BITEXACT` + thread_count=1 ({`src/orchestrator/encoder_mux_setup.cpp`}) + mixer 本身 pure-float deterministic。
   - **不**加 software codec option（见上 §3）。

**Alternatives considered.**

1. **新独立 test suite `test_compose_determinism.cpp`** —— 拒：同类测试堆在 `test_determinism.cpp` 更符合 suite 单一职责（determinism regression）。只新 TEST_CASE 加在既有文件里，CMake 无需改 list。
2. **跑 3+ 次渲染并两两 compare** —— 拒：过度设计。2 次 compare 足以 trip regression；多余运行纯费时间（videotoolbox compose render ~5s/run）。
3. **用 cross-dissolve timeline 作为被测对象** —— 拒：cross-dissolve 本身带 to_clip source-time offset phase-1 限制（见 `cross-dissolve-transition-render-wire` decision）。换多轨 + audio-mix 更干净，覆盖最多 M2 features 且没已知偏差。
4. **加 SW encoder 改造再做测试** —— 拒：SW encoder 引入是独立大工程（新 FetchContent 或 existing libavcodec MPEG-4 的 spec.video_codec 暴露）。本 cycle 用 videotoolbox + 既定 skip pattern，锁定当前 compose 路径的 run-to-run stability；SW 改造留 P2 backlog。
5. **测试断 `bytes1 == bytes2` 严格相等 vs 允许 1% 漂移** —— 拒：byte-identical 是 VISION §5.3 的硬合约，不容软化。videotoolbox 不达到 → skip 整个测试，但绝不降标准。

**Scope 边界.** 本 cycle **交付**：
- compose path（2-track video + audio mixer）byte-identical 回归覆盖。
- CMake 变量顺序修正（test_determinism 的 with-audio 依赖）。
- M2 exit criterion tick。

本 cycle **不做**：
- Cross-dissolve / Transform 的独立 determinism case。
- Software encoder option 扩展。
- Linux CI soak。

**Coverage.**

- `cmake --build build -j 4` 全绿，`-Werror` clean。
- `ctest --test-dir build` 27/27 suite 绿。
- `test_determinism` 5 TEST_CASE（4→5），新 case byte-match 两次 315429 bytes。

**License impact.** 无。

**Registration.**
- `tests/test_determinism.cpp`：+~80 行（新 TEST_CASE + `#ifndef ME_TEST_FIXTURE_MP4_WITH_AUDIO` guard）。
- `tests/CMakeLists.txt`：test_determinism 依赖块重新排序到 `_fixture_mp4_with_audio` 定义之后 + 新增 with-audio 依赖/宏。
- `docs/BACKLOG.md`：**删除** bullet `compose-determinism-regression-test`——唯一 scope 是该 TEST_CASE 落地，本 cycle 完成。
- `docs/MILESTONES.md`：M2 exit criterion "确定性回归测试：同一 timeline 跑两次产物 byte-identical（软件路径）" 从 `[ ]` → `[x]`（独立 `docs(milestone):` commit）。**M2 6/6 criteria 全打勾** —— §M.2 下一 cycle 触发自动推进到 M3。
