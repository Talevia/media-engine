## 2026-04-23 — debt-output-sink-smoke-test：`make_output_sink` 直接断言（Milestone §M1-debt · Rubric §5.2）

**Context.** `me::orchestrator::make_output_sink`（`src/orchestrator/output_sink.cpp`）是 Exporter → `PassthroughSink` / `H264AacSink` 的唯一分类入口，负责 2 条支持 spec 的放行 + 4 条拒绝路径（null path、空 clip ranges、h264/aac + 多 clip、unsupported spec）。这些路径此前只被 `01_passthrough` / `05_reencode` 两个 end-to-end 例子**间接**覆盖——一个拒绝路径的 error string 意外改了、一个 spec 意外被放行到 process()，和"上游 timeline JSON 错"、"encoder 打不开" 等真正的运行期故障会共享同一条"render failed" 信号。过不了多久就会分不清 factory 层的 regression 和下游 encode 层的 regression。

**Decision.** 新增 `tests/test_output_sink.cpp`，6 个 doctest case：

- 2 个放行：passthrough spec、h264/aac single-clip spec，都 `sink != nullptr && err.empty()`。
- 4 个拒绝：`spec.path=nullptr`、`clip_ranges.empty()`、`video=h264 + audio=passthrough` 混搭、`h264/aac + 2 clips`。每个都断言 `sink == nullptr` + `err` 里能找到**特征子串**（"output.path"、"at least one clip"、"supported specs"、"single clip"）。

只断言 factory 返回值 + err 子串，**不调用 sink->process()**——process() 覆盖留给 01_passthrough / 05_reencode 的 integration path。拆解的理由：process() 需要真实 demux / codec / 磁盘 I/O，把 factory 层的 classification 契约和 I/O 副作用合在一起测试会让"哪层 regression" 难诊断；而 factory 本身足够轻，6 个 case 跑完 < 1 秒。

错误串对比用 `err.find(<substring>) != std::string::npos`——故意只 assert 关键 token（"output.path"、"at least one clip"…）而不是完整消息，这样未来改拒绝措辞时 test 不会因 cosmetic wording 意外挂掉，但**删掉这条语义**（比如把 "at least one clip" 改成 "clip_ranges must be non-empty" 再改成 "N > 0"）仍会被抓到。

`tests/CMakeLists.txt` 里把 `test_output_sink` 加进 `_test_suites` 列表；加一条 `target_include_directories(test_output_sink PRIVATE ${CMAKE_SOURCE_DIR}/src)`——`orchestrator/output_sink.hpp` 是 `src/` 下的内部头，复用 `test_content_hash` / `test_timeline_schema` 已有的 src-as-include 模式。

**Alternatives considered.**

1. **把 classification 逻辑从 output_sink.cpp 里抽成一个 pure function 再测它**——拒：额外拆一层抽象只为测试性，违反 CLAUDE.md "不 over-engineer" 的指导；现在的 `make_output_sink` 只有 30 行，直接测外观就够。
2. **让 `make_output_sink` 返回 `tl::expected<unique_ptr, ErrorCode>` 式的 typed error**——拒：`string*` 输出是仓库已建立的 internal-layer convention（demux/mux/reencode_pipeline 都用 `std::string*` err）。单独给 output_sink 换一层会让调用侧双标；换了就要全仓改。本 cycle 不扩 scope。
3. **端到端跑一次 process() 直到失败**（测 h264+多clip 真拒绝到 I/O 层）——拒：slow、需要真 fixture、跨 FFmpeg 版本脆；factory 层 gate 应该能独立断言。
4. **完全不测 factory，只留 integration 覆盖**——拒，就是这个 gap 本身要解决的。

业界共识来源：doctest 作者的 FAQ 明确建议 classification / parsing / predicate 层做 unit，I/O 侧做 integration——同一条断言别两层同时测；这条抽象边界跟 Google Test 社区的 "test the seams" 原则一致。

**Coverage.**

- 6 个新 test case / 12 个 assertion，`ctest --test-dir build` 7/7 suite 绿。
- Build 保持 `-Werror` clean。
- 未改 01_passthrough / 05_reencode / reencode_pipeline；integration 行为不动。

**License impact.** 无新依赖。沿用已有的 doctest（tests/CMakeLists.txt FetchContent）。

**Registration.** 无注册点变更：
- 无新 TaskKindId / kernel。
- 无新 CodecPool / Orchestrator factory。
- 无新 C API 函数。
- 无 JSON schema 字段。
- CMake 侧新增一个 test executable `test_output_sink`（tests/ 内部，不 `install()`）和一条 include-directories 声明。
