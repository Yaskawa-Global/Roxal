#include "BreakpointManager.h"

#include <filesystem>

#include "ModuleDebugIndex.h"
#include "StopCoordinator.h"
#include "../Chunk.h"
#include "../Object.h"
#include "../VM.h"

namespace roxal {

BreakpointManager& BreakpointManager::instance()
{
    static BreakpointManager m;
    static bool hooked = [] {
        // Rebind stored requests whenever a source (re)registers -- reload,
        // recompile, cache load.
        ModuleDebugIndex::setRegistrationHook([](const std::string& key) {
            BreakpointManager::instance().onSourceRegistered(key);
        });
        return true;
    }();
    (void)hooked;
    return m;
}

static std::string canonicalRequestKey(const std::string& source)
{
    std::error_code ec;
    if (!source.empty() && std::filesystem::exists(source, ec) && !ec) {
        auto c = std::filesystem::weakly_canonical(source, ec);
        if (!ec)
            return c.string();
    }
    return source;   // synthetic keys pass through verbatim
}

ChunkBreakpoints* BreakpointManager::ensureChunkWords(Chunk* chunk)
{
    ChunkBreakpoints* bp = chunk->breakpointsRaw.load(std::memory_order_acquire);
    if (bp)
        return bp;
    if (!chunk->debugInfo || chunk->debugInfo->stmts.empty())
        return nullptr;
    auto owned = std::make_unique<ChunkBreakpoints>(chunk->debugInfo->stmts.size());
    bp = owned.get();
    storage_.push_back(std::move(owned));
    chunk->breakpointsRaw.store(bp, std::memory_order_release);
    return bp;
}

std::vector<BreakpointManager::Bound>
BreakpointManager::setBreakpoints(const std::string& source,
                                  const std::vector<int>& lines)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string key = canonicalRequestKey(source);
    if (lines.empty())
        requests_.erase(key);
    else
        requests_[key] = lines;
    auto out = bindLocked(key);
    updateSlowPathDemand();
    return out;
}

void BreakpointManager::onSourceRegistered(const std::string& sourceKey)
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::string requestKey = sourceKey;
    if (requests_.find(requestKey) == requests_.end()) {
        // A synthetic source registers as "name#content-hash", but a web
        // client stores its request under the bare display name: rebind
        // that request against the fresh registration.
        const size_t hash = sourceKey.rfind('#');
        if (hash == std::string::npos)
            return;
        requestKey = sourceKey.substr(0, hash);
        if (requests_.find(requestKey) == requests_.end())
            return;
    }
    bindLocked(requestKey);
    updateSlowPathDemand();
}

std::vector<BreakpointManager::Bound>
BreakpointManager::bindLocked(const std::string& sourceKey)
{
    std::vector<Bound> out;

    // Full-set replace: clear every word previously armed for this source
    // (a chunk belongs to exactly one source), under the seqlock so a
    // concurrent crossing never claims a hit from a half-edited state.
    auto armedIt = armedBySource_.find(sourceKey);
    if (armedIt != armedBySource_.end()) {
        for (const ptr<Chunk>& ch : armedIt->second) {
            if (ChunkBreakpoints* bp = ch->breakpointsRaw.load(std::memory_order_acquire)) {
                bp->seq.fetch_add(1, std::memory_order_acq_rel);   // odd: editing
                for (size_t i = 0; i < bp->count; ++i)
                    bp->words[i].store(0, std::memory_order_relaxed);
                bp->seq.fetch_add(1, std::memory_order_release);   // even: stable
            }
        }
        armedIt->second.clear();
    }

    auto reqIt = requests_.find(sourceKey);
    if (reqIt == requests_.end())
        return out;

    auto entry = ModuleDebugIndex::instance().lookupBySource(sourceKey);

    struct Target { ptr<Chunk> chunk; uint32_t stmtIdx; int line; };
    std::vector<Target> targets;

    for (int reqLine : reqIt->second) {
        Bound b;
        b.requestedLine = reqLine;
        if (entry.has_value() && isList(entry->functions)) {
            // Forward-within-line policy: the statement with the smallest
            // (line, offset) at-or-after the requested line, across every
            // function of the source.
            ptr<Chunk> bestChunk;
            uint32_t bestIdx = 0;
            int bestLine = INT32_MAX;
            uint32_t bestOffset = 0;
            ObjList* fns = asList(entry->functions);
            const int32_t n = fns->length();
            for (int32_t fi = 0; fi < n; ++fi) {
                Value fv = fns->getElement(size_t(fi));
                if (!isFunction(fv))
                    continue;
                const ptr<Chunk>& ch = asFunction(fv)->chunk;
                if (!ch || !ch->debugInfo)
                    continue;
                const auto& stmts = ch->debugInfo->stmts;
                // Bind to StatementStart entries; a chunk with none (a
                // default-argument expression chunk) falls back to its
                // FunctionEntry -- the boundary locator resolves offset 0
                // to the LAST entry there, which is the FunctionEntry in
                // that case, so the armed index matches the fired index.
                bool hasStatementStarts = false;
                for (const auto& s : stmts)
                    if (s.kind == uint8_t(DebugStmtKind::StatementStart)) {
                        hasStatementStarts = true;
                        break;
                    }
                for (uint32_t si = 0; si < stmts.size(); ++si) {
                    const auto& s = stmts[si];
                    if (hasStatementStarts
                        && s.kind != uint8_t(DebugStmtKind::StatementStart))
                        continue;
                    if (s.line < reqLine)
                        continue;
                    if (s.line < bestLine
                        || (s.line == bestLine && bestChunk == ch
                            && s.offset < bestOffset)) {
                        bestChunk = ch;
                        bestIdx = si;
                        bestLine = s.line;
                        bestOffset = s.offset;
                    }
                }
            }
            if (bestChunk) {
                targets.push_back(Target{ bestChunk, bestIdx, bestLine });
                b.boundLine = bestLine;
                b.verified = true;
            }
        }
        out.push_back(b);
    }

    // Arm the targets, one seqlock cycle per chunk.
    auto& armed = armedBySource_[sourceKey];
    for (const auto& t : targets) {
        ChunkBreakpoints* bp = ensureChunkWords(t.chunk.get());
        if (!bp || t.stmtIdx >= bp->count)
            continue;
        bp->seq.fetch_add(1, std::memory_order_acq_rel);
        bp->words[t.stmtIdx].fetch_or(ChunkBreakpoints::Enabled,
                                      std::memory_order_relaxed);
        bp->seq.fetch_add(1, std::memory_order_release);
        bool known = false;
        for (const ptr<Chunk>& c : armed)
            if (c.get() == t.chunk.get()) { known = true; break; }
        if (!known)
            armed.push_back(t.chunk);
    }
    return out;
}

void BreakpointManager::updateSlowPathDemand()
{
    bool any = false;
    for (const auto& kv : armedBySource_)
        if (!kv.second.empty()) { any = true; break; }
    auto& coord = VM::instance().stopCoordinator();
    if (any && !slowPathDemandHeld_) {
        coord.addSlowPathDemand();
        slowPathDemandHeld_ = true;
    } else if (!any && slowPathDemandHeld_) {
        coord.removeSlowPathDemand();
        slowPathDemandHeld_ = false;
    }
}

void BreakpointManager::clearAllBreakpoints()
{
    std::lock_guard<std::mutex> lock(mutex_);
    requests_.clear();
    for (auto& kv : armedBySource_) {
        for (const ptr<Chunk>& ch : kv.second) {
            if (ChunkBreakpoints* bp = ch->breakpointsRaw.load(std::memory_order_acquire)) {
                bp->seq.fetch_add(1, std::memory_order_acq_rel);
                for (size_t i = 0; i < bp->count; ++i)
                    bp->words[i].store(0, std::memory_order_relaxed);
                bp->seq.fetch_add(1, std::memory_order_release);
            }
        }
    }
    armedBySource_.clear();
    updateSlowPathDemand();
}

} // namespace roxal
