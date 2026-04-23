## 2026-04-23 — test-reencode-multi-clip：多段 reencode 确定性 CI tripwire（Milestone §M1-debt · Rubric §5.3）

**Context.** `reencode-multi-clip` 在 2bfa6cd 落地——N-segment h264/aac concat，shared encoder 跨 segments，`next_video_pts` / `next_audio_pts` 跨段单调。决策文档记录的 E2E 回归是一次性手测（`/tmp/me-multi.timeline.json` × `05_reencode` → 60 h264 + 89 aac + 2.04s）。落地后几个 cycle（`debt-reencode-pipeline-split-process-segment`、`debt-split-reencode-pipeline-audio-fifo` 各自搬家 `process_segment` 和 FIFO 到独立 TU），每次都手测通过，但 `tests/test_determinism.cpp` 的 h264/aac reencode case **只跑单 clip**（`grep` 返回 line 190 的 `.add_clip(...)` 单例）。多段 encoder-shared 路径在 CI 里**无 tripwire**：任何 refactor `SharedEncState.next_*_pts` 累加、`process_segment` 里 swr/sws 复用、encoder flush 时机——不 trip 在单 clip 路径上，但都可能挂在多段。

**Decision.** `tests/test_determinism.cpp` 附加第 4 个 TEST_CASE: "h264/aac reencode concat across N segments is byte-deterministic"。和单 clip case 同 fixture（`determinism_input.mp4`，640×480 @ 25fps，无音频），timeline 2 clip 各 1s 背靠背（cumulative 2s），h264/aac reencode 跑两次断言 byte-identical。

沿用单 clip case 的 "skip on videotoolbox unavailable" pattern（`ME_E_UNSUPPORTED / ME_E_ENCODE → MESSAGE + return`）——Linux CI 没 VideoToolbox 时跳过，mac dev machine 每次 CI 都跑实路径。fixture 无音轨意味着 reencode 退化到纯 video，仍覆盖 multi-segment 主路径（`process_segment` 的 video encoder loop + `next_video_pts` 累加），这正是 bullet 要 tripwire 的部分。

**为什么 2 个 clip 就够了（不要 3+）.** `process_segment` 的参数里唯一"per-segment 索引"相关的逻辑是 err message 前缀（`"segment[" + std::to_string(seg_idx) + "] "`）和 compat check（segment i vs segment 0 的 codec params）。3-segment 比 2-segment 多的覆盖只在 "segment 2 继续用 segment 1 刚 flush 的 decoder" 那条路径——而 `process_segment` 每次都 `open_decoder()` 重建 per-clip decoder（decoder 不 pool，只 encoder shared）。所以多加 segment 不增加 tripwire 价值。2 segment 就够抓 "cross-segment encoder state mishandle"。

**fixture 无音轨的含义.** `gen_fixture` 产的 fixture 只有 video。reencode path 中 `a0dec` 为 null → `shared.aenc` 为 null → `SharedEncState::afifo`、`next_audio_pts`、`feed_audio_frame` 都不进。于是本 case 覆盖的是 **video-only** 多段 reencode。Audio 多段 coverage 要等 future fixture 扩展（`debt-test-thumbnail-color-tagged-fixture` 同类工作的延伸）或手工跑 `/tmp/me-multi.timeline.json` × `/tmp/input.mp4`（1920x1080 h264+aac）。本 cycle 不扩 scope。

**Alternatives considered.**

1. **另开 `tests/test_reencode_multi_clip.cpp` 独立 TU**——拒：单 clip reencode determinism + multi clip reencode determinism 是强相关的同一 determinism 契约的两面，共 fixture + 共 slurp / diff helper。分独立 TU 增加 CMake 登记 + `test_main` 重复链接 + "哪个文件管 reencode determinism" 分摊决策，无收益。
2. **改用 `/tmp/input.mp4` 拿有音频的 fixture**——拒：`/tmp/input.mp4` 是 dev machine 特定文件（1920x1080 h264+aac 来源不明），不进 CI。坚持用 build-time `gen_fixture`。如需音频在 `gen_fixture` 里加 AAC track 是另一个 cycle（"gen_fixture-audio-track" 或类似）。
3. **只加单次 render 断言产物存在非空**——拒：本 bullet 明确是 "确定性 tripwire" 语义，必须 2 次 render + byte-compare。
4. **同时断言帧数 / container = mov**（集成测试风格）——考虑过；`05_reencode` + `ffprobe` 手测早已验，doctest 里再加 ffprobe-like 解析是 over-engineering。byte-equality 是最小充分条件。

业界共识来源：deterministic-build tripwire + 相关 variant（single-input vs multi-input concat）的测试 strategy 是 Bazel 的 stamping tests、FFmpeg 自己的 fate "compare against reference byte-identical" 模式、Rust/Cargo 的 build-output-determinism test。单 case + 多 case 并存是这个生态的 standard。

**Coverage.**

- `cmake --build build` 与 `-Werror` clean。
- `ctest --test-dir build` 12/12 suite 绿。
- `build/tests/test_determinism -s` 4 case / 22 assertion / 0 skip（从 3/16 升 → 4/22）。
- 多段 case 在 mac dev 跑实路径：两次 render 产 byte-identical MP4（同 BITEXACT flags / 同 video_pts_delta / 同 segment 边界的 decoder reopen）。
- 不动 src/，其他 11 个 suite 继续绿。

**License impact.** 无。

**Registration.** 无 C API / schema / kernel 变更。仅 `tests/test_determinism.cpp` 加 1 个 TEST_CASE。
