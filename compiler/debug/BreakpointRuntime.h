#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

namespace roxal {

// Per-chunk, NON-SERIALIZED breakpoint state: bytecode stays immutable;
// breakpoints live in one atomic word per statement-table entry.
//
// Edit protocol (seqlock): a writer bumps `seq` to odd, updates words, then
// publishes even.  The dispatch-loop reader samples seq around its word
// read and claims a hit only from a stable, even snapshot -- so edits are
// legal AT ANY TIME (running included) with no code mutation and no
// stop-to-patch cycle.  While seq is odd a crossing may miss the edit in
// flight; the setter's response is sent only after the even publication.
//
// Lifetime: allocated once per chunk by the BreakpointManager (which owns
// the storage) and published into Chunk::breakpointsRaw with a release
// store; readers acquire-load the raw pointer.  Storage is never freed
// before manager clearAll/shutdown, so a dying chunk simply strands an
// unused allocation until then.
struct ChunkBreakpoints {
    explicit ChunkBreakpoints(size_t stmtCount)
        : words(new std::atomic<uint32_t>[stmtCount]), count(stmtCount)
    {
        for (size_t i = 0; i < stmtCount; ++i)
            words[i].store(0, std::memory_order_relaxed);
    }

    // bit 0: breakpoint enabled at this statement index
    static constexpr uint32_t Enabled = 1u << 0;

    std::unique_ptr<std::atomic<uint32_t>[]> words;
    size_t count { 0 };
    std::atomic<uint32_t> seq { 0 };   // odd while an edit is in progress
};

} // namespace roxal
