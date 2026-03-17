#ifdef ROXAL_ENABLE_DDS

#include "dds/ModuleDDS.h"
#include "dds/DdsAdapter.h"
#include "dds/AsyncDDSManager.h"

#include "Object.h"
#include "Value.h"
#include "VM.h"
#include "dataflow/Signal.h"

#include <dds/dds.h>
#include <dds/ddsrt/types.h>
#include <dds/ddsrt/heap.h>
#include <dds/ddsc/dds_public_impl.h>
#include <dds/ddsc/dds_public_alloc.h>
#include <dds/ddsc/dds_opcodes.h>
#include <dds/ddsc/dds_public_qos.h>
#include <dds/ddsc/dds_public_dynamic_type.h>
#include <dds/ddsi/ddsi_typelib.h>

#include <filesystem>
#include <stdexcept>
#include <cstring>
#include <functional>
#include <algorithm>
#include <cctype>
#include <vector>

using namespace roxal;

namespace {
std::mutex gEntityMutex;
std::unordered_set<dds_entity_t> gDeletedEntities;
}

static Value getFieldValue(const Value& msg, const std::string& name);

ModuleDDS::ModuleDDS()
{
    moduleTypeValue = Value::objVal(newModuleTypeObj(toUnicodeString("dds")));
    ObjModuleType::allModules.push_back(moduleTypeValue);
}

ModuleDDS::~ModuleDDS()
{
    stopReaderThread();
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

void ModuleDDS::onModuleLoaded(VM& vm)
{
    // Register this module with VM for IDL import support
    vm.ddsModule = this;
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
    // register constants from IDL in the module
    if (adapter) {
        ObjModuleType* mod = asModuleType(moduleVal);
        auto shortName = [](const std::string& full) -> std::string {
            auto pos = full.rfind("::");
            if (pos == std::string::npos)
                return full;
            return full.substr(pos + 2);
        };
        for (const auto& c : adapter->constants()) {
            if (c.value.isNil())
                continue;
            icu::UnicodeString name = toUnicodeString(shortName(c.fullName));
            mod->vars.store(name, c.value, true);
        }
        // register typedef aliases as additional module vars pointing to the aliased type
        for (const auto& td : adapter->typedefs()) {
            icu::UnicodeString name = toUnicodeString(shortName(td.fullName));
            Value target = Value::nilVal();
            switch (td.aliasedType.kind) {
                case FieldType::Kind::StructRef:
                case FieldType::Kind::EnumRef:
                    target = resolveTypeValue(td.aliasedType.refName);
                    break;
                case FieldType::Kind::Int32:
                case FieldType::Kind::Bool:
                case FieldType::Kind::Float64:
                case FieldType::Kind::String:
                case FieldType::Kind::List:
                case FieldType::Kind::Int64:
                case FieldType::Kind::UInt64:
                    target = (td.aliasedType.kind == FieldType::Kind::Bool)   ? Value::typeSpecVal(ValueType::Bool)
                           : (td.aliasedType.kind == FieldType::Kind::Float64)? Value::typeSpecVal(ValueType::Real)
                           : (td.aliasedType.kind == FieldType::Kind::String) ? Value::typeSpecVal(ValueType::String)
                           : (td.aliasedType.kind == FieldType::Kind::List)   ? Value::typeSpecVal(ValueType::List)
                           : Value::typeSpecVal(ValueType::Int);
                    break;
                default:
                    break;
            }
            if (!target.isNil()) {
                mod->vars.store(name, target, true);
            }
        }
    }
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
    auto linkFn = [&](const char* name, NativeFn fn, uint32_t resolveArgMask = 0) {
        auto val = mod->vars.load(toUnicodeString(name));
        if (val.has_value() && isClosure(val.value())) {
            ObjClosure* cl = asClosure(val.value());
            asFunction(cl->function)->builtinInfo = make_ptr<BuiltinFuncInfo>(fn, std::vector<Value>{}, resolveArgMask);
        }
    };

    linkFn("create_participant", &ModuleDDS::dds_create_participant);
    linkFn("create_topic", &ModuleDDS::dds_create_topic);
    linkFn("create_writer", &ModuleDDS::dds_create_writer);
    linkFn("create_reader", &ModuleDDS::dds_create_reader);
    linkFn("close", &ModuleDDS::dds_close_entity);
    linkFn("write", &ModuleDDS::dds_write);
    linkFn("read", &ModuleDDS::dds_read);
    linkFn("create_writer_signal", &ModuleDDS::dds_create_writer_signal);
    linkFn("create_reader_signal", &ModuleDDS::dds_create_reader_signal);
    linkFn("writer_signal", &ModuleDDS::dds_writer_signal);
    linkFn("reader_signal", &ModuleDDS::dds_reader_signal);
}

void ModuleDDS::setProperty(ObjectInstance* obj, const icu::UnicodeString& name, const Value& v)
{
    auto h = name.hashCode();
    auto it = obj->findProperty(h);
    if (it)
        it->assign(v);
}

Value ModuleDDS::makeHandleValue(dds_entity_t ent)
{
    auto fp = newForeignPtrObj(reinterpret_cast<void*>(static_cast<intptr_t>(ent)));
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
    int32_t domainId = 0;
    Value qosVal = Value::nilVal();
    if (!args.empty()) {
        if (args[0].isNumber()) {
            domainId = args[0].asInt();
            if (args.size() > 1)
                qosVal = args[1];
        } else {
            qosVal = args[0];
        }
    }
    ModuleDDS* self = VM::instance().ddsModule;
    auto qos = self ? self->qosFromValue(qosVal) : std::unique_ptr<dds_qos_t, decltype(&dds_delete_qos)>(nullptr, dds_delete_qos);
    dds_entity_t participant = ::dds_create_participant(domainId, qos.get(), nullptr);
    if (participant < 0)
        throw std::runtime_error(std::string("dds_create_participant failed: ") + ::dds_strretcode(-participant));
    self = VM::instance().ddsModule;
    Value handleVal = self ? self->makeHandleValue(participant) : Value::nilVal();
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
        throw std::invalid_argument("dds.create_topic(participant, name, msg_type, qos=None) expects at least 3 args");
    Value part = args[0];
    Value nameVal = args[1];
    Value typeVal = args[2];
    Value qosVal = args.size() > 3 ? args[3] : Value::nilVal();
    std::string topicName;
    if (isString(nameVal))
        topicName = toUTF8StdString(asStringObj(nameVal)->s);
    else
        topicName = typeNameFromValue(nameVal);
    std::string typeName = typeNameFromValue(typeVal);
    if (topicName.empty())
        throw std::invalid_argument("topic name must be string");

    ModuleDDS* self = VM::instance().ddsModule;
    auto emptyQos = [](){ return std::unique_ptr<dds_qos_t, decltype(&dds_delete_qos)>(nullptr, dds_delete_qos); };
    auto qos = self ? self->qosFromValue(qosVal) : emptyQos();
    auto support = self ? self->buildDynamicTopic(part, topicName, typeName, qos.get()) : nullptr;
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

std::shared_ptr<ModuleDDS::TopicSupport> ModuleDDS::buildDynamicTopic(Value participantVal, const std::string& topicName, const std::string& typeName, dds_qos_t* qos)
{
    dds_entity_t participant = entityFromValue(participantVal, true);
    if (participant <= 0)
        return nullptr;

    auto self = VM::instance().ddsModule;
    if (!self || !self->adapter)
        return nullptr;

    auto existing = self->supportByType.find(typeName);
    if (existing != self->supportByType.end() && existing->second && existing->second->descriptor) {
        dds_entity_t topic = ::dds_create_topic(participant,
                                                existing->second->descriptor.get(),
                                                topicName.c_str(),
                                                qos,
                                                nullptr);
        if (topic > 0) {
            auto support = std::make_shared<TopicSupport>();
            support->descriptor = existing->second->descriptor;
            support->typeinfo = existing->second->typeinfo;
            support->typeName = typeName;
            support->entity = topic;
            support->handle = self->makeHandleValue(topic);
            support->nameStorage = existing->second->nameStorage;
            return support;
        }
    }

    const auto* structInfo = self->adapter->findStruct(typeName);
    if (!structInfo)
        return nullptr;
    bool hasArray = false;
    for (const auto& f : structInfo->fields) {
        if (f.type.isArray) { hasArray = true; break; }
    }

    auto nameStorage = std::make_shared<std::vector<std::string>>();
    auto keepName = [&](const std::string& n) -> const char* {
        nameStorage->push_back(n);
        return nameStorage->back().c_str();
    };

    // Try using serialized typeinfo from adapter if available. We skip it when
    // arrays are present because the CycloneDDS topic descriptor (m_ops) layout
    // differs from our manual packing for fixed arrays; using the generated
    // descriptor with our marshal code would misalign fields. Until we decode
    // and honor m_ops for arrays, stick to the manual layout for array-bearing
    // types.
    if (!hasArray) {
        std::vector<unsigned char> perInfo;
        std::vector<unsigned char> perMap;
        if (self->adapter->typeMetaFor(typeName, perInfo, perMap) && !perInfo.empty()) {
            ddsi_typeinfo* ti = ddsi_typeinfo_deser(perInfo.data(),
                                                   static_cast<uint32_t>(perInfo.size()));
            if (ti) {
                dds_topic_descriptor_t* descOut = nullptr;
                dds_return_t rc = dds_create_topic_descriptor(DDS_FIND_SCOPE_LOCAL_DOMAIN,
                                                             participant,
                                                             reinterpret_cast<const dds_typeinfo_t*>(ti),
                                                             DDS_SECS(5),
                                                             &descOut);
                if (rc == DDS_RETCODE_OK && descOut) {
                    dds_entity_t topic = ::dds_create_topic(participant, descOut, topicName.c_str(), qos, nullptr);
                    if (topic > 0) {
                        auto support = std::make_shared<TopicSupport>();
                        support->descriptor = std::shared_ptr<dds_topic_descriptor_t>(descOut, dds_delete_topic_descriptor);
                        support->typeinfo = std::shared_ptr<ddsi_typeinfo>(ti, ddsi_typeinfo_free);
                        support->typeName = typeName;
                        support->entity = topic;
                        support->handle = self ? self->makeHandleValue(topic) : Value::nilVal();
                        support->nameStorage = nameStorage;
                        return support;
                    }
                    dds_delete_topic_descriptor(descOut);
                }
                ddsi_typeinfo_free(ti);
            }
        }
    }

    std::unordered_map<std::string, dds_dynamic_type_t> enumTypes;
    std::unordered_map<std::string, dds_dynamic_type_t> structTypes;
    std::unordered_map<std::string, dds_dynamic_type_t> stringTypes;

    auto canonicalStructName = [&](const std::string& name) -> std::string {
        const auto* sInfo = self->adapter->findStruct(name);
        return sInfo ? sInfo->fullName : name;
    };
    auto canonicalEnumName = [&](const std::string& name) -> std::string {
        const auto* eInfo = self->adapter->findEnum(name);
        return eInfo ? eInfo->fullName : name;
    };

    std::function<dds_dynamic_type_spec_t(const FieldType&)> makeSpec;
    std::function<dds_dynamic_type_t(const std::string&)> makeStructType =
        [&](const std::string& requestedName) -> dds_dynamic_type_t {
            std::string full = canonicalStructName(requestedName);
            auto it = structTypes.find(full);
            if (it != structTypes.end())
                return it->second;
            const auto* sInfo = self->adapter->findStruct(full);
            dds_dynamic_type_descriptor_t sd{};
            sd.kind = DDS_DYNAMIC_STRUCTURE;
            sd.name = keepName(full);
            dds_dynamic_type_t stype = dds_dynamic_type_create(participant, sd);
            if (stype.ret != DDS_RETCODE_OK || !sInfo) {
                structTypes.emplace(full, stype);
                return stype;
            }
            dds_dynamic_type_extensibility ext = DDS_DYNAMIC_TYPE_EXT_APPENDABLE;
            if (sInfo->extensibility == IDL_FINAL)
                ext = DDS_DYNAMIC_TYPE_EXT_FINAL;
            else if (sInfo->extensibility == IDL_MUTABLE)
                ext = DDS_DYNAMIC_TYPE_EXT_MUTABLE;
            dds_dynamic_type_set_extensibility(&stype, ext);
            dds_dynamic_type_set_nested(&stype, false);
            dds_dynamic_type_set_autoid(&stype, DDS_DYNAMIC_TYPE_AUTOID_SEQUENTIAL);
            uint32_t memberId = 0;
            for (const auto& field : sInfo->fields) {
                auto spec = makeSpec(field.type);
                dds_dynamic_member_descriptor_t mdesc{};
                uint32_t thisId = memberId++;
                if (spec.kind == DDS_DYNAMIC_TYPE_KIND_PRIMITIVE)
                    mdesc = DDS_DYNAMIC_MEMBER_PRIM(spec.type.primitive, field.name.c_str());
                else
                    mdesc = DDS_DYNAMIC_MEMBER_(spec, field.name.c_str(), thisId, DDS_DYNAMIC_MEMBER_INDEX_END);
                mdesc.id = thisId;
                dds_return_t mrc = dds_dynamic_type_add_member(&stype, mdesc);
                if (mrc != DDS_RETCODE_OK)
                    throw std::runtime_error("Failed to add member " + field.name + ": " + dds_strretcode(-mrc));
                if (field.isKey) {
                    dds_return_t krc = dds_dynamic_member_set_key(&stype, mdesc.id, true);
                    if (krc != DDS_RETCODE_OK) {
                        fprintf(stderr, "Failed to set key on %s.%s: %s\n",
                                sInfo->fullName.c_str(), field.name.c_str(), dds_strretcode(-krc));
                    }
                }
                if (field.isOptional)
                    dds_dynamic_member_set_optional(&stype, mdesc.id, true);
            }
            structTypes.emplace(full, stype);
            return stype;
        };

    auto makeEnumType = [&](const std::string& name) -> dds_dynamic_type_t {
        std::string enameCanonical = canonicalEnumName(name);
        auto it = enumTypes.find(enameCanonical);
        if (it != enumTypes.end())
            return it->second;
        const auto* einfo = self->adapter->findEnum(name);
        dds_dynamic_type_descriptor_t edesc{};
        edesc.kind = DDS_DYNAMIC_ENUMERATION;
        std::string ename = enameCanonical.empty() ? std::string("enum") : enameCanonical;
        edesc.name = keepName(ename);
        dds_dynamic_type_t etype = dds_dynamic_type_create(participant, edesc);
        if (etype.ret != DDS_RETCODE_OK) {
            throw std::runtime_error("Failed to create enum type " + ename + ": " + dds_strretcode(-etype.ret));
        }
        if (etype.ret == DDS_RETCODE_OK && einfo) {
            for (const auto& val : einfo->values) {
                dds_dynamic_type_add_enum_literal(&etype, val.first.c_str(),
                                                  DDS_DYNAMIC_ENUM_LITERAL_VALUE(val.second),
                                                  false);
            }
        }
        enumTypes.emplace(enameCanonical, etype);
        return etype;
    };

    auto seqTypeName = [&](const FieldType& elem, bool bounded, uint32_t bound) {
        std::string base;
        switch (elem.kind) {
            case FieldType::Kind::StructRef: base = canonicalStructName(elem.refName); break;
            case FieldType::Kind::EnumRef: base = canonicalEnumName(elem.refName); break;
            case FieldType::Kind::String: base = "string"; break;
            case FieldType::Kind::Bool: base = "bool"; break;
            case FieldType::Kind::Int32: base = "int32"; break;
            case FieldType::Kind::Int64: base = "int64"; break;
            case FieldType::Kind::UInt64: base = "uint64"; break;
            case FieldType::Kind::Float64: base = "float64"; break;
            case FieldType::Kind::List: base = "list"; break;
            default: base = "seq";
        }
        std::string name = "seq<" + base + ">";
        if (bounded && bound > 0)
            name += "[b" + std::to_string(bound) + "]";
        return name;
    };

    makeSpec =
        [&](const FieldType& ft) -> dds_dynamic_type_spec_t {
            dds_dynamic_type_spec_t spec{};
            switch (ft.kind) {
                case FieldType::Kind::Bool: spec.kind = DDS_DYNAMIC_TYPE_KIND_PRIMITIVE; spec.type.primitive = DDS_DYNAMIC_BOOLEAN; break;
                case FieldType::Kind::Int32: spec.kind = DDS_DYNAMIC_TYPE_KIND_PRIMITIVE; spec.type.primitive = DDS_DYNAMIC_INT32; break;
                case FieldType::Kind::Int64: spec.kind = DDS_DYNAMIC_TYPE_KIND_PRIMITIVE; spec.type.primitive = DDS_DYNAMIC_INT64; break;
                case FieldType::Kind::UInt64: spec.kind = DDS_DYNAMIC_TYPE_KIND_PRIMITIVE; spec.type.primitive = DDS_DYNAMIC_UINT64; break;
                case FieldType::Kind::Float64: spec.kind = DDS_DYNAMIC_TYPE_KIND_PRIMITIVE; spec.type.primitive = DDS_DYNAMIC_FLOAT64; break;
                case FieldType::Kind::String: {
                    std::string key = ft.bounded ? "string8[b" + std::to_string(ft.bound) + "]" : "string8";
                    dds_dynamic_type_t st{};
                    auto it = stringTypes.find(key);
                    if (it != stringTypes.end()) {
                        st = it->second;
                    } else {
                        dds_dynamic_type_descriptor_t sdesc{};
                        sdesc.kind = DDS_DYNAMIC_STRING8;
                        sdesc.name = keepName("_str");
                        if (ft.bounded && ft.bound > 0) {
                            static thread_local uint32_t boundsBuf[1];
                            boundsBuf[0] = ft.bound;
                            sdesc.bounds = boundsBuf;
                            sdesc.num_bounds = 1;
                        }
                        st = dds_dynamic_type_create(participant, sdesc);
                        if (st.ret != DDS_RETCODE_OK)
                            throw std::runtime_error("Failed to build string type: " + std::string(dds_strretcode(-st.ret)));
                        stringTypes.emplace(key, st);
                    }
                    spec = DDS_DYNAMIC_TYPE_SPEC(dds_dynamic_type_ref(&st));
                    break;
                }
                case FieldType::Kind::List: {
                    dds_dynamic_type_descriptor_t seqDesc{};
                    seqDesc.kind = ft.isArray ? DDS_DYNAMIC_ARRAY : DDS_DYNAMIC_SEQUENCE;
                    seqDesc.element_type = ft.element ? makeSpec(*ft.element)
                                                      : dds_dynamic_type_spec_t{DDS_DYNAMIC_TYPE_KIND_PRIMITIVE, {.primitive = DDS_DYNAMIC_INT32}};
                    if (ft.bounded || ft.isArray) {
                        static thread_local uint32_t boundsBuf[1];
                        boundsBuf[0] = ft.bound;
                        seqDesc.bounds = boundsBuf;
                        seqDesc.num_bounds = 1;
                    }
                    std::string seqName = seqTypeName(ft.element ? *ft.element : FieldType{FieldType::Kind::Int32}, ft.bounded || ft.isArray, ft.bound);
                    seqDesc.name = keepName(seqName);
                    dds_dynamic_type_t seqType = dds_dynamic_type_create(participant, seqDesc);
                    if (seqType.ret != DDS_RETCODE_OK)
                        throw std::runtime_error("Failed to build sequence type: " + std::string(dds_strretcode(-seqType.ret)));
                    spec = DDS_DYNAMIC_TYPE_SPEC(dds_dynamic_type_ref(&seqType));
                    break;
                }
                case FieldType::Kind::EnumRef: {
                    dds_dynamic_type_t etype = makeEnumType(ft.refName);
                    spec = DDS_DYNAMIC_TYPE_SPEC(dds_dynamic_type_ref(&etype));
                    break;
                }
                case FieldType::Kind::StructRef: {
                    std::string snameCanonical = canonicalStructName(ft.refName);
                    dds_dynamic_type_t st = makeStructType(snameCanonical);
                    if (st.ret == DDS_RETCODE_OK)
                        spec = DDS_DYNAMIC_TYPE_SPEC(dds_dynamic_type_ref(&st));
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
    dds_entity_t topic = ::dds_create_topic(participant, descOut, topicName.c_str(), qos, nullptr);
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
    support->handle = self ? self->makeHandleValue(topic) : Value::nilVal();
    support->nameStorage = nameStorage;
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
    Value qosVal = args.size() > 2 ? args[2] : Value::nilVal();
    auto qos = self->qosFromValue(qosVal);
    auto topicSupport = self->lookupSupport(args[1]);
    Value handleVal = Value::nilVal();
    std::string typeName = typeNameFromValue(args[1]);
    if (participant > 0 && topicEnt > 0) {
        dds_entity_t writer = ::dds_create_writer(participant, topicEnt, qos.get(), nullptr);
        if (writer < 0)
            throw std::runtime_error(std::string("dds_create_writer failed: ") + dds_strretcode(-writer));
        handleVal = self->makeHandleValue(writer);
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
    Value qosVal = args.size() > 2 ? args[2] : Value::nilVal();
    auto qos = self->qosFromValue(qosVal);
    auto topicSupport = self->lookupSupport(args[1]);
    Value handleVal = Value::nilVal();
    std::string typeName = typeNameFromValue(args[1]);
    if (participant > 0 && topicEnt > 0) {
        dds_entity_t reader = ::dds_create_reader(participant, topicEnt, qos.get(), nullptr);
        if (reader < 0)
            throw std::runtime_error(std::string("dds_create_reader failed: ") + dds_strretcode(-reader));
        handleVal = self->makeHandleValue(reader);
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
        auto it = inst->findProperty(handleName.hashCode());
        if (it && isForeignPtr(it->value))
            fp = asForeignPtr(it->value);
    }
    if (fp) {
        dds_entity_t e = static_cast<dds_entity_t>(reinterpret_cast<intptr_t>(fp->ptr));
        if (e > 0) {
            if (ModuleDDS* self = VM::instance().ddsModule)
                self->deleteEntityOnce(e);
        }
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
    // Allocate sample - ownership will be transferred to async operation
    void* sample = dds_alloc(desc->m_size);
    if (!sample)
        throw std::runtime_error("dds.write failed to allocate sample");
    self->fillSampleFromValue(*info, desc, sample, args[1]);

    // Submit async write operation
    PendingDDSOp op;
    op.type = PendingDDSOp::Type::DdsWrite;
    op.writer = writer;
    op.sample = sample;  // Transfer ownership
    op.descriptor = support->descriptor;  // Shared ownership for cleanup

    return AsyncDDSManager::instance().submit(std::move(op));
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

Value ModuleDDS::dds_create_writer_signal(VM&, ArgsView args)
{
    if (args.size() < 1)
        throw std::invalid_argument("dds.create_writer_signal(writer, initial=nil) expects writer");
    Value writerVal = args[0];
    Value initial = args.size() > 1 ? args[1] : Value::nilVal();
    ModuleDDS* self = VM::instance().ddsModule;
    if (!self)
        return Value::nilVal();
    return self->createWriterSignal(writerVal, initial);
}

Value ModuleDDS::dds_create_reader_signal(VM&, ArgsView args)
{
    if (args.size() < 1)
        throw std::invalid_argument("dds.create_reader_signal(reader, initial=nil) expects reader");
    Value readerVal = args[0];
    Value initial = args.size() > 1 ? args[1] : Value::nilVal();
    ModuleDDS* self = VM::instance().ddsModule;
    if (!self)
        return Value::nilVal();
    return self->createReaderSignal(readerVal, initial);
}

dds_entity_t ModuleDDS::entityFromValue(const Value& v, bool allowNil)
{
    if (isForeignPtr(v)) {
        return static_cast<dds_entity_t>(reinterpret_cast<intptr_t>(asForeignPtr(v)->ptr));
    } else if (isObjectInstance(v)) {
        ObjectInstance* inst = asObjectInstance(v);
        icu::UnicodeString handleName = toUnicodeString("handle");
        auto it = inst->findProperty(handleName.hashCode());
        if (it && isForeignPtr(it->value))
            return static_cast<dds_entity_t>(reinterpret_cast<intptr_t>(asForeignPtr(it->value)->ptr));
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

Value ModuleDDS::createWriterSignal(const Value& writerVal, const Value& initial)
{
    dds_entity_t writer = entityFromValue(writerVal, true);
    if (writer <= 0)
        throw std::invalid_argument("dds.create_writer_signal requires a valid writer");
    auto support = lookupSupport(writerVal);
    if (!support)
        throw std::runtime_error("Writer has no associated topic support");
    auto sigPtr = df::Signal::newSourceSignal(0.0);
    Value sigVal = Value::signalVal(sigPtr);
    if (!initial.isNil())
        sigPtr->set(initial);
    registerWriterSignal(sigVal, writerVal, writer, support->typeName);
    return sigVal;
}

Value ModuleDDS::createReaderSignal(const Value& readerVal, const Value& initial)
{
    dds_entity_t reader = entityFromValue(readerVal, true);
    if (reader <= 0)
        throw std::invalid_argument("dds.create_reader_signal requires a valid reader");
    auto support = lookupSupport(readerVal);
    if (!support)
        throw std::runtime_error("Reader has no associated topic support");
    auto sigPtr = df::Signal::newSourceSignal(0.0);
    Value sigVal = Value::signalVal(sigPtr);
    if (!initial.isNil())
        sigPtr->set(initial);
    registerReaderSignal(sigVal, readerVal, reader, support->typeName);
    startReaderThread();
    return sigVal;
}

void ModuleDDS::registerWriterSignal(const Value& sigVal, const Value& writerVal, dds_entity_t writer, const std::string& typeName)
{
    auto support = lookupSupport(writerVal);
    auto desc = support ? support->descriptor : nullptr;
    std::lock_guard<std::mutex> lock(signalMutex);
    writerSignals.push_back({sigVal.weakRef(), writer, typeName, desc});
    if (isSignal(sigVal)) {
        ObjSignal* objSig = asSignal(sigVal);
        auto sig = objSig->signal;
        std::shared_ptr<dds_topic_descriptor_t> descHold = desc;
        sig->addValueChangedCallback([this, writer, typeName, descHold](TimePoint, ptr<df::Signal>, const Value& sampleVal){
            if (writer <= 0)
                return;
            auto info = findStructInfo(typeName);
            if (!info)
                return;
            Value typeVal = resolveTypeValue(typeName);
            if (typeVal.isNil())
                return;
            const dds_topic_descriptor_t* descPtr = descHold ? descHold.get() : nullptr;
            size_t sampleSize = descPtr ? descPtr->m_size : computeLayout(*info, *new std::vector<size_t>());
            auto sample = std::unique_ptr<void, std::function<void(void*)>>(
                dds_alloc(sampleSize),
                [descPtr, sampleSize](void* p){
                    if (p) {
                        if (descPtr)
                            dds_sample_free(p, descPtr, DDS_FREE_ALL);
                        else
                            dds_free(p);
                    }
                });
            fillSampleFromValue(*info, descPtr, sample.get(), sampleVal);
            dds_return_t rc = ::dds_write(writer, sample.get());
            if (rc < 0) {
                fprintf(stderr, "dds_write signal error: %s\n", dds_strretcode(-rc));
            }
        });
    }
}

Value ModuleDDS::dds_writer_signal(VM& vm, ArgsView args)
{
    if (args.size() < 2)
        throw std::invalid_argument("dds.writer_signal(name, msg_type, participant=nil, qos=nil, initial=nil) expects at least name and msg_type");
    Value nameVal = args[0];
    Value typeVal = args[1];
    Value participantVal = args.size() > 2 ? args[2] : Value::nilVal();
    Value qosVal = args.size() > 3 ? args[3] : Value::nilVal();
    Value initial = args.size() > 4 ? args[4] : Value::nilVal();

    ModuleDDS* self = VM::instance().ddsModule;
    if (!self)
        return Value::nilVal();

    auto ensureParticipant = [&](const Value& existing) -> Value {
        if (isObjectInstance(existing) || isForeignPtr(existing))
            return existing;
        if (self->defaultParticipant.isNonNil())
            return self->defaultParticipant;
        std::vector<Value> pargs;
        if (qosVal.isNonNil()) {
            pargs.push_back(Value::intVal(0));
            pargs.push_back(qosVal);
        } else {
            pargs.push_back(Value::intVal(0));
        }
        Value p = dds_create_participant(vm, ArgsView(pargs.data(), pargs.size()));
        self->defaultParticipant = p;
        return p;
    };

    Value participant = ensureParticipant(participantVal);
    std::vector<Value> targs{participant, nameVal, typeVal};
    if (qosVal.isNonNil())
        targs.push_back(qosVal);
    Value topic = dds_create_topic(vm, ArgsView(targs.data(), targs.size()));

    std::vector<Value> wargs{participant, topic};
    if (qosVal.isNonNil())
        wargs.push_back(qosVal);
    Value writer = dds_create_writer(vm, ArgsView(wargs.data(), wargs.size()));

    std::vector<Value> sargs{writer, initial};
    return dds_create_writer_signal(vm, ArgsView(sargs.data(), sargs.size()));
}

Value ModuleDDS::dds_reader_signal(VM& vm, ArgsView args)
{
    if (args.size() < 2)
        throw std::invalid_argument("dds.reader_signal(name, msg_type, participant=nil, qos=nil) expects at least name and msg_type");
    Value nameVal = args[0];
    Value typeVal = args[1];
    Value participantVal = args.size() > 2 ? args[2] : Value::nilVal();
    Value qosVal = args.size() > 3 ? args[3] : Value::nilVal();
    Value initial = args.size() > 4 ? args[4] : Value::nilVal();

    ModuleDDS* self = VM::instance().ddsModule;
    if (!self)
        return Value::nilVal();

    auto ensureParticipant = [&](const Value& existing) -> Value {
        if (isObjectInstance(existing) || isForeignPtr(existing))
            return existing;
        if (self->defaultParticipant.isNonNil())
            return self->defaultParticipant;
        std::vector<Value> pargs;
        if (qosVal.isNonNil()) {
            pargs.push_back(Value::intVal(0));
            pargs.push_back(qosVal);
        } else {
            pargs.push_back(Value::intVal(0));
        }
        Value p = dds_create_participant(vm, ArgsView(pargs.data(), pargs.size()));
        self->defaultParticipant = p;
        return p;
    };

    Value participant = ensureParticipant(participantVal);
    std::vector<Value> targs{participant, nameVal, typeVal};
    if (qosVal.isNonNil())
        targs.push_back(qosVal);
    Value topic = dds_create_topic(vm, ArgsView(targs.data(), targs.size()));

    std::vector<Value> rargs{participant, topic};
    if (qosVal.isNonNil())
        rargs.push_back(qosVal);
    Value reader = dds_create_reader(vm, ArgsView(rargs.data(), rargs.size()));

    std::vector<Value> sargs{reader, initial};
    return dds_create_reader_signal(vm, ArgsView(sargs.data(), sargs.size()));
}

void ModuleDDS::registerReaderSignal(const Value& sigVal, const Value& readerVal, dds_entity_t reader, const std::string& typeName)
{
    auto support = lookupSupport(readerVal);
    auto desc = support ? support->descriptor : nullptr;
    std::lock_guard<std::mutex> lock(signalMutex);
    readerSignals.push_back({sigVal.weakRef(), reader, typeName, desc});
}

void ModuleDDS::unregisterSignal(const Value& sigVal)
{
    std::lock_guard<std::mutex> lock(signalMutex);
    auto prune = [&](std::vector<SignalBinding>& vec) {
        vec.erase(std::remove_if(vec.begin(), vec.end(), [&](const SignalBinding& b){
            return !b.signal.isAlive() || b.signal == sigVal || b.signal.strongRef() == sigVal;
        }), vec.end());
    };
    prune(writerSignals);
    prune(readerSignals);
}

static Value getFieldValue(const Value& msg, const std::string& name)
{
    if (!isObjectInstance(msg))
        return Value::nilVal();
    ObjectInstance* inst = asObjectInstance(msg);
    auto it = inst->findProperty(toUnicodeString(name).hashCode());
    if (it)
        return it->value;
    return Value::nilVal();
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
        case FieldType::Kind::String: {
            if (ft.bounded && ft.bound > 0)
                return static_cast<size_t>(ft.bound + 1); // inline buffer including null
            return sizeof(char*);
        }
        case FieldType::Kind::List:
            if (ft.isArray && ft.element) {
                size_t elemSz = typeSizeInternal(*ft.element, mod);
                return elemSz * static_cast<size_t>(ft.bound);
            }
            return sizeof(dds_sequence_t);
        case FieldType::Kind::EnumRef: return sizeof(int32_t);
        case FieldType::Kind::Int64: return sizeof(int64_t);
        case FieldType::Kind::UInt64: return sizeof(uint64_t);
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
        default: return 0;
    }
}

size_t ModuleDDS::fieldAlignInternal(const FieldType& ft, ModuleDDS* mod)
{
    switch (ft.kind) {
        case FieldType::Kind::Bool: return alignof(bool);
        case FieldType::Kind::Int32: return alignof(int32_t);
        case FieldType::Kind::Float64: return alignof(double);
        case FieldType::Kind::Int64: return alignof(int64_t);
        case FieldType::Kind::UInt64: return alignof(uint64_t);
        case FieldType::Kind::EnumRef: return alignof(int32_t);
        case FieldType::Kind::String:
            if (ft.bounded && ft.bound > 0)
                return alignof(char);
            return alignof(char*);
        case FieldType::Kind::List:
            if (ft.isArray && ft.element)
                return fieldAlignInternal(*ft.element, mod);
            return alignof(dds_sequence_t);
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
    auto canonicalName = [&](const std::string& n) {
        if (adapter) {
            if (const StructInfo* si = adapter->findStruct(n))
                return si->fullName;
        }
        return n;
    };

    auto handleField = [&](size_t offset, const FieldInfo& field, const Value& fval) {
        if (field.isOptional && fval.isNil()) {
            // leave zeroed/null to indicate absence
            return;
        }
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
            case FieldType::Kind::Int64: {
                int64_t v = fval.isNumber() ? fval.asInt() : 0;
                *reinterpret_cast<int64_t*>(target) = v;
                break;
            }
            case FieldType::Kind::UInt64: {
                uint64_t v = fval.isNumber() ? static_cast<uint64_t>(fval.asInt()) : 0;
                *reinterpret_cast<uint64_t*>(target) = v;
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
                if (field.type.bounded && field.type.bound > 0) {
                    if (s.size() > field.type.bound)
                        throw std::runtime_error("DDS string field '" + field.name + "' exceeds bound " + std::to_string(field.type.bound));
                    size_t cap = static_cast<size_t>(field.type.bound + 1);
                    std::memset(target, 0, cap);
                    if (!s.empty())
                        std::memcpy(target, s.c_str(), s.size());
                } else {
                    char** ptr = reinterpret_cast<char**>(target);
                    *ptr = s.empty() ? nullptr : dds_string_dup(s.c_str());
                }
                break;
            }
            case FieldType::Kind::StructRef: {
                std::string refName = canonicalName(field.type.refName);
                const StructInfo* subInfo = findStructInfo(refName);
                const dds_topic_descriptor_t* subDesc = nullptr;
                auto supIt = supportByType.find(refName);
                if (supIt != supportByType.end())
                    subDesc = supIt->second ? supIt->second->descriptor.get() : nullptr;
                if (subInfo)
                    fillSampleFromValue(*subInfo, subDesc, target, fval);
                break;
            }
            case FieldType::Kind::List: {
                if (!isList(fval) || !field.type.element) {
                    if (!field.type.isArray) {
                        dds_sequence_t* seq = reinterpret_cast<dds_sequence_t*>(target);
                        seq->_maximum = seq->_length = 0;
                        seq->_buffer = nullptr;
                        seq->_release = false;
                    }
                    break;
                }
                ObjList* lst = asList(fval);
                size_t len = lst->length();
                size_t elemSz = typeSizeInternal(*field.type.element, this);
                if (field.type.isArray) {
                    if (field.type.bounded && field.type.bound > 0 && len != field.type.bound)
                        throw std::runtime_error("DDS array field '" + field.name + "' length mismatch: expected " + std::to_string(field.type.bound) + " got " + std::to_string(len));
                    if (elemSz == 0)
                        break;
                    std::memset(target, 0, elemSz * field.type.bound);
                    for (size_t idx = 0; idx < len; ++idx) {
                        Value ev = lst->getElement(idx);
                        char* elemPtr = static_cast<char*>(target) + elemSz * idx;
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
                            case FieldType::Kind::Int64:
                                *reinterpret_cast<int64_t*>(elemPtr) = ev.isNumber() ? ev.asInt() : 0;
                                break;
                            case FieldType::Kind::UInt64:
                                *reinterpret_cast<uint64_t*>(elemPtr) = ev.isNumber() ? static_cast<uint64_t>(ev.asInt()) : 0;
                                break;
                            case FieldType::Kind::EnumRef:
                                *reinterpret_cast<int32_t*>(elemPtr) = ev.isEnum() ? ev.asEnum() : (ev.isNumber() ? ev.asInt() : 0);
                                break;
                            case FieldType::Kind::String: {
                                std::string s = isString(ev) ? toUTF8StdString(asStringObj(ev)->s) : "";
                                if (field.type.element->bounded && field.type.element->bound > 0) {
                                    size_t cap = static_cast<size_t>(field.type.element->bound + 1);
                                    if (s.size() > field.type.element->bound)
                                        throw std::runtime_error("DDS string array element in '" + field.name + "' exceeds bound " + std::to_string(field.type.element->bound));
                                    std::memset(elemPtr, 0, cap);
                                    if (!s.empty())
                                        std::memcpy(elemPtr, s.c_str(), s.size());
                                } else {
                                    auto strPtr = reinterpret_cast<char**>(elemPtr);
                                    *strPtr = s.empty() ? nullptr : dds_string_dup(s.c_str());
                                }
                                break;
                            }
                            case FieldType::Kind::StructRef: {
                                std::string refName = canonicalName(field.type.element->refName);
                                const StructInfo* subInfo = findStructInfo(refName);
                                const dds_topic_descriptor_t* subDesc = nullptr;
                                auto sup = supportByType.find(refName);
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
                } else {
                    dds_sequence_t* seq = reinterpret_cast<dds_sequence_t*>(target);
                    if (field.type.bounded && field.type.bound > 0 && len > field.type.bound)
                        throw std::runtime_error("DDS sequence field '" + field.name + "' exceeds bound " + std::to_string(field.type.bound));
                    uint32_t max = field.type.bounded && field.type.bound > 0 ? field.type.bound : static_cast<uint32_t>(len);
                    seq->_maximum = max;
                    seq->_length = static_cast<uint32_t>(len);
                    seq->_buffer = elemSz > 0 ? static_cast<uint8_t*>(dds_alloc(elemSz * len)) : nullptr;
                    seq->_release = true;
                    if (!seq->_buffer || elemSz == 0)
                        break;
                    std::memset(seq->_buffer, 0, elemSz * len);
                    for (size_t idx = 0; idx < len; ++idx) {
                        Value ev = lst->getElement(idx);
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
                            case FieldType::Kind::Int64:
                                *reinterpret_cast<int64_t*>(elemPtr) = ev.isNumber() ? ev.asInt() : 0;
                                break;
                            case FieldType::Kind::UInt64:
                                *reinterpret_cast<uint64_t*>(elemPtr) = ev.isNumber() ? static_cast<uint64_t>(ev.asInt()) : 0;
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
                                std::string refName = canonicalName(field.type.element->refName);
                                const StructInfo* subInfo = findStructInfo(refName);
                                const dds_topic_descriptor_t* subDesc = nullptr;
                                auto sup = supportByType.find(refName);
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
                }
                break;
            }
            default:
                break;
        }
    };

    if (desc && desc->m_ops) {
        auto offsets = offsetsFor(info, desc);
        for (size_t idx = 0; idx < offsets.size() && idx < info.fields.size(); ++idx) {
            const FieldInfo& field = info.fields[idx];
            Value fval = getFieldValue(msg, field.name);
            handleField(offsets[idx], field, fval);
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
    auto canonicalName = [&](const std::string& n) {
        if (adapter) {
            if (const StructInfo* si = adapter->findStruct(n))
                return si->fullName;
        }
        return n;
    };

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
            case FieldType::Kind::Int64:
                val = Value::intVal(*reinterpret_cast<const int64_t*>(src));
                break;
            case FieldType::Kind::UInt64:
                val = Value::intVal(static_cast<int64_t>(*reinterpret_cast<const uint64_t*>(src)));
                break;
            case FieldType::Kind::EnumRef:
                val = Value::intVal(*reinterpret_cast<const int32_t*>(src));
                break;
            case FieldType::Kind::String: {
                if (field.type.bounded && field.type.bound > 0) {
                    const char* buf = reinterpret_cast<const char*>(src);
                    val = Value::stringVal(toUnicodeString(std::string(buf ? buf : "")));
                } else {
                    const char* s = *reinterpret_cast<char* const*>(src);
                    val = s ? Value::stringVal(toUnicodeString(std::string(s))) : Value::nilVal();
                }
                break;
            }
            case FieldType::Kind::StructRef: {
                std::string refName = canonicalName(field.type.refName);
                const StructInfo* subInfo = findStructInfo(refName);
                const dds_topic_descriptor_t* subDesc = nullptr;
                auto sup = supportByType.find(refName);
                if (sup != supportByType.end())
                    subDesc = sup->second ? sup->second->descriptor.get() : nullptr;
                Value subtypeVal = resolveTypeValue(refName);
                if (subInfo && !subtypeVal.isNil())
                    val = valueFromSample(*subInfo, subDesc, src, subtypeVal);
                break;
            }
            case FieldType::Kind::List: {
                Value listVal = Value::listVal();
                ObjList* lst = asList(listVal);
                if (field.type.element) {
                    size_t elemSz = typeSizeInternal(*field.type.element, this);
                    if (field.type.isArray) {
                        uint32_t len = field.type.bound;
                        for (uint32_t idx = 0; idx < len && elemSz > 0; ++idx) {
                            const char* eptr = static_cast<const char*>(src) + elemSz * idx;
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
                                case FieldType::Kind::Int64:
                                    ev = Value::intVal(*reinterpret_cast<const int64_t*>(eptr));
                                    break;
                                case FieldType::Kind::UInt64:
                                    ev = Value::intVal(static_cast<int64_t>(*reinterpret_cast<const uint64_t*>(eptr)));
                                    break;
                                case FieldType::Kind::EnumRef:
                                    ev = Value::intVal(*reinterpret_cast<const int32_t*>(eptr));
                                    break;
                            case FieldType::Kind::String: {
                                if (field.type.element->bounded && field.type.element->bound > 0) {
                                    const char* buf = reinterpret_cast<const char*>(eptr);
                                    ev = Value::stringVal(toUnicodeString(std::string(buf ? buf : "")));
                                } else {
                                    const char* s = *reinterpret_cast<char* const*>(eptr);
                                    ev = s ? Value::stringVal(toUnicodeString(std::string(s))) : Value::nilVal();
                                }
                                break;
                            }
                                case FieldType::Kind::StructRef: {
                                    std::string refName = canonicalName(field.type.element->refName);
                                    const StructInfo* subInfo = findStructInfo(refName);
                                    const dds_topic_descriptor_t* subDesc = nullptr;
                                    auto sup = supportByType.find(refName);
                                    if (sup != supportByType.end())
                                        subDesc = sup->second ? sup->second->descriptor.get() : nullptr;
                                    Value subtypeVal = resolveTypeValue(refName);
                                    if (subInfo && !subtypeVal.isNil())
                                        ev = valueFromSample(*subInfo, subDesc, eptr, subtypeVal);
                                    break;
                                }
                                default:
                                    break;
                            }
                            lst->append(ev);
                        }
                    } else {
                        const dds_sequence_t* seq = reinterpret_cast<const dds_sequence_t*>(src);
                        if (seq && seq->_buffer) {
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
                                    case FieldType::Kind::Int64:
                                        ev = Value::intVal(*reinterpret_cast<const int64_t*>(eptr));
                                        break;
                                    case FieldType::Kind::UInt64:
                                        ev = Value::intVal(static_cast<int64_t>(*reinterpret_cast<const uint64_t*>(eptr)));
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
                                        std::string refName = canonicalName(field.type.element->refName);
                                        const StructInfo* subInfo = findStructInfo(refName);
                                        const dds_topic_descriptor_t* subDesc = nullptr;
                                        auto sup = supportByType.find(refName);
                                        if (sup != supportByType.end())
                                            subDesc = sup->second ? sup->second->descriptor.get() : nullptr;
                                        Value subtypeVal = resolveTypeValue(refName);
                                        if (subInfo && !subtypeVal.isNil())
                                            ev = valueFromSample(*subInfo, subDesc, eptr, subtypeVal);
                                        break;
                                    }
                                    default:
                                        break;
                                }
                                lst->append(ev);
                            }
                        }
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
        auto offsets = offsetsFor(info, desc);
        for (size_t idx = 0; idx < offsets.size() && idx < info.fields.size(); ++idx) {
            handleField(offsets[idx], info.fields[idx]);
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

void ModuleDDS::deleteEntityOnce(dds_entity_t ent)
{
    if (ent <= 0)
        return;
    std::lock_guard<std::mutex> lock(gEntityMutex);
    if (gDeletedEntities.find(ent) != gDeletedEntities.end())
        return;
    gDeletedEntities.insert(ent);
    dds_delete(ent);
}

std::vector<size_t> ModuleDDS::offsetsFor(const StructInfo& info, const dds_topic_descriptor_t* desc) const
{
    (void)desc;
    std::vector<size_t> offsets;
    computeLayout(info, offsets);
    return offsets;
}

std::unique_ptr<dds_qos_t, decltype(&dds_delete_qos)> ModuleDDS::qosFromValue(const Value& v) const
{
    auto bad = [](const std::string& msg){ throw std::invalid_argument("DDS QoS: " + msg); };
    if (v.isNil())
        return {nullptr, dds_delete_qos};
    if (!isDict(v))
        bad("expected dict");
    ObjDict* dict = asDict(v);
    auto qos = std::unique_ptr<dds_qos_t, decltype(&dds_delete_qos)>(dds_create_qos(), dds_delete_qos);
    if (!qos)
        bad("failed to allocate qos");

    auto toLower = [](std::string s){
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        return s;
    };
    auto asInt64 = [](const Value& vv) -> int64_t {
        if (!vv.isNumber())
            throw std::invalid_argument("QoS value must be number");
        return vv.asInt();
    };

    for (const auto& kv : dict->items()) {
        if (!isString(kv.first))
            continue;
        std::string key = toLower(toUTF8StdString(asStringObj(kv.first)->s));
        const Value& val = kv.second;

        if (key == "reliability") {
            if (!isString(val))
                bad("reliability must be string");
            std::string mode = toLower(toUTF8StdString(asStringObj(val)->s));
            if (mode == "reliable")
                dds_qset_reliability(qos.get(), DDS_RELIABILITY_RELIABLE, DDS_SECS(1));
            else if (mode == "best_effort" || mode == "besteffort")
                dds_qset_reliability(qos.get(), DDS_RELIABILITY_BEST_EFFORT, DDS_SECS(0));
            else
                bad("unknown reliability '" + mode + "'");
        } else if (key == "durability") {
            if (!isString(val))
                bad("durability must be string");
            std::string mode = toLower(toUTF8StdString(asStringObj(val)->s));
            if (mode == "volatile")
                dds_qset_durability(qos.get(), DDS_DURABILITY_VOLATILE);
            else if (mode == "transient_local" || mode == "transientlocal")
                dds_qset_durability(qos.get(), DDS_DURABILITY_TRANSIENT_LOCAL);
            else
                bad("unknown durability '" + mode + "'");
        } else if (key == "history") {
            dds_history_kind_t kind = DDS_HISTORY_KEEP_LAST;
            int depth = 1;
            if (isDict(val)) {
                ObjDict* h = asDict(val);
                for (const auto& hk : h->items()) {
                    if (!isString(hk.first))
                        continue;
                    std::string hkey = toLower(toUTF8StdString(asStringObj(hk.first)->s));
                    if (hkey == "kind") {
                        if (!isString(hk.second))
                            bad("history.kind must be string");
                        std::string m = toLower(toUTF8StdString(asStringObj(hk.second)->s));
                        if (m == "keep_all" || m == "keepall")
                            kind = DDS_HISTORY_KEEP_ALL;
                        else if (m == "keep_last" || m == "keeplast")
                            kind = DDS_HISTORY_KEEP_LAST;
                        else
                            bad("unknown history.kind '" + m + "'");
                    } else if (hkey == "depth") {
                        depth = static_cast<int>(asInt64(hk.second));
                    }
                }
            } else if (val.isNumber()) {
                depth = static_cast<int>(asInt64(val));
            } else {
                bad("history must be dict or number");
            }
            dds_qset_history(qos.get(), kind, depth);
        } else if (key == "deadline_ms") {
            int64_t ms = asInt64(val);
            dds_qset_deadline(qos.get(), DDS_MSECS(ms));
        } else if (key == "lifespan_ms") {
            int64_t ms = asInt64(val);
            dds_qset_lifespan(qos.get(), DDS_MSECS(ms));
        } else if (key == "latency_budget_ms") {
            int64_t ms = asInt64(val);
            dds_qset_latency_budget(qos.get(), DDS_MSECS(ms));
        } else if (key == "liveliness") {
            if (!isDict(val))
                bad("liveliness must be dict");
            dds_liveliness_kind_t lk = DDS_LIVELINESS_AUTOMATIC;
            int64_t leaseMs = 0;
            ObjDict* lv = asDict(val);
            for (const auto& lkpair : lv->items()) {
                if (!isString(lkpair.first))
                    continue;
                std::string lkey = toLower(toUTF8StdString(asStringObj(lkpair.first)->s));
                if (lkey == "kind") {
                    if (!isString(lkpair.second))
                        bad("liveliness.kind must be string");
                    std::string m = toLower(toUTF8StdString(asStringObj(lkpair.second)->s));
                    if (m == "automatic")
                        lk = DDS_LIVELINESS_AUTOMATIC;
                    else if (m == "manual_by_topic" || m == "manualbytopic")
                        lk = DDS_LIVELINESS_MANUAL_BY_TOPIC;
                    else if (m == "manual_by_participant" || m == "manualbyparticipant")
                        lk = DDS_LIVELINESS_MANUAL_BY_PARTICIPANT;
                    else
                        bad("unknown liveliness.kind '" + m + "'");
                } else if (lkey == "lease_ms" || lkey == "lease_duration_ms") {
                    leaseMs = asInt64(lkpair.second);
                }
            }
            dds_qset_liveliness(qos.get(), lk, DDS_MSECS(leaseMs));
        } else if (key == "ownership") {
            if (!isString(val))
                bad("ownership must be string");
            std::string m = toLower(toUTF8StdString(asStringObj(val)->s));
            if (m == "shared")
                dds_qset_ownership(qos.get(), DDS_OWNERSHIP_SHARED);
            else if (m == "exclusive")
                dds_qset_ownership(qos.get(), DDS_OWNERSHIP_EXCLUSIVE);
            else
                bad("unknown ownership '" + m + "'");
        } else if (key == "partition") {
            if (!isList(val))
                bad("partition must be list of strings");
            std::vector<std::string> parts;
            ObjList* lst = asList(val);
            auto entries = lst->getElements();
            for (const auto& entry : entries) {
                if (!isString(entry))
                    bad("partition entries must be strings");
                parts.push_back(toUTF8StdString(asStringObj(entry)->s));
            }
            std::vector<const char*> names;
            names.reserve(parts.size());
            for (auto& s : parts)
                names.push_back(s.c_str());
            dds_qset_partition(qos.get(), static_cast<int>(names.size()), names.data());
        } else {
            bad("unknown key '" + key + "'");
        }
    }

    return qos;
}

void ModuleDDS::readerThreadLoop()
{
    while (readerThreadRunning.load()) {
        std::vector<SignalBinding> readersCopy;
        {
            std::lock_guard<std::mutex> lock(signalMutex);
            readersCopy = readerSignals;
        }
        for (const auto& binding : readersCopy) {
            if (!binding.signal.isAlive())
                continue;
            dds_entity_t reader = binding.entity;
            if (reader <= 0)
                continue;
            void* samples[1] = { nullptr };
            dds_sample_info_t si;
            std::memset(&si, 0, sizeof(si));
            dds_return_t rc = ::dds_take(reader, samples, &si, 1, 1);
            if (rc < 0) {
                fprintf(stderr, "dds_take error: %s\n", dds_strretcode(-rc));
                continue;
            }
            if (rc == 0)
                continue;
            if (si.valid_data && samples[0]) {
                const StructInfo* info = findStructInfo(binding.typeName);
                Value typeVal = resolveTypeValue(binding.typeName);
                const dds_topic_descriptor_t* desc = binding.descriptor ? binding.descriptor.get() : nullptr;
                if (info && typeVal.isNonNil()) {
                    Value val = valueFromSample(*info, desc, samples[0], typeVal);
                    Value sigStrong = binding.signal.strongRef();
                    if (isSignal(sigStrong)) {
                        ObjSignal* objSig = asSignal(sigStrong);
                        objSig->signal->set(val);
                    }
                }
            }
            ::dds_return_loan(reader, samples, 1);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void ModuleDDS::startReaderThread()
{
    bool expected = false;
    if (readerThreadRunning.compare_exchange_strong(expected, true)) {
        readerThread = std::thread([this](){ readerThreadLoop(); });
    }
}

void ModuleDDS::stopReaderThread()
{
    bool expected = true;
    if (readerThreadRunning.compare_exchange_strong(expected, false)) {
        if (readerThread.joinable())
            readerThread.join();
    }
}

#endif // ROXAL_ENABLE_DDS
