# Architecture pain points — running log

Observations about where a **hard rule** (VISION axiom, CLAUDE.md anti-requirement / architecture invariant, ARCHITECTURE.md ABI commitment) caused recurring friction during `iterate-gap` cycles. Purpose: collect evidence for periodic rule review. Not a todo list (that's `docs/BACKLOG.md`); decision rationale per cycle lives in commit bodies, not a separate file.

**Admission bar** (all three must hold — miss any → write a `debt-*` bullet in `BACKLOG.md` instead):

1. Friction traces to a named rule, not just missing abstraction or threshold-not-reached.
2. Cost recurs per new codec / effect / asset type, not one-off.
3. Resolution would likely amend the rule (or formally accept its cost), not just add a helper.

**Format.** One entry per pain point, appended at the bottom:

    ### <YYYY-MM-DD> · <cycle-slug> — <short title>

    <2–4 sentences: which rule, why the rule creates recurring friction, what a rule-amendment would be trading off>

No rewriting, no reordering — this is a timeline of impressions. When a rule is amended (or the friction evaporates within the rule), remove the entry in a cleanup pass; leaving stale entries misleads future readers.

Expected volume: 0–2 entries per quarter. More → admission bar is leaking; do a purge + tighten.

---

### 2026-04-22 · reencode-h264-videotoolbox — `me_output_spec_t` 用 `const char*` 做 codec 选择

`video_codec = "h264"`、`audio_codec = "aac"` 是字符串——好处是 ABI 稳、C 友好；坏处是 `is_passthrough_spec` / `is_h264_aac_spec` 这种分支要靠 `strcmp` + 每加一种就要维护一个新的 helper。更痛的是 `spec.video_bitrate_bps` 不区分是给 h264 用、还是给 ProRes 用，所有 codec-specific 选项都只能共用这一层 flat struct。**规则压力来源：** VISION §3.2（typed effect params）+ CLAUDE.md invariant #1（公共头 POD 跨边界、无 STL、无异常）两条硬规则的合力——每加一个 codec 都在这个张力点上撕扯。**方向 / 权衡：** 未来某个 milestone 需要每 codec 一个 typed option struct（`me_h264_opts_t`、`me_aac_opts_t`），spec 里带个 union / tagged 指针；但跨 C ABI 整 union 成本高（版本演进、大小稳定性、bindgen 友好度都要重设计）。现在还只有两种 codec 不痛，等 M3–M4 第 3、4 种落地时才是评估"要不要在 C ABI 层正式引入 typed option union 范式"的决策点。

### 2026-04-26 · player-orchestrator-vs-vision — VISION §1 "不是播放器"字面读法 vs 引擎级播放会话

VISION §1 北极星原句"**不是 NLE，不是 compositor GUI，不是播放器。是一个被调用的库**"和 §4 不做清单的"实时预览 UI"在字面上把任何带时钟 / 状态机的 orchestrator 一起否决了。但实践里只要宿主想做 timeline preview，**A/V sync 必须由引擎承担**（音频是 master clock 的天然位置，video 跟随——拆给宿主必须暴露半内部接口、或承担漂移）。本 cycle 把这条规则改写成"不画 surface，但承担会话语义"，新增执行模型 (c)。**规则压力来源：** VISION §1 / §4 原措辞把"UI surface"和"会话语义"两件事捆在一句里，撕开后才能让 Player 与"不做编辑器"的真正红线共存。**方向 / 权衡：** 未来再有同类需求（例如网络流式预览、低延迟 frame-server-with-sync）时，应直接复用这次确立的"surface 在外、会话在内"分界，而不是再次推翻北极星。代价是 Player 一旦让"内部时钟"住进引擎，确定性回归测试只能继续锁在 Exporter 路径——Player 自己**不**承诺 bit-identical（壁钟驱动 = 非确定）。

注：本条目原版用 `Previewer` / `CompositionThumbnailer` 两个 C++ 类作为"两条路径"的样本。后续审计发现这两个类各自只有一个方法、零跨调用状态——本质是带 `(engine*, tl)` 两个成员的函数，包成类纯属过度设计。已扁掉为 `src/orchestrator/compose_frame.{hpp,cpp}` 里的自由函数（`resolve_active_clip_at` / `compile_frame_graph` / `compose_frame_at` / `compose_png_at`）。`me_render_frame` / `Player::producer_loop` / `compose_png_at` 三家直接调自由函数，路径 (a) 不再有 orchestrator 类。这次扁化没有打破前面的 surface vs 会话分界——只是把之前的"两个壳"合并成"一组函数"，路径 (a) 与路径 (c) 的分界（无状态 vs 有状态 + 内部时钟）依然清晰。
