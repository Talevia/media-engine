/*
 * Task kinds — schema + kernel registration.
 *
 * Every Node has a TaskKindId that selects a registered KindInfo, which
 * carries the kernel function pointer, I/O port schema, param schema,
 * and scheduling hints (affinity / latency / time_invariant).
 *
 * See docs/ARCHITECTURE_GRAPH.md §Task 运行时与 Kernel 注册.
 */
#pragma once

#include "graph/types.hpp"
#include "media_engine/types.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace me::task {

/* Dense kind enum. Values are stable (registration uses them as keys).
 * Append new kinds at the end; never reorder. */
enum class TaskKindId : uint32_t {
    /* Bootstrap test kinds — used by graph-task-bootstrap smoke tests; will
     * remain useful for future unit tests. */
    TestConstInt   = 0x0001,
    TestAddInt     = 0x0002,
    TestEchoString = 0x0003,

    /* Real kinds — added by dedicated backlog items. */
    IoDemux            = 0x1001,
    IoDecodeVideo      = 0x1002,
    IoDecodeAudio      = 0x1003,

    AlgoContentHash    = 0x2001,
    AlgoThumbnail      = 0x2002,

    RenderComposeCpu   = 0x3001,
    RenderCrossDissolve= 0x3002,
    RenderEffectChainGpu = 0x3003,
    RenderConvertRgba8 = 0x3004,
    RenderAffineBlit   = 0x3005,
    RenderEncodePng    = 0x3006,
    RenderFaceSticker  = 0x3007,
    RenderFaceMosaic   = 0x3008,
    RenderBodyAlphaKey = 0x3009,

    AudioMix           = 0x4001,
    AudioResample      = 0x4002,
    AudioTimestretch   = 0x4003,
};

enum class Affinity : uint8_t { Cpu, Gpu, HwDecoder, HwEncoder, Io };
enum class Latency  : uint8_t { Short, Medium, Long };

/* Parameter declarations for a kind. Kernels read params by key from
 * graph::Properties. */
struct ParamDecl {
    std::string name;
    graph::TypeId type = graph::TypeId::Empty;
    /* Optional default provided at registration time; left empty if required. */
    graph::InputValue default_value{};
};

/* Forward decl of TaskContext — full definition in context.hpp. */
struct TaskContext;

using KernelFn = me_status_t (*)(TaskContext&                   ctx,
                                 const graph::Properties&       props,
                                 std::span<const graph::InputValue> inputs,
                                 std::span<graph::OutputSlot>   outputs);

struct KindInfo {
    TaskKindId              kind{};
    Affinity                affinity      = Affinity::Cpu;
    Latency                 latency       = Latency::Medium;
    bool                    time_invariant = false;
    /* Whether the kernel's outputs are safe to cache + replay.
     * Pure functional kernels (decode-frame, color-correct, conv-rgba8) are
     * cacheable. Kernels that emit handles to externally-mutable runtime
     * state (e.g. an opened AVFormatContext whose read pointer advances)
     * must mark this false — sharing the handle across evaluations would
     * leak state between callers. Default true matches the common case. */
    bool                    cacheable     = true;
    /* If true, the LAST entry in input_schema describes a repeating port:
     * the Builder accepts ≥ (input_schema.size() - 1) actual input refs,
     * with all repeats sharing the schema entry's type. Used for kernels
     * like RenderComposeCpu where the layer count is timeline-driven. */
    bool                    variadic_last_input = false;
    KernelFn                kernel        = nullptr;
    std::vector<graph::Port> input_schema;
    std::vector<graph::Port> output_schema;
    std::vector<ParamDecl>   param_schema;
};

}  // namespace me::task
