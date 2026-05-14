#pragma once

#include <core/common.h>
#include "Value.h"
#include "Object.h"
#include <core/types.h>
#include <optional>
#include <algorithm>
#ifdef DEBUG_BUILTINS
#include <cstdio>
#endif
#include <string>
#include <vector>

namespace df {
class Signal;
}

namespace roxal {

class VM;
struct ObjModuleType;

class BuiltinModule {
public:
    virtual ~BuiltinModule() {};

    // True if a <module-bame>.rox script for this module should be executed on import
    virtual bool hasModuleScript() const { return true; }

    // must call setVM(vm) then register builtin module funcs and methods
    virtual void registerBuiltins(VM& vm) = 0;

    // called when code imports the module (via _init() builtin if declared in .rox file)
    virtual void initialize() {};

    virtual Value moduleType() const = 0; // ObjModuleType
    virtual std::vector<std::string> additionalModulePaths() const { return {}; }

    // Called after module is fully loaded (after registerBuiltins).
    // Use for: registering special VM pointers, starting background threads.
    virtual void onModuleLoaded(VM& vm) {}

    // Called during VM shutdown, before destructor.
    // Use for: stopping background threads, cleanup.
    virtual void onModuleUnloading(VM& vm) {}

protected:
    // only valid after call to setVM() in registerBuiltins(VM&)
    VM& vm() { return vm_.value().get(); }

    // Helper to construct function parameter type information. Parameter types
    // may be omitted (nullopt) for untyped parameters.
    static std::vector<type::Type::FuncType::ParamType>
    constructParams(const std::vector<std::pair<std::string,
                                                std::optional<type::BuiltinType>>>& infos,
                    const std::vector<Value>& defaults);

    // Convenience helper to build a Func type descriptor
    static ptr<type::Type>
    makeFuncType(const std::vector<std::pair<std::string,
                                             std::optional<type::BuiltinType>>>& infos,
                 const std::vector<Value>& defaults = {},
                 bool isProc = false);

    // Attach C++ implementation to function declared in builtin .rox module
    void link(const std::string& name, NativeFn fn,
              std::vector<Value> defaults = {},
              uint32_t resolveArgMask = 0);

    // Attach C++ implementation to object method declared in builtin .rox module
    void linkMethod(const std::string& typeName,
                    const std::string& methodName,
                    NativeFn fn,
                    std::vector<Value> defaults = {}, // default arg values (if not in .rox file)
                    uint32_t resolveArgMask = 0,// bitmask of argument indices (0-based) that should be passed as resolved values (not futures)
                    bool noMutateSelf = false,  // does this method modify instance state?
                    uint32_t noMutateArgs = 0); // bitmask of argument indices (0-based) that are not mutated by this method (for optimization)

    // Fetch a module-level source signal declared in the builtin .rox file.
    // If \p required is false, returns nullptr when the signal cannot be found.
    roxal::ptr<df::Signal> moduleSourceSignal(const std::string& name,
                                              bool required = true);

    // Convenience helpers for updating the value of a module-level source
    // signal from native code, mirroring signal.set() semantics.
    void setModuleSourceSignalValue(const std::string& name, const Value& value);
    void setModuleSourceSignalValue(const roxal::ptr<df::Signal>& signal,
                                    const Value& value,
                                    const std::string& signalName = "");

    static void destroyModuleType(Value& moduleTypeValue);

    /// Resolve a child value extracted from a const parent for correct MVCC
    /// snapshot access.  Call this when native code reads a reference-type child
    /// (e.g. list element, dict value, object property) from a const argument
    /// and needs to see the value as it was at snapshot time rather than the
    /// current (possibly mutated) state.
    ///
    /// If the parent is not const or has no snapshot token, this simply returns
    /// the child with transitive const propagation (no MVCC overhead).
    static Value resolveConstChildValue(const Value& parent, const Value& child);

    void setVM(VM& vm) { vm_ = vm; }


    // helpers for instantiating & manipulating module object types

    bool instanceOf(const Value& objInstance, const Value& objectType);


private:
    // reference to VM stored via setVM() (call in registerBuiltins())
    std::optional<std::reference_wrapper<VM>> vm_;
};


inline bool BuiltinModule::instanceOf(const Value& objInstance, const Value& objectType)
{
    // ensure we have an object instance and an object type
    if (!isObjectInstance(objInstance))
        return false;
    ObjectInstance* inst { asObjectInstance(objInstance) };
    if (!inst) return false;

    if (!isObjectType(objectType))
        return false;
    ObjObjectType* type { asObjectType(objectType) };
    if (!type) return false;

    // check the instance's type is the type or its super-type
    //  (traversing up the inheretance heirarchy)
    ObjObjectType* current = asObjectType(inst->instanceType);
    while (current) {
        if (current == type)
            return true;
        if (current->superType.isNil())
            break;
        current = asObjectType(current->superType);
    }
    return false;
}

inline std::vector<type::Type::FuncType::ParamType>
BuiltinModule::constructParams(const std::vector<std::pair<std::string,
                                                std::optional<type::BuiltinType>>>& infos,
                               const std::vector<Value>& defaults)
{
    using PT = type::Type::FuncType::ParamType;
    std::vector<PT> params;
    params.reserve(infos.size());
    for (size_t i = 0; i < infos.size(); ++i) {
        PT p(toUnicodeString(infos[i].first));
        if (infos[i].second.has_value())
            p.type = make_ptr<type::Type>(infos[i].second.value());
        if (i < defaults.size() && !defaults[i].isNil())
            p.hasDefault = true;
        else
            p.hasDefault = false;
        params.push_back(p);
    }
    return params;
}

inline ptr<type::Type>
BuiltinModule::makeFuncType(const std::vector<std::pair<std::string,
                                            std::optional<type::BuiltinType>>>& infos,
                            const std::vector<Value>& defaults,
                            bool isProc)
{
    auto t = make_ptr<type::Type>(type::BuiltinType::Func);
    t->func = type::Type::FuncType();
    t->func->isProc = isProc;
    auto params = constructParams(infos, defaults);
    t->func->params.resize(params.size());
    for(size_t i=0;i<params.size();++i) t->func->params[i]=params[i];
    return t;
}

inline void BuiltinModule::link(const std::string& name, NativeFn fn,
                                std::vector<Value> defaults,
                                uint32_t resolveArgMask)
{
    auto val = asModuleType(moduleType())->vars.load(toUnicodeString(name));
    if (val.has_value() && isClosure(val.value())) {
        ObjClosure* cl = asClosure(val.value());
        asFunction(cl->function)->builtinInfo = make_ptr<BuiltinFuncInfo>(
            fn, std::move(defaults), resolveArgMask);
#ifdef DEBUG_BUILTINS
        std::string modName;
        asModuleType(moduleType())->name.toUTF8String(modName);
        std::fprintf(stderr, "[builtins] linked %s.%s\n",
                     modName.c_str(), name.c_str());
#endif
    }
}

inline void BuiltinModule::linkMethod(const std::string& typeName,
                                      const std::string& methodName,
                                      NativeFn fn,
                                      std::vector<Value> defaults,
                                      uint32_t resolveArgMask,
                                      bool noMutateSelf,
                                      uint32_t noMutateArgs)
{
    // Resolve `typeName` as either a flat module-level type (e.g. "Foo") or a
    // dotted path to a nested type (e.g. "Outer.Inner.Innermost"). The first
    // segment is looked up in the module's `vars`; each subsequent segment is
    // looked up in the preceding type's `nestedTypes` map.
    //
    // Same logic shape as the runtime walker at VM.cpp (GetProp on object
    // types), but with explicit per-segment error messages so misconfigured
    // builtin link calls fail fast at registration time.
    auto splitDotted = [](const std::string& s) {
        std::vector<std::string> out;
        size_t start = 0;
        for (size_t i = 0; i <= s.size(); ++i) {
            if (i == s.size() || s[i] == '.') {
                out.emplace_back(s.substr(start, i - start));
                start = i + 1;
            }
        }
        return out;
    };

    std::vector<std::string> segments = splitDotted(typeName);
    if (segments.empty() || segments.front().empty()) {
        throw std::runtime_error("BuiltinModule::linkMethod: Empty type name.");
    }

    auto firstVal = asModuleType(moduleType())->vars.load(toUnicodeString(segments[0]));
    if (!firstVal.has_value() || !isObjectType(firstVal.value())) {
        throw std::runtime_error("BuiltinModule::linkMethod: Type '" + segments[0] +
                                 "' not found or not an object type.");
    }
    ObjObjectType* type = asObjectType(firstVal.value());

    // Walk nested-type segments. After the loop, `type` is the final type that
    // should own `methodName`. `consumedPath` accumulates the resolved path for
    // error messages so unmatched segments cite the partial prefix.
    std::string consumedPath = segments[0];
    for (size_t i = 1; i < segments.size(); ++i) {
        if (segments[i].empty()) {
            throw std::runtime_error("BuiltinModule::linkMethod: Empty segment in dotted type name '" +
                                     typeName + "'.");
        }
        int32_t segHash = toUnicodeString(segments[i]).hashCode();
        auto nit = type->nestedTypes.find(segHash);
        if (nit == type->nestedTypes.end() || !isObjectType(nit->second.type)) {
            throw std::runtime_error("BuiltinModule::linkMethod: Nested type '" + segments[i] +
                                     "' not found in '" + consumedPath + "'.");
        }
        type = asObjectType(nit->second.type);
        consumedPath += "." + segments[i];
    }

    auto it = type->methods.find(toUnicodeString(methodName).hashCode());
    // Builtin modules register methods one-by-one — never overloaded.
    if (it != type->methods.end() && it->second.overloads.size() == 1) {
        Value val = it->second.overloads[0].closure;
        if (isClosure(val)) {
            ObjClosure* cl = asClosure(val);
            asFunction(cl->function)->builtinInfo = make_ptr<BuiltinFuncInfo>(
                fn, std::move(defaults), resolveArgMask, noMutateSelf, noMutateArgs);
#ifdef DEBUG_BUILTINS
            std::string modName;
            asModuleType(moduleType())->name.toUTF8String(modName);
            std::fprintf(stderr, "[builtins] linked %s.%s.%s\n",
                         modName.c_str(), typeName.c_str(), methodName.c_str());
#endif
        }
    }
    else {
        throw std::runtime_error("BuiltinModule::linkMethod: Method '" + methodName +
                                 "' not found in type '" + typeName + "'.");
    }
}

inline void BuiltinModule::destroyModuleType(Value& moduleTypeValue)
{
    if (moduleTypeValue.isNil()) {
        return;
    }

    ObjModuleType* moduleType = asModuleType(moduleTypeValue);
    moduleType->dropReferences();

    ObjModuleType::allModules.unsafeApply([moduleType](std::vector<Value>& modules) {
        modules.erase(std::remove_if(modules.begin(), modules.end(),
                                     [moduleType](const Value& value) {
                                         return value.isObj() && value.asObj() == moduleType;
                                     }),
                      modules.end());
    });

    moduleTypeValue = Value::nilVal();
}

} // namespace roxal
