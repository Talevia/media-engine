## 2026-04-23 — debt-test-reencode-cancel-mid-render：cancel API 的 CI tripwire（Milestone §M1-debt · Rubric §5.2）

**Context.** `me_render_cancel(job)`（`include/media_engine/render.h`）是 public C API，impl 在 `src/api/render.cpp:60` `job->job->cancel.store(true, memory_order_release)`——cooperative cancel pattern。worker 在 `reencode_segment.cpp:258` / `muxer_state.cpp` 的主循环顶部轮询 `shared.cancel->load(memory_order_acquire)`，命中就 `terminal = ME_E_CANCELLED; break`。一整套 cooperative cancel 合约全部没有 CI 覆盖：`grep -rn 'me_render_cancel\|cancel' tests/` 在本 cycle 开始前返回空。任何 tweak（cancel-check 频率、worker join 时机、双 cancel 的 race、cancel 后 progress callback 是不是发 FAILED、post-cancel wait 的 return 值）都会 silently regress。

**Decision.** 新 `tests/test_render_cancel.cpp`，3 个 TEST_CASE / 15 assertion：

1. **mid-render cancel 走 h264/aac reencode 路径**：
   - 3-clip 1s-each timeline（fixture_path × 3 背靠背 = 3s 目标产物）
   - h264/aac reencode（比 passthrough 耗时明显——初版用 passthrough 失败：2-clip 甚至 10-clip 的 stream-copy 都 < 200ms，cancel 之前 render 已完）
   - 主 thread `sleep(200ms) → cancel` 
   - `me_render_wait` 返回 `ME_E_CANCELLED`（非 `ME_OK`）
   - progress callback 捕获到 `ME_PROGRESS_STARTED` + 没看到 `ME_PROGRESS_COMPLETED` + 看到 `ME_PROGRESS_FAILED` 且 `.status == ME_E_CANCELLED`
   - 非 mac 上 videotoolbox 不可用 → wait 返回 `ME_E_UNSUPPORTED` / `ME_E_ENCODE`，打 `MESSAGE` 并 `return`（cancel 路径没真正 exercise）
2. **Idempotent double-cancel**：start render → sleep → cancel → 立刻再 cancel → 两次都返回 `ME_OK`（不是 error）。host 常见场景（用户双击取消 / cleanup path 重试）的契约。
3. **Null-arg rejection**：`me_render_cancel(nullptr) == ME_E_INVALID_ARG`。Guard API surface 层面的 null-safety。

**选用 h264/aac reencode 而非 passthrough 的原因.** 初版用 passthrough + 2-clip、10-clip、200ms 都 fail——stream-copy 在 dev 机上耗时 < 200ms，render 结束时 cancel 还没发出。Reencode 每帧都要 decode + sws + videotoolbox encode + AAC encode + mux，3-clip / 75 frames 很稳地超过 200ms。Side effect：这个 test 实际 exercise 的是 `process_segment` 里 的 cancel-check 路径（reencode-specific），passthrough 路径的 cancel-check（`muxer_state.cpp`）没被这个 suite 覆盖——但那条路径代码量极小（也是 `opts.cancel->load(...)`），未来如果真要覆盖加一个独立的 passthrough 大 fixture test case 便是。本 cycle 优先保证 reencode 路径的 cancel tripwire，passthrough 留给 future 不紧急。

**Progress event capture 的 thread-safety.** callback 在 engine 的 worker thread 执行（`docs/API.md` contract），主 thread 里 assertions。doctest assertions 非 thread-safe，所以 callback 只 `mutex + vector::push_back` snapshot events，然后主 thread `wait` 完后枚举 vector 走 CHECK。`me_progress_event_t` 里的 `message` / `output_path` 是 transient（lifetime 只在 callback scope 内），snapshot 时 zero 掉——本 suite 不 inspect 它们。

**CMake 排序坑（commit 发现的）.** 首版把 `add_dependencies(test_render_cancel determinism_fixture) + target_compile_definitions(... ME_TEST_FIXTURE_MP4=...)` 加在了 tests/CMakeLists.txt 的第 73-75 行——但 `_fixture_mp4` 变量直到第 97 行才 `set(...)`。结果 `${_fixture_mp4}` 展开成空字符串，test 里的 `fixture_path.empty()` 判断直接 `return`，ctest 报 pass 但只 1 个 assertion（其他 14 个根本没跑）。移到 `_fixture_mp4` 定义之后（在其他 `target_compile_definitions` 旁边）就修了。教训：CMake 变量是 top-to-bottom 解析，后置依赖要放后面。

**Alternatives considered.**

1. **用 passthrough + 更大的 fixture（10s+）**——拒：fixture 是 gen_fixture build-time 产物，放大会让每次 clean build 多花 ~秒；reencode 是既有 code path，复用更好。
2. **Start → 立刻 cancel（不 sleep）**——不可靠：worker 可能根本没到 cancel check 就 exit 了，race 不稳。
3. **加可观察的 "worker reached read-loop" signal**（e.g. condition variable 等 first `ME_PROGRESS_FRAMES` 事件）然后 cancel——更精确但复杂度上升；200ms sleep 在 reencode 路径足够稳定。
4. **拆成独立 `tests/test_render_cancel_passthrough.cpp` + `tests/test_render_cancel_reencode.cpp`**——拒：本 cycle 只测 reencode。passthrough 单独 suite 是 future work，加不加看后续有没有 cancel-specific regression。
5. **不测 progress callback，只测 wait 的 return**——拒：progress event 序列是 host UI 行为的核心契约（`debt-test-progress-callback-sequencing` 是下一个 P1 bullet，本 case 提前兼顾）。

业界共识来源：cooperative cancel test pattern 是 libcurl `CURLOPT_XFERINFOFUNCTION` + `CURLE_ABORTED_BY_CALLBACK`、Python 的 `asyncio.Task.cancel()` test、gRPC 的 cancellation tests 都同构——start → sleep past wire-up → cancel → wait → assert cancelled status + partial observable output。200ms 是业界 median sleep 值（gRPC 默认 250ms）。

**Coverage.**

- `cmake --build build` 与 `-Werror` clean。
- `ctest --test-dir build` 13/13 suite 绿（新 `test_render_cancel` 是第 13 个）。
- `build/tests/test_render_cancel` 3 case / 15 assertion / 0 fail（mac dev 跑真 videotoolbox 路径；Linux CI 走 MESSAGE + return skip 分支）。
- 不动 src/，其他 12 个 suite 继续绿。

**License impact.** 无。

**Registration.** 无 C API / schema / kernel 变更。
- `tests/test_render_cancel.cpp` 新 suite。
- `tests/CMakeLists.txt` 的 `_test_suites` 加 `test_render_cancel`；在 `_fixture_mp4` 定义之后加 `add_dependencies` + `target_compile_definitions`。
