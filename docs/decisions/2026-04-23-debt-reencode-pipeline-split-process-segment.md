## 2026-04-23 — debt-reencode-pipeline-split-process-segment：把 process_segment + SharedEncState 挪到自己的 TU（Milestone §M1-debt · Rubric §5.2）

**Context.** `src/orchestrator/reencode_pipeline.cpp` 在前两个 cycle（`reencode-multi-clip` + `debt-split-reencode-pipeline-audio-fifo`）之后还 523 行。剩余的体量集中在三块：

- ~30 行：`SharedEncState` struct + `video_params_compatible / audio_params_compatible` 两个 helper。
- ~20 行：`total_output_us()` —— 计算 progress ratio 的 total microseconds。
- ~210 行：`process_segment()` —— per-clip 打开 decoder、seek、read-decode-encode 循环、段末 flush decoder 的全部逻辑。

这三块**内部**强耦合（SharedEncState 只有 process_segment 读写，两个 compat helper 只被 process_segment 用），**外部**只通过 `reencode_mux` 的最后一层框架调用。典型"模块内部有大量相互依赖但 API 边界干净"的抽取候选：挪到自己的 TU 后，`reencode_pipeline.cpp` 只剩"open mux + open encoder + for-each segment + flush encoder + write trailer" 的 orchestration。

**Decision.** 新 `src/orchestrator/reencode_segment.{hpp,cpp}`：

- **header** 暴露：`detail::SharedEncState` struct、`detail::open_decoder()`（pipeline 和 process_segment 都要）、`detail::total_output_us()`、`detail::process_segment()`。注释写清楚跨段 PTS 连续性的契约、compat check 的 rationale、encoder 不在 segment 末 flush 的 why。
- **.cpp** 吸收 process_segment 的全部实现 + 两个 compat helper（file-local）+ `best_stream()` file-local helper + `open_decoder()` TU 级定义 + `total_output_us()` 实现。

`reencode_pipeline.cpp` 砍到 199 行（-324），只留：
- `reencode_mux()` 函数签名 / 各种 `if (xxx != "h264") return fail(...)` 的 codec 校验。
- segment[0] decoder 开（通过 `detail::open_decoder`）做 encoder param-sniffing。
- `MuxContext::open`、`avformat_new_stream`、`open_video_encoder / open_audio_encoder`、FIFO 分配、SharedEncState 填充。
- `for (i...)` 调 `detail::process_segment(..., shared, i, err)`。
- 段后一次性 flush shared encoder（video null-frame send + `drain_audio_fifo(..., flush=true)` + `encode_audio_frame(nullptr, ...)`）。
- `write_trailer`、progress 收尾。

**open_decoder 双用路径.** `reencode_mux` 需要在开 encoder **之前**对 segment[0] 的 streams 开一次临时 decoder 来拿 params（width/height/pix_fmt/sample_rate/channels），之后立刻 `reset()` 释放——`process_segment(0)` 会**重新**开同一套 decoder 正式处理 segment[0]。这条 "decoder 开两次" 看起来浪费，实际 `avcodec_open2` 是 ms 级操作，只在 segment[0] 发生一次，且 decoder state 本身不跨这一对 close/open 携带语义。相反如果要避免 double-open，就得把 decoder handle 从 `reencode_mux` 传到 `process_segment`，对 process_segment 的 self-containment 是损失；加上 segment[0] decoder 正好被 codec-compat check 的 reference point 用，self-open 更干净。

**行数变化.** 
- `reencode_pipeline.cpp`: 523 → **199** (-324 lines / -62%)
- `reencode_segment.cpp`: 0 → **308** (new)
- `reencode_segment.hpp`: 0 → **115** (new)
- Net +99 行，但关注点彻底分离：pipeline 层做 orchestration / mux setup / encoder flush 的 "once-per-job" 工作，segment 层做 decoder lifecycle / swr / sws / PTS 累积 的 "per-clip" 工作。

下一个文件级 debt 监控点：`reencode_segment.cpp` 308 行仍在 scan-debt.sh 的 "<= 400, not flagged" 区间。未来加 `reencode-multi-clip` 的 follow-up（不同 segments 带不同 trim / 色彩、fade-in/out、variable-bitrate 切换）时可能再涨——那时视情况把 `push_video_frame` / `push_audio_frame` 的 lambda 再抽出去。现在没必要抢跑。

**Alternatives considered.**

1. **把 `SharedEncState` 放 `.cpp` file-local，header 只 forward-declare**——拒：`reencode_pipeline.cpp` 的 `reencode_mux` 要构造并 mutate SharedEncState 几十个字段，header 必须 export struct definition。
2. **把 `SharedEncState` 改成 class + public getter/setter**——拒：纯数据结构 + 朋友函数（process_segment / reencode_mux）直接 mutate 是最直观的形态，加 class 外壳反而增加 getter/setter 样板。C++ plain struct 是仓库里的同类 pattern 惯例（`ReencodeOptions`、`SinkCommon`、`ClipTimeRange`）。
3. **把 `reencode_mux` 一起挪进 `reencode_segment.cpp`，保留单 TU**——拒：就是回到起点；本 cycle 目的就是把 pipeline / segment 两个关注点分开。
4. **把 `open_decoder` 放回 `reencode_pipeline.cpp`，`process_segment` 重复定义一个 file-local 副本**——拒：本来就是一个函数，复制反而是 debt。暴露为 `detail::open_decoder` 清楚表示"两个 TU 共享的 helper"。
5. **把 `process_segment` 再拆成 `open_segment_decoders` + `drive_segment_loop` + `flush_segment_decoders` 三个函数**——考虑过，暂缓：每个函数会需要 `SharedEncState&` + `SwrPtr&` + `SwsPtr&` + 一堆临时状态的 call signature，call site 从一行变三行，readability 反而下降。等真有第二个 consumer（eg M4 audio-only reencode 可能需要只跑 audio 流程）再拆。

业界共识来源：把 "run-per-input" loop 逻辑和 "setup + teardown" 的 "run-once" 逻辑分成两个 TU 是 FFmpeg `fftools/ffmpeg.c` 和 `fftools/ffmpeg_mux.c` 的划分模式——前者是 per-input 的 demux/decode driver，后者是 mux lifecycle。本 cycle 的分法在思路上同构。

**Coverage.**

- `cmake --build build` 与 `-Werror` clean（静态库 + 6 examples + 10 tests 全部 rebuild）。
- `ctest --test-dir build` 10/10 suite 绿。
- **Multi-clip 回归**：`/tmp/me-multi.timeline.json`（两段 1s each of `/tmp/input.mp4`）→ 产物 60 h264 frames + 89 aac frames + 2.04s duration。**和 refactor 前 byte-identical**（BITEXACT flags + determinism tripwire 所以"byte-identical" 是可断言的）。
- 单 clip 回归隐式通过 `test_determinism` 的 h264/aac case（`debt-render-bitexact-flags` cycle 的 tripwire）——ctest 绿即通过。

**License impact.** 无新依赖。纯 C++ 内部重组。

**Registration.** 无 C API / schema / kernel 变更。
- `src/CMakeLists.txt` 的 `media_engine` target source 列表新增 `orchestrator/reencode_segment.cpp`。
- `src/orchestrator/reencode_segment.hpp` 新头（internal，不 export）。
- `src/orchestrator/reencode_pipeline.cpp` 内容大幅缩减但 `reencode_mux()` 公开签名不变。
