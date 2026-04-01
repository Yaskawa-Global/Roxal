#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace roxal {

// Compute protocol magic and version.
// Keep this independent from the module cache version because the wire shape
// can evolve without changing .roc serialization.
constexpr char ComputeMagic[4] = {'R', 'X', 'C', 'S'};
constexpr std::uint32_t ComputeVersion = 30;

// Default listen port for `roxal --server`
constexpr std::uint16_t ComputeDefaultPort = 26925;


// Message types
//
// Frame layout: [uint32_t payload_len][uint8_t msg_type][payload bytes...]
//
enum class ComputeMsg : std::uint8_t {
    Hello        = 0x01,  // client→server: magic(4) + version(4)
    HelloOk      = 0x02,  // server→client: (no payload)
    HelloErr     = 0x03,  // server→client: reason string
    SpawnActor   = 0x10,  // client→server: call_id(8) + dependency preamble + serialized ObjObjectType + init call spec + serialized init-args
    SpawnResult  = 0x11,  // server→client: call_id(8) + ok(1) + actor_id(8) | error string
    CallMethod   = 0x20,  // either dir:   call_id(8) + actor_id(8) + method_name_len(4) + method_name + serialized args
    CallResult   = 0x21,  // either dir:   call_id(8) + ok(1) + serialized result | error string
    PrintOutput  = 0x22,  // either dir:   call_id(8) + flush(1) + text string
    ActorDropped = 0x30,  // either dir:   actor_id(8)
    Bye          = 0xFF,  // either dir:   clean shutdown
};


// --- Low-level framing helpers ---
// These operate on byte vectors and are used by ComputeConnection for
// building/parsing wire frames.  They do NOT touch the socket directly.

// Append a uint8_t to a byte buffer
inline void writeU8(std::vector<std::uint8_t>& buf, std::uint8_t v) {
    buf.push_back(v);
}

// Append a uint32_t in network byte order (big-endian)
inline void writeU32(std::vector<std::uint8_t>& buf, std::uint32_t v) {
    buf.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
    buf.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    buf.push_back(static_cast<std::uint8_t>((v >>  8) & 0xFF));
    buf.push_back(static_cast<std::uint8_t>( v        & 0xFF));
}

// Append a uint64_t in network byte order (big-endian)
inline void writeU64(std::vector<std::uint8_t>& buf, std::uint64_t v) {
    for (int i = 56; i >= 0; i -= 8)
        buf.push_back(static_cast<std::uint8_t>((v >> i) & 0xFF));
}

inline std::uint64_t fnv1a64(const std::uint8_t* data, std::size_t len) {
    std::uint64_t hash = 1469598103934665603ull;
    for (std::size_t i = 0; i < len; ++i) {
        hash ^= static_cast<std::uint64_t>(data[i]);
        hash *= 1099511628211ull;
    }
    return hash;
}

inline std::uint64_t fnv1a64(const std::vector<std::uint8_t>& data) {
    return fnv1a64(data.data(), data.size());
}

// Append raw bytes
inline void writeBytes(std::vector<std::uint8_t>& buf,
                       const void* data, std::size_t len) {
    auto p = static_cast<const std::uint8_t*>(data);
    buf.insert(buf.end(), p, p + len);
}

// Append a length-prefixed string (uint32_t length + UTF-8 bytes)
inline void writeString(std::vector<std::uint8_t>& buf, const std::string& s) {
    writeU32(buf, static_cast<std::uint32_t>(s.size()));
    writeBytes(buf, s.data(), s.size());
}


// --- Reading helpers (from a pointer into a received buffer) ---

// Read a uint8_t, advancing the cursor
inline std::uint8_t readU8(const std::uint8_t*& p, const std::uint8_t* end) {
    if (p + 1 > end) throw std::runtime_error("ComputeProtocol: truncated u8");
    return *p++;
}

// Read a uint32_t (big-endian), advancing the cursor
inline std::uint32_t readU32(const std::uint8_t*& p, const std::uint8_t* end) {
    if (p + 4 > end) throw std::runtime_error("ComputeProtocol: truncated u32");
    std::uint32_t v = (static_cast<std::uint32_t>(p[0]) << 24)
                    | (static_cast<std::uint32_t>(p[1]) << 16)
                    | (static_cast<std::uint32_t>(p[2]) <<  8)
                    |  static_cast<std::uint32_t>(p[3]);
    p += 4;
    return v;
}

// Read a uint64_t (big-endian), advancing the cursor
inline std::uint64_t readU64(const std::uint8_t*& p, const std::uint8_t* end) {
    if (p + 8 > end) throw std::runtime_error("ComputeProtocol: truncated u64");
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v = (v << 8) | p[i];
    p += 8;
    return v;
}

// Read a length-prefixed string, advancing the cursor
inline std::string readString(const std::uint8_t*& p, const std::uint8_t* end) {
    auto len = readU32(p, end);
    if (p + len > end) throw std::runtime_error("ComputeProtocol: truncated string");
    std::string s(reinterpret_cast<const char*>(p), len);
    p += len;
    return s;
}


// Build a complete wire frame: [payload_len (4)][msg_type (1)][payload...]
inline std::vector<std::uint8_t> buildFrame(ComputeMsg type,
                                            const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> frame;
    frame.reserve(4 + 1 + payload.size());
    writeU32(frame, static_cast<std::uint32_t>(payload.size()));
    writeU8(frame, static_cast<std::uint8_t>(type));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

// Build a HELLO frame: magic(4) + version(4)
inline std::vector<std::uint8_t> buildHelloPayload() {
    std::vector<std::uint8_t> payload;
    writeBytes(payload, ComputeMagic, 4);
    writeU32(payload, ComputeVersion);
    return payload;
}

// Validate a HELLO payload, returning an error string (empty on success)
inline std::string validateHello(const std::uint8_t* data, std::size_t len) {
    if (len < 8)
        return "HELLO payload too short";
    if (std::memcmp(data, ComputeMagic, 4) != 0)
        return "bad magic (expected RXCS)";
    const std::uint8_t* p = data + 4;
    const std::uint8_t* end = data + len;
    auto version = readU32(p, end);
    if (version != ComputeVersion)
        return "version mismatch: expected " + std::to_string(ComputeVersion)
             + ", got " + std::to_string(version);
    return {};
}

} // namespace roxal
