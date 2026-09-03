#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/memory.h"

namespace roxal {

class Chunk;
struct ChunkBreakpoints;

// Source breakpoints over the immutable-bytecode runtime: requests live by
// stable source identity, bind to the first executable statement at-or-after
// the requested line across every function the ModuleDebugIndex holds for
// that source, and are editable AT ANY TIME -- running included -- through
// the ChunkBreakpoints seqlock protocol.
// Re-registration of a source (reload/recompile) rebinds outstanding
// requests to the new generation via the index registration hook.
class BreakpointManager {
public:
    static BreakpointManager& instance();

    struct Bound {
        int requestedLine { 0 };
        int boundLine { 0 };       // adjusted (first stmt at-or-after)
        bool verified { false };   // false: source unknown / no such line
    };

    // DAP setBreakpoints semantics: REPLACE the full set for one source.
    // `source` may be a stable key or a raw path (canonicalized).
    std::vector<Bound> setBreakpoints(const std::string& source,
                                      const std::vector<int>& lines);

    // Called by the ModuleDebugIndex after (re)registration of a source, so
    // stored requests rebind to the new generation.
    void onSourceRegistered(const std::string& sourceKey);

    void clearAllBreakpoints();

private:
    BreakpointManager() = default;

    // Bind stored requests for one source key; returns the bound set.
    std::vector<Bound> bindLocked(const std::string& sourceKey);
    ChunkBreakpoints* ensureChunkWords(Chunk* chunk);
    void updateSlowPathDemand();

    std::mutex mutex_;
    std::unordered_map<std::string, std::vector<int>> requests_;   // key -> lines
    // Chunks armed per source (so a full-set replace clears stale words; a
    // chunk belongs to exactly one source).  STRONG refs: index
    // re-registration retires the old generation's GC roots BEFORE the
    // rebind hook runs, so a raw pointer could be cleared after the chunk
    // was reclaimed.  Chunks are plain shared-ptr-owned C++ (their
    // GC-object constants are never read here), so a strong ref is a safe,
    // categorical lease.
    std::unordered_map<std::string, std::vector<ptr<Chunk>>> armedBySource_;
    // Storage ownership: never freed before clearAll/shutdown (readers hold
    // only the raw pointer published into the chunk).
    std::vector<std::unique_ptr<ChunkBreakpoints>> storage_;
    bool slowPathDemandHeld_ { false };
    int enabledCount_ { 0 };
};

} // namespace roxal
