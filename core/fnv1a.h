#pragma once

// FNV-1a 64: the hash Roxal uses wherever content needs a cheap identity --
// fileio's content tags, the compute protocol's dependency checks, the names
// ai.nn deploys models to Triton under, and math.fnv1a64.  Fast and simple;
// not cryptographic.  (String interning hashes UTF-16 code units with its own
// variant, in Object.cpp.)

#include <cstddef>
#include <cstdint>

namespace roxal {

constexpr std::uint64_t fnv1a64Basis = 0xcbf29ce484222325ull;
constexpr std::uint64_t fnv1a64Prime = 0x100000001b3ull;

// The hash of `size` bytes, continuing from `hash` -- the basis to start, or
// an earlier result to hash further bytes after those.
inline std::uint64_t fnv1a64(const void* data, std::size_t size, std::uint64_t hash = fnv1a64Basis)
{
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= fnv1a64Prime;
    }
    return hash;
}

} // namespace roxal
