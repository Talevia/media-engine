## 2026-04-23 — test-probe-coverage：`me_probe` + accessor doctest 覆盖（Milestone §M1-debt · Rubric §5.2）

**Context.** 最近几个 cycle 里 `me_probe` 从 stub 变成真实装（M1 早期），又在 `me-probe-more-fields` cycle 新增 6 个 `me_media_info_video_*` accessor（rotation / color_range / primaries / transfer / space / bit_depth）。全部改动**只靠** `examples/04_probe` 端到端 print 手测 + 对比 ffprobe。没有 doctest 覆盖意味着：

- refactor probe.cpp 的解码分支或 accessor 结构体字段时，字段 regression / 返回空串 / 返回 garbage 都会**静默滑过 CI**。
- null-safety 合约（"accessor 接受 nullptr 退回 documented default"）没有断言，未来改动可能加 deref 而不报警。
- 错误路径（non-existent URI, null args）的 status 码 + last_error 填充行为没 pin 住。

现在 API 已经稳定，补齐基础 doctest 覆盖，把"手测通过"变成"CI 守护"。

**Decision.** 新增 `tests/test_probe.cpp`，5 个 test case / 45 assertion，全部走 `gen_fixture` 产的 `determinism_input.mp4`（640×480 @ 25fps MPEG-4 Part 2, no audio, BITEXACT-encoded 所以 color tags 都是 UNSPECIFIED）——这个 fixture **刚好**是 probe 覆盖的理想 baseline：每个字段都有可预测的 expected value（video=mpeg4, W×H=640×480, fps=25, bit_depth=8, rotation=0, 所有 color 字段=`"unknown"`，has_audio=0）。

Test cases：

1. **容器 + codec + 尺寸 + 帧率**：断言 container=`"mov"`、video_codec=`"mpeg4"`、W×H=640×480、fr 约简到 25/1 ratio（允许 libav 重写到等价 50/2 之类）、has_video=1、has_audio=0。
2. **扩展视频字段**：rotation=0、bit_depth=8（YUV420P pix desc `comp[0].depth`）、4 个 color 字段 = `"unknown"`（`av_color_*_name` 对 UNSPECIFIED enum 返回的字符串）。
3. **不存在 URI 的 IO 错误**：`me_probe(..., "file:///nonexistent/...", &info)` 返回 `ME_E_IO`、`*info` 被清成 `nullptr`（`API.md` 合约）、`me_engine_last_error` 包含 `"avformat_open_input"` 子串。
4. **null 参数 invalid_arg**：engine null / uri null / out null 三种各自返回 `ME_E_INVALID_ARG`。
5. **accessor null-tolerance**：所有 15 个 `me_media_info_*` accessor 吃 `nullptr` → 返回 documented default（0 int / `{0,1}` rational / `""` c-str）。

**Fixture 复用策略.** `test_probe` 共用 `determinism_fixture` target（`add_dependencies(test_probe determinism_fixture)` + `ME_TEST_FIXTURE_MP4` 宏）。方案 alternatives：

1. **生成第二个 color-tagged fixture** 覆盖 `color_range="tv"` / `color_space="bt709"` 等非 unknown 路径——拒：要扩 `gen_fixture` 签名吃参数，或者写第二个 `gen_fixture_tagged`。本 cycle 不扩 scope，留作 follow-up（下面 "Known-weak" 章节）。
2. **shell out 给 ffmpeg CLI 生成 tagged fixture**——拒：正是 `debt-fixture-gen-libav` cycle 刚消除的依赖，不能再引入。
3. **只跑 assertion-against-fixture，不测 null / error 路径**——拒：这两条是 C API 稳定性的核心合约，手测没覆盖、也不会被其他测试间接覆盖。

**Known-weak（留作 follow-up 而非本 cycle 的 scope）.**

- 只验证 `"unknown"` 路径的 color 字段；`"tv"` / `"bt709"` / `"smpte2084"` 等实际 tag 值没断言。那条路径被 `04_probe /tmp/h264tagged.mp4` 手测过（写在 `2026-04-23-me-probe-more-fields` decision 里）但没进 CI。下一个 cycle 可以扩 `gen_fixture` 加 `-tagged` 模式或加新 helper。
- 只覆盖 video；audio 字段（sample_rate, channels, codec）因为 fixture 无音轨没验证。Audio 覆盖随 M2 `audio-mix-two-track` 落地自然出现。
- 没测 rotation 非 0 的路径。未来可加 `gen_fixture --display-matrix=90` 模式。

这些 known-weak 项不进 backlog（不是 debt，是"scope 的明确边界"），在本 decision 里记录避免下次 repopulate 把它们当成缺口重报。

**Alternatives considered.**

1. **把 test 写进 `test_determinism.cpp`**——拒：`test_determinism` 专注 byte-equality tripwire，把 accessor 合约测试塞进去会让一个文件跨两个关注点。独立 suite 更清晰，ctest 输出也能一眼看出是 probe regression 还是 determinism regression。
2. **直接跑 `04_probe` example 然后 grep 输出**——拒：依赖 `04_probe` 的 print 格式（容易变），且 grep-based 验证比直接 assert accessor 返回值脆弱。
3. **用 gmock 或独立 fixture struct**——拒：doctest 已经是项目的 test framework；引入新框架浪费。简单 RAII handle（`EngineHandle` / `InfoHandle`）够了。

业界共识来源：doctest / Catch2 / GoogleTest 生态里 "C API + opaque handle" 类库的标准测试模式就是 `SetUp` RAII + `CHECK` 逐字段，与 sqlite3 testsuite / curl testsuite 一致。

**Coverage.**

- `cmake --build build` `-Werror` clean。
- `ctest --test-dir build` 8/8 suite 绿（新加 test_probe）。
- `build/tests/test_probe -s` 5 case / 45 assertion / 0 skip / 0 fail，显式 verbose 确认每条断言都真跑。
- 无代码改动触动 src/，其他 7 个 suite 继续绿。

**License impact.** 无新依赖。沿用 doctest + media_engine。

**Registration.** 无 C API / schema / kernel 注册变更。CMake 侧：
- `tests/CMakeLists.txt` 的 `_test_suites` 列表加 `test_probe`。
- `test_probe` 目标新加 `add_dependencies(... determinism_fixture)` + `target_compile_definitions(... ME_TEST_FIXTURE_MP4=...)`。
