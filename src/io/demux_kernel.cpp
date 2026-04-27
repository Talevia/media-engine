#include "io/demux_kernel.hpp"

#include "graph/eval_error.hpp"
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

std::string av_err_str_local(int rc) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(rc, buf, sizeof(buf));
    return std::string{buf};
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

    /* Throw EvalError with the libav error string instead of returning
     * a bare ME_E_IO — callers (api/thumbnail.cpp, api/render.cpp) catch
     * EvalError to recover both the status code AND a descriptive
     * message including "avformat_open_input(...)" so the test
     * test_thumbnail.cpp:178 ("last_error contains avformat_open_input")
     * can match. The scheduler's catch records both fields onto the
     * EvalInstance; Future::await re-throws an EvalError carrying them. */
    int rc = avformat_open_input(&ctx->fmt, path.c_str(), nullptr, nullptr);
    if (rc < 0) {
        throw graph::EvalError(ME_E_IO,
            "avformat_open_input(\"" + path + "\") failed: " + av_err_str_local(rc));
    }

    rc = avformat_find_stream_info(ctx->fmt, nullptr);
    if (rc < 0) {
        throw graph::EvalError(ME_E_DECODE,
            "avformat_find_stream_info(\"" + path + "\") failed: " + av_err_str_local(rc));
    }

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
        /* DemuxContext wraps an AVFormatContext whose read pointer
         * advances as packets are pulled. Sharing one across
         * evaluations would replay a half-consumed demuxer to the
         * second consumer; each evaluate_port must open a fresh one. */
        .cacheable      = false,
        .kernel         = demux_kernel,
        .input_schema   = {},
        .output_schema  = { {"source", graph::TypeId::DemuxCtx} },
        .param_schema   = { {.name = "uri", .type = graph::TypeId::String} },
    };
    task::register_kind(info);
}

}  // namespace me::io
