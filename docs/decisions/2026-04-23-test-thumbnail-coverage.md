## 2026-04-23 — test-thumbnail-coverage：`me_thumbnail_png` PNG 输出回归（Milestone §M1-debt · Rubric §5.2）

**Context.** `thumbnail-impl` 上线后 `me_thumbnail_png` 走 libavformat demux + 软解 + sws + libavcodec PNG encoder 全链路已经跑通，但 `tests/` 一条 doctest 都没有——唯一验证渠道是 `06_thumbnail` example 里"文件有没有产出来"的眼看。任何静默劣化（PNG magic 被改错、IHDR 尺寸误算、seek 精度漂移、上采样误当下采样处理等）都会滑过 CI。本 cycle 补 doctest 覆盖。

**Decision.** 新 `tests/test_thumbnail.cpp`，6 个 test case / 30 assertion，复用 `determinism_input.mp4` fixture（640×480 @ 25fps）：

1. **Native 尺寸 passthrough**（`max_width=0, max_height=0`）：产物 PNG magic 正确、IHDR W×H = 640×480。
2. **按 max_width=320 bound 缩放**：产物 320×240（等比例缩放）。
3. **按 max_height=120 bound 缩放**：产物 160×120。
4. **Bounding box 大于 native 不上采样**（`max=1280x1024`）：产物仍是 640×480。
5. **Non-existent URI → `ME_E_IO`**：`*out_png = nullptr` + `out_size = 0` + `me_engine_last_error` 含 `"avformat_open_input"` 子串。
6. **Null arg 拒绝**：null engine / null uri / null out_png / null out_size → `ME_E_INVALID_ARG`。

**PNG 解析策略.** 只断言 signature + IHDR 的 W×H，**不做 pixel-level compare**。理由：sws_scale 的 filter（默认 SWS_BILINEAR）在 FFmpeg minor 版本间会调整—同 host 同 libav 构建出的像素是稳定的，但跨 libav 版本 byte-identical 不成立。Signature + 尺寸足够捕捉"PNG 结构破了"和"缩放逻辑错了"两类 regression，这正是本轮 scope 要守的那两条线。PNG 头 parse 不引入 libpng / zlib / 任何第三方——PNG 规范 header 布局固定（signature 8 字节、IHDR chunk 起始在 offset 8、chunk payload 前 8 字节是 W×H 大端），16 行 helper 就够。

**Fixture 复用.** 和 `test_probe` 走同一套 `add_dependencies(test_thumbnail determinism_fixture)` + `ME_TEST_FIXTURE_MP4` 注入。集中的 fixture source-of-truth 让未来扩 fixture（tagged-color 变种、带 audio track 变种）时所有 consumer 自动跟进。

**Known-weak / 非本 cycle scope.**

- 只在 `time=0/1` 取帧——没有 seek 精度 / EOF 附近帧 / 中段帧的回归。`decode_first_frame_at_or_after` 对 timestamp 很敏感，未来值得加 `time=0.5s / 1.0s` 的覆盖，确保帧选的是对的。但本 bullet 的 `方向:` 只列了 PNG magic + IHDR W×H，保持与 bullet 对齐不扩 scope。
- 没断言解码到的**是哪一帧**（通过 pixel 或 hash 对比可以 pin 死，但跨 libav 版本脆）。留作"seek 精度"独立 bullet 的线。
- PNG 只跑默认压缩级别；未来如果加 `me_thumbnail_spec_t` 支持 quality 选项，compression level 路径也要覆盖。

**Alternatives considered.**

1. **完整 pixel-compare** 基于预录 expected PNG bytes——拒：跨 FFmpeg 版本脆（本轮测试的目标是稳定 CI，不是 snapshot regression testing）。
2. **引入 libpng 或 stb_image 做 parse**——拒：PNG magic + IHDR 布局是 8+16 字节的固定结构，引入 header-only lib 也比手写 16 行多了一个依赖 license 表需要的条目。
3. **把 thumbnail test 塞进 `test_determinism`** 作为 extra case——拒：`test_determinism` 专注 byte-equality；thumbnail 按 bullet 明确不 pixel-compare，混进去语义会乱。独立 suite 更干净，ctest 日志也是"哪个功能 regression" 一眼可见。
4. **走 `06_thumbnail` example shell-out + grep**——拒：上轮 probe coverage 同理由拒过，依赖 stdout 格式比直接 asser t 返回值脆。

业界共识来源：测试"产物格式正确性" 按"structural invariants → 字节比对"分层，是 libpng fate suite 和 OpenImageIO 的 regression test 的通用写法。Signature + IHDR 是 PNG 结构 invariants 的最小正交子集。

**Coverage.**

- `cmake --build build` 与 `-Werror` clean。
- `ctest --test-dir build` 9/9 suite 绿（新加 test_thumbnail）。
- `build/tests/test_thumbnail -s` 6 case / 30 assertion / 0 skip / 0 fail。
- 不动 src/，其他 8 个 suite 继续绿。

**License impact.** 无新依赖。

**Registration.** 无 C API / schema / kernel 注册变更。CMake 侧：
- `tests/CMakeLists.txt` 的 `_test_suites` 列表末尾加 `test_thumbnail`。
- `add_dependencies(test_thumbnail determinism_fixture)` + `target_compile_definitions(test_thumbnail PRIVATE ME_TEST_FIXTURE_MP4=...)`。
