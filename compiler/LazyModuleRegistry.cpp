#include "LazyModuleRegistry.h"
#include "BuiltinModule.h"
#include "VM.h"
#include "Object.h"
#include <algorithm>

namespace roxal {

void LazyModuleRegistry::registerFactory(const std::string& moduleName, ModuleFactory factory)
{
    std::lock_guard<std::mutex> lock(registryMutex_);
    entries_.emplace(moduleName, ModuleEntry(std::move(factory)));
}

bool LazyModuleRegistry::isRegistered(const icu::UnicodeString& name) const
{
    std::string nameStr = toUTF8StdString(name);
    std::lock_guard<std::mutex> lock(registryMutex_);
    return entries_.find(nameStr) != entries_.end();
}

ptr<BuiltinModule> LazyModuleRegistry::ensureLoaded(const icu::UnicodeString& name, VM& vm)
{
    std::string nameStr = toUTF8StdString(name);

    // Quick check without per-module lock
    {
        std::lock_guard<std::mutex> lock(registryMutex_);
        auto it = entries_.find(nameStr);
        if (it == entries_.end())
            return nullptr;
        if (it->second.loaded)
            return it->second.instance;
    }

    // Acquire per-module mutex for loading
    std::mutex* moduleMutex = nullptr;
    {
        std::lock_guard<std::mutex> lock(registryMutex_);
        auto it = entries_.find(nameStr);
        if (it == entries_.end())
            return nullptr;
        moduleMutex = it->second.loadMutex.get();
    }

    // Lock per-module mutex to serialize loading of the same module
    std::lock_guard<std::mutex> loadLock(*moduleMutex);

    // Double-check after acquiring lock
    {
        std::lock_guard<std::mutex> lock(registryMutex_);
        auto& entry = entries_[nameStr];
        if (entry.loaded)
            return entry.instance;

        // Perform loading under locks
        doLoad(entry, vm, nameStr);
        return entry.instance;
    }
}

ptr<BuiltinModule> LazyModuleRegistry::getLoadedModule(const icu::UnicodeString& name) const
{
    std::string nameStr = toUTF8StdString(name);
    std::lock_guard<std::mutex> lock(registryMutex_);
    auto it = entries_.find(nameStr);
    if (it != entries_.end() && it->second.loaded)
        return it->second.instance;
    return nullptr;
}

void LazyModuleRegistry::clear()
{
    std::lock_guard<std::mutex> lock(registryMutex_);
    entries_.clear();
}

void LazyModuleRegistry::doLoad(ModuleEntry& entry, VM& vm, const std::string& name)
{
    // 1. Create module instance from factory
    entry.instance = entry.factory();
    if (!entry.instance)
        return;

    // 2. Add module's additional search paths
    vm.appendModulePaths(entry.instance->additionalModulePaths());

    // 3. Add to builtinModules for GC traversal
    vm.builtinModules.push_back(entry.instance);

    if (entry.instance->hasModuleScript()) {
        // 4. Parse the .rox file to populate type declarations
        //    Convert dots to path separators for nested modules (e.g. "ai.nn" → "ai/nn.rox")
        std::string roxFile = name;
        std::replace(roxFile.begin(), roxFile.end(), '.', '/');
        roxFile += ".rox";
        vm.executeBuiltinModuleScript(roxFile, entry.instance->moduleType());
    }

    // 5. Link native implementations
    entry.instance->registerBuiltins(vm);

    // 6. Call module-loaded hook for module-specific initialization
    entry.instance->onModuleLoaded(vm);

    entry.loaded = true;
}

} // namespace roxal
