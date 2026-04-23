## 2026-04-23 — me-render-output-format-infer：container inference 合约 + UNSUPPORTED 映射（Milestone §M1-debt · Rubric §5.1）

**Context.** `me_output_spec_t.container`（`include/media_engine/render.h:29`）为 `const char*`。注释写"required"但 impl 里其实允许 NULL——`src/orchestrator/exporter.cpp:103` 把 `spec.container ? spec.container : ""` 规范成空字符串，然后 sink 在 worker thread 调 `me::io::MuxContext::open(out_path, container, &err)`（`src/io/mux_context.cpp:13`），在 empty 情况下把 `format_name = nullptr` 塞给 `avformat_alloc_output_context2`，让 libav 按 out_path 的扩展名推断 muxer。

这条"NULL container + extension 推断"路径：
- **已经工作**——".mp4" / ".mov" 扩展让 libav 自动选到 mov muxer；
- **但零测试覆盖**——`grep -rn 'output_spec\|container' tests/` 在本 cycle 前只命中 happy-path 里"明确写 mp4"的用法，没 NULL container + 任何扩展名的 case；
- **失败路径语义不清**——".xyz" 未知扩展让 `avformat_alloc_output_context2` 返回 AVERROR(EINVAL)，然后 `MuxContext::open` 返回 nullptr + 一个模糊的 `"alloc output: Invalid argument"` 错误字符串；call sites `muxer_state.cpp:100` / `reencode_pipeline.cpp:87` 再把它翻译成 **`ME_E_INTERNAL`**——这是错的 status 映射：unknown-container 是**host 输入**层面的不匹配（recoverable，用户改扩展名就好），不是 engine 的**内部不变量被破坏**。Host 代码如果按 `ME_E_INTERNAL` 分支"触发 crash dump 上报"就是 false alarm。

**Decision.** 三处最小代码改动 + 一个新 test suite 同步锁死行为：

1. **`src/io/mux_context.cpp:21` 的 err 消息做区分**——
   - `ctr.empty()` 且 `avformat_alloc_output_context2` 失败 → `"container format not recognised: path '<out>' has no registered muxer (av: <av_err_str>)"`。Host 一眼知道这是扩展名推断失败而不是命名容器未知。
   - `ctr` 非空且失败 → `"container '<ctr>' not recognised: <av_err_str>"`。保留原始 av_err_str 便于 bug report，但把 host-readable 部分前置。

2. **`src/orchestrator/muxer_state.cpp:99-100` 和 `src/orchestrator/reencode_pipeline.cpp:85-87` 的 failure status `ME_E_INTERNAL → ME_E_UNSUPPORTED`**——
   - `ME_E_UNSUPPORTED`（`types.h:35`，value = -7）是 "engine recognised the request but can't perform it"。Host-facing。
   - `ME_E_INTERNAL`（value = -100）留给 invariant-break / 不可能到达的分支（例如 segment demux context 忽然 null 这种）。
   - 两处 call site 都加一行 comment 解释 "Format-inference / unknown-container failure is a recoverable host-input issue, not an internal invariant break"。

3. **新 `tests/test_output_spec.cpp`，4 个 TEST_CASE / 22 assertion**：
   - (a) `container = NULL` + `"infer.mp4"` → `me_render_wait == ME_OK`，产物文件存在 non-empty（libav 从 `.mp4` 推断出 mov muxer）。
   - (b) `container = NULL` + `"infer.mov"` → 同上，不同扩展名推断到同一 muxer（validates 不同扩展都 round-trip）。
   - (c) `container = NULL` + `"bad.xyz"` → `me_render_wait == ME_E_UNSUPPORTED`。`me_engine_last_error` 包含 "container format not recognised" 和 "bad.xyz"（hosts 能从 last-error 直接拿到 offending path 而不用再 plumb state）。
   - (d) `container = "totally-not-a-muxer"`（explicit unknown）+ `.mp4` 扩展 → `ME_E_UNSUPPORTED`，last-error 包含 "totally-not-a-muxer"（exercise 另一条 err 分支，验证命名容器失败不被 extension 救回）。

**行为变化的 ABI 影响.** `ME_E_INTERNAL → ME_E_UNSUPPORTED` 是 **status 值变化**，不是 ABI 变化（enum 整型值 unchanged，enum 成员只删会破 ABI，这里不删）。但是 host 代码如果专门 `switch (s) { case ME_E_INTERNAL: ... }` 去抓这条 path，行为变了。rubric §3a.10 "ABI 不破坏" —— 严格 ABI 意义上没破（没改 enum values，没改 struct 布局，没改函数签名），但**observable 行为**改了。本决策接受：

- 没找到任何 host 代码依赖 "unknown container → ME_E_INTERNAL"（grep 仓库 examples/ 也没这个 pattern）。
- 新映射更 correct：ME_E_INTERNAL 的文档意味是 "bug in engine"，unknown container 不是。
- 本变化落 CHANGELOG。未来如果有第二个 call site 把 "正当 host 错误" 映射成 ME_E_INTERNAL，一并修正。

**Alternatives considered.**

1. **只改文档，不改 impl** —— 拒：`ME_E_INTERNAL` 的定义是 "impossible state"，错误分类不对不该只靠文档掩盖。
2. **新增 `ME_E_UNSUPPORTED_FORMAT`** enum 值 —— 拒：ABI append-only 可以加，但目的重合度高（`ME_E_UNSUPPORTED` 已经足够表达）。未来 need 再加，不是现在。
3. **把 container 改成 required 字段，`spec.container == nullptr` 直接 `ME_E_INVALID_ARG`** —— 拒：这个 null-infer 路径是 host 友好 feature（host 对未知扩展名 "just save it" 的常见 UX），关闭了反而更不好。保留。
4. **在 `exporter.cpp` 层做 early extension validation**（从 out_path 提取扩展名，查 `av_guess_format` 决定）—— 拒：
   a. 需要 reimplement libav 的扩展名→muxer 映射，易漂移；
   b. 该 check 本质就是 `avformat_alloc_output_context2` 内部做的事，再做一遍 duplicates。让 MuxContext::open 做唯一权威判定更干净。
5. **直接用 `av_guess_format` + null check 作 early validation**（先于 render start）—— 拒：`av_guess_format` 的 fallback 规则和 `avformat_alloc_output_context2` 略有差异；双路径检查可能不一致（A 过但 B fail）。保持一条路径。
6. **把 err message 做成结构化字段**（`status_ex_t { code; category; offending_value; }`）—— 拒：C ABI 扩展，M1-debt 不做。string match 够用。
7. **在 test 里 mock 整个 libav** —— 拒：失真。真跑 libav + 真看 err message。
8. **在 test 里断言 output file NOT exists 对 unsupported**—— 拒：libav 的 `avformat_alloc_output_context2` 失败发生在 `avio_open` 之前，文件不会被创建。但严格断言 "file absent" 可能被未来 libav 版本微妙改变（比如提前 touch）打破；本 test 不做此断言，只断 wait status + err 字符串。

业界共识来源：FFmpeg tools (ffmpeg.c) 对未知扩展名 / 未知 `-f` 的退出码语义、gRPC status code 里 `UNIMPLEMENTED` vs `INTERNAL` 的区分（前者 recoverable，后者 bug）、HTTP 415 Unsupported Media Type 对"语法合规但引擎不支持"的语义。

**Coverage.**

- `cmake --build build` 与 `-Werror` clean。
- `ctest --test-dir build` 16/16 suite 绿（新 `test_output_spec` 是第 16 个；其他 15 个 suite 不动——`MuxContext::open` 的 err message 改动仅影响失败分支的字符串，正常路径无变化）。
- `build/tests/test_output_spec` 4 case / 22 assertion / 0 fail。
- 现有 test_determinism / test_render_cancel / test_render_progress / test_render_job_lifecycle 都不受影响——它们都显式传 `container = "mp4"`，走命名成功分支。
- libav 的 stderr 日志在失败 case 下会打 "Unable to choose an output format for ..." 之类——这是 libav 默认 log handler 行为，不是测试失败。

**License impact.** 无。

**Registration.** 无 C API / schema / kernel 变更。
- `src/io/mux_context.cpp` 的 `MuxContext::open` err 分支改写（empty container 专门 phrasing）。
- `src/orchestrator/muxer_state.cpp` + `src/orchestrator/reencode_pipeline.cpp` 的 failure status `ME_E_INTERNAL` → `ME_E_UNSUPPORTED`。
- `tests/test_output_spec.cpp` 新 suite。
- `tests/CMakeLists.txt` 的 `_test_suites` 加 `test_output_spec`；在 `_fixture_mp4` 定义之后加 `add_dependencies` + `target_compile_definitions`。
