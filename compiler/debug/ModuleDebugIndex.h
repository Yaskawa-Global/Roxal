#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "../GCRoots.h"
#include "../Value.h"

namespace roxal {

struct ObjFunction;

// Source-identity-keyed registry of debug-indexed function graphs.
// Registration walks the whole reachable function graph -- chunk constants
// AND ObjFunction::paramDefaultFunc, via the one shared walker that both
// prior partial traversals are generalized into -- and retains every
// function strongly, as one ObjList Value per generation held in a
// TracedMember root (a typed persistent root: on precise wasm GC, nothing
// else keeps index-only references alive).
//
// Generations: re-registering a source identity retires the previous
// generation, so a reload/recompile does not pin the old chunk graph
// indefinitely.  Breakpoint requests bind by stable source identity
// through lookupBySource() and rebind on new generations.
class ModuleDebugIndex {
public:
    static ModuleDebugIndex& instance();

    // Walk rootFunction's graph and register it under its chunk's source
    // name, with retained source text (may be empty when unavailable).
    // Returns the generation token; 0 when root is not a registrable
    // function.  Any prior generation for the same source is retired.
    uint64_t registerFunctionGraph(const Value& rootFunction, std::string sourceText);

    void retireGeneration(uint64_t generation);

    // VM shutdown: drop every retained Value while the collector and root
    // registry are still alive.
    void clearAll();

    struct SourceEntry {
        uint64_t generation { 0 };
        std::string sourceName;
        std::string sourceText;   // may be empty (unavailable)
        Value functions;          // ObjList of ObjFunction values
    };
    std::optional<SourceEntry> lookupBySource(const std::string& sourceName);

    // Provisional stable source identity (serialized content-hash
    // SourceKey records are still to come): a name that resolves to
    // an existing file keys by its canonical path (relative and absolute
    // spellings converge); synthetic names (REPL "cli", -e, generated) key
    // by name + content hash, so distinct fragments COEXIST and only an
    // identical resubmission supersedes its predecessor.
    static std::string stableSourceKey(const std::string& sourceName,
                                       const std::string& sourceText);

    // The shared function-graph walker: visits root and every function
    // reachable through chunk constants and paramDefaultFunc, cycle-guarded
    // by function identity.
    static void forEachFunctionInGraph(const Value& root,
                                       const std::function<void(ObjFunction*)>& fn);

    // Invoked (outside the index lock) after every successful registration
    // with the source's stable key -- the breakpoint manager rebinds its
    // stored requests through this.
    static void setRegistrationHook(std::function<void(const std::string&)> hook);

private:
    ModuleDebugIndex() = default;

    std::mutex mutex_;
    // generation -> ObjList of function Values.  A map<uint64_t, Value>
    // keeps the traced shape simple (one adapted container).
    TracedMember<std::unordered_map<uint64_t, Value>> generations_;
    struct Meta { std::string sourceName; std::string sourceText; };
    std::unordered_map<uint64_t, Meta> meta_;
    std::unordered_map<std::string, uint64_t> bySource_;   // newest generation per source
    uint64_t nextGen_ { 1 };
};

} // namespace roxal
