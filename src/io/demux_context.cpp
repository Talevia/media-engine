#include "io/demux_context.hpp"

extern "C" {
#include <libavformat/avformat.h>
}

namespace me::io {

DemuxContext::~DemuxContext() {
    if (fmt) {
        avformat_close_input(&fmt);
    }
}

}  // namespace me::io
