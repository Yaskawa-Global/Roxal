#include "ModuleDebugIndex.h"

#include <filesystem>
#include <unordered_set>
#include <vector>

#include "../Object.h"
#include "../SimpleMarkSweepGC.h"

namespace roxal {

namespace {
std::mutex s_hookMutex;
std::function<void(const std::string&)> s_registrationHook;
} // namespace

ModuleDebugIndex& ModuleDebugIndex::instance()
{
    static ModuleDebugIndex idx;
    return idx;
}

void ModuleDebugIndex::forEachFunctionInGraph(const Value& root,
                                              const std::function<void(ObjFunction*)>& fn)
{
    std::unordered_set<ObjFunction*> visited;
    std::vector<ObjFunction*> stack;
    auto enqueue = [&](const Value& v) {
        if (!isFunction(v))
            return;
        ObjFunction* f = asFunction(v);
        if (f && visited.insert(f).second)
            stack.push_back(f);
    };
    enqueue(root);
    while (!stack.empty()) {
        ObjFunction* f = stack.back();
        stack.pop_back();
        fn(f);
        if (f->chunk) {
            for (const auto& c : f->chunk->constants)
                enqueue(c);
        }
        // Default-argument functions are retained separately from the
        // constant table: without this leg a breakpoint in
        // a default-value expression would be unreachable.
        for (const auto& kv : f->paramDefaultFunc)
            enqueue(kv.second);
    }
}

std::string ModuleDebugIndex::stableSourceKey(const std::string& sourceName,
                                              const std::string& sourceText)
{
    std::error_code ec;
    if (!sourceName.empty()
        && std::filesystem::exists(sourceName, ec) && !ec) {
        auto canon = std::filesystem::weakly_canonical(sourceName, ec);
        if (!ec)
            return canon.string();
    }
    return sourceName + "#" + std::to_string(std::hash<std::string>{}(sourceText));
}

uint64_t ModuleDebugIndex::registerFunctionGraph(const Value& rootFunction,
                                                 std::string sourceText)
{
    if (!isFunction(rootFunction))
        return 0;
    ObjFunction* rootFn = asFunction(rootFunction);
    if (!rootFn->chunk)
        return 0;
    std::string sourceName =
        stableSourceKey(toUTF8StdString(rootFn->chunk->sourceName), sourceText);

    // Build the retained-function list before taking the lock; the fresh
    // Values live in C++ locals across allocations -- cover the window
    // (wasm precise-GC safe; native callers are already covered).
    SimpleMarkSweepGC::GCNoParkScope nativeCover;
    Value listVal { Value::objVal(newListObj()) };
    ObjList* list = asList(listVal);
    forEachFunctionInGraph(rootFunction, [&](ObjFunction* f) {
        list->append(Value::objRef(f));
    });

    uint64_t gen = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        gen = nextGen_++;
        auto prev = bySource_.find(sourceName);
        if (prev != bySource_.end()) {
            // Reload/recompile: retire the superseded generation so it does
            // not pin the old chunk graph.
            generations_->erase(prev->second);
            meta_.erase(prev->second);
        }
        (*generations_)[gen] = listVal;
        meta_[gen] = Meta{ sourceName, std::move(sourceText) };
        bySource_[sourceName] = gen;
    }
    // Registration hook OUTSIDE the index lock (the breakpoint manager's
    // rebind calls back into lookupBySource).
    std::function<void(const std::string&)> hook;
    {
        std::lock_guard<std::mutex> hl(s_hookMutex);
        hook = s_registrationHook;
    }
    if (hook)
        hook(sourceName);
    return gen;
}

void ModuleDebugIndex::setRegistrationHook(std::function<void(const std::string&)> hook)
{
    std::lock_guard<std::mutex> hl(s_hookMutex);
    s_registrationHook = std::move(hook);
}

void ModuleDebugIndex::retireGeneration(uint64_t generation)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = meta_.find(generation);
    if (it == meta_.end())
        return;
    auto bs = bySource_.find(it->second.sourceName);
    if (bs != bySource_.end() && bs->second == generation)
        bySource_.erase(bs);
    meta_.erase(it);
    generations_->erase(generation);
}

void ModuleDebugIndex::clearAll()
{
    std::lock_guard<std::mutex> lock(mutex_);
    generations_->clear();
    meta_.clear();
    bySource_.clear();
}

std::optional<ModuleDebugIndex::SourceEntry>
ModuleDebugIndex::lookupBySource(const std::string& sourceName)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto bs = bySource_.find(sourceName);
    if (bs == bySource_.end()) {
        // Accept a raw file path too: canonicalize the way registration did.
        std::error_code ec;
        if (!sourceName.empty()
            && std::filesystem::exists(sourceName, ec) && !ec) {
            auto canon = std::filesystem::weakly_canonical(sourceName, ec);
            if (!ec)
                bs = bySource_.find(canon.string());
        }
    }
    if (bs == bySource_.end() && sourceName.find('#') == std::string::npos) {
        // Bare-name lookup of a SYNTHETIC source (registered as
        // "name#content-hash" -- submitted editor scripts, REPL fragments):
        // pick the newest generation among that name's registrations (the
        // web IDE addresses sources by display name).
        const std::string prefix = sourceName + "#";
        uint64_t bestGen = 0;
        for (auto it = bySource_.begin(); it != bySource_.end(); ++it) {
            if (it->first.rfind(prefix, 0) == 0 && it->second >= bestGen) {
                bestGen = it->second;
                bs = it;
            }
        }
    }
    if (bs == bySource_.end())
        return std::nullopt;
    SourceEntry entry;
    entry.generation = bs->second;
    auto m = meta_.find(bs->second);
    if (m != meta_.end()) {
        entry.sourceName = m->second.sourceName;
        entry.sourceText = m->second.sourceText;
    }
    auto g = generations_->find(bs->second);
    if (g != generations_->end())
        entry.functions = g->second;
    return entry;
}

} // namespace roxal
