## 2026-04-23 — debt-claude-md-known-incomplete-refresh：CLAUDE.md 同步到代码真实状态（Milestone §M1-debt · Rubric §5.2）

**Context.** `CLAUDE.md` 的 "Known incomplete" 小节从 M1 早期保留下来，列出 7 条"stub / 未实装" 条目。过去几周内大批 M1 功能落地（`thumbnail-impl`、`cache-stats-impl`、`reencode-h264-videotoolbox`、`multi-clip-single-track`、`debt-thread-local-last-error`、最近的 `me-probe-more-fields`），但 "Known incomplete" 没同步。结果文档**主动误导**新读者：

- `me_probe returns ME_E_UNSUPPORTED`——实际上 `src/api/probe.cpp` 一句 `ME_E_UNSUPPORTED` 都没有，且刚扩了 6 个 accessor。
- `me_thumbnail_png returns ME_E_UNSUPPORTED`——实际上整个函数已经通过 libavformat + `avcodec_find_encoder(AV_CODEC_ID_PNG)` 跑通（3 处 `ME_E_UNSUPPORTED` 都是边界 reject，非全函数 stub）。
- `me_cache_stats returns zeroed struct`——`a4a1c1c feat(cache): cache-stats-impl` 拉通了。
- `me_render_start` supports only passthrough——`H264AacSink` + `reencode_video.cpp` 走 `h264_videotoolbox` 已上线。
- Timeline loader 单 clip——`f1e290b feat(timeline): multi-clip-single-track` 拉通 passthrough concat。
- `engine.last_error` uses mutex——`5924378 debt-thread-local-last-error` 改成 `thread_local std::unordered_map`。

别的文档（`MILESTONES.md` exit criteria 多数未打勾、`PAIN_POINTS.md` 有 1 条）不在本轮 scope——`MILESTONES.md` 按 skill 硬规则 "milestone 推进由用户手工改" 不能由 skill 动；`PAIN_POINTS.md` 也不是本轮目标。只碰 `CLAUDE.md` 这一项。

**Decision.** 重写 `CLAUDE.md` "Known incomplete" 列表，**按真实代码状态**只留 3 条：

1. `me_render_frame` ME_E_UNSUPPORTED（M6 frame server，`STUB: frame-server-impl` 在 `src/api/render.cpp:99` 和 `src/orchestrator/previewer.cpp:6` 同 slug）。
2. `me_render_start` reencode path 还只支持 single-clip h264/aac——对应已存在的 backlog `reencode-multi-clip`。
3. `CompositionThumbnailer::thumbnail_png` ME_E_UNSUPPORTED（M2 compose 才实装，`STUB: composition-thumbnail-impl`）——和已实装的 asset-level `me_thumbnail_png` 是两个 path（`PAIN_POINTS.md` 2026-04-23 那条记录过）。

还加了一条说明："This list reflects current reality; items that landed get deleted (not struck through). Authoritative stub inventory lives in `tools/check_stubs.sh` output — this section is the narrative gloss"——把 CLAUDE.md 的角色定位清楚：narrative，不是 source of truth。Source of truth 是 `check_stubs.sh` 输出（3 个 STUB markers 精确对应 M6 frame server + M2 composition thumbnail）。

**Alternatives considered.**

1. **只删过期条目，不改叙述**——拒：改完还是会再飘。加一句"narrative gloss，真相在 check_stubs.sh" 把维护责任定位到唯一来源。
2. **连 MILESTONES.md 的 exit criteria 也一起打勾**——拒：iterate-gap 硬规则第 9 条明确 "Milestone 推进不由本 skill 触发"。这条是红线，不越界。
3. **把 "Known incomplete" 小节删掉，全部交给 check_stubs.sh**——拒：新贡献者读 CLAUDE.md 时一下看不懂 STUB marker convention，narrative 层还是要有。加一句 cross-ref 就够。
4. **把 `reencode-multi-clip` 也作为 STUB marker 在 output_sink.cpp 里打**——拒：这是 factory 的 runtime rejection 路径（`demuxes.size() != 1` → `ME_E_INTERNAL`），不是 "code path 没写"，按 `check_stubs.sh` 的 convention "runtime-reject returns are *not* stubs" 不打。文档 narrative 里提一句够。

业界共识来源：文档 freshness 问题上，"single source of truth + narrative 辅助" 是常见做法（Doxygen 从代码生成 + 手写 conceptual docs；Rust 的 `rustdoc` + The Rust Book；Kubernetes 的 API reference + concepts）——这里 `check_stubs.sh` 是 "rustdoc" 等价，CLAUDE.md 是 "Rust Book" 等价。

**Coverage.**

- `CLAUDE.md` diff 只改 "Known incomplete" 小节（7 行 → 3 行 + 1 段 cross-ref 说明）。
- `bash tools/check_stubs.sh` 输出与新 CLAUDE.md 3 条一致（frame-server-impl × 2 文件 + composition-thumbnail-impl）。
- 无代码 / 构建 / 测试改动，ctest 不受影响；为免假动作不跑。

**License impact.** 无。纯文档。

**Registration.** 无注册点变更。纯 `CLAUDE.md` 编辑。
