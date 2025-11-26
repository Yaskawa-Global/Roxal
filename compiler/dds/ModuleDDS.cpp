#ifdef ROXAL_ENABLE_DDS

#include "dds/ModuleDDS.h"
#include "dds/DdsAdapter.h"

#include "Object.h"
#include "VM.h"

#include <dds/dds.h>
#include <dds/ddsrt/types.h>

#include <filesystem>
#include <stdexcept>

using namespace roxal;

ModuleDDS::ModuleDDS()
{
    moduleTypeValue = Value::objVal(newModuleTypeObj(toUnicodeString("dds")));
    ObjModuleType::allModules.push_back(moduleTypeValue);
}

ModuleDDS::~ModuleDDS()
{
    if (!moduleTypeValue.isNil())
        destroyModuleType(moduleTypeValue);
}

void ModuleDDS::registerBuiltins(VM& vm)
{
    setVM(vm);
}

Value ModuleDDS::importIdl(const std::string& idlFilename)
{
    if (!functionsLinked) {
        linkNativeFunctions();
        functionsLinked = true;
    }
    if (!std::filesystem::exists(std::filesystem::path(idlFilename)))
        throw std::invalid_argument("DDS import - IDL file '"+idlFilename+"' not found.");

    if (!adapter)
        adapter = std::make_unique<DdsAdapter>();

    if (!functionsLinked) {
        linkNativeFunctions();
        functionsLinked = true;
    }

    auto types = adapter->allocateTypes(idlFilename);

    std::filesystem::path pp(idlFilename);
    std::string moduleName = adapter->packageName();
    if (moduleName.empty())
        moduleName = pp.stem().string();
    Value moduleVal = getOrCreateModule(moduleName);

    registerGeneratedTypes(moduleVal, types);
    return moduleVal;
}

Value ModuleDDS::getOrCreateModule(const std::string& name)
{
    auto it = idlModules.find(name);
    if (it != idlModules.end())
        return it->second;

    Value moduleVal = Value::moduleTypeVal(toUnicodeString(name));
    ObjModuleType::allModules.push_back(moduleVal);
    idlModules[name] = moduleVal;

    // make available as global
    vm().globals.storeGlobal(toUnicodeString(name), moduleVal);
    return moduleVal;
}

void ModuleDDS::registerGeneratedTypes(Value moduleVal, const std::vector<Value>& types)
{
    ObjModuleType* mod = asModuleType(moduleVal);
    for (const auto& typeVal : types) {
        if (!isObjectType(typeVal) && !isEnumType(typeVal))
            continue;

        ObjObjectType* type = asObjectType(typeVal);
        mod->vars.store(type->name, typeVal, true);
    }
}

void ModuleDDS::linkNativeFunctions()
{
    ObjModuleType* mod = asModuleType(moduleTypeValue);
    auto linkFn = [&](const char* name, NativeFn fn) {
        auto val = mod->vars.load(toUnicodeString(name));
        if (val.has_value() && isClosure(val.value())) {
            ObjClosure* cl = asClosure(val.value());
            asFunction(cl->function)->nativeImpl = fn;
        }
    };

    linkFn("create_participant", &ModuleDDS::dds_create_participant);
    linkFn("create_topic", &ModuleDDS::dds_create_topic);
    linkFn("create_writer", &ModuleDDS::dds_create_writer);
    linkFn("create_reader", &ModuleDDS::dds_create_reader);
}

Value ModuleDDS::dds_create_participant(VM&, ArgsView args)
{
    // TODO: add QoS support from args
    int32_t domainId = 0;
    if (args.size() >= 1 && args[0].isNumber())
        domainId = args[0].asInt();
    dds_entity_t participant = ::dds_create_participant(domainId, nullptr, nullptr);
    if (participant < 0)
        throw std::runtime_error(std::string("dds_create_participant failed: ") + ::dds_strretcode(-participant));
    auto fp = newForeignPtrObj(reinterpret_cast<void*>(static_cast<intptr_t>(participant)));
    fp->registerCleanup([](void* p){
        dds_entity_t e = static_cast<dds_entity_t>(reinterpret_cast<intptr_t>(p));
        if (e > 0)
            dds_delete(e);
    });
    return Value::objVal(std::move(fp));
}

Value ModuleDDS::dds_create_topic(VM&, ArgsView args)
{
    (void)args;
    // TODO: create topic using ddsc once type support is wired
    return Value::nilVal();
}

Value ModuleDDS::dds_create_writer(VM&, ArgsView args)
{
    (void)args;
    // TODO: create writer using ddsc
    return Value::nilVal();
}

Value ModuleDDS::dds_create_reader(VM&, ArgsView args)
{
    (void)args;
    // TODO: create reader using ddsc
    return Value::nilVal();
}

#endif // ROXAL_ENABLE_DDS
