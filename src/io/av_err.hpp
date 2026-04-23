/*
 * av_err_str — single source of truth for wrapping `av_strerror` into
 * `std::string`.
 *
 * Every TU that talks to libav needs this 4-line helper. Before this
 * header existed, seven TUs each defined their own copy (some as
 * `av_err_str`, one as `av_err_to_string`) — see PR trail /
 * `debt-av-err-str-shared` decision for why that didn't scale.
 *
 * Kept as a header-only inline function: the body is 4 lines, the libav
 * header is already pulled in transitively by every caller that has a
 * status code to describe, and one extra symbol in the TU pays off by
 * eliminating the pattern from the project's vocabulary.
 */
#pragma once

extern "C" {
#include <libavutil/error.h>
}

#include <string>

namespace me::io {

inline std::string av_err_str(int rc) {
    char buf[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(rc, buf, sizeof(buf));
    return std::string{buf};
}

}  // namespace me::io
