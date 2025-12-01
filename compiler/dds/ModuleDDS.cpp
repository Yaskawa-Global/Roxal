#ifdef ROXAL_ENABLE_DDS

#include "dds/ModuleDDS.h"
#include "dds/DdsAdapter.h"

#include "Object.h"
#include "Value.h"
#include "VM.h"

#include <dds/dds.h>
#include <dds/ddsrt/types.h>
#include <dds/ddsc/dds_public_impl.h>
#include <dds/ddsc/dds_public_alloc.h>
#include <dds/ddsc/dds_opcodes.h>
#include <dds/ddsc/dds_public_dynamic_type.h>
#include <dds/ddsi/ddsi_typelib.h>

#include <filesystem>
#include <stdexcept>
#include <cstring>
#include <functional>

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
    if (!typesRegistered) {
        registerNativeTypes();
        typesRegistered = true;
    }
    if (!functionsLinked) {
        linkNativeFunctions();
        functionsLinked = true;
    }
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

Value ModuleDDS::resolveTypeValue(const std::string& fullName)
{
    auto pos = fullName.rfind("::");
    std::string moduleName = pos == std::string::npos ? "" : fullName.substr(0, pos);
    std::string shortName = pos == std::string::npos ? fullName : fullName.substr(pos + 2);
    auto modIt = idlModules.find(moduleName.empty() ? fullName : moduleName);
    if (modIt != idlModules.end()) {
        auto maybe = asModuleType(modIt->second)->vars.load(toUnicodeString(shortName));
        if (maybe.has_value())
            return maybe.value();
    }
    return Value::nilVal();
}

void ModuleDDS::registerNativeTypes()
{
    auto makeType = [&](const char* name) {
        Value t = Value::objectTypeVal(toUnicodeString(name), false);
        ObjObjectType* obj = asObjectType(t);

        ObjObjectType::Property hprop;
        hprop.name = toUnicodeString("handle");
        hprop.type = Value::typeSpecVal(ValueType::Nil);
        hprop.initialValue = Value::nilVal();
        hprop.ownerType = t.weakRef();
        auto hh = hprop.name.hashCode();
        obj->properties.emplace(hh, hprop);
        obj->propertyOrder.push_back(hh);

        ObjObjectType::Property nprop;
        nprop.name = toUnicodeString("name");
        nprop.type = Value::typeSpecVal(ValueType::String);
        nprop.initialValue = Value::stringVal(toUnicodeString(""));
        nprop.ownerType = t.weakRef();
        auto nh = nprop.name.hashCode();
        obj->properties.emplace(nh, nprop);
        obj->propertyOrder.push_back(nh);

        ObjObjectType::Property tprop;
        tprop.name = toUnicodeString("type_name");
        tprop.type = Value::typeSpecVal(ValueType::String);
        tprop.initialValue = Value::stringVal(toUnicodeString(""));
        tprop.ownerType = t.weakRef();
        auto th = tprop.name.hashCode();
        obj->properties.emplace(th, tprop);
        obj->propertyOrder.push_back(th);

        ObjObjectType::Property dprop;
        dprop.name = toUnicodeString("_descriptor");
        dprop.type = Value::typeSpecVal(ValueType::Nil);
        dprop.initialValue = Value::nilVal();
        dprop.ownerType = t.weakRef();
        auto dh = dprop.name.hashCode();
        obj->properties.emplace(dh, dprop);
        obj->propertyOrder.push_back(dh);

        ObjObjectType::Property tiprop;
        tiprop.name = toUnicodeString("_typeinfo");
        tiprop.type = Value::typeSpecVal(ValueType::Nil);
        tiprop.initialValue = Value::nilVal();
        tiprop.ownerType = t.weakRef();
        auto tih = tiprop.name.hashCode();
        obj->properties.emplace(tih, tiprop);
        obj->propertyOrder.push_back(tih);

        return t;
    };

    participantType = makeType("_DDSParticipant");
    topicType = makeType("_DDSTopic");
    writerType = makeType("_DDSWriter");
    readerType = makeType("_DDSReader");

    ObjModuleType* mod = asModuleType(moduleTypeValue);
    mod->vars.store(toUnicodeString("_DDSParticipant"), participantType, true);
    mod->vars.store(toUnicodeString("_DDSTopic"), topicType, true);
    mod->vars.store(toUnicodeString("_DDSWriter"), writerType, true);
    mod->vars.store(toUnicodeString("_DDSReader"), readerType, true);
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
    linkFn("close", &ModuleDDS::dds_close_entity);
    linkFn("write", &ModuleDDS::dds_write);
    linkFn("read", &ModuleDDS::dds_read);
}

void ModuleDDS::setProperty(ObjectInstance* obj, const icu::UnicodeString& name, const Value& v)
{
    auto h = name.hashCode();
    auto it = obj->properties.find(h);
    if (it != obj->properties.end())
        it->second.assign(v);
}

Value ModuleDDS::makeHandleValue(dds_entity_t ent)
{
    auto fp = newForeignPtrObj(reinterpret_cast<void*>(static_cast<intptr_t>(ent)));
    fp->registerCleanup([](void* p){
        dds_entity_t e = static_cast<dds_entity_t>(reinterpret_cast<intptr_t>(p));
        if (e > 0)
            dds_delete(e);
    });
    return Value::objVal(std::move(fp));
}

std::string ModuleDDS::typeNameFromValue(const Value& v)
{
    if (auto self = VM::instance().ddsModule) {
        std::string full = self->adapter ? self->adapter->fullNameForType(v) : "";
        if (!full.empty())
            return full;
    }
    if (isObjectType(v)) {
        ObjObjectType* t = asObjectType(v);
        return toUTF8StdString(t->name);
    }
    if (isObjectInstance(v)) {
        ObjObjectType* t = asObjectType(asObjectInstance(v)->instanceType);
        return toUTF8StdString(t->name);
    }
    if (isString(v)) {
        return toUTF8StdString(asStringObj(v)->s);
    }
    return "";
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
    Value handleVal = makeHandleValue(participant);
    ModuleDDS* self = VM::instance().ddsModule;
    if (!self || self->participantType.isNil())
        return handleVal;
    Value inst = Value::objectInstanceVal(self->participantType);
    ObjectInstance* obj = asObjectInstance(inst);
    setProperty(obj, toUnicodeString("handle"), handleVal);
    setProperty(obj, toUnicodeString("name"), Value::stringVal(toUnicodeString("participant")));
    setProperty(obj, toUnicodeString("type_name"), Value::stringVal(toUnicodeString("participant")));
    return inst;
}

Value ModuleDDS::dds_create_topic(VM&, ArgsView args)
{
    if (args.size() < 3)
        throw std::invalid_argument("dds.create_topic(participant, name, msg_type) expects 3 args");
    Value part = args[0];
    Value nameVal = args[1];
    Value typeVal = args[2];
    std::string topicName;
    if (isString(nameVal))
        topicName = toUTF8StdString(asStringObj(nameVal)->s);
    else
        topicName = typeNameFromValue(nameVal);
    std::string typeName = typeNameFromValue(typeVal);
    if (topicName.empty())
        throw std::invalid_argument("topic name must be string");

    ModuleDDS* self = VM::instance().ddsModule;
    auto support = self ? self->buildDynamicTopic(part, topicName, typeName) : nullptr;
    if (!support)
        return Value::nilVal();
    self->supportByType[typeName] = support;
    self->supportByEntity[support->entity] = support;

    Value handleVal = support->handle;
    if (!self || self->topicType.isNil())
        return handleVal;
    Value inst = Value::objectInstanceVal(self->topicType);
    ObjectInstance* obj = asObjectInstance(inst);
    setProperty(obj, toUnicodeString("handle"), handleVal);
    setProperty(obj, toUnicodeString("name"), Value::stringVal(toUnicodeString(topicName)));
    setProperty(obj, toUnicodeString("type_name"), Value::stringVal(toUnicodeString(typeName)));
    if (support->descriptor) {
        auto fp = newForeignPtrObj(support->descriptor.get());
        fp->registerCleanup(nullptr);
        setProperty(obj, toUnicodeString("_descriptor"), Value::objVal(std::move(fp)));
    }
    if (support->typeinfo) {
        auto fp = newForeignPtrObj(support->typeinfo.get());
        fp->registerCleanup(nullptr);
        setProperty(obj, toUnicodeString("_typeinfo"), Value::objVal(std::move(fp)));
    }
    return inst;
}

std::shared_ptr<ModuleDDS::TopicSupport> ModuleDDS::buildDynamicTopic(Value participantVal, const std::string& topicName, const std::string& typeName)
{
    dds_entity_t participant = entityFromValue(participantVal, true);
    if (participant <= 0)
        return nullptr;

    auto self = VM::instance().ddsModule;
    if (!self || !self->adapter)
        return nullptr;

    const auto* structInfo = self->adapter->findStruct(typeName);
    if (!structInfo)
        return nullptr;

    auto shortName = [](const std::string& full) {
        auto pos = full.rfind("::");
        return pos == std::string::npos ? full : full.substr(pos + 2);
    };

    std::unordered_map<std::string, dds_dynamic_type_t> enumTypes;
    std::unordered_map<std::string, dds_dynamic_type_t> structTypes;
    std::unordered_map<std::string, dds_dynamic_type_t> stringTypes;

    std::function<dds_dynamic_type_spec_t(const FieldType&)> makeSpec;
    std::function<dds_dynamic_type_t(const std::string&)> makeStructType =
        [&](const std::string& full) -> dds_dynamic_type_t {
            auto it = structTypes.find(full);
            if (it != structTypes.end())
                return it->second;
            const auto* sInfo = self->adapter->findStruct(full);
            dds_dynamic_type_descriptor_t sd{};
            sd.kind = DDS_DYNAMIC_STRUCTURE;
            std::string sname = shortName(full);
            sd.name = sname.c_str();
            dds_dynamic_type_t stype = dds_dynamic_type_create(participant, sd);
            if (stype.ret != DDS_RETCODE_OK || !sInfo) {
                structTypes.emplace(full, stype);
                return stype;
            }
            uint32_t memberId = 0;
            for (const auto& field : sInfo->fields) {
                auto spec = makeSpec(field.type);
                dds_dynamic_member_descriptor_t mdesc{};
                if (spec.kind == DDS_DYNAMIC_TYPE_KIND_PRIMITIVE)
                    mdesc = DDS_DYNAMIC_MEMBER_PRIM(spec.type.primitive, field.name.c_str());
                else
                    mdesc = DDS_DYNAMIC_MEMBER_(spec, field.name.c_str(), DDS_DYNAMIC_MEMBER_ID_AUTO, DDS_DYNAMIC_MEMBER_INDEX_END);
                dds_return_t mrc = dds_dynamic_type_add_member(&stype, mdesc);
                if (mrc != DDS_RETCODE_OK)
                    throw std::runtime_error("Failed to add member " + field.name + ": " + dds_strretcode(-mrc));
                if (field.isKey)
                    dds_dynamic_member_set_key(&stype, mdesc.id, true);
                if (field.isOptional)
                    dds_dynamic_member_set_optional(&stype, mdesc.id, true);
            }
            structTypes.emplace(full, stype);
            return stype;
        };

    makeSpec =
        [&](const FieldType& ft) -> dds_dynamic_type_spec_t {
            dds_dynamic_type_spec_t spec{};
            switch (ft.kind) {
                case FieldType::Kind::Bool: spec.kind = DDS_DYNAMIC_TYPE_KIND_PRIMITIVE; spec.type.primitive = DDS_DYNAMIC_BOOLEAN; break;
                case FieldType::Kind::Int32: spec.kind = DDS_DYNAMIC_TYPE_KIND_PRIMITIVE; spec.type.primitive = DDS_DYNAMIC_INT32; break;
                case FieldType::Kind::Int64Pair: spec.kind = DDS_DYNAMIC_TYPE_KIND_PRIMITIVE; spec.type.primitive = DDS_DYNAMIC_INT64; break;
                case FieldType::Kind::Float64: spec.kind = DDS_DYNAMIC_TYPE_KIND_PRIMITIVE; spec.type.primitive = DDS_DYNAMIC_FLOAT64; break;
                case FieldType::Kind::String: {
                    auto it = stringTypes.find("string8");
                    dds_dynamic_type_t st{};
                    if (it != stringTypes.end()) {
                        st = it->second;
                    } else {
                        dds_dynamic_type_descriptor_t sdesc{};
                        sdesc.kind = DDS_DYNAMIC_STRING8;
                        sdesc.name = "_str";
                        st = dds_dynamic_type_create(participant, sdesc);
                        stringTypes.emplace("string8", st);
                    }
                    spec = DDS_DYNAMIC_TYPE_SPEC(st);
                    break;
                }
                case FieldType::Kind::List: {
                    dds_dynamic_type_descriptor_t seqDesc{};
                    seqDesc.kind = DDS_DYNAMIC_SEQUENCE;
                    seqDesc.element_type = ft.element ? makeSpec(*ft.element)
                                                      : dds_dynamic_type_spec_t{DDS_DYNAMIC_TYPE_KIND_PRIMITIVE, {.primitive = DDS_DYNAMIC_INT32}};
                    dds_dynamic_type_t seqType = dds_dynamic_type_create(participant, seqDesc);
                    if (seqType.ret != DDS_RETCODE_OK)
                        throw std::runtime_error("Failed to build sequence type: " + std::string(dds_strretcode(-seqType.ret)));
                    spec = DDS_DYNAMIC_TYPE_SPEC(seqType);
                    break;
                }
                case FieldType::Kind::EnumRef: {
                    auto it = enumTypes.find(ft.refName);
                    dds_dynamic_type_t etype;
                    if (it != enumTypes.end()) {
                        etype = it->second;
                    } else {
                        const auto* einfo = self->adapter->findEnum(ft.refName);
                        dds_dynamic_type_descriptor_t edesc{};
                        edesc.kind = DDS_DYNAMIC_ENUMERATION;
                        std::string ename = shortName(ft.refName);
                        edesc.name = ename.c_str();
                        etype = dds_dynamic_type_create(participant, edesc);
                        if (etype.ret == DDS_RETCODE_OK && einfo) {
                            for (const auto& val : einfo->values) {
                                dds_dynamic_type_add_enum_literal(&etype, val.first.c_str(),
                                                                  DDS_DYNAMIC_ENUM_LITERAL_VALUE(val.second),
                                                                  false);
                            }
                        }
                        enumTypes.emplace(ft.refName, etype);
                    }
                    spec = DDS_DYNAMIC_TYPE_SPEC(etype);
                    break;
                }
                case FieldType::Kind::StructRef: {
                    dds_dynamic_type_t st = makeStructType(ft.refName);
                    if (st.ret == DDS_RETCODE_OK)
                        spec = DDS_DYNAMIC_TYPE_SPEC(st);
                    else {
                        spec.kind = DDS_DYNAMIC_TYPE_KIND_PRIMITIVE;
                        spec.type.primitive = DDS_DYNAMIC_STRING8;
                    }
                    break;
                }
                default:
                    spec.kind = DDS_DYNAMIC_TYPE_KIND_PRIMITIVE;
                    spec.type.primitive = DDS_DYNAMIC_STRING8;
                    break;
            }
            return spec;
        };

    dds_dynamic_type_t dynType = makeStructType(typeName);
    if (dynType.ret != DDS_RETCODE_OK)
        throw std::runtime_error("Failed to build dynamic type for " + typeName + ": " + dds_strretcode(-dynType.ret));

    ddsi_typeinfo* typeinfo = nullptr;
    dds_return_t rc = dds_dynamic_type_register(&dynType, &typeinfo);
    if (rc != DDS_RETCODE_OK || !typeinfo) {
        dds_dynamic_type_unref(&dynType);
        throw std::runtime_error("dds_dynamic_type_register failed for " + typeName + ": " + dds_strretcode(-rc));
    }

    dds_topic_descriptor_t* descOut = nullptr;
    rc = dds_create_topic_descriptor(DDS_FIND_SCOPE_LOCAL_DOMAIN,
                                     participant,
                                     reinterpret_cast<const dds_typeinfo_t*>(typeinfo),
                                     DDS_SECS(5),
                                     &descOut);
    if (rc != DDS_RETCODE_OK || !descOut) {
        ddsi_typeinfo_free(typeinfo);
        dds_dynamic_type_unref(&dynType);
        throw std::runtime_error("dds_create_topic_descriptor failed for " + typeName + ": " + dds_strretcode(-rc));
    }

    dds_entity_t topic = ::dds_create_topic(participant, descOut, topicName.c_str(), nullptr, nullptr);
    for (auto& kv : enumTypes) dds_dynamic_type_unref(&kv.second);
    for (auto& kv : structTypes) dds_dynamic_type_unref(&kv.second);
    for (auto& kv : stringTypes) dds_dynamic_type_unref(&kv.second);
    if (topic <= 0) {
        dds_delete_topic_descriptor(descOut);
        ddsi_typeinfo_free(typeinfo);
        throw std::runtime_error("dds_create_topic failed: " + std::string(dds_strretcode(-topic)));
    }

    dds_dynamic_type_unref(&dynType);

    auto support = std::make_shared<TopicSupport>();
    support->descriptor = std::shared_ptr<dds_topic_descriptor_t>(descOut, dds_delete_topic_descriptor);
    support->typeinfo = std::shared_ptr<ddsi_typeinfo>(typeinfo, ddsi_typeinfo_free);
    support->typeName = typeName;
    support->entity = topic;
    support->handle = makeHandleValue(topic);
    return support;
}

Value ModuleDDS::dds_create_writer(VM&, ArgsView args)
{
    if (args.size() < 2)
        throw std::invalid_argument("dds.create_writer(participant, topic, qos=None) expects at least 2 args");
    ModuleDDS* self = VM::instance().ddsModule;
    if (!self || self->writerType.isNil())
        return Value::nilVal();
    dds_entity_t participant = entityFromValue(args[0], true);
    dds_entity_t topicEnt = entityFromValue(args[1], true);
    auto topicSupport = self->lookupSupport(args[1]);
    Value handleVal = Value::nilVal();
    std::string typeName = typeNameFromValue(args[1]);
    if (participant > 0 && topicEnt > 0) {
        dds_entity_t writer = ::dds_create_writer(participant, topicEnt, nullptr, nullptr);
        if (writer < 0)
            throw std::runtime_error(std::string("dds_create_writer failed: ") + dds_strretcode(-writer));
        handleVal = makeHandleValue(writer);
        if (topicSupport) {
            self->supportByEntity[writer] = topicSupport;
        }
    }
    Value inst = Value::objectInstanceVal(self->writerType);
    ObjectInstance* obj = asObjectInstance(inst);
    setProperty(obj, toUnicodeString("handle"), handleVal);
    setProperty(obj, toUnicodeString("name"), Value::stringVal(toUnicodeString("writer")));
    setProperty(obj, toUnicodeString("type_name"), Value::stringVal(toUnicodeString(typeName)));
    if (topicSupport && topicSupport->descriptor) {
        auto fp = newForeignPtrObj(topicSupport->descriptor.get());
        fp->registerCleanup(nullptr);
        setProperty(obj, toUnicodeString("_descriptor"), Value::objVal(std::move(fp)));
    }
    if (topicSupport && topicSupport->typeinfo) {
        auto fp = newForeignPtrObj(topicSupport->typeinfo.get());
        fp->registerCleanup(nullptr);
        setProperty(obj, toUnicodeString("_typeinfo"), Value::objVal(std::move(fp)));
    }
    return inst;
}

Value ModuleDDS::dds_create_reader(VM&, ArgsView args)
{
    if (args.size() < 2)
        throw std::invalid_argument("dds.create_reader(participant, topic, qos=None) expects at least 2 args");
    ModuleDDS* self = VM::instance().ddsModule;
    if (!self || self->readerType.isNil())
        return Value::nilVal();
    dds_entity_t participant = entityFromValue(args[0], true);
    dds_entity_t topicEnt = entityFromValue(args[1], true);
    auto topicSupport = self->lookupSupport(args[1]);
    Value handleVal = Value::nilVal();
    std::string typeName = typeNameFromValue(args[1]);
    if (participant > 0 && topicEnt > 0) {
        dds_entity_t reader = ::dds_create_reader(participant, topicEnt, nullptr, nullptr);
        if (reader < 0)
            throw std::runtime_error(std::string("dds_create_reader failed: ") + dds_strretcode(-reader));
        handleVal = makeHandleValue(reader);
        if (topicSupport) {
            self->supportByEntity[reader] = topicSupport;
        }
    }
    Value inst = Value::objectInstanceVal(self->readerType);
    ObjectInstance* obj = asObjectInstance(inst);
    setProperty(obj, toUnicodeString("handle"), handleVal);
    setProperty(obj, toUnicodeString("name"), Value::stringVal(toUnicodeString("reader")));
    setProperty(obj, toUnicodeString("type_name"), Value::stringVal(toUnicodeString(typeName)));
    if (topicSupport && topicSupport->descriptor) {
        auto fp = newForeignPtrObj(topicSupport->descriptor.get());
        fp->registerCleanup(nullptr);
        setProperty(obj, toUnicodeString("_descriptor"), Value::objVal(std::move(fp)));
    }
    if (topicSupport && topicSupport->typeinfo) {
        auto fp = newForeignPtrObj(topicSupport->typeinfo.get());
        fp->registerCleanup(nullptr);
        setProperty(obj, toUnicodeString("_typeinfo"), Value::objVal(std::move(fp)));
    }
    return inst;
}

Value ModuleDDS::dds_close_entity(VM&, ArgsView args)
{
    if (args.empty())
        return Value::nilVal();
    Value target = args[0];
    ObjForeignPtr* fp = nullptr;
    if (isForeignPtr(target)) {
        fp = asForeignPtr(target);
    } else if (isObjectInstance(target)) {
        ObjectInstance* inst = asObjectInstance(target);
        icu::UnicodeString handleName = toUnicodeString("handle");
        auto it = inst->properties.find(handleName.hashCode());
        if (it != inst->properties.end() && isForeignPtr(it->second.value))
            fp = asForeignPtr(it->second.value);
    }
    if (fp) {
        dds_entity_t e = static_cast<dds_entity_t>(reinterpret_cast<intptr_t>(fp->ptr));
        if (e > 0)
            dds_delete(e);
        fp->ptr = nullptr;
        fp->registerCleanup(nullptr);
    }
    return Value::nilVal();
}

Value ModuleDDS::dds_write(VM&, ArgsView args)
{
    if (args.size() < 2)
        throw std::invalid_argument("dds.write(writer, msg) expects writer and message");
    dds_entity_t writer = entityFromValue(args[0], true);
    if (writer <= 0)
        throw std::runtime_error("dds.write requires a valid writer handle");
    ModuleDDS* self = VM::instance().ddsModule;
    if (!self)
        return Value::nilVal();
    auto support = self->lookupSupport(args[0]);
    if (!support || !support->descriptor)
        throw std::runtime_error("dds.write missing topic descriptor");
    const StructInfo* info = self->findStructInfo(support->typeName);
    if (!info)
        throw std::runtime_error("dds.write unknown struct type: " + support->typeName);

    const dds_topic_descriptor_t* desc = support->descriptor.get();
    auto sample = std::unique_ptr<void, std::function<void(void*)>>(
        dds_alloc(desc->m_size),
        [desc](void* p){ if (p) dds_sample_free(p, desc, DDS_FREE_ALL); });
    self->fillSampleFromValue(*info, desc, sample.get(), args[1]);
    dds_return_t rc = ::dds_write(writer, sample.get());
    if (rc < 0)
        throw std::runtime_error(std::string("dds_write failed: ") + dds_strretcode(-rc));
    return Value::nilVal();
}

Value ModuleDDS::dds_read(VM&, ArgsView args)
{
    if (args.empty())
        throw std::invalid_argument("dds.read(reader) expects reader");
    dds_entity_t reader = entityFromValue(args[0], true);
    if (reader <= 0)
        throw std::runtime_error("dds.read requires a valid reader handle");
    ModuleDDS* self = VM::instance().ddsModule;
    if (!self)
        return Value::nilVal();
    auto support = self->lookupSupport(args[0]);
    if (!support || !support->descriptor)
        throw std::runtime_error("dds.read missing topic descriptor");
    const StructInfo* info = self->findStructInfo(support->typeName);
    if (!info)
        throw std::runtime_error("dds.read unknown struct type: " + support->typeName);
    Value typeVal = self->resolveTypeValue(support->typeName);
    if (typeVal.isNil())
        throw std::runtime_error("dds.read could not resolve type " + support->typeName);

    void* samples[1] = { nullptr };
    dds_sample_info_t si;
    std::memset(&si, 0, sizeof(si));
    dds_return_t rc = ::dds_take(reader, samples, &si, 1, 1);
    if (rc <= 0)
        return Value::nilVal();
    Value result = Value::nilVal();
    if (si.valid_data && samples[0]) {
        result = self->valueFromSample(*info, support->descriptor.get(), samples[0], typeVal);
    }
    ::dds_return_loan(reader, samples, 1);
    return result;
}

dds_entity_t ModuleDDS::entityFromValue(const Value& v, bool allowNil)
{
    if (isForeignPtr(v)) {
        return static_cast<dds_entity_t>(reinterpret_cast<intptr_t>(asForeignPtr(v)->ptr));
    } else if (isObjectInstance(v)) {
        ObjectInstance* inst = asObjectInstance(v);
        icu::UnicodeString handleName = toUnicodeString("handle");
        auto it = inst->properties.find(handleName.hashCode());
        if (it != inst->properties.end() && isForeignPtr(it->second.value))
            return static_cast<dds_entity_t>(reinterpret_cast<intptr_t>(asForeignPtr(it->second.value)->ptr));
    }
    if (allowNil)
        return 0;
    throw std::invalid_argument("Invalid DDS entity handle");
}

std::shared_ptr<ModuleDDS::TopicSupport> ModuleDDS::lookupSupport(const Value& v) const
{
    dds_entity_t ent = entityFromValue(v, true);
    if (ent > 0) {
        auto it = supportByEntity.find(ent);
        if (it != supportByEntity.end())
            return it->second;
    }
    std::string tn = typeNameFromValue(v);
    auto it2 = supportByType.find(tn);
    if (it2 != supportByType.end())
        return it2->second;
    return nullptr;
}

const StructInfo* ModuleDDS::findStructInfo(const std::string& typeName) const
{
    if (!adapter)
        return nullptr;
    return adapter->findStruct(typeName);
}

static Value getFieldValue(const Value& msg, const std::string& name)
{
    if (!isObjectInstance(msg))
        return Value::nilVal();
    ObjectInstance* inst = asObjectInstance(msg);
    auto it = inst->properties.find(toUnicodeString(name).hashCode());
    if (it != inst->properties.end())
        return it->second.value;
    return Value::nilVal();
}

static int64_t pairToInt64(const Value& listVal)
{
    if (!isList(listVal))
        return 0;
    ObjList* lst = asList(listVal);
    if (lst->elts.size() < 2)
        return 0;
    Value hiVal = lst->elts.at(0);
    Value loVal = lst->elts.at(1);
    int64_t hi = hiVal.isNumber() ? static_cast<int64_t>(hiVal.asInt()) : 0;
    uint64_t lo = loVal.isNumber() ? static_cast<uint32_t>(loVal.asInt()) : 0;
    return (hi << 32) | lo;
}

static Value int64ToPair(int64_t v)
{
    Value l = Value::listVal();
    ObjList* lst = asList(l);
    lst->elts.push_back(Value::intVal(static_cast<int32_t>(v >> 32)));
    lst->elts.push_back(Value::intVal(static_cast<int32_t>(v & 0xffffffff)));
    return l;
}

static size_t alignTo(size_t offset, size_t align)
{
    if (align == 0)
        return offset;
    size_t rem = offset % align;
    return rem ? offset + (align - rem) : offset;
}

size_t ModuleDDS::computeLayout(const StructInfo& info, std::vector<size_t>& offsets) const
{
    auto itCached = cachedOffsets.find(info.fullName);
    if (itCached != cachedOffsets.end()) {
        offsets = itCached->second;
        return cachedSizes.at(info.fullName);
    }
    if (computingLayouts.count(info.fullName))
        return 0;
    computingLayouts.insert(info.fullName);
    size_t offset = 0;
    size_t maxAlign = 1;
    offsets.clear();
    for (const auto& field : info.fields) {
        size_t align = fieldAlignInternal(field.type, const_cast<ModuleDDS*>(this));
        size_t sz = typeSizeInternal(field.type, const_cast<ModuleDDS*>(this));
        maxAlign = std::max(maxAlign, align);
        offset = alignTo(offset, align);
        offsets.push_back(offset);
        offset += sz;
    }
    size_t total = alignTo(offset, maxAlign);
    cachedOffsets[info.fullName] = offsets;
    cachedSizes[info.fullName] = total;
    computingLayouts.erase(info.fullName);
    return total;
}

size_t ModuleDDS::typeSizeInternal(const FieldType& ft, ModuleDDS* mod)
{
    switch (ft.kind) {
        case FieldType::Kind::Bool: return sizeof(bool);
        case FieldType::Kind::Int32: return sizeof(int32_t);
        case FieldType::Kind::Float64: return sizeof(double);
        case FieldType::Kind::String: return sizeof(char*);
        case FieldType::Kind::EnumRef: return sizeof(int32_t);
        case FieldType::Kind::StructRef: {
            if (mod) {
                auto sup = mod->supportByType.find(ft.refName);
                if (sup != mod->supportByType.end() && sup->second->descriptor)
                    return sup->second->descriptor->m_size;
                const StructInfo* info = mod->findStructInfo(ft.refName);
                if (info) {
                    std::vector<size_t> offs;
                    return mod->computeLayout(*info, offs);
                }
            }
            return 0;
        }
        case FieldType::Kind::List: return sizeof(dds_sequence_t);
        case FieldType::Kind::Int64Pair: return sizeof(int64_t);
        default: return 0;
    }
}

size_t ModuleDDS::fieldAlignInternal(const FieldType& ft, ModuleDDS* mod)
{
    switch (ft.kind) {
        case FieldType::Kind::Bool: return alignof(bool);
        case FieldType::Kind::Int32: return alignof(int32_t);
        case FieldType::Kind::Float64: return alignof(double);
        case FieldType::Kind::Int64Pair: return alignof(int64_t);
        case FieldType::Kind::EnumRef: return alignof(int32_t);
        case FieldType::Kind::String: return alignof(char*);
        case FieldType::Kind::List: return alignof(dds_sequence_t);
        case FieldType::Kind::StructRef: {
            if (mod) {
                auto sup = mod->supportByType.find(ft.refName);
                if (sup != mod->supportByType.end() && sup->second && sup->second->descriptor)
                    return sup->second->descriptor->m_align;
                const StructInfo* info = mod->findStructInfo(ft.refName);
                if (info) {
                    size_t maxAlign = 1;
                    for (const auto& f : info->fields) {
                        maxAlign = std::max(maxAlign, fieldAlignInternal(f.type, mod));
                    }
                    return maxAlign;
                }
            }
            return alignof(char);
        }
        default:
            return alignof(char);
    }
}

void ModuleDDS::fillSampleFromValue(const StructInfo& info,
                                    const dds_topic_descriptor_t* desc,
                                    void* sample,
                                    const Value& msg)
{
    if (!sample)
        return;
    std::vector<size_t> fallbackOffsets;
    size_t clearSize = desc ? desc->m_size : computeLayout(info, fallbackOffsets);
    std::memset(sample, 0, clearSize);

    auto handleField = [&](size_t offset, const FieldInfo& field, const Value& fval) {
        char* target = static_cast<char*>(sample) + offset;
        switch (field.type.kind) {
            case FieldType::Kind::Bool: {
                bool b = fval.isBool() ? fval.asBool() : fval.isNumber() ? fval.asInt() != 0 : false;
                *reinterpret_cast<bool*>(target) = b;
                break;
            }
            case FieldType::Kind::Int32: {
                int32_t v = fval.isNumber() ? fval.asInt() : 0;
                *reinterpret_cast<int32_t*>(target) = v;
                break;
            }
            case FieldType::Kind::Float64: {
                double d = fval.isNumber() ? fval.asReal() : 0.0;
                *reinterpret_cast<double*>(target) = d;
                break;
            }
            case FieldType::Kind::Int64Pair: {
                int64_t v = pairToInt64(fval);
                *reinterpret_cast<int64_t*>(target) = v;
                break;
            }
            case FieldType::Kind::EnumRef: {
                int32_t v = 0;
                if (fval.isEnum())
                    v = fval.asEnum();
                else if (fval.isNumber())
                    v = fval.asInt();
                *reinterpret_cast<int32_t*>(target) = v;
                break;
            }
            case FieldType::Kind::String: {
                std::string s = isString(fval) ? toUTF8StdString(asStringObj(fval)->s) : "";
                char** ptr = reinterpret_cast<char**>(target);
                *ptr = s.empty() ? nullptr : dds_string_dup(s.c_str());
                break;
            }
            case FieldType::Kind::StructRef: {
                const StructInfo* subInfo = findStructInfo(field.type.refName);
                const dds_topic_descriptor_t* subDesc = nullptr;
                auto supIt = supportByType.find(field.type.refName);
                if (supIt != supportByType.end())
                    subDesc = supIt->second ? supIt->second->descriptor.get() : nullptr;
                if (subInfo)
                    fillSampleFromValue(*subInfo, subDesc, target, fval);
                break;
            }
            case FieldType::Kind::List: {
                dds_sequence_t* seq = reinterpret_cast<dds_sequence_t*>(target);
                if (!isList(fval) || !field.type.element) {
                    seq->_maximum = seq->_length = 0;
                    seq->_buffer = nullptr;
                    seq->_release = false;
                    break;
                }
                ObjList* lst = asList(fval);
                size_t len = lst->elts.size();
                seq->_maximum = static_cast<uint32_t>(len);
                seq->_length = static_cast<uint32_t>(len);
                size_t elemSz = typeSizeInternal(*field.type.element, this);
                seq->_buffer = elemSz > 0 ? static_cast<uint8_t*>(dds_alloc(elemSz * len)) : nullptr;
                seq->_release = true;
                if (!seq->_buffer || elemSz == 0)
                    break;
                std::memset(seq->_buffer, 0, elemSz * len);
                for (size_t idx = 0; idx < len; ++idx) {
                    Value ev = lst->elts.at(idx);
                    char* elemPtr = reinterpret_cast<char*>(seq->_buffer + elemSz * idx);
                    switch (field.type.element->kind) {
                        case FieldType::Kind::Bool:
                            *reinterpret_cast<bool*>(elemPtr) = ev.isBool() ? ev.asBool() : ev.isNumber() ? ev.asInt() != 0 : false;
                            break;
                        case FieldType::Kind::Int32:
                            *reinterpret_cast<int32_t*>(elemPtr) = ev.isNumber() ? ev.asInt() : 0;
                            break;
                        case FieldType::Kind::Float64:
                            *reinterpret_cast<double*>(elemPtr) = ev.isNumber() ? ev.asReal() : 0.0;
                            break;
                        case FieldType::Kind::Int64Pair:
                            *reinterpret_cast<int64_t*>(elemPtr) = pairToInt64(ev);
                            break;
                        case FieldType::Kind::EnumRef:
                            *reinterpret_cast<int32_t*>(elemPtr) = ev.isEnum() ? ev.asEnum() : (ev.isNumber() ? ev.asInt() : 0);
                            break;
                        case FieldType::Kind::String: {
                            auto strPtr = reinterpret_cast<char**>(elemPtr);
                            *strPtr = isString(ev) ? dds_string_dup(toUTF8StdString(asStringObj(ev)->s).c_str()) : nullptr;
                            break;
                        }
                        case FieldType::Kind::StructRef: {
                            const StructInfo* subInfo = findStructInfo(field.type.element->refName);
                            const dds_topic_descriptor_t* subDesc = nullptr;
                            auto sup = supportByType.find(field.type.element->refName);
                            if (sup != supportByType.end())
                                subDesc = sup->second ? sup->second->descriptor.get() : nullptr;
                            if (subInfo)
                                fillSampleFromValue(*subInfo, subDesc, elemPtr, ev);
                            break;
                        }
                        default:
                            break;
                    }
                }
                break;
            }
            default:
                break;
        }
    };

    if (desc && desc->m_ops) {
        const uint32_t* ops = desc->m_ops;
        size_t fieldIndex = 0;
        for (size_t i = 0; i < desc->m_nops; ) {
            uint32_t op = ops[i++];
            if (DDS_OP(op) == DDS_OP_RTS)
                break;
            if (DDS_OP(op) != DDS_OP_ADR)
                continue;
            if (fieldIndex >= info.fields.size())
                break;
            uint32_t offset = ops[i++];
            const FieldInfo& field = info.fields[fieldIndex++];
            Value fval = getFieldValue(msg, field.name);
            handleField(offset, field, fval);
        }
    } else {
        std::vector<size_t> offsets;
        computeLayout(info, offsets);
        for (size_t idx = 0; idx < info.fields.size() && idx < offsets.size(); ++idx) {
            handleField(offsets[idx], info.fields[idx], getFieldValue(msg, info.fields[idx].name));
        }
    }
}

Value ModuleDDS::valueFromSample(const StructInfo& info,
                                 const dds_topic_descriptor_t* desc,
                                 const void* sample,
                                 Value typeVal)
{
    Value inst = Value::objectInstanceVal(typeVal);
    ObjectInstance* obj = isObjectInstance(inst) ? asObjectInstance(inst) : nullptr;
    if (!sample || !obj)
        return inst;

    auto handleField = [&](size_t offset, const FieldInfo& field) {
        const char* src = static_cast<const char*>(sample) + offset;
        Value val = Value::nilVal();
        switch (field.type.kind) {
            case FieldType::Kind::Bool:
                val = Value::boolVal(*reinterpret_cast<const bool*>(src));
                break;
            case FieldType::Kind::Int32:
                val = Value::intVal(*reinterpret_cast<const int32_t*>(src));
                break;
            case FieldType::Kind::Float64:
                val = Value::realVal(*reinterpret_cast<const double*>(src));
                break;
            case FieldType::Kind::Int64Pair:
                val = int64ToPair(*reinterpret_cast<const int64_t*>(src));
                break;
            case FieldType::Kind::EnumRef:
                val = Value::intVal(*reinterpret_cast<const int32_t*>(src));
                break;
            case FieldType::Kind::String: {
                const char* s = *reinterpret_cast<char* const*>(src);
                val = s ? Value::stringVal(toUnicodeString(std::string(s))) : Value::nilVal();
                break;
            }
            case FieldType::Kind::StructRef: {
                const StructInfo* subInfo = findStructInfo(field.type.refName);
                const dds_topic_descriptor_t* subDesc = nullptr;
                auto sup = supportByType.find(field.type.refName);
                if (sup != supportByType.end())
                    subDesc = sup->second ? sup->second->descriptor.get() : nullptr;
                Value subtypeVal = resolveTypeValue(field.type.refName);
                if (subInfo && !subtypeVal.isNil())
                    val = valueFromSample(*subInfo, subDesc, src, subtypeVal);
                break;
            }
            case FieldType::Kind::List: {
                const dds_sequence_t* seq = reinterpret_cast<const dds_sequence_t*>(src);
                Value listVal = Value::listVal();
                ObjList* lst = asList(listVal);
                if (seq && seq->_buffer && field.type.element) {
                    size_t elemSz = typeSizeInternal(*field.type.element, this);
                    for (uint32_t idx = 0; idx < seq->_length && elemSz > 0; ++idx) {
                        const char* eptr = reinterpret_cast<const char*>(seq->_buffer + elemSz * idx);
                        Value ev = Value::nilVal();
                        switch (field.type.element->kind) {
                            case FieldType::Kind::Bool:
                                ev = Value::boolVal(*reinterpret_cast<const bool*>(eptr));
                                break;
                            case FieldType::Kind::Int32:
                                ev = Value::intVal(*reinterpret_cast<const int32_t*>(eptr));
                                break;
                            case FieldType::Kind::Float64:
                                ev = Value::realVal(*reinterpret_cast<const double*>(eptr));
                                break;
                            case FieldType::Kind::Int64Pair:
                                ev = int64ToPair(*reinterpret_cast<const int64_t*>(eptr));
                                break;
                            case FieldType::Kind::EnumRef:
                                ev = Value::intVal(*reinterpret_cast<const int32_t*>(eptr));
                                break;
                            case FieldType::Kind::String: {
                                const char* s = *reinterpret_cast<char* const*>(eptr);
                                ev = s ? Value::stringVal(toUnicodeString(std::string(s))) : Value::nilVal();
                                break;
                            }
                            case FieldType::Kind::StructRef: {
                                const StructInfo* subInfo = findStructInfo(field.type.element->refName);
                                const dds_topic_descriptor_t* subDesc = nullptr;
                                auto sup = supportByType.find(field.type.element->refName);
                                if (sup != supportByType.end())
                                    subDesc = sup->second ? sup->second->descriptor.get() : nullptr;
                                Value subtypeVal = resolveTypeValue(field.type.element->refName);
                                if (subInfo && !subtypeVal.isNil())
                                    ev = valueFromSample(*subInfo, subDesc, eptr, subtypeVal);
                                break;
                            }
                            default:
                                break;
                        }
                        lst->elts.push_back(ev);
                    }
                }
                val = listVal;
                break;
            }
            default:
                break;
        }
        setProperty(obj, toUnicodeString(field.name), val);
    };

    if (desc && desc->m_ops) {
        const uint32_t* ops = desc->m_ops;
        size_t fieldIndex = 0;
        for (size_t i = 0; i < desc->m_nops; ) {
            uint32_t op = ops[i++];
            if (DDS_OP(op) == DDS_OP_RTS)
                break;
            if (DDS_OP(op) != DDS_OP_ADR)
                continue;
            if (fieldIndex >= info.fields.size())
                break;
            uint32_t offset = ops[i++];
            handleField(offset, info.fields[fieldIndex++]);
        }
    } else {
        std::vector<size_t> offsets;
        computeLayout(info, offsets);
        for (size_t idx = 0; idx < info.fields.size() && idx < offsets.size(); ++idx) {
            handleField(offsets[idx], info.fields[idx]);
        }
    }
    return inst;
}

#endif // ROXAL_ENABLE_DDS
