/*
 * Core graph data types — pure data, no logic.
 *
 * See docs/ARCHITECTURE_GRAPH.md §Graph 内部 for the contract.
 */
#pragma once

#include "media_engine/types.h"

#include <cstdint>
#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

struct AVFrame;

namespace me::graph { struct InputValue; }
namespace me::resource { class FrameHandle; }
namespace me::io       { class DemuxContext; }

namespace me::graph {

/* RgbaFrameData — tightly-packed RGBA8 frame carried as a graph value.
 * Mirrors the public `me_frame` shape (width / height / stride / bytes)
 * but lives in the internal graph layer so kernels can produce + consume
 * it without dragging in the C ABI. The orchestrator wraps this into
 * `me_frame` at the boundary. */
struct RgbaFrameData {
    int width  = 0;
    int height = 0;
    std::size_t stride = 0;          /* row stride in bytes — width * 4 for tightly packed */
    std::vector<uint8_t> rgba;       /* size == stride * height */
};

/* Node index within a Graph::Builder / Graph. Stable through build(). */
struct NodeId {
    uint32_t v = 0;
    constexpr bool operator==(const NodeId&) const = default;
};

/* Reference to a specific output port of a specific node. */
struct PortRef {
    NodeId  node{};
    uint8_t port_idx = 0;
    constexpr bool operator==(const PortRef&) const = default;
};

/* Typed tag for input/output values. Mirrors the order of the InputValue
 * variant so index-to-type is 1:1 and hashable.
 *
 * APPEND-ONLY: never reorder. Adding new types appends at the end and
 * extends the variant in lock-step. */
enum class TypeId : uint8_t {
    Empty         = 0,   /* monostate */
    Int64         = 1,
    Float64       = 2,
    Bool          = 3,
    String        = 4,
    Frame         = 5,   /* resource::FrameHandle */
    DemuxCtx      = 6,   /* io::DemuxContext — packet stream source */
    AvFrameHandle = 7,   /* shared_ptr<AVFrame> with libav-aware deleter — decoded raw frame */
    RgbaFrame     = 8,   /* shared_ptr<RgbaFrameData> — tightly-packed RGBA8 frame */
    ByteBuffer    = 9,   /* shared_ptr<vector<uint8_t>> — opaque encoded bytes (PNG, JPEG, ...) */
    /* AudioBuf / MetaBlob will be appended; never reordered. */
};

/* The typed value carried along a port. Kernels read inputs as InputValue
 * and write outputs into the same variant type. */
struct InputValue {
    std::variant<
        std::monostate,
        int64_t,
        double,
        bool,
        std::string,
        std::shared_ptr<resource::FrameHandle>,
        std::shared_ptr<io::DemuxContext>,
        std::shared_ptr<AVFrame>,
        std::shared_ptr<RgbaFrameData>,
        std::shared_ptr<std::vector<uint8_t>>
    > v;

    TypeId type() const noexcept {
        return static_cast<TypeId>(v.index());
    }
};

/* OutputSlot is structurally identical; named separately for API clarity. */
using OutputSlot = InputValue;

/* Typed, named port declarations — used by both Node (runtime instances) and
 * task::KindInfo (schema registration). */
struct Port {
    std::string name;    /* "video_in", "frame", "mask", … */
    TypeId      type = TypeId::Empty;
};

struct InputPort {
    std::string name;
    TypeId      type = TypeId::Empty;
    PortRef     source{};
};

struct OutputPort {
    std::string name;
    TypeId      type = TypeId::Empty;
};

/* Properties — typed map from string key to InputValue. Keys are sorted so
 * content_hash is order-stable across platforms and rebuilds. */
using Properties = std::map<std::string, InputValue>;

}  // namespace me::graph
