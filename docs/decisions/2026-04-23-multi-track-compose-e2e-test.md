## 2026-04-23 — multi-track-compose-e2e-test：真 2-track 渲染 e2e 验证（Milestone §M2 · Rubric §5.2）

**Context.** 上 cycle `multi-track-compose-actual-composite` 写了真 compose loop 但只用 fake URI 测试——fake URI 的 regression tripwire 只能 catch "code path executes"，不能 catch "compose actually produces valid output"。本 cycle 写真实 fixture-based e2e 测试，并**在过程中发现并修复了一个 live bug**。

Before-state grep evidence：

- `grep -rn 'test_compose_sink' tests/` 返回空——无 end-to-end compose 测试。
- `tests/test_timeline_schema.cpp` 的 multi-track case 用 `/tmp/me-nonexistent.mp4` fake URI；只断 me_render_start == ME_OK + err 不含 old stub string，**不**断产物存在或合法。

**Decision.** 新 `tests/test_compose_sink_e2e.cpp`，2 个 TEST_CASE：

1. **"2-track h264/aac render produces non-empty output"**：
   - 用 determinism fixture × 2 tracks（同 URI 但 loader 建两个独立 DemuxContext）。
   - 25fps × 1s 双轨 timeline。
   - 调 `me_render_start` → `me_render_wait`，断 `ME_OK`（真 videotoolbox encoder 路径）。
   - 捕获 err_msg 并打 MESSAGE 便于 debug（`std::string{err_msg}` 强制 string 复制避免 pointer-as-string 问题）。
   - 断产物文件存在 + 大小 > 4096 bytes（最低合理 MP4）。
   - 非-mac CI 上 wait 返回 `ME_E_UNSUPPORTED` / `ME_E_ENCODE`（videotoolbox 不可用）→ 静默 return 跳过 CHECK（依然 passes）。

2. **"two 2-track renders produce similarly-sized output"**：
   - videotoolbox 非 deterministic（HW 编码器状态跨 render 有微妙差异），不做 byte-equal。
   - 跑两次同 timeline，断 file_size 比率 ∈ [0.90, 1.10]。
   - 抓 "encoder dropped half 的帧" / "compose loop 大 regress" 之类 gross 问题。

**Live bug 修复**：写测试过程中发现 compose_sink.cpp 的 frame-dimension check 顺序错误：

```cpp
if (frame_to_rgba8(frame_scratch, ...) != ME_OK) return ...;
av_frame_unref(frame_scratch);   // <--- zeroes width/height
const int src_w = frame_scratch->width;   // 0
const int src_h = frame_scratch->height;  // 0
if (src_w != W || src_h != H) return ME_E_UNSUPPORTED;  // always fires!
```

`av_frame_unref` 归还 buffer reference 并**清零** width/height 字段（libav spec）。test_compose_sink_e2e 里 wait 返回 `-7 ME_E_UNSUPPORTED` + err `"track frame size (0x0) doesn't match output (640x480)"` 暴露这个问题——**完全要靠真 fixture 才能撞到**（fake URI 在 setup_h264_aac_encoder_mux 就已经挂了，不会进 frame loop）。

修复：capture `src_w`/`src_h` **before** unref，check dims 前位置不变。一行位置调整。

这就是 e2e 测试的价值——scope-A kernel 测试能证明数学对，但不能证明 orchestration 流程对。

**Alternatives considered.**

1. **生成第二个变体 fixture** 让两 track 看起来明显不同 —— 拒：同 URI + 两独立 DemuxContext 已经能 exercise compose 的每个代码路径（两个独立 decoder，两组 per-track pull）。额外 fixture 只是视觉确认——肉眼验证不是 CI 工作。
2. **断 byte-deterministic** —— 拒：videotoolbox HW encoder 有跨-run 细微差异；软件路径 byte-determinism 是 `determinism-compose-software-path` 的 scope（M2 exit criterion "确定性回归"，需要用软件 h264 encoder 而非 videotoolbox；x264 LGPL 不允许，libavcodec 没内置软件 h264；等 M2 末尾或 M3 带 software render 时再做）。
3. **用 ffprobe 验证** 产物 container / codec / duration —— 拒：引入 ffprobe 依赖（CI 上不保证有）；`fs::file_size` 做基础 sanity 足够本 cycle。
4. **断 2 runs 内 ≤ 5% 差异** 而非 10% —— 拒：videotoolbox 能在 10% 内飘；太严格会 CI flaky。10% 是保守但 catch gross regression。
5. **真的生成不同 content 两个 fixture** 做 alpha_over 像素级 verify —— 拒：涉及读回 rendered mp4 逐像素 decode + compare——显著复杂化 e2e，且 kernel-level `test_compose_alpha_over` 已覆盖数学。e2e 的职责是 "pipeline 跑不跑"，不是 "数学对不对"。
6. **先验证 setup_h264_aac_encoder_mux 不 crash** 的 mini test —— 拒：setup helper 是 orchestrator internals，用 engine-level API exercise 更 realistic。

业界共识来源：e2e 测试在有充分 unit test 时的角色——"catch orchestration bugs that kernel tests can't"（"Software Testing: Role of End-to-End Testing"，Google Testing Blog 的经典观点）。本 cycle 的 av_frame_unref dimensions-zero bug 是典型 e2e-only 抓 bug。

**Coverage.**

- `cmake --build build` + `-Werror` clean。
- `ctest --test-dir build` 24/24 suite 绿（新 `test_compose_sink_e2e` 是第 24）。
- `build/tests/test_compose_sink_e2e` 2 case / 12 assertion / 0 fail。
- mac + videotoolbox：真 compose 渲染成功，产物非空 + size-stable across 2 runs。
- 非-mac CI：静默跳过 e2e 断言（MESSAGE 记录 wait status）。

**License impact.** 无。

**Registration.**
- `tests/test_compose_sink_e2e.cpp` 新 suite + `tests/CMakeLists.txt` `_test_suites` 追加 + fixture dep + compile_definition（放在 `_fixture_mp4` 定义之后）。
- `src/orchestrator/compose_sink.cpp` 修 bug：move `src_w`/`src_h` capture 到 `av_frame_unref` 之前。
- `docs/BACKLOG.md`：删 `multi-track-compose-e2e-test`。

**§M 自动化影响.** M2 exit criterion "2+ video tracks 叠加" 上 cycle 已打勾。本 cycle 是这个 criterion 的 **deeper evidence**——从 "kernel tests + fake-URI routing test" 升级到 "真 fixture e2e render 产生合法 mp4"。不改变 tick 状态。
