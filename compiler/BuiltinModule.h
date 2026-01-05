#pragma once

#include <core/common.h>
#include "Value.h"
#include "Object.h"
#include <core/types.h>
#include <optional>
#include <algorithm>
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
              std::vector<Value> defaults = {});

    // Attach C++ implementation to object method declared in builtin .rox module
    void linkMethod(const std::string& typeName,
                    const std::string& methodName,
                    NativeFn fn,
                    std::vector<Value> defaults = {});

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
                                std::vector<Value> defaults)
{
    auto val = asModuleType(moduleType())->vars.load(toUnicodeString(name));
    if (val.has_value() && isClosure(val.value())) {
        ObjClosure* cl = asClosure(val.value());
        asFunction(cl->function)->nativeImpl = fn;
        asFunction(cl->function)->nativeDefaults = std::move(defaults);
    }
}

inline void BuiltinModule::linkMethod(const std::string& typeName,
                                      const std::string& methodName,
                                      NativeFn fn,
                                      std::vector<Value> defaults)
{
    auto typeVal = asModuleType(moduleType())->vars.load(toUnicodeString(typeName));
    if (typeVal.has_value() && isObjectType(typeVal.value())) {
        ObjObjectType* type = asObjectType(typeVal.value());
        auto it = type->methods.find(toUnicodeString(methodName).hashCode());
        if (it != type->methods.end()) {
            Value val = it->second.closure;
            if (isClosure(val)) {
                ObjClosure* cl = asClosure(val);
                asFunction(cl->function)->nativeImpl = fn;
                asFunction(cl->function)->nativeDefaults = std::move(defaults);
            }
        }
        else {
            throw std::runtime_error("BuiltinModule::linkMethod: Method '" + methodName +
                                     "' not found in type '" + typeName + "'.");
        }
    }
    else {
        throw std::runtime_error("BuiltinModule::linkMethod: Type '" + typeName +
                                 "' not found or not an object type.");
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
