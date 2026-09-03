#pragma once

#include <cstdint>
#include <iosfwd>
#include <vector>

#include <core/types.h>

namespace roxal {

// Per-Chunk source-debugging metadata.  Deliberately Value-free (plain
// data only) so it needs no GC involvement and can be serialized with the
// chunk into module caches, compute payloads and generic serialization.
// Nullable on Chunk: tier 0 (no debug info) serializes as a single 0 byte.
//
// Coordinates are code offsets into Chunk::code; slots are frame slots
// (slot 0 = callee/this, 1..arity = parameters, then body locals -- the
// same numbering GetLocal/SetLocal operands use, since the compiler
// reserves locals[0]).

enum class DebugStmtKind : uint8_t {
    StatementStart = 0,   // first byte of a block-level statement
    FunctionEntry  = 1,   // offset 0 of every function chunk
};

struct DebugStmtEntry {
    uint32_t offset { 0 };
    int32_t line { 0 };
    int32_t column { 0 };
    uint8_t kind { 0 };   // DebugStmtKind
};

// LocalVarInfo flags
constexpr uint8_t DebugLocalParam     = 1u << 0;
constexpr uint8_t DebugLocalCaptured  = 1u << 1;
constexpr uint8_t DebugLocalConst     = 1u << 2;
constexpr uint8_t DebugLocalTypeConst = 1u << 3;
constexpr uint8_t DebugLocalSynthetic = 1u << 4;  // compiler-generated (__iterable__ etc.);
                                                  // debugger UIs hide these by default

struct DebugLocalVarInfo {
    ustring name;
    uint16_t slot { 0 };
    uint32_t startOffset { 0 };   // first offset at which the slot is live/initialized
    uint32_t endOffset { 0 };     // one past the last live offset
    uint8_t flags { 0 };
    ustring typeName;             // declared type display name; empty = untyped
};

struct DebugUpvalueInfo {
    ustring name;
    uint16_t index { 0 };
    uint8_t isLocal { 0 };        // captures enclosing local (vs enclosing upvalue)
};

// Index of the statement containing code offset `off` (the LAST entry with
// offset <= off, which lands on the StatementStart when a FunctionEntry
// shares offset 0), or -1 when off precedes every entry / no entries.
struct DebugStmtEntry;
struct DebugInfo;
int32_t debugLocateStatement(const DebugInfo& di, uint32_t off);

struct DebugInfo {
    std::vector<DebugStmtEntry> stmts;        // offset-ordered
    std::vector<DebugLocalVarInfo> locals;
    std::vector<DebugUpvalueInfo> upvalues;

    void serialize(std::ostream& out) const;
    // Bounds-validated: throws std::runtime_error on malformed input
    // (counts/lengths sanity-checked against `codeSize`) rather than
    // allocating unbounded memory from a corrupt cache/payload.
    void deserialize(std::istream& in, uint32_t codeSize);
};

} // namespace roxal
