#include "DebugInfo.h"

#include <algorithm>
#include <istream>
#include <ostream>
#include <stdexcept>
#include <string>

namespace roxal {

namespace {

void writeU32(std::ostream& out, uint32_t v) {
    out.write(reinterpret_cast<const char*>(&v), 4);
}
void writeI32(std::ostream& out, int32_t v) {
    out.write(reinterpret_cast<const char*>(&v), 4);
}
void writeU16(std::ostream& out, uint16_t v) {
    out.write(reinterpret_cast<const char*>(&v), 2);
}
void writeU8(std::ostream& out, uint8_t v) {
    out.write(reinterpret_cast<const char*>(&v), 1);
}
void writeUS(std::ostream& out, const ustring& us) {
    std::string s;
    us.toUTF8String(s);
    writeU32(out, static_cast<uint32_t>(s.size()));
    out.write(s.data(), static_cast<std::streamsize>(s.size()));
}

uint32_t readU32(std::istream& in) {
    uint32_t v = 0;
    in.read(reinterpret_cast<char*>(&v), 4);
    return v;
}
int32_t readI32(std::istream& in) {
    int32_t v = 0;
    in.read(reinterpret_cast<char*>(&v), 4);
    return v;
}
uint16_t readU16(std::istream& in) {
    uint16_t v = 0;
    in.read(reinterpret_cast<char*>(&v), 2);
    return v;
}
uint8_t readU8(std::istream& in) {
    uint8_t v = 0;
    in.read(reinterpret_cast<char*>(&v), 1);
    return v;
}

constexpr uint32_t MaxNameBytes = 1u << 16;   // 64KB per identifier: generous

ustring readUS(std::istream& in) {
    uint32_t len = readU32(in);
    if (!in || len > MaxNameBytes)
        throw std::runtime_error("DebugInfo: malformed string length");
    std::string s(len, '\0');
    if (len)
        in.read(s.data(), len);
    if (!in)
        throw std::runtime_error("DebugInfo: truncated string");
    return ustring::fromUTF8(s);
}

} // namespace

int32_t debugLocateStatement(const DebugInfo& di, uint32_t off)
{
    auto it = std::upper_bound(di.stmts.begin(), di.stmts.end(), off,
        [](uint32_t o, const DebugStmtEntry& e) { return o < e.offset; });
    if (it == di.stmts.begin())
        return -1;
    return int32_t((it - di.stmts.begin()) - 1);
}

void DebugInfo::serialize(std::ostream& out) const
{
    writeU32(out, static_cast<uint32_t>(stmts.size()));
    for (const auto& s : stmts) {
        writeU32(out, s.offset);
        writeI32(out, s.line);
        writeI32(out, s.column);
        writeU8(out, s.kind);
    }
    writeU32(out, static_cast<uint32_t>(locals.size()));
    for (const auto& l : locals) {
        writeUS(out, l.name);
        writeU16(out, l.slot);
        writeU32(out, l.startOffset);
        writeU32(out, l.endOffset);
        writeU8(out, l.flags);
        writeUS(out, l.typeName);
    }
    writeU32(out, static_cast<uint32_t>(upvalues.size()));
    for (const auto& u : upvalues) {
        writeUS(out, u.name);
        writeU16(out, u.index);
        writeU8(out, u.isLocal);
    }
}

void DebugInfo::deserialize(std::istream& in, uint32_t codeSize)
{
    // Statement entries are offset-deduped at emission (at most one
    // StatementStart per code offset, plus a FunctionEntry), so code size
    // bounds them.  Locals and upvalues are bounded by the COMPILER's
    // per-function limits (255 locals + reserved slot 0; 256 upvalues) --
    // NEVER by code size: a Release-build function can have many parameters
    // and almost no bytecode; a code-size-based bound would reject its own
    // valid cache and silently recompile every run.
    const uint32_t maxStmts = codeSize + 16;
    const uint32_t maxLocals = 512;
    const uint32_t maxUpvalues = 256;
    constexpr uint8_t knownLocalFlags =
        DebugLocalParam | DebugLocalCaptured | DebugLocalConst
        | DebugLocalTypeConst | DebugLocalSynthetic;

    uint32_t stmtCount = readU32(in);
    if (!in || stmtCount > maxStmts)
        throw std::runtime_error("DebugInfo: malformed statement count");
    stmts.clear();
    stmts.reserve(stmtCount);
    uint32_t prevOffset = 0;
    for (uint32_t i = 0; i < stmtCount; ++i) {
        DebugStmtEntry e;
        e.offset = readU32(in);
        e.line = readI32(in);
        e.column = readI32(in);
        e.kind = readU8(in);
        if (!in || e.offset > codeSize
            || e.kind > uint8_t(DebugStmtKind::FunctionEntry)
            || e.line < 0 || e.column < 0
            || e.offset < prevOffset)   // emission appends in code order
            throw std::runtime_error("DebugInfo: malformed statement entry");
        prevOffset = e.offset;
        stmts.push_back(e);
    }

    uint32_t localCount = readU32(in);
    if (!in || localCount > maxLocals)
        throw std::runtime_error("DebugInfo: malformed local count");
    locals.clear();
    locals.reserve(localCount);
    for (uint32_t i = 0; i < localCount; ++i) {
        DebugLocalVarInfo l;
        l.name = readUS(in);
        l.slot = readU16(in);
        l.startOffset = readU32(in);
        l.endOffset = readU32(in);
        l.flags = readU8(in);
        l.typeName = readUS(in);
        if (!in || l.slot >= maxLocals
            || l.startOffset > codeSize || l.endOffset > codeSize
            || l.startOffset > l.endOffset
            || (l.flags & ~knownLocalFlags) != 0)
            throw std::runtime_error("DebugInfo: malformed local entry");
        locals.push_back(std::move(l));
    }

    uint32_t upCount = readU32(in);
    if (!in || upCount > maxUpvalues)
        throw std::runtime_error("DebugInfo: malformed upvalue count");
    upvalues.clear();
    upvalues.reserve(upCount);
    for (uint32_t i = 0; i < upCount; ++i) {
        DebugUpvalueInfo u;
        u.name = readUS(in);
        u.index = readU16(in);
        u.isLocal = readU8(in);
        if (!in || u.index >= maxUpvalues || u.isLocal > 1)
            throw std::runtime_error("DebugInfo: malformed upvalue entry");
        upvalues.push_back(std::move(u));
    }
}

} // namespace roxal
