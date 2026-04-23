## 2026-04-23 — debt-test-progress-callback-sequencing：progress event 合约 tripwire（Milestone §M1-debt · Rubric §5.2）

**Context.** `me_progress_event_t` 的 4 种 kind（STARTED / FRAMES / COMPLETED / FAILED，`include/media_engine/render.h:46-51`）定义了 host UI 与 engine 之间最重要的合约之一——STARTED 初始化 progress UI、FRAMES 推进它、exactly 一个 COMPLETED 或 FAILED 收尾。`src/orchestrator/exporter.cpp:134-180` 是合约的发射点：thread 启动时发 STARTED，sink-level `on_ratio` 在处理过程中发 FRAMES（`muxer_state.cpp:287`、`reencode_segment.cpp:287`），thread join 前按 `final_status` 发 COMPLETED 或 FAILED（注意：每条路径**各只发一次**，两条分支互斥）。

合约 surface 虽然 small，但历史上有两类 silent-regression 风险：

1. **Ordering 逆转**：refactor 时把 STARTED 搬到了 sink 初始化之后——于是 sink 有可能先发一次 FRAMES(0.0)（`muxer_state.cpp:168`、`reencode_pipeline.cpp:174`），host 看到 FRAMES 却没收到 STARTED，UI 状态机跳过初始化段。
2. **终结事件丢失**：早期版本有过 "成功路径忘了发 COMPLETED" 的 bug（某次 final_status 判断 early-return），host `wait` 返回 ME_OK 但 UI 没收到 COMPLETED → 无限 spinner。

`grep -rn 'ME_PROGRESS' tests/` 在本 cycle 之前只命中 `test_render_cancel.cpp`——而且只是 `saw_started = true` / `saw_completed = false` 这种**布尔采样式**检查，根本不是 **sequence** 断言（STARTED 在 FRAMES 之前？COMPLETED 确实在最后？ratio 真的单调？）。

**Decision.** 新 `tests/test_render_progress.cpp` + `tests/CMakeLists.txt`：3 个 TEST_CASE / 111 assertion（后者大头是 ratio monotonicity 的循环断言），把事件 shape 钉成 CI-enforced 合约：

1. **Normal-path sequence（passthrough 单 clip）**：
   - Cardinality：`started == 1 && completed == 1 && failed == 0`（FRAMES 个数不断言，不同 sink 粒度不同——`muxer_state` 每 packet 发一次，`reencode_segment` 每 video frame 发一次，具体数字不是合约）
   - Ordering：`events.front().kind == STARTED && events.back().kind == COMPLETED`（sink 的 0.0f / 1.0f 端点都在 front/back 之间，所以这两条 check 同时 pins down STARTED-before-any-FRAMES 和 COMPLETED-after-all-FRAMES）
   - `output_path` 在 COMPLETED 中 non-null 且等于 `spec.path`（回转合约；host 要从这里读产物路径）
   - Ratio monotonicity：对所有 FRAMES 事件按时间顺序遍历，每个 `ratio ∈ [0,1]` 且 `ratio >= last_ratio`（把 `muxer_state.cpp:282-287` 的每 packet 发送以及 `reencode_segment.cpp:280-287` 的 video-frame based ratio 计算合约化）
2. **Cancel-path sequence（3-clip h264/aac reencode）**：
   - Cardinality：`started == 1 && completed == 0 && failed_cancelled == 1 && failed_other == 0`（拒 FAILED 带非 CANCELLED 状态——若 cancel path 上因其他原因失败，那是 regression）
   - Ordering：`front == STARTED && back.kind == FAILED && back.status == ME_E_CANCELLED`
   - 复用 `test_render_cancel.cpp` 的 3-clip reencode + 200ms sleep pattern（见 2026-04-23-debt-test-reencode-cancel-mid-render 决策），在 videotoolbox 不可用（Linux CI / ME_E_UNSUPPORTED / ME_E_ENCODE）时 MESSAGE + return。
3. **Null-cb acceptance**：`me_render_start(eng, tl, &spec, nullptr, nullptr, &job)` → `ME_OK`，render 跑完，产物文件非空。Guard `exporter.cpp:94` 和 `:133-137` 的 `if (cb)` 前置检查——host 不 care progress 时（batch tool 直接跑到底那种场景）调用不该强制 callback。

**EventSnap 结构舍弃 `output_path` 指针缓存的理由.** `me_progress_event_t::output_path` 的 lifetime 是 callback scope（`render.h:56` comment）。callback 后主 thread 再 deref 是 UAF。于是 EventSnap 只存 `bool output_path_matched`——在 callback 内部当下比对 `ev->output_path == expected`，把**真正需要的那一位 observation** 持久化，其余丢弃。同理 `message` 不存（本 suite 不断言它）。

**与 test_render_cancel 的职责分工.**

- `test_render_cancel` = "cancel 机制本身"：`me_render_wait` 返回值、double-cancel 幂等、null-arg rejection。它顺带扫了 cancel 后 callback 会看到 FAILED+CANCELLED（但**布尔**检查，不是 sequence）。
- `test_render_progress` = "progress event 合约 shape"：ordering / cardinality / ratio / output_path。normal + cancel 两条路径都走一遍 strict sequence。

两者在 cancel-path 的 sequence assertion 上**故意重叠**——因为合约 surface 同一处 regression 会被两条测试同时捕到，冗余是 regression-catch 的特性而非 bug。如果未来觉得重复，删 test_render_cancel 里的 `saw_*` 三行 loose check 更合适（strict 在 test_render_progress 里），保留 test_render_cancel 只做 cancel 机制。本 cycle 不顺手做。

**Alternatives considered.**

1. **把断言塞进 test_render_cancel**——拒：test_render_cancel 的名字是"cancel 机制"，把 normal-path sequence 塞进去会让它 scope creep；新 suite 有独立 header 说清 intent。
2. **使用 `std::vector<me_progress_event_t>` 原生 copy**——拒：`output_path` / `message` 的 lifetime 是 callback scope，vector 保留指针就是 UAF。EventSnap 是 value-type，无悬垂。
3. **断言 FRAMES 事件**的具体计数（例如 "passthrough 1s clip 至少 N 次 FRAMES"）——拒：`muxer_state.cpp:282-287` 按 packet PTS emit，passthrough + bitexact 下 packet 数不稳定（与 B-frame order / GOP 分布相关，不是 stable invariant）。count 不是合约，ordering + monotonicity 才是。
4. **断言 ratio = 1.0f 出现在 COMPLETED 之前**——拒：`muxer_state.cpp:311` 的 `on_ratio(1.0f)` 只在 `terminal == ME_OK` 才发，已经隐含在 "COMPLETED 路径 last FRAMES 的 ratio 是 1.0"——但这是实现细节，VISION 没承诺。Ratio monotonicity + [0,1] 足够严格。
5. **用 `std::atomic<int>` counter 代替 mutex + vector**——拒：counter 能做 cardinality 但做不到 ordering。mutex + vector 是对合约建模最干净的。callback 本身 append 很轻（一次 mutex + push_back），不阻塞 render worker。
6. **Cancel-path sequence 另起新 suite**——拒：本 bullet 明确同时覆盖两条路径；分两 suite 会让"progress sequence 合约"分散。

业界共识来源：gRPC `cq.Next()` 事件序列测试、libcurl `CURLOPT_XFERINFOFUNCTION` 的 progress-callback 单调性合约、FFmpeg 的 `AVCodecContext::progress_cb` 设计都遵循"at-most-one STARTED，at-most-one terminal"。doctest 内多 TEST_CASE + thread-safe callback snapshot（mutex + vector + atomic<bool>）是 boost.test / catch2 社区推的成熟 pattern。

**Coverage.**

- `cmake --build build` 与 `-Werror` clean（修掉一处 `frames` 变量未用的 `-Wunused-but-set-variable`——把 FRAMES 计数从 switch 里拿掉）。
- `ctest --test-dir build` 14/14 suite 绿。
- `build/tests/test_render_progress` 3 case / 111 assertion / 0 fail（Normal 1 + Cancel 1 + Null-cb 1；111 个 assertion 主要来自 Normal 里 FRAMES ratio 的三重 CHECK 循环）。
- 其他 13 个 suite 继续绿。

**License impact.** 无。

**Registration.** 无 C API / schema / kernel 变更。
- `tests/test_render_progress.cpp` 新 suite。
- `tests/CMakeLists.txt` 的 `_test_suites` 加 `test_render_progress`；在 `_fixture_mp4` 定义之后加 `add_dependencies` + `target_compile_definitions`（沿袭 `test_render_cancel` 的 pattern，排序坑在上一 cycle 决策已记录）。
