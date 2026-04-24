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
