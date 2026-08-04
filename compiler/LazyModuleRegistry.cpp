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

bool LazyModuleRegistry::isRegistered(const ustring& name) const
{
    std::string nameStr = toUTF8StdString(name);
    std::lock_guard<std::mutex> lock(registryMutex_);
    return entries_.find(nameStr) != entries_.end();
}

ptr<BuiltinModule> LazyModuleRegistry::ensureLoaded(const ustring& name, VM& vm)
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

    // Lock per-module mutex to serialize loading of the same module.
    // Do NOT hold registryMutex_ across doLoad: loading scripts may import
    // other modules and call back into isRegistered/ensureLoaded.
    std::lock_guard<std::mutex> loadLock(*moduleMutex);

    // Double-check under registry lock.
    {
        std::lock_guard<std::mutex> lock(registryMutex_);
        auto it = entries_.find(nameStr);
        if (it == entries_.end())
            return nullptr;
        if (it->second.loaded)
            return it->second.instance;
    }

    // doLoad re-acquires registryMutex_ for each entry mutation; it never
    // holds the registry lock across script execution, which is what makes
    // nested imports safe.
    doLoad(vm, nameStr);

    // Re-fetch the result under lock.
    std::lock_guard<std::mutex> lock(registryMutex_);
    auto it = entries_.find(nameStr);
    if (it == entries_.end())
        return nullptr;
    return it->second.instance;
}

ptr<BuiltinModule> LazyModuleRegistry::getLoadedModule(const ustring& name) const
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

void LazyModuleRegistry::doLoad(VM& vm, const std::string& name)
{
    // Caller holds the per-module loadMutex; this function never holds
    // registryMutex_ across calls that can re-enter the registry. It briefly
    // locks for each entry read/write and works against a local instance
    // pointer the rest of the time.

    // 1. Create the module instance from its factory. Stash the instance in
    //    the entry under lock so any concurrent (different-thread) query
    //    sees consistent state once we set loaded=true at the end.
    ptr<BuiltinModule> instance;
    {
        std::lock_guard<std::mutex> lock(registryMutex_);
        auto it = entries_.find(name);
        if (it == entries_.end())
            return;
        instance = it->second.factory();
        if (!instance)
            return;
        it->second.instance = instance;
    }

    // 2. Add module's additional search paths
    vm.appendModulePaths(instance->additionalModulePaths());

    // 3. Add to builtinModules for GC traversal
    vm.builtinModules.push_back(instance);

    if (instance->hasModuleScript()) {
        // 4. Parse the .rox file to populate type declarations
        //    Convert dots to path separators for nested modules (e.g. "ai.nn" → "ai/nn.rox")
        std::string roxFile = name;
        std::replace(roxFile.begin(), roxFile.end(), '.', '/');
        roxFile += ".rox";
        vm.executeBuiltinModuleScript(roxFile, instance->moduleType());
    }

    // 5. Link native implementations
    instance->registerBuiltins(vm);

    // 6. Call module-loaded hook for module-specific initialization
    instance->onModuleLoaded(vm);

    // 7. Mark loaded under the registry lock.
    std::lock_guard<std::mutex> lock(registryMutex_);
    auto it = entries_.find(name);
    if (it != entries_.end())
        it->second.loaded = true;
}

} // namespace roxal
