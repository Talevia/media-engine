## 2026-04-23 — multi-track-video-compose：multi-track schema/IR/loader 先行，compose 内核拆出子 bullet（Milestone §M2 · Rubric §5.1）

**Context.** M2 exit criterion "2+ video tracks 叠加，alpha / blend mode 正确" 需要四件事端到端就位：(1) 多轨 schema 被 loader 接受；(2) 多轨 IR 能表达；(3) alpha + blend mode 合成内核；(4) sink 接收合成帧而非 demux packet。(3) + (4) 是真正的"compose"工作（像素内核、重构 reencode 管道读 frame 流而非 demux-context），规模足够撑起独立 bullet；(1) + (2) 是其前置 plumbing，本身独立有价值（让 compose cycle 不用和 schema/IR refactor 搅在一起）。

本 cycle 做 (1) + (2)，并把 (3) + (4) 作为 `multi-track-compose-kernel` 从主 bullet 拆出来。拆分的业界惯例：Premiere / DaVinci / FFmpeg complex_filter 都把 "accept N-track timeline" 和 "composite N-track into frame stream" 作两个独立层。

Before-state grep evidence：

- `src/timeline/timeline_loader.cpp:238` 旧行 `require(tracks.size() == 1, ME_E_UNSUPPORTED, "phase-1: exactly one track supported")` —— loader 硬拒多轨 JSON。
- `src/timeline/timeline_impl.hpp:67` `struct Clip` 只有 asset_id / time_* / source_start / transform；**无** track_id。`Timeline` 只有 flat `vector<Clip> clips`；**无** `tracks` vector。IR 层完全不区分 "哪个 clip 属于哪个 track"。
- `src/orchestrator/exporter.cpp:58` exporter 开局只 check `tl_->clips.empty()`；无 multi-track gate（它 never gets there 因为 loader 前置拒绝了）。

**Decision.** 三个 source + 五个 new tests + backlog 重组：

1. **IR 扩展**（`src/timeline/timeline_impl.hpp`）：
   - 新 `struct Track { std::string id; bool enabled{true}; }`——仅 metadata，**不**内嵌 clips（clips 保留在 `Timeline::clips` flat 列表）。
   - `Timeline::tracks: std::vector<Track>`——JSON 声明顺序（未来 compose 用作 bottom→top layer order）。
   - `Clip::track_id: std::string`——loader 填，未来 compose 按此 group。
   - **不**改 `Timeline::clips` 的类型 / 语义：仍是跨所有 track 的 flat list。consumer（segmentation / exporter）读法不变（在 single-track 场景下 clips 全来自 track 0，behavior 一致）。

2. **Loader 多轨解析**（`src/timeline/timeline_loader.cpp`）：
   - 删除旧 `require(tracks.size() == 1, ...)`。
   - 外层 for-loop iterate tracks；内层保留原有 per-clip validation；per-track 计算 `running` 确保 within-track contiguity（no gap/overlap）；per-track cumulative end time → `max_duration` 累积取 max，最后赋给 `tl.duration`。
   - 新增 per-track 校验：(a) `id` 非空，(b) track id 跨 tracks 唯一（`unordered_map<string,size_t> track_id_seen` guard），(c) `kind == video`（audio-mix-two-track / M5 text 另走），(d) `enabled` 字段读取但仍载入（默认 true）。
   - 每个 clip 进入 IR 前 stamp `c.track_id = track_id`。

3. **Exporter gate 迁移**（`src/orchestrator/exporter.cpp:58-62` 附近）：
   - Add `if (tl_->tracks.size() > 1) return ME_E_UNSUPPORTED "multi-track compose not yet implemented — see multi-track-compose-kernel backlog item"`。
   - 这是"搬"，不是"新增"：old ME_E_UNSUPPORTED 在 loader，new ME_E_UNSUPPORTED 在 exporter。**stub 净增 +0**，符合 SKILL §3a rule 7。

4. **Tests**（`tests/test_timeline_schema.cpp`）—— 删掉 1 个 stale test（"phase-1 rejects multi-track timeline as ME_E_UNSUPPORTED"），换成 5 个 new TEST_CASE：
   - 多轨 load OK：2 tracks (v0 enabled, v1 enabled=false)，IR 校验 `tracks.size()==2` + `tracks[0].id="v0" enabled=true` + `tracks[1].id="v1" enabled=false` + flat clips[0].track_id="v0" / clips[1].track_id="v1" + `duration == max(v0,v1)`（90/30 > 60/30）。
   - 多轨在 render 层被 Exporter 拒绝：load OK → `me_render_start` → ME_E_UNSUPPORTED + err 含 "multi-track compose not yet implemented"。
   - 重复 track id → ME_E_PARSE + err 含 "duplicate track id"。
   - Within-track gap（跨 track 的 gap 是 OK 的，但 within-track 必须 contiguous）→ ME_E_UNSUPPORTED + err 含 "within this track" + "track[0]"（per-track error prefix 证明 error context 正确）。
   - 单轨 timeline backward-compat：仍然 load OK，新增的 `tracks.size()==1` + `tracks[0].id == "v0"` + `clips[0].track_id == "v0"`（stamped）。

5. **BACKLOG 重组**——同 commit：
   - 删掉 `multi-track-video-compose` 主 bullet（本 cycle 处理完 (1)+(2)）。
   - P1 末尾 append `multi-track-compose-kernel`——承载 (3)+(4) 的 compose 内核 + sink 重构。Gap 引用 `src/orchestrator/exporter.cpp` 的新 gate line + `src/color/pipeline.hpp` 的 Pipeline 接口 + `src/orchestrator/output_sink.hpp` 的 sink 接口。

**Scope gate 取舍.** Could alternatively 做 scope B（一把端到端 compose），但：

- 工作量 estimate 一周以上（2D blit kernel、RGBA over 合成、N-path sink 重构、确定性保证）。
- 每 cycle 一个 closed-loop 是 iterate-gap 的核心假设，长 cycle 会导致 decision 文件失焦 / tests 写不完 / ABI 疑问积累。
- 拆成两半能保证每 cycle 都 CI-green + 生产可用（single-track 路径始终不坏）。

Scope A 先行的一个风险：IR 多了字段，consumer 没更新——但 grep 表明只有 segmentation / exporter / 2 tests / 1 example 使用 `Timeline::clips`，都不需要改（flat list 语义不变；新字段是 additive）。

**Alternatives considered.**

1. **`Timeline::clips` 也 refactor 成 `Timeline::tracks[N].clips`**（真正的 per-track nested 结构）—— 拒：影响半打文件，本 cycle 只需 metadata 就够；compose 内核 cycle 自己再决定是否 nest（它就是 clips 的真正消费者）。
2. **每个 track 自己的 `vector<Clip>` 存 IR 里，`Timeline::clips` 作 flatten view**（method，不是字段）—— 拒：flat field + track_id stamp 更直接，不引入"view"概念的抽象。flatten method 总是可以加。
3. **把多轨 gate 留在 loader**（即：loader 仍然拒绝 `tracks.size() > 1`，但在 `require()` 里写"see multi-track-compose-kernel"）—— 拒：IR 永远见不到多轨，compose 内核 cycle 要同时做 loader + IR + kernel，压力更大。让 loader 先开门是更渐进的 delivery。
4. **在 exporter gate 里 ignore 多轨而只 render track 0** —— 拒：静默 drop data 是"wrong output"的典型 footgun。明确 UNSUPPORTED 强制 host 认识到 compose 未实装，比让 host 误以为 render 成功但少帧好得多。
5. **同 cycle 引入 `multi-track-compose-kernel` bullet 作 P0** —— 拒：它是 M2 exit criterion 的延续，自然 P1；没证据它比 `ocio-pipeline-enable` / `cross-dissolve-transition` 更"阻塞"。P1 末尾 append，下次 /iterate-gap 按现有顺序挑。

业界共识来源：FFmpeg `complex_filter -filter_complex "[0][1]overlay"` 的 "timeline has N tracks, filter graph composites them" 分层、OTIO `Timeline.tracks` 作 list + `Track.children` 作 clip list 的 nested 结构、Premiere / DaVinci Resolve / FCP 的 "track is just metadata, clips live elsewhere" （Premiere 的 model） vs "clips nested in tracks"（OTIO）—— 两派都有产品级实现。本项目当前偏 Premiere-metadata 模式（flat clips + track_id），成本低且符合 reencode 路径的 flat-processing 天性。

**Coverage.**

- `cmake --build build` 与 `-Werror` clean。
- `ctest --test-dir build` 16/16 suite 绿。
- `build/tests/test_timeline_schema` 29 case / 153 assertion（先前 25/122；+4 case / +31 assertion；5 new - 1 deleted = +4 case net）。
- 其他 15 个 suite 全绿，无 side-effect：render 路径在 single-track 下 exit 前检查 `tracks.size() > 1` 早不命中；IR 新字段是 additive；loader 旧错误信息中 "cumulative prior clip duration" 改成 "... within this track" 只影响负面断言的具体字符串——没其他 test 检查过该字符串。

**License impact.** 无。

**Registration.** 无 C API / schema / kernel 变更（schema doc 已经承认 `compositions[].tracks` 为数组；loader 的改动只是放宽而不超出 schema 原本定义）。
- `src/timeline/timeline_impl.hpp`：`struct Track`、`Clip::track_id`、`Timeline::tracks`。
- `src/timeline/timeline_loader.cpp`：track-loop 重写，per-track id uniqueness + 非空校验。
- `src/orchestrator/exporter.cpp`：multi-track gate。
- `tests/test_timeline_schema.cpp`：-1 stale test +5 new tests。
- `docs/BACKLOG.md`：P1 里删 `multi-track-video-compose`，末尾加 `multi-track-compose-kernel`。

**§M 自动化影响.** M2 exit criterion "2+ video tracks 叠加, alpha / blend mode 正确" 本 cycle **未完成**——schema/IR 就位但 compose 内核缺。§M.1 evidence check 结果：`src/` 无合成内核，合成在 `multi-track-compose-kernel` bullet 里；本 exit criterion 保留未打勾。
