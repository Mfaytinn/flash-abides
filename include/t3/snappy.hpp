// Minimal Snappy block compressor. Emits a standard snappy stream: varint
// uncompressed-length preamble, then literal/copy elements (4-byte hash
// matching, 64 KiB window, copy2 tags). Any conformant decompressor reads
// it (pyarrow, C++ snappy, python-snappy).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace snappy {

// Appends the snappy-compressed form of in[0,n) to out.
void compress(const uint8_t *in, size_t n, std::string &out);

// Appends a valid Snappy stream holding in[0,n) as literal elements only (no
// match search).
void literal(const uint8_t *in, size_t n, std::string &out);

} // namespace snappy
