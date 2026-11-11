#pragma once

// Triton's BYTES tensor contents, with no Roxal values and no protocol
// library: what ai.nn's front end encodes a predict's string and byte inputs
// with, and its Triton backend splits a reply's outputs by.

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace roxal::nn {

// A BYTES tensor's raw contents are its elements, in order, each a 4-byte
// little-endian length followed by that many bytes.

// Append one element.  Throws std::length_error past 4 GB.
void appendBytesElement(std::string& out, const void* data, size_t size);

// The elements of a BYTES tensor's raw contents, as (offset, size) pairs into
// `data`.  False when the contents are malformed: a length cut short, or one
// running past the end.
bool splitBytesElements(const void* data, size_t size,
                        std::vector<std::pair<size_t, size_t>>& elements);

// Whether `data` is well-formed UTF-8: no overlong forms, surrogates, code
// points past U+10FFFF, or truncated sequences.
bool isValidUtf8(const void* data, size_t size);

} // namespace roxal::nn
