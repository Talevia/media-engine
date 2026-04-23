## 2026-04-23 — audio-mix-two-track：audio schema/IR 先行，mix 内核拆出子 bullet（Milestone §M2 · Rubric §5.1）

**Context.** M2 exit criterion "2+ audio tracks 混音，带 peak limiter" 需要四件事：(1) 多条独立 audio track 能被 schema/loader 承认；(2) IR 区分 audio vs video track/clip；(3) 音频 resample + 相加 + peak limiter 内核；(4) 把合成后的音频流送进 encoder（h264/aac reencode path 目前是从 video clip 的 MP4 里读 audio，不是独立 audio clip）。(3) + (4) 是 engine-level 的大工程（libswresample 接线、公共输出采样率协商、peak limiter ballpark、AAC encoder 的 frame-aligned 喂入重构），撑得起独立 bullet。

本 cycle 做 (1) + (2)。与 multi-track-video-compose 完全同构的拆法：schema+IR+loader 前置，kernel+sink 后置。

Before-state grep evidence：

- `src/timeline/timeline_loader.cpp:277-279` 旧行 `require(track.at("kind").get<std::string>() == "video", ME_E_UNSUPPORTED, ...: phase-1: only video tracks supported)` —— loader 硬拒 audio track。
- `src/timeline/timeline_impl.hpp:67` `struct Clip` 无 `type` 字段——IR 不区分 audio / video clip。
- `Timeline::tracks` 的 `Track` struct 只有 `id` + `enabled`——无 `kind`。
- `grep -n 'gainDb\|gain_db' src/timeline/` 返回空——schema 里定义的 audio clip `gainDb` 字段从来没被 loader 消费过。

**Decision.** 三个文件 source + 8 个 new tests + BACKLOG 重组：

1. **IR 扩展**（`src/timeline/timeline_impl.hpp`）：
   - 新 `enum class TrackKind : uint8_t { Video = 0, Audio = 1 }`。
   - 新 `enum class ClipType : uint8_t { Video = 0, Audio = 1 }`。
   - `Track::kind`（默认 `Video`）。
   - `Clip::type`（默认 `Video`）。
   - `Clip::gain_db: std::optional<double>`——audio-only，static-only，phase-1 复用 `parse_animated_static_number`（keyframes 形式仍被统一拒绝）。

2. **Loader 多 kind 解析**（`src/timeline/timeline_loader.cpp`）：
   - 删除旧 `require(track.kind == "video", ...)`。替换成 `video`/`audio` 两选一；未知 kind（如 `"subtitle"`）返回 `ME_E_UNSUPPORTED` 并带上实际值做报错。
   - 引入 `expected_clip_type` 本地变量（"video" vs "audio"），把每个 clip 的 `type` 字段严格对比；mismatch → `ME_E_PARSE "must match parent track.kind (expected 'X', got 'Y')"`。
   - Audio clip 解析：同样的 `assetId` / `timeRange` / `sourceRange` 校验（per-track contiguity、positive duration、source_start ≥ 0）。`gainDb` 可选，命中则 `parse_animated_static_number` 塞进 `Clip::gain_db`；video 上的 `gainDb` → `ME_E_PARSE "not valid on video clip"`。
   - Video clip 的 `transform` 保留；audio clip 上的 `transform` → `ME_E_PARSE "not valid on audio clip (2D positional transform is meaningless for audio)"`。
   - 每个 clip 根据 track_kind stamp `c.type = Video/Audio`。
   - `tl.tracks.push_back(me::Track{track_id, track_kind, track_enabled})`。

3. **Exporter audio gate**（`src/orchestrator/exporter.cpp`）：
   - 新 `for (const auto& t : tl_->tracks) if (t.kind == Audio) return ME_E_UNSUPPORTED "standalone audio tracks not yet implemented — see audio-mix-kernel backlog item"`。
   - 位于现有 multi-track gate 之前，给 audio-only 场景更准确的错误消息（multi-track gate 只在 `tracks.size() > 1` 触发，audio-only 单轨会绕过它）。
   - **Stub 净增 ±0**：Loader 原本就硬拒 audio（1 条 UNSUPPORTED），Exporter 接管（1 条 UNSUPPORTED）。

4. **Tests**（`tests/test_timeline_schema.cpp`）—— 8 个 TEST_CASE append：
   - audio track + audio clip + gainDb=-6.0 → IR 里 `tracks[0].kind == Audio` + `clips[0].type == Audio` + `clips[0].gain_db == -6.0` + `transform == nullopt`。
   - audio track 无 gainDb → `clips[0].gain_db == nullopt`（区分"没 gainDb 键"和"gainDb=0"）。
   - audio track + passthrough render → `ME_E_UNSUPPORTED` + err 含 "standalone audio tracks not yet implemented"。
   - audio clip 放进 video track → `ME_E_PARSE` + err 含 "must match parent track.kind"。
   - video clip 放进 audio track → `ME_E_PARSE`（对称反向用例）。
   - video clip 带 gainDb → `ME_E_PARSE` + err 含 "gainDb" + "not valid on video clip"。
   - audio clip 带 transform → `ME_E_PARSE` + err 含 "transform" + "audio clip"。
   - 未知 track.kind（`"subtitle"`）→ `ME_E_UNSUPPORTED` + err 含 "only 'video' and 'audio'"。

5. **BACKLOG 重组**：
   - 删掉 `audio-mix-two-track` bullet。
   - P1 末尾 append `audio-mix-kernel`——载体音频内核 + sink 重构。

**Scope 取舍.** M2 exit criterion 不会在本 cycle 满足（需要内核），但把"schema 承认独立 audio track"和"audio mix 内核"解耦是真实生产力：host 现在可以开始写 JSON 描述多轨音频作品，等内核落地时只要翻 UNSUPPORTED gate 就能跑（和 multi-track-video-compose 同构的 delivery model）。

**Alternatives considered.**

1. **不加 `gainDb` 解析，只加 schema 承认**——拒：`gainDb` 是 audio clip schema 的核心语义，不解析它等于 loader 对 audio clip 结构不完整；将来 audio-mix-kernel 的工作会被"schema 其实还没 ready"拖累。gainDb 是 static-only 一行代码复用现有 helper，边际成本极低。
2. **支持 gainDb 的 `{"keyframes": [...]}` 形式**——拒：同 transform 一样，animated 形式整体留给 M3 animated-params 工作，统一时机比零散突破好。`transform-animated-support` backlog bullet 的覆盖范围扩大到包含 `gainDb`，**不**新增独立 bullet（避免 bullet 碎片化）。
3. **在 audio track 上接受 video clip，反之亦然，当成 type-erased clip 统一处理**——拒：audio 和 video 渲染管道根本不同（采样率 vs 帧率、resample vs scale、不同 codec 枚举）。早期严格匹配能防将来一系列 "哎 audio clip 怎么进了 video sink" 的 bug。
4. **把 audio clip 的 `sourceRange` 改成 optional**（schema 里 video/audio 差异可能有）—— 检查 TIMELINE_SCHEMA.md，audio clip 示例仍保留 sourceRange。保持一致；audio clip 的 source sample range 同样有意义（只不过 denominator 通常是 sample_rate 而非 video frame_rate）。
5. **不拒绝 video clip 上的 `gainDb`**（就当 hint，未来某天用）——拒：silent-accept unknown-sense fields 是典型 footgun。严格拒绝逼 host 把"视频的音轨 gain"表达成 separate audio track。
6. **把 `Clip::type` 改成 `std::variant<VideoClip, AudioClip>` 这种分类型结构**——拒：太早的 variant 抽象；当前两种类型只差 `gain_db` 一个可选字段，共享 struct 性价比高。M5 加入 TextClip 时若需要再考虑 variant。
7. **不加 `TrackKind` 和 `ClipType` 的区分，只 stamp clip.type 足够**——拒：TrackKind 决定 loader 的 expected_clip_type，没这个字段 loader 就得 string 比较 track.kind 两次。enum 简单直接。
8. **一条 `audio-mix-track-schema` + 另一条 `audio-mix-kernel`**（彻底 schema 和 impl 分两 bullet）——拒：scope A 本身就是 audio-mix-two-track bullet 的前置 plumbing，同名 bullet 处理完 schema 部分 + 删除它 + 加 `audio-mix-kernel` 是更符合 iterate-gap 粒度的切分。

业界共识来源：DAW（Logic / Ableton）的 "audio track 和 video track 是两个 object 类型" 分层、Premiere / FCP 的 audio channel lane 独立于 video track、OTIO 的 `kind: "audio"` / `kind: "video"` 区分符 —— 一致支持 track-level kind 区分。

**Coverage.**

- `cmake --build build` 与 `-Werror` clean。
- `ctest --test-dir build` 16/16 suite 绿。
- `build/tests/test_timeline_schema` 37 case / 198 assertion（先前 29/153；+8 case / +45 assertion）。
- 其他 15 个 suite 全绿。单 video track render 路径完全不受影响（`Track::kind` 默认 Video，现有 single-track 测试建的 timeline 经过 Track 构造器依然是 Video）。
- Single-track 向后兼容：前一 cycle 刚加的 `track_id stamp` 测试依然通过；Clip::type 默认值 Video，所有旧测试不改。

**License impact.** 无。

**Registration.**
- `src/timeline/timeline_impl.hpp`：`enum class ClipType`、`enum class TrackKind`、`Track::kind`、`Clip::type`、`Clip::gain_db`。
- `src/timeline/timeline_loader.cpp`：video/audio kind dispatch，clip-type 匹配校验，gainDb / transform 交叉排除。
- `src/orchestrator/exporter.cpp`：audio track gate。
- `tests/test_timeline_schema.cpp`：+8 TEST_CASE。
- `docs/BACKLOG.md`：删 `audio-mix-two-track`，P1 末尾加 `audio-mix-kernel`。

**§M 自动化影响.** M2 exit criterion "2+ audio tracks 混音, 带 peak limiter" 本 cycle **未完成**——schema/IR 就位但混音内核缺。§M.1 evidence check 结果：`src/` 无混音内核实装，混音在 `audio-mix-kernel` bullet 里；本 exit criterion 保留未打勾。
