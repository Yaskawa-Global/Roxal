#pragma once

#include <core/common.h>
#include <core/memory.h>
#include <unicode/unistr.h>

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace roxal {

class BuiltinModule;
class VM;

// Factory function type for creating builtin modules on-demand
using ModuleFactory = std::function<ptr<BuiltinModule>()>;

// Registry for lazy-loaded builtin modules.
// Modules are registered with factory functions at VM startup,
// but only instantiated when first imported.
class LazyModuleRegistry {
public:
    LazyModuleRegistry() = default;
    ~LazyModuleRegistry() = default;

    // Non-copyable
    LazyModuleRegistry(const LazyModuleRegistry&) = delete;
    LazyModuleRegistry& operator=(const LazyModuleRegistry&) = delete;

    // Register a factory for lazy module creation (called during VM init)
    void registerFactory(const std::string& moduleName, ModuleFactory factory);

    // Check if a module name is registered (even if not loaded yet)
    bool isRegistered(const icu::UnicodeString& name) const;

    // Ensure module is fully loaded; thread-safe, idempotent.
    // Returns the loaded module or nullptr if not registered.
    // Loading performs: instantiate, parse .rox, registerBuiltins(), onModuleLoaded()
    ptr<BuiltinModule> ensureLoaded(const icu::UnicodeString& name, VM& vm);

    // Get already-loaded module (returns nullptr if not loaded yet)
    ptr<BuiltinModule> getLoadedModule(const icu::UnicodeString& name) const;

    // Clear all loaded modules (for VM shutdown)
    // Note: Does NOT call onModuleUnloading() - caller should do that first
    void clear();

private:
    struct ModuleEntry {
        ModuleFactory factory;
        ptr<BuiltinModule> instance;
        bool loaded = false;
        std::unique_ptr<std::mutex> loadMutex;

        ModuleEntry() : loadMutex(std::make_unique<std::mutex>()) {}
        ModuleEntry(ModuleFactory f) : factory(std::move(f)), loadMutex(std::make_unique<std::mutex>()) {}
    };

    mutable std::mutex registryMutex_;
    std::unordered_map<std::string, ModuleEntry> entries_;

    // Helper to perform loading steps (called under per-module lock)
    void doLoad(ModuleEntry& entry, VM& vm, const std::string& name);
};

} // namespace roxal
