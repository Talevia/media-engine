#include "media_engine/types.h"

#include "core/version.inl"

extern "C" me_version_t me_version(void) {
    return me_version_t{ME_VERSION_MAJOR, ME_VERSION_MINOR, ME_VERSION_PATCH,
                        ME_GIT_SHA};
}

extern "C" const char* me_status_str(me_status_t status) {
    switch (status) {
        case ME_OK:              return "ok";
        case ME_E_INVALID_ARG:   return "invalid argument";
        case ME_E_OUT_OF_MEMORY: return "out of memory";
        case ME_E_IO:            return "i/o error";
        case ME_E_PARSE:         return "parse error";
        case ME_E_DECODE:        return "decode error";
        case ME_E_ENCODE:        return "encode error";
        case ME_E_UNSUPPORTED:   return "unsupported";
        case ME_E_CANCELLED:     return "cancelled";
        case ME_E_NOT_FOUND:     return "not found";
        case ME_E_INTERNAL:      return "internal error";
    }
    return "unknown";
}
