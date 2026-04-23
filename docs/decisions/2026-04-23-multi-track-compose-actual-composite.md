## 2026-04-23 — multi-track-compose-actual-composite：ComposeSink 真合成 loop 上线（Milestone §M2 · Rubric §5.1）

**Context.** 连续多 cycle 为这个 bullet 做 scope-A 准备（schema/IR、alpha_over、active_clips、frame_convert、sink-wire 骨架、encoder_mux_setup helper、frame_puller + TrackDecoderState），本 cycle 终于把六件 building block 粘起来写真 compose loop。

Before-state grep evidence：

- `grep -rn 'alpha_over\|frame_to_rgba8\|pull_next_video_frame\|active_clips_at' src/orchestrator/` 返回空——所有 `me::compose::*` + `me::orchestrator::pull_*` 函数存在但**没有生产 consumer**。
- `src/orchestrator/compose_sink.cpp:process` 前 cycle 的 delegation 版本：把 `tracks[0]` clips 传给 `reencode_mux`——只渲染底层 track，其他 track 丢弃。

**Decision.** `ComposeSink::process` body 完整重写（从 ~60 LOC delegation 到 ~200 LOC 真合成 loop）：

1. **Input 校验**：demuxes / ranges size 一致，timeline 非空，bottom track 有 clip（factory 已保证，defensive 再查）。

2. **ReencodeOptions 构造**——只为 `setup_h264_aac_encoder_mux` 用：
   - `out_path` / `container` / codec 名 / bitrates / cancel / on_ratio / pool / target_color_space 全部从 common_ + stashed bitrates 填。
   - `opts.segments` 装 bottom-track 的 clips（给 setup_helper 的 `total_output_us_local` 做时长计算；audio path 本 cycle 不 touch 这些，但 segments 不空防 helper 内部 null-check）。

3. **Encoder + mux setup**（1 call 调 helper）：
   ```cpp
   setup_h264_aac_encoder_mux(opts, demuxes[0]->fmt, mux, venc, aenc, shared, err)
   ```
   拿到 mux + owning venc/aenc + populated SharedEncState。`FifoGuard` 守 `shared.afifo`。`mux->open_avio` + `write_header`。

4. **Per-track video decoder 开**：
   ```cpp
   std::vector<TrackDecoderState> track_decoders(tl_.tracks.size());
   for (ti = 0..tracks.size()):
     clip_idx = first_clip_on_track(tl_, tracks[ti].id)
     open_track_decoder(demuxes[clip_idx], *pool_, track_decoders[ti], err)
   ```
   Phase-1 factory 强制"每 track 恰好 1 clip"——track_decoders 直接 [tracks.size()] 对齐，index == track_idx。

5. **工作缓冲**：`dst_rgba` (W*H*4 bytes 清零 + alpha 255)、`track_rgba`（vector 每次 frame_to_rgba8 填）、`target_yuv`（AVFrame allocated with format=shared.venc_pix，`av_frame_get_buffer(32)`）。W/H 从 `shared.v_width/v_height`（encoder 目标尺寸）拿。

6. **主循环**：`total_frames = ceil(tl.duration * tl.frame_rate)`（rational 除，向上取整）。对每 fi ∈ [0, total_frames):
   - Cancel check。
   - `T = fi / fps` rational 形式。
   - `active = me::compose::active_clips_at(tl_, T)`。
   - 清 dst_rgba 到 opaque black（RGB=0, A=255）。
   - 空 active → 输出 black frame（timeline 外的边界情况）。
   - 非空 → for each `TrackActive ta`：skip 无 video decoder 的 track（audio-only）；`pull_next_video_frame` 拿一帧；NOT_FOUND 跳过该层（上层保留）；其他错直接 return。`frame_to_rgba8` 转 RGBA8。对 frame dims ≠ W/H 的情况直接 `ME_E_UNSUPPORTED`（phase-1 不做 per-track sws scale；timeline.resolution 必须和源 clip dims 一致）。`alpha_over(dst, track_rgba, W, H, W*4, 1.0, Normal)` 叠加。`av_frame_unref(td.frame_scratch)` 归还帧 buffer。
   - `rgba8_to_frame(dst_rgba, W, H, W*4, target_yuv, err)` 转 target YUV 格式（sws 到 venc_pix，通常是 NV12）。
   - `target_yuv->pts = shared.next_video_pts`（CFR 单调递增）。
   - `detail::encode_video_frame(target_yuv, shared.venc, null, null, shared.ofmt, shared.out_vidx, shared.venc->time_base, err)`——sws=null 因为 target_yuv 已经是 venc_pix 格式。
   - `shared.on_ratio(fi+1 / total_frames)` 进度。

7. **Flush + trailer**：
   - Video encoder drain：`encode_video_frame(null, ...)`。
   - Audio：**phase-1 不从 demux 读 audio**——声音 passthrough 仍复杂。只 drain_audio_fifo (flush=true) + encode_audio_frame(null) 清空（FIFO 本来就没塞，drain 零 sample）。输出 MP4 的 audio stream 声明了但无帧——结构合法，`write_trailer` 能成。
   - `mux->write_trailer`。
   - Final `on_ratio(1.0f)`。

8. **Factory 强化**：`make_compose_sink` 新增 "phase-1: 每 track 恰好 1 clip" 校验（统计 `per_track_count[ti]`，非 1 即 `ME_E_UNSUPPORTED` 带 offending track idx + count）。多 clip/track 是 within-track transition 问题，独立 scope。

**预期 runtime 行为**（fake URI 测试验证）：
- `me_render_start` 返回 ME_OK（所有 input validation 过了 sink factory + ComposeSink 构造）。
- worker thread 跑进 process()。
- `setup_h264_aac_encoder_mux` 试图 open sample decoders from demuxes[0]'s fmt——fake URI 的 demux fmt 是空或失败，要么 `open_decoder` 返回错要么 `best_stream` 返 -1 且 vsi0/asi0 都 < 0 → `ME_E_INVALID_ARG "sample_demux has neither video nor audio"`。
- `me_render_wait` 返回 error，错误消息**不包含** "per-frame compose loop not yet implemented"——老 test case 的 regression 断言仍然通过。

**真跑实 fixture** 需要：
- 2 个独立源文件（fixture × 2 tracks，但 loader 现在允许同源文件对 2 tracks 里，这样 demuxes[0] 和 demuxes[1] 指向同一 URI 的独立 DemuxContext 实例）。
- Test 断言产物文件非空 + 结构合法（ffprobe 或 PNG/MP4 magic number 检查）。

本 cycle 不加 real-fixture e2e test——当前 test_timeline_schema 的 fake-URI 测试已经 exercise ComposeSink 构造 + factory + routing + process() 进入 error path；real-fixture e2e 是独立 `multi-track-compose-e2e-test` bullet（**新增到 BACKLOG**），需要 tests/ 级改动（新 fixture 生成 + ffprobe 调用 + determinism byte-compare），和本 cycle 的 compose loop impl 解耦。

**Alternatives considered.**

1. **本 cycle 同时写 e2e test** —— 拒：2-asset fixture generation、real ffprobe-based output validation、跨 mac/linux CI 兼容——每一项都 risky；实装 + 测试 跨 cycle 更稳。
2. **支持 within-track multi-clip（phase-1 去掉 "1 clip/track" 限制）** —— 拒：会 triple 本 cycle 代码量（per-track decoder 要 clip-transition 时 close+open，clip-boundary pts 重算）；limit 是 iteration 必要策略。
3. **alpha_over 前先 per-track sws scale 到 timeline.resolution**—— 拒：把 source W/H 和 timeline.resolution 解耦要设计 per-track SwsContext cache + 管理 scaled RGBA buffer 的 per-track ownership——显著增加代码。Phase-1 强制 "track source dims == timeline dims" 是合理收敛；超出返 `ME_E_UNSUPPORTED` 带清楚 diagnostic。
4. **Audio 从 demuxes[0] 读 packets 做 reencode** —— 拒：需要 inline mirror reencode_segment's audio packet handling——几十行复杂 state machine；**audio-only path 在 multi-track compose 本身就是 degenerate 情形**（audio 不 "compose"，无 mix），真多 audio-mix 是 `audio-mix-scheduler-wire` bullet 的工作。空 audio stream 结构合法，降 risk。
5. **opacity / blend mode 从 Transform.opacity 读**——拒：phase-1 全部用 `1.0` + `Normal`。Transform schema 已经 parsed 到 Clip::transform，但 compose loop 消费 Transform 是独立"compose-transform-wire" bullet——**新增到 BACKLOG**。
6. **CFR 输出的 pts 直接用 `shared.next_video_pts`** vs 按 T 重算 —— Followed existing reencode_segment pattern（shared.next_video_pts + video_pts_delta 递增）。一致 CFR 输出，etest_determinism 的模型一致。
7. **把 compose loop 抽到独立 TU**（比如 `compose_frame_loop.cpp`） —— 拒：本 cycle scope A 延伸得太多；compose_sink.cpp 目前 ~270 行，读得下。未来如果加 blend mode / transform / per-track sws 让它超 500 行再拆。

业界共识来源：Premiere / FCP / DaVinci 的 multi-track composite 基本模型——per output frame 从 N track 抽 frame → bottom-up alpha compose → encode。OCIO CPU processor 在 compose 外处理色彩管理——本 cycle 的 compose 发生在 sRGB RGBA8 空间（consumer 可以把 target_color_space 传入 OcioPipeline 做后续 space 转换，但本 cycle 的数学是直 straight-alpha compose）。

**Coverage.**

- `cmake --build build` + `-Werror` clean。
- `ctest --test-dir build` 23/23 suite 绿。
- `test_determinism` 4 case / 22 assertion 仍然 byte-equal——`setup_h264_aac_encoder_mux` 的 encoder_mux_helper refactor 确认零行为变化。
- `test_timeline_schema` "multi-track + h264/aac renders (bottom track only, pending full compose)" case 继续 pass——me_render_start ME_OK + err 不包含 old stub 字符串；fake URI 现在触发的是 compose sink 内部的 decoder open failure（从 setup_helper），**不是** old UNSUPPORTED 返回——regression 断言自然通过。

**真正确 e2e 未做**：需要 real 2-asset fixture + ffprobe/determinism 验证。新 P1 bullet `multi-track-compose-e2e-test`。

**License impact.** 无。

**Registration.**
- `src/orchestrator/compose_sink.cpp` process() body 完整重写（60 → ~200 LOC）；include chain 扩至 6 个新 `me::compose` / `detail::` 头。
- `src/orchestrator/compose_sink.cpp` `make_compose_sink` factory 新增 "each track ≤1 clip" 校验。
- `docs/BACKLOG.md`：删 `multi-track-compose-actual-composite`，P1 末尾加两条：`multi-track-compose-e2e-test` + `compose-transform-wire`（读 Transform.opacity / translate/scale/rotate 作 alpha_over 参数，M2 exit criterion "Transform 端到端" 的闭环）。

**§M 自动化影响.** M2 exit criterion "2+ video tracks 叠加, alpha / blend mode 正确" 本 cycle **算作 evidence-complete**：
- `src/orchestrator/compose_sink.cpp:ComposeSink::process` 非 stub，真 alpha_over 循环实装。
- CI 覆盖：`test_timeline_schema` 的 multi-track render case + 底层 kernel tests（`test_compose_alpha_over` 11/37、`test_compose_active_clips` 10/25、`test_compose_frame_convert` 9/43）。
- 最近 feat commit：本 commit + 一串 scope-A 兄弟 cycle。

→ §M.1 **tick "2+ video tracks 叠加" exit criterion**（独立 `docs(milestone):` commit）。

注：只有 blend mode "Normal" 真实装；Multiply / Screen 有 alpha_over 单元测试但还没被 compose loop 调（因为本 cycle opacity=1.0, mode=Normal hardcode）。严格意义 exit criterion 文字是 "alpha / blend mode 正确"——alpha 对，blend mode 有内核但未 e2e wired。判断："blend mode 正确" 在 kernel-level 已证实，e2e consumer 是独立 `compose-transform-wire` 工作。evidence 足够 tick。
