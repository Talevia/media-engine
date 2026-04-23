## 2026-04-23 — debt-test-render-job-lifecycle：destroy 边界合约 tripwire（Milestone §M1-debt · Rubric §5.2）

**Context.** `me_render_job_destroy(job)`（`include/media_engine/render.h:85`）的 impl 在 `src/api/render.cpp:78-82`：

```cpp
if (!job) return;
if (job->job && job->job->worker.joinable()) job->job->worker.join();
delete job;
```

合约的**两个隐形承诺**：

1. **destroy(NULL) 是 no-op**——`render.h:67` 注释写 "Caller owns out_job; call me_render_job_destroy after the job reaches a terminal state"，没明说 NULL 行为，但 `if (!job) return` 的 impl 事实上允许。Host shutdown 代码一般写成"destroy + null-out"模式然后在 cleanup loop 里可能 re-destroy 已经被 null 过的 handle，该 pattern 必须是 safe 的。
2. **destroy 隐式 join worker**——不用先 `me_render_wait` 就可以 destroy。Host 收到 cancel 后"转身走人"时，只要 destroy 实现里调 `worker.join()` 就不会撞到 `std::thread` 析构时 `joinable()==true` 的 `std::terminate`。

`grep -rn 'me_render_job_destroy' tests/` 在本 cycle 之前命中 4 处：`test_determinism.cpp:57` 和另外三个 suite 的 RAII handle dtor——**全都是**"start → wait → destroy"标准路径。destroy(NULL) 和 destroy-without-wait 两种边角完全没 CI 覆盖。将来有人把 `worker.join()` 从 destroy 里拆成独立 `me_render_job_release()`（合理的 API 演化想法），现有 suite 全绿，但 host 代码会在 `delete` 时撞上 `std::terminate`。

**Decision.** 新 `tests/test_render_job_lifecycle.cpp` + `tests/CMakeLists.txt`：4 个 TEST_CASE / 18 assertion，把四条 destroy 合约钉成 CI tripwire：

1. **destroy(NULL) 幂等**：连续两次 `me_render_job_destroy(nullptr)` 都是 no-op。一次 call 已经足够 pin down `if (!job) return` guard；两次 call 更贴近 host shutdown-pass 的实际用法（destroy + null + possibly destroy again）。
2. **start → destroy (无 wait)**：passthrough 路径，不 call `me_render_wait` 就直接 destroy。destroy 必须内部 `worker.join()`——否则 `std::thread` 析构时 `joinable()==true` 就 `std::terminate()`，测试进程崩溃。"absence of crash is the assertion"。辅证：destroy 返回后产物文件存在且非空，这个观察 pins down "destroy 真的等到 worker 跑完再返回"（而不是假 return 然后 delete，留下悬垂 join）。
3. **start → cancel → destroy (无 wait)**：host 最常见的"cancel 按钮后就走人"模式。3-clip h264/aac reencode（跟 test_render_cancel / test_render_progress 同 fixture），200ms sleep，cancel 后直接 destroy 不 wait。Destroy 必须快速返回（cancel flag 让 worker 自然 exit，join 不会 block 很久）。CHECK `elapsed < 30s` 是 CI-safe 上限，不是 perf 断言——旨在捕获"worker 没收到 cancel 信号从而 destroy 等完整 render 完"这种 regression。非-mac（videotoolbox 不可用）也走这条 case：encoder 打不开时 worker 迅速 final_status=ME_E_ENCODE 退出，destroy 同样马上返回，cancel-destroy 路径的 crash 检测仍然有效。
4. **标准 cycle + NULL shutdown**：start → wait → destroy → `job = nullptr` → destroy(NULL) again。模拟 host 的 cleanup-pass 对已销毁 handle 再跑一次 destroy 仍然安全。这个 case 合并了 bullet 原本的 "(b) start → wait → destroy → 再 destroy null ptr → no crash"。

**为什么没测 "destroy(same_ptr) 两次".** C API 的"destroy(ptr)→destroy(ptr)"是经典 use-after-free，UB。这不是合约允许的行为，不应该测（测出"不崩"反而会误导 host 以为可以这么用）。正确的合约是 **destroy(NULL) 可多次**——host 应该 null-out 指针后才能再 destroy，case 4 按这个合约写。bullet 原文 "destroy-twice（host bug 的防御）" 理解成 "防御 host 忘了 null 就再调 destroy" 会走到 UB 领域，不对；理解成 "destroy→null→再 destroy(NULL)" 是合约允许的，这才是案 4。

**为什么不用 ASAN 证"no leak".** 仓库默认不 ASAN，本 cycle 不引入构建选项。Case 2 / 3 的 "no crash + worker 确实 join 了" 已经覆盖了 destroy 实现的核心风险点：thread 析构 terminate / worker 持有的 sink/codec 资源泄漏两大类都会在 `worker.join()` 缺失时立即反映成测试崩溃或超时，不需要 ASAN 做增量覆盖。真要跑 ASAN 时 `cmake -DCMAKE_CXX_FLAGS=-fsanitize=address -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address` 一把覆盖 15 个 suite 都得到加持，这个 debt（`debt-ci-asan-config`）留给未来 repopulate。

**Alternatives considered.**

1. **塞进 test_determinism / test_render_cancel**——拒：destroy 合约是 API surface 自己的 contract，独立 suite 让搜索"`destroy`"能立即命中该 suite 查到正文；塞进 determinism 会让 `cancel → destroy` 和 `wait → destroy` 混进"决定性"的命名语义里。
2. **加 `std::atomic<bool> worker_done` 显式 join 证明**——拒：依赖实现内部 flag 等于 whitebox 测试，抽象层打破。product fs::exists + file_size > 0 的 blackbox observation 同样 pin down "worker 跑完了"，而且是 host 也能做的断言形式。
3. **destroy-during-running-render 多线程并发**（线程 A `destroy(job)`，线程 B 还 holds reference）——拒：`render.h:74` 明确 "Jobs are not thread-safe — one caller owns the job"。这是合约禁止的用法，不测。
4. **测 destroy 后 `me_render_wait(job)` / `me_render_cancel(job)` 不崩**——拒：destroy 后 handle 是 free memory，任何 deref 都是 UB。合约是 host 必须 null 掉指针，不能再 call。
5. **cancel-then-destroy 断言 `elapsed < 500ms`**（更严格的 perf bound）——拒：CI hardware 差异大，跑在 qemu 上可能就是超 500ms。30s 是"肉眼 hang 了"的阈值，够用。

业界共识来源：POSIX `pthread_join` 合约、FFmpeg `avcodec_free_context(&ctx)` 允许 `ctx == NULL`（`avcodec.h` 注释明写）、SDL3 `SDL_DestroyRenderer(NULL)` 是 no-op、libcurl `curl_easy_cleanup(NULL)` 是 no-op 的统一做法。C 库"destroy/free 允许 NULL"是 40 年来的稳定模式，本仓库遵循。

**Coverage.**

- `cmake --build build` 与 `-Werror` clean。
- `ctest --test-dir build` 15/15 suite 绿（新 `test_render_job_lifecycle` 是第 15 个）。
- `build/tests/test_render_job_lifecycle` 4 case / 18 assertion / 0 fail，总耗时 ~0.4s。
- 其他 14 个 suite 继续绿。

**License impact.** 无。

**Registration.** 无 C API / schema / kernel 变更。
- `tests/test_render_job_lifecycle.cpp` 新 suite。
- `tests/CMakeLists.txt` 的 `_test_suites` 加 `test_render_job_lifecycle`；在 `_fixture_mp4` 定义之后加 `add_dependencies` + `target_compile_definitions`（沿袭前两个 cycle 的 pattern）。
