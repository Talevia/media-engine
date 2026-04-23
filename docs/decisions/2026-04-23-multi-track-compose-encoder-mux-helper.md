## 2026-04-23 — multi-track-compose-encoder-mux-helper（scope-A of actual-composite：encoder+mux setup 抽出独立 helper）（Milestone §M2 · Rubric §5.2）

**Context.** `multi-track-compose-actual-composite` 的 bullet direction 第一条就是 "mirror `reencode_pipeline::reencode_mux` 的 encoder + mux setup block（共享 `SharedEncState` / audio FIFO 等）"——而且剩余两条 P1 (`cross-dissolve-transition-wire`、`audio-mix-scheduler-wire`) 都需要同样的 h264/aac encoder + MP4 mux setup。如果不抽成 helper，每个后续 cycle 都得复制 ~95 LOC 的样板。

本 cycle scope-A：纯 refactor 抽 `setup_h264_aac_encoder_mux` 到独立 TU。零行为变化。

Before-state grep evidence：

- `reencode_pipeline.cpp:65-158`（本 cycle 前）~95 LOC 的 inline setup：`open_decoder(ifmt0 video)` + `open_decoder(ifmt0 audio)` + `MuxContext::open` + `SharedEncState` 初始化 + `avformat_new_stream` 两次 + `open_video_encoder` / `open_audio_encoder` + `avcodec_parameters_from_context` + `av_audio_fifo_alloc`。
- `grep -rn 'setup_.*encoder.*mux\|EncoderMux' src/` 返回空——没有可复用 helper。
- ComposeSink 上 cycle 写 delegation 复用了整个 `reencode_mux`（即间接使用了 setup block），但真 compose loop cycle 需要 finer-grained access——不能走 delegation，需要直接调 setup 之后自己管 per-frame loop。

**Decision.** 新 `src/orchestrator/encoder_mux_setup.{hpp,cpp}`，一个 free function：

```cpp
me_status_t setup_h264_aac_encoder_mux(
    const ReencodeOptions&               opts,
    AVFormatContext*                     sample_demux,
    std::unique_ptr<me::io::MuxContext>& out_mux,
    me::resource::CodecPool::Ptr&        out_venc,
    me::resource::CodecPool::Ptr&        out_aenc,
    detail::SharedEncState&              out_shared,
    std::string*                         err);
```

Ownership: caller 声明 `mux` / `venc` / `aenc` 作 local `unique_ptr` / `CodecCtxPtr`，function 填它们。`shared` 是 caller stack 上的 value，populate in-place。afifo 依然是 raw pointer in `shared.afifo`；caller 用自己的 FifoGuard RAII 做 cleanup（和现有 reencode_mux 保持一致，避免改动 SharedEncState 结构）。

Body 是 `reencode_pipeline.cpp` 现行 lines 65-158 逐行搬过去：
- vsi0 / asi0 = `best_stream(ifmt0, TYPE)`；两者都 < 0 → `ME_E_INVALID_ARG`。
- open sample decoders via `detail::open_decoder` for video / audio。
- `MuxContext::open(opts.out_path, opts.container, &open_err)`; nullptr → `ME_E_UNSUPPORTED`。
- `out_shared` zero-init 后填：ofmt, cancel, on_ratio, total_us, color_pipeline, target_color_space。
- 若 v0dec：`avformat_new_stream` + `open_video_encoder` + `params_from_context` + 填 shared.venc/v_width/v_height/v_pix/venc_pix/out_vidx/video_pts_delta（用 `av_guess_frame_rate` 做 CFR delta，fallback 25/1 避 0 延迟）。
- 若 a0dec：同构 audio 路径，外加 `av_audio_fifo_alloc` 给 `shared.afifo`。
- v0dec / a0dec reset（sample decoders 用完即丢，encoders 保留）。
- Move `mux` / `venc` / `aenc` 给 out 参数。

**Refactor 的 caller 侧**（`reencode_pipeline.cpp:reencode_mux`）：
- 保留所有前置 opts 校验 + `ifmt0` 提取。
- 原 95 LOC setup 块变成：
  ```cpp
  std::unique_ptr<me::io::MuxContext> mux;
  CodecCtxPtr venc, aenc;
  SharedEncState shared;
  if (auto s = setup_h264_aac_encoder_mux(opts, ifmt0, mux, venc, aenc, shared, err);
      s != ME_OK) return s;
  AVFormatContext* ofmt = mux->fmt();
  ```
- 保留 FifoGuard inline（shared.afifo ownership）+ 原 `open_avio` / `write_header` / segment loop / flush / trailer。

**-Wunused 修复**：refactor 过程中 `best_stream` helper local function 不再被 reencode_pipeline 用（现在在 helper 里重声明）；`-Werror -Wunused-function` 触发；删除。

**Follow-up**：删 `multi-track-compose-actual-composite`，加同名窄化版——剩余工作的 scope 从 "整套 ComposeSink::process 实现" 缩成 "ComposeSink::process 内用 setup_h264_aac_encoder_mux 得到 encoder 后的 per-output-frame 合成循环（active_clips_at + decode + frame_to_rgba8 + alpha_over + rgba8_to_frame + encode_video_frame + passthrough audio）+ e2e test"。

**Alternatives considered.**

1. **直接在 ComposeSink 里 copy-paste 那 95 LOC** —— 拒：dup code 在三个 sink（compose / cross-dissolve / audio-mix）重复会有 300 LOC 冗余；未来任意一个点的 encoder bug 得同步改三处。Helper 是正确解。
2. **把整个 `reencode_mux` 改成可以接受 "compose frame provider" callback** 让 ComposeSink 复用 —— 拒：callback 的 shape（接受 time + 返回 frames map）牵涉 multi-demux 调度，不是 API 清晰的模式；独立 helper + 调用方自己管 loop 更 direct。
3. **返回 owning bundle struct** `{ mux, venc, aenc, shared_with_afifo_guard }` 而非个别 out-params —— 拒：需要设计 bundle 的 move 语义 + RAII afifo member（non-trivial movable struct）；out-params 方式跟现有 reencode_mux pattern 一致，重构更少。
4. **把 helper 放到 `src/orchestrator/reencode_pipeline.{hpp,cpp}` 内部作 static free function** —— 拒：需要 compose_sink 从 reencode_pipeline 里 expose 该 helper，违反 reencode_pipeline 的 narrow API 合约（它的 hpp 只声明 `reencode_mux`）。独立 TU 更 clean。
5. **`setup_...` 同时做 `mux->open_avio + write_header`** —— 拒：caller 可能想在 write_header 之间做额外 stream 配置（本 cycle 没 such case，但给 ComposeSink 留余地）。保持 "setup stops before AVIO open"。
6. **把 audio FIFO 也 handle RAII-wrap** 在 helper 内部 —— 拒：会让 helper 返回 non-trivial bundle；inline FifoGuard 在 caller 里是已证实可维护的。
7. **加单元测试给 helper** —— 拒：helper 的 correctness 完全由现有 22 个 suite 通过 reencode_mux 间接验证；独立 test 需要 mock 或 real fixture，overhead 不值。行为零变化就是"test by regression"。

业界共识来源：FFmpeg 示例代码 `doc/examples/encode_video.c` + `mux.c` 的 encoder/muxer bootstrap 模式——通常 抽 helper + call sites 反复 call 的风格，和本 refactor 同构。

**Coverage.**

- `cmake --build build` + `-Werror` clean（debug 过 `-Wunused-function` on `best_stream`——删）。
- `ctest --test-dir build` 22/22 suite 绿；无 test 修改——零行为变化回归保证。
- Test suite 包括 determinism tests (`test_determinism`) 验证 byte-for-byte output 不变。

**License impact.** 无。

**Registration.**
- `src/orchestrator/encoder_mux_setup.{hpp,cpp}` 新 TU。
- `src/CMakeLists.txt` 追加 source。
- `src/orchestrator/reencode_pipeline.cpp`：delete inline setup 块，call helper；删除 `best_stream` unused helper；add include。
- `docs/BACKLOG.md`：原 `multi-track-compose-actual-composite` bullet direction 第一条已完成 → 更新 bullet 文本到 "使用新 helper + 写 per-frame compose loop + e2e test"；slug 保持不变。

**§M 自动化影响.** 本 cycle 是 Rubric §5.2 (developer experience) 的纯重构，不对应任何 M2 exit criterion。§M.1 evidence check 不 tick 任何 box。M2 exit criterion "2+ video tracks 叠加" 保留未打勾。
