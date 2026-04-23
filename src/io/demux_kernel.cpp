#include "io/demux_kernel.hpp"

#include "graph/types.hpp"
#include "io/demux_context.hpp"
#include "task/context.hpp"
#include "task/registry.hpp"
#include "task/task_kind.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/error.h>
}

#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace me::io {

namespace {

std::string strip_file_scheme(const std::string& uri) {
    constexpr std::string_view p{"file://"};
    if (uri.size() >= p.size() &&
        std::equal(p.begin(), p.end(), uri.begin())) {
        return uri.substr(p.size());
    }
    return uri;
}

me_status_t demux_kernel(task::TaskContext&,
                         const graph::Properties& props,
                         std::span<const graph::InputValue>,
                         std::span<graph::OutputSlot> outs) {
    auto it = props.find("uri");
    if (it == props.end() ||
        !std::holds_alternative<std::string>(it->second.v)) {
        return ME_E_INVALID_ARG;
    }
    const std::string uri  = std::get<std::string>(it->second.v);
    const std::string path = strip_file_scheme(uri);

    auto ctx = std::make_shared<DemuxContext>();
    ctx->uri = uri;

    int rc = avformat_open_input(&ctx->fmt, path.c_str(), nullptr, nullptr);
    if (rc < 0) return ME_E_IO;

    rc = avformat_find_stream_info(ctx->fmt, nullptr);
    if (rc < 0) return ME_E_DECODE;

    outs[0].v = std::move(ctx);
    return ME_OK;
}

}  // namespace

void register_demux_kind() {
    task::KindInfo info{
        .kind           = task::TaskKindId::IoDemux,
        .affinity       = task::Affinity::Io,   /* blocking I/O */
        .latency        = task::Latency::Medium,
        .time_invariant = true,                  /* opening a file is idempotent per URI */
        .kernel         = demux_kernel,
        .input_schema   = {},
        .output_schema  = { {"source", graph::TypeId::DemuxCtx} },
        .param_schema   = { {.name = "uri", .type = graph::TypeId::String} },
    };
    task::register_kind(info);
}

}  // namespace me::io
