/*
 * Asset content hashing — SHA-256 over raw bytes or streamed file content.
 *
 * Returns 64-char lowercase hex (no "sha256:" prefix). Callers that need
 * the schema prefix prepend it themselves. Errors surface via *err; the
 * return value is empty string on failure.
 *
 * Implementation uses libavutil's AVSHA (LGPL, already in link graph —
 * no new dependencies).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace me::resource {

/* Hex-encode SHA-256 of the given byte range. Never fails for valid inputs;
 * returned string has length 64. */
std::string sha256_hex(const uint8_t* data, size_t len);

/* Stream the file at `path` (or `file://...` URI) through SHA-256 and return
 * its hex digest. On open/read failure, returns empty string and writes a
 * diagnostic to *err when non-null. */
std::string sha256_hex_streaming(std::string_view path_or_uri,
                                  std::string* err = nullptr);

}  // namespace me::resource
