#ifdef ROXAL_ENABLE_DDS

#include "dds/ModuleDDS.h"
#include "dds/DdsAdapter.h"
#include "dds/AsyncDDSManager.h"

#include "Object.h"
#include "Value.h"
#include "VM.h"
#include "SimpleMarkSweepGC.h"
#include "dataflow/Signal.h"

#include <dds/dds.h>
#include <dds/ddsrt/types.h>
#include <dds/ddsrt/heap.h>
#include <dds/ddsc/dds_public_impl.h>
#include <dds/ddsc/dds_public_alloc.h>
#include <dds/ddsc/dds_opcodes.h>
#include <dds/ddsc/dds_public_qos.h>
#include <dds/ddsi/ddsi_typelib.h>

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <cstring>
#include <functional>
#include <optional>
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
    // Rooting: every Value-holding member is a PersistentRoot/TracedMember
    // (GC v2 phase B pilot) -- self-registered with the collector, no
    // hand-written visitRoots() enumeration to forget entries in.
    moduleTypeValue = Value::objVal(newModuleTypeObj(toUnicodeString("dds")));
    ObjModuleType::allModules.push_back(moduleTypeValue);
}

ModuleDDS::~ModuleDDS()
{
    stopReaderThread();
    if (!moduleTypeValue->isNil())
        destroyModuleType(moduleTypeValue);
}

void ModuleDDS::onModuleUnloading(VM& vm)
{
    (void)vm;
    // Release every roxal Value this module retains in plain C++ containers
    // NOW, while the VM's object graph is still alive.  These containers are
    // invisible to the GC mark phase; the shutdown collection sweeps their
    // targets as unreachable, and letting the Values linger into the module
    // destructor (static teardown, after VM::shutdown) decRefs freed objects
    // (ASan/teardown-SEGV confirmed via TopicSupport::handle).  The reader
    // thread is stopped first -- it consults these maps.
    stopReaderThread();
    writerSignals->clear();
    readerSignals->clear();
    supportByType->clear();
    supportByEntity->clear();
    idlModules->clear();
    typesByFullName_->clear();
    participantType = Value();
    topicType = Value();
    writerType = Value();
    readerType = Value();
    defaultParticipant = Value();
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

Value ModuleDDS::importIdl(const std::string& idlFilename,
                           const std::vector<std::string>& annotations,
                           std::vector<std::string>* outGlobals)
{
    if (!functionsLinked) {
        linkNativeFunctions();
        functionsLinked = true;
    }
    if (!std::filesystem::exists(std::filesystem::path(idlFilename)))
        throw std::invalid_argument("DDS import - IDL file '"+idlFilename+"' not found.");

    // Interpret the import annotations this module recognises. @ros applies
    // ROS 2 (rmw_cyclonedds) wire-name mangling; anything else is ignored
    // with a warning (import annotations are generic language surface).
    bool rosProfile = false;
    for (const auto& name : annotations) {
        if (name == "ros")
            rosProfile = true;
        else
            std::cerr << "warning: ignoring unrecognised annotation '@" << name
                      << "' on IDL import '" << idlFilename << "'" << std::endl;
    }

    // The same file cannot be imported under two profiles: compile-time and
    // cache-reload both route through here, and the adapter maps accumulate.
    {
        std::error_code ec;
        auto canon = std::filesystem::canonical(idlFilename, ec);
        const std::string key = ec ? idlFilename : canon.string();
        auto pit = idlProfileByPath_.find(key);
        if (pit != idlProfileByPath_.end()) {
            if (pit->second != rosProfile)
                throw std::invalid_argument("IDL '" + idlFilename
                    + "' already imported with a different @ros profile");
        } else {
            idlProfileByPath_.emplace(key, rosProfile);
        }
    }

    if (!adapter)
        adapter = std::make_unique<DdsAdapter>();

    if (!functionsLinked) {
        linkNativeFunctions();
        functionsLinked = true;
    }

    // Module search paths double as the IDL #include search roots (so e.g.
    // `-p /opt/ros/jazzy/share` lets stock ROS idl includes resolve).
    auto types = adapter->allocateTypes(idlFilename, vm().getModulePaths(), rosProfile);

    std::filesystem::path pp(idlFilename);
    // Bind the import to: (a) the top-level module matching the file stem,
    // else (b) the first module declared in the main file's own text (after
    // include splicing the first module *parsed* may come from an included
    // file), else (c) the first module parsed overall, else the file stem.
    const auto& tops = adapter->topModules();
    const std::string stem = pp.stem().string();
    std::string moduleName;
    if (std::find(tops.begin(), tops.end(), stem) != tops.end()) {
        moduleName = stem;
    } else if (!adapter->mainFirstModule().empty()
               && std::find(tops.begin(), tops.end(), adapter->mainFirstModule()) != tops.end()) {
        moduleName = adapter->mainFirstModule();
    } else {
        moduleName = adapter->packageName();
        if (moduleName.empty())
            moduleName = stem;
    }
    Value moduleVal = getOrCreateModule(moduleName);
    if (outGlobals) {
        outGlobals->push_back(moduleName);
        for (const auto& t : tops)
            if (t != moduleName)
                outGlobals->push_back(t);
    }

    registerGeneratedTypes(moduleVal, types);

    // @ros convenience: also expose each mangled type under its ORIGINAL
    // (stock) name -- scripts may write sensor_msgs.msg.Image() while the
    // wire name stays the mangled sensor_msgs::msg::dds_::Image_ (the type
    // Value itself carries the mangled name via fullNameByType_).
    for (const auto& alias : adapter->rosAliases()) {
        auto it = typesByFullName_->find(alias.second);
        if (it == typesByFullName_->end())
            continue;
        typesByFullName_->emplace(alias.first, it->second);
        storeAtScope(moduleVal, alias.first, it->second);
    }

    // register constants + typedef aliases from IDL, nested at their scope
    if (adapter) {
        for (const auto& c : adapter->constants()) {
            if (c.value.isNil())
                continue;
            storeAtScope(moduleVal, c.fullName, c.value);
        }
        // register typedef aliases as additional module vars pointing to the aliased type
        for (const auto& td : adapter->typedefs()) {
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
                storeAtScope(moduleVal, td.fullName, target);
            }
        }
    }
    return moduleVal;
}

Value ModuleDDS::getOrCreateModule(const std::string& name)
{
    auto it = idlModules->find(name);
    if (it != idlModules->end())
        return it->second;

    Value moduleVal = Value::moduleTypeVal(toUnicodeString(name));
    ObjModuleType::allModules.push_back(moduleVal);
    (*idlModules)[name] = moduleVal;

    // make available as global
    vm().globals.storeGlobal(toUnicodeString(name), moduleVal);
    return moduleVal;
}

Value ModuleDDS::getOrCreateNestedModule(Value topModuleVal, const std::vector<std::string>& intermediateParts)
{
    Value current = topModuleVal;
    for (const auto& part : intermediateParts) {
        ObjModuleType* mod = asModuleType(current);
        icu::UnicodeString uname = toUnicodeString(part);
        auto existing = mod->vars.load(uname);
        if (existing.has_value() && isModuleType(existing.value())) {
            current = existing.value();
            continue;
        }
        Value childVal = Value::moduleTypeVal(uname);
        ObjModuleType::allModules.push_back(childVal);
        mod->vars.store(uname, childVal, true);
        current = childVal;
    }
    return current;
}

void ModuleDDS::storeAtScope(Value topModuleVal, const std::string& fullName, const Value& val)
{
    // Split the full scoped name (a::b::c::Leaf) into parts.
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        size_t sep = fullName.find("::", start);
        if (sep == std::string::npos) { parts.push_back(fullName.substr(start)); break; }
        parts.push_back(fullName.substr(start, sep - start));
        start = sep + 2;
    }
    // Route scoped names by their OWN package (parts[0]) -- each distinct
    // top-level module becomes (or reuses) a global roxal module, so a parse
    // containing several packages (e.g. spliced ROS includes) registers each
    // type under the right one. Unscoped names go to the import's default
    // module.
    Value topVal = parts.size() == 1 ? topModuleVal : getOrCreateModule(parts[0]);
    ObjModuleType* topMod = asModuleType(topVal);
    icu::UnicodeString shortName = toUnicodeString(parts.back());

    // Single-level (Top::Leaf) or unscoped: store directly in the top module.
    if (parts.size() <= 2) {
        topMod->vars.store(shortName, val, true);
        return;
    }

    // Nested: materialise intermediate modules (parts[1..n-2]) and store the leaf inside,
    // so a script can reference the full nested path (e.g. sensor_msgs.msg.dds_.Image_).
    std::vector<std::string> intermediates(parts.begin() + 1, parts.end() - 1);
    Value leaf = getOrCreateNestedModule(topVal, intermediates);
    asModuleType(leaf)->vars.store(shortName, val, true);

    // Best-effort convenience: also expose the short name at the top module when it does
    // not collide, so single-nested references (e.g. sensor_msgs.Image_) keep working.
    if (!topMod->vars.load(shortName).has_value())
        topMod->vars.store(shortName, val, true);
}

void ModuleDDS::registerGeneratedTypes(Value moduleVal, const std::vector<Value>& types)
{
    for (const auto& typeVal : types) {
        if (!isObjectType(typeVal) && !isEnumType(typeVal))
            continue;

        ObjObjectType* type = asObjectType(typeVal);
        std::string full = adapter ? adapter->fullNameForType(typeVal) : "";
        if (full.empty()) {
            // No scope info: fall back to flat registration by short name.
            asModuleType(moduleVal)->vars.store(type->name, typeVal, true);
            continue;
        }
        // Item 1: index by full name so resolveTypeValue resolves deeply-nested (ROS) types.
        (*typesByFullName_)[full] = typeVal;
        // Item 2: expose the type at its real nested-module path.
        storeAtScope(moduleVal, full, typeVal);
    }
}

Value ModuleDDS::resolveTypeValue(const std::string& fullName)
{
    // Item 1: direct full-name lookup handles arbitrarily nested / ROS-style names,
    // for which no module keyed by the parent scope exists.
    auto direct = typesByFullName_->find(fullName);
    if (direct != typesByFullName_->end())
        return direct->second;

    auto pos = fullName.rfind("::");
    std::string moduleName = pos == std::string::npos ? "" : fullName.substr(0, pos);
    std::string shortName = pos == std::string::npos ? fullName : fullName.substr(pos + 2);
    auto modIt = idlModules->find(moduleName.empty() ? fullName : moduleName);
    if (modIt != idlModules->end()) {
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
    if (!self || self->participantType->isNil())
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
    (*self->supportByType)[typeName] = support;
    (*self->supportByEntity)[support->entity] = support;

    Value handleVal = support->handle;
    if (!self || self->topicType->isNil())
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

namespace {
// Backing storage for a runtime-built topic descriptor; the descriptor holds
// raw pointers into these buffers, so the bundle must live as long as the
// dds_topic_descriptor_t (tied together by the shared_ptr deleter in
// buildTopicDescriptor).
struct BuiltDescriptorStorage {
    std::vector<uint32_t> ops;
    std::vector<std::string> keyNames;
    std::vector<dds_key_descriptor_t> keys;
    std::string typeName;
    std::vector<unsigned char> typeInfoBlob;
    std::vector<unsigned char> typeMapBlob;
};
} // namespace

std::shared_ptr<dds_topic_descriptor_t> ModuleDDS::buildTopicDescriptor(const std::string& typeName)
{
    const StructInfo* top = adapter ? adapter->findStruct(typeName) : nullptr;
    if (!top)
        throw std::runtime_error("dds: unknown IDL struct type '" + typeName + "'");
    const std::string topFull = top->fullName;

    auto storage = std::make_shared<BuiltDescriptorStorage>();
    std::vector<uint32_t>& ops = storage->ops;

    // Emission state: one ops block per struct (top-level first, dependencies
    // appended in discovery order, idlc style), with forward jumps patched
    // once every block's start index is known.
    std::unordered_map<std::string, uint32_t> blockStart; // canonical name -> ops word index
    std::vector<std::string> emitQueue;                   // canonical names, FIFO
    struct JumpPatch {
        size_t jumpWord;    // ops index of the [next-insn, elem-insn] word
        size_t insnStart;   // ops index of the ADR op the jump is relative to
        uint16_t nextInsn;  // instruction length for this op form
        std::string target; // canonical struct name jumped to
    };
    std::vector<JumpPatch> patches;
    struct KeyRef { std::string name; uint32_t adrWord; };
    std::vector<KeyRef> keyRefs;

    auto canonicalStruct = [&](const std::string& name) -> std::string {
        const StructInfo* si = adapter->findStruct(name);
        if (!si)
            throw std::runtime_error("dds: unknown struct '" + name + "' referenced by " + topFull);
        return si->fullName;
    };
    auto enqueueStruct = [&](const std::string& name) -> std::string {
        std::string full = canonicalStruct(name);
        if (!blockStart.count(full) &&
            std::find(emitQueue.begin(), emitQueue.end(), full) == emitQueue.end())
            emitQueue.push_back(full);
        return full;
    };
    auto structSize = [&](const std::string& full) -> uint32_t {
        const StructInfo* si = adapter->findStruct(full);
        std::vector<size_t> offs;
        return static_cast<uint32_t>(computeLayout(*si, offs));
    };
    auto enumMax = [&](const std::string& name) -> uint32_t {
        const EnumInfo* e = adapter->findEnum(name);
        if (!e || e->values.empty())
            throw std::runtime_error("dds: unknown enum '" + name + "' referenced by " + topFull);
        int32_t mx = e->values.front().second;
        for (const auto& v : e->values)
            mx = std::max(mx, v.second);
        return static_cast<uint32_t>(mx);
    };
    // Pre-cast: C++20 deprecates bitwise ops between different enum types,
    // and every op word starts with DDS_OP_ADR (left-assoc then keeps the
    // accumulating type uint32_t).
    constexpr uint32_t kAdr = static_cast<uint32_t>(DDS_OP_ADR);
    // Enums are stored as 4 bytes; idlc encodes that in the op's size flag.
    constexpr uint32_t enuSz4 = 2u << DDS_OP_FLAG_SZ_SHIFT;

    // Type bits (plus SGN/FP flags) for primitive leaves. shift 16 yields the
    // primary type code of an ADR op, shift 8 the sequence/array subtype.
    auto primCode = [](FieldType::Kind k, unsigned shift) -> std::optional<uint32_t> {
        switch (k) {
            case FieldType::Kind::Bool:    return DDS_OP_VAL_BLN << shift;
            case FieldType::Kind::Byte:    return DDS_OP_VAL_1BY << shift;
            case FieldType::Kind::Int32:   return (DDS_OP_VAL_4BY << shift) | DDS_OP_FLAG_SGN;
            case FieldType::Kind::Int64:   return (DDS_OP_VAL_8BY << shift) | DDS_OP_FLAG_SGN;
            case FieldType::Kind::UInt64:  return DDS_OP_VAL_8BY << shift;
            case FieldType::Kind::Float64: return (DDS_OP_VAL_8BY << shift) | DDS_OP_FLAG_FP;
            default: return std::nullopt;
        }
    };

    std::function<void(const FieldType&, uint32_t, uint32_t)> emitMember =
        [&](const FieldType& ft, uint32_t offset, uint32_t flags) {
        if (auto pc = primCode(ft.kind, 16)) {
            ops.push_back(kAdr | *pc | flags);
            ops.push_back(offset);
            return;
        }
        switch (ft.kind) {
            case FieldType::Kind::String:
                if (ft.bounded && ft.bound > 0) {
                    ops.push_back(kAdr | DDS_OP_TYPE_BST | flags);
                    ops.push_back(offset);
                    ops.push_back(ft.bound + 1);
                } else {
                    ops.push_back(kAdr | DDS_OP_TYPE_STR | flags);
                    ops.push_back(offset);
                }
                return;
            case FieldType::Kind::EnumRef:
                ops.push_back(kAdr | DDS_OP_TYPE_ENU | enuSz4 | flags);
                ops.push_back(offset);
                ops.push_back(enumMax(ft.refName));
                return;
            case FieldType::Kind::StructRef: {
                std::string full = enqueueStruct(ft.refName);
                size_t insnStart = ops.size();
                ops.push_back(kAdr | DDS_OP_TYPE_EXT | flags);
                ops.push_back(offset);
                patches.push_back({ops.size(), insnStart, 3, full});
                ops.push_back(0); // [next-insn, elem-insn], patched later
                return;
            }
            case FieldType::Kind::List:
                break; // handled below
            default:
                throw std::runtime_error("dds: unsupported field type in " + topFull);
        }

        const FieldType* elem = ft.element.get();
        if (!elem)
            throw std::runtime_error("dds: sequence/array in " + topFull + " has unknown element type");

        if (ft.isArray) {
            uint32_t alen = ft.bound;
            // Flatten array-of-array chains (multi-dim declarators are already
            // flattened by idl_array_size; typedef-of-array nesting lands here).
            while (elem->kind == FieldType::Kind::List && elem->isArray) {
                if (elem->bound == 0 || !elem->element)
                    throw std::runtime_error("dds: bad nested array in " + topFull);
                alen *= elem->bound;
                elem = elem->element.get();
            }
            if (alen == 0)
                throw std::runtime_error("dds: fixed array with no size in " + topFull);

            if (auto pc = primCode(elem->kind, 8)) {
                ops.push_back(kAdr | DDS_OP_TYPE_ARR | *pc | flags);
                ops.push_back(offset);
                ops.push_back(alen);
                return;
            }
            switch (elem->kind) {
                case FieldType::Kind::EnumRef:
                    ops.push_back(kAdr | DDS_OP_TYPE_ARR | DDS_OP_SUBTYPE_ENU | enuSz4 | flags);
                    ops.push_back(offset);
                    ops.push_back(alen);
                    ops.push_back(enumMax(elem->refName));
                    return;
                case FieldType::Kind::String:
                    if (elem->bounded && elem->bound > 0) {
                        ops.push_back(kAdr | DDS_OP_TYPE_ARR | DDS_OP_SUBTYPE_BST | flags);
                        ops.push_back(offset);
                        ops.push_back(alen);
                        ops.push_back(0);
                        ops.push_back(elem->bound + 1);
                    } else {
                        ops.push_back(kAdr | DDS_OP_TYPE_ARR | DDS_OP_SUBTYPE_STR | flags);
                        ops.push_back(offset);
                        ops.push_back(alen);
                    }
                    return;
                case FieldType::Kind::StructRef: {
                    std::string full = enqueueStruct(elem->refName);
                    size_t insnStart = ops.size();
                    ops.push_back(kAdr | DDS_OP_TYPE_ARR | DDS_OP_SUBTYPE_STU | flags);
                    ops.push_back(offset);
                    ops.push_back(alen);
                    patches.push_back({ops.size(), insnStart, 5, full});
                    ops.push_back(0); // [next-insn, elem-insn]
                    ops.push_back(structSize(full));
                    return;
                }
                case FieldType::Kind::List: { // array of sequences: inline element block
                    size_t insnStart = ops.size();
                    ops.push_back(kAdr | DDS_OP_TYPE_ARR |
                                  ((elem->bounded && elem->bound > 0 && !elem->isArray)
                                       ? DDS_OP_SUBTYPE_BSQ : DDS_OP_SUBTYPE_SEQ) | flags);
                    ops.push_back(offset);
                    ops.push_back(alen);
                    size_t jumpWord = ops.size();
                    ops.push_back(0);
                    ops.push_back(static_cast<uint32_t>(typeSizeInternal(*elem, this)));
                    size_t blockAt = ops.size();
                    emitMember(*elem, 0, 0);
                    ops.push_back(DDS_OP_RTS);
                    ops[jumpWord] = (static_cast<uint32_t>(ops.size() - insnStart) << 16) |
                                    static_cast<uint32_t>(blockAt - insnStart);
                    return;
                }
                default:
                    throw std::runtime_error("dds: unsupported array element type in " + topFull);
            }
        }

        // (bounded) sequence
        const bool bsq = ft.bounded && ft.bound > 0;
        const uint32_t seqType = bsq ? DDS_OP_TYPE_BSQ : DDS_OP_TYPE_SEQ;
        auto pushSeqHead = [&](uint32_t subtypeBits) -> size_t {
            size_t insnStart = ops.size();
            ops.push_back(kAdr | seqType | subtypeBits | flags);
            ops.push_back(offset);
            if (bsq)
                ops.push_back(ft.bound);
            return insnStart;
        };
        if (auto pc = primCode(elem->kind, 8)) {
            pushSeqHead(*pc);
            return;
        }
        switch (elem->kind) {
            case FieldType::Kind::EnumRef:
                pushSeqHead(DDS_OP_SUBTYPE_ENU | enuSz4);
                ops.push_back(enumMax(elem->refName));
                return;
            case FieldType::Kind::String:
                if (elem->bounded && elem->bound > 0) {
                    pushSeqHead(DDS_OP_SUBTYPE_BST);
                    ops.push_back(elem->bound + 1);
                } else {
                    pushSeqHead(DDS_OP_SUBTYPE_STR);
                }
                return;
            case FieldType::Kind::StructRef: {
                std::string full = enqueueStruct(elem->refName);
                size_t insnStart = pushSeqHead(DDS_OP_SUBTYPE_STU);
                ops.push_back(structSize(full)); // element size in memory
                patches.push_back({ops.size(), insnStart, static_cast<uint16_t>(bsq ? 5 : 4), full});
                ops.push_back(0); // [next-insn, elem-insn]
                return;
            }
            case FieldType::Kind::List: { // sequence of collections: inline element block
                uint32_t sub = elem->isArray ? DDS_OP_SUBTYPE_ARR
                             : (elem->bounded && elem->bound > 0) ? DDS_OP_SUBTYPE_BSQ
                                                                  : DDS_OP_SUBTYPE_SEQ;
                size_t insnStart = pushSeqHead(sub);
                ops.push_back(static_cast<uint32_t>(typeSizeInternal(*elem, this)));
                size_t jumpWord = ops.size();
                ops.push_back(0);
                size_t blockAt = ops.size();
                emitMember(*elem, 0, 0);
                ops.push_back(DDS_OP_RTS);
                ops[jumpWord] = (static_cast<uint32_t>(ops.size() - insnStart) << 16) |
                                static_cast<uint32_t>(blockAt - insnStart);
                return;
            }
            default:
                throw std::runtime_error("dds: unsupported sequence element type in " + topFull);
        }
    };

    // @key only on top-level members (nested key chains are not supported;
    // they were not supported by the previous dynamic-type path either).
    auto fieldFlags = [&](const FieldInfo& f, bool isTop) -> uint32_t {
        if (f.isKey && isTop) {
            keyRefs.push_back({f.name, static_cast<uint32_t>(ops.size())});
            return DDS_OP_FLAG_KEY;
        }
        return 0;
    };

    auto emitStructBlock = [&](const std::string& full) {
        const StructInfo* si = adapter->findStruct(full);
        blockStart[full] = static_cast<uint32_t>(ops.size());
        const bool isTop = (full == topFull);
        std::vector<size_t> offsets;
        computeLayout(*si, offsets);
        if (si->extensibility == IDL_MUTABLE) {
            // Parameter-list CDR: PLC + [PLM, elem-insn][member-id] list, then
            // one RTS-terminated ADR block per member. Member ids are
            // sequential, matching the AUTOID_SEQUENTIAL the dynamic-type
            // path used.
            ops.push_back(DDS_OP_PLC);
            size_t plmStart = ops.size();
            for (size_t i = 0; i < si->fields.size(); ++i) {
                ops.push_back(DDS_OP_PLM); // elem-insn patched below
                ops.push_back(static_cast<uint32_t>(i));
            }
            ops.push_back(DDS_OP_RTS);
            for (size_t i = 0; i < si->fields.size(); ++i) {
                size_t plmWord = plmStart + 2 * i;
                ops[plmWord] |= static_cast<uint32_t>((ops.size() - plmWord) & 0xffffu);
                emitMember(si->fields[i].type, static_cast<uint32_t>(offsets[i]),
                           fieldFlags(si->fields[i], isTop));
                ops.push_back(DDS_OP_RTS);
            }
        } else {
            if (si->extensibility == IDL_APPENDABLE)
                ops.push_back(DDS_OP_DLC); // XCDR2 delimited CDR (DHEADER)
            for (size_t i = 0; i < si->fields.size(); ++i)
                emitMember(si->fields[i].type, static_cast<uint32_t>(offsets[i]),
                           fieldFlags(si->fields[i], isTop));
            ops.push_back(DDS_OP_RTS);
        }
    };

    enqueueStruct(topFull);
    for (size_t qi = 0; qi < emitQueue.size(); ++qi)
        emitStructBlock(emitQueue[qi]);

    for (const auto& p : patches) {
        auto it = blockStart.find(p.target);
        if (it == blockStart.end())
            throw std::runtime_error("dds: internal: no ops block emitted for " + p.target);
        int32_t rel = static_cast<int32_t>(it->second) - static_cast<int32_t>(p.insnStart);
        if (rel < INT16_MIN || rel > INT16_MAX)
            throw std::runtime_error("dds: ops jump out of range for " + p.target);
        ops[p.jumpWord] = (static_cast<uint32_t>(p.nextInsn) << 16) |
                          (static_cast<uint32_t>(rel) & 0xffffu);
    }

    storage->keyNames.reserve(keyRefs.size());
    storage->keys.reserve(keyRefs.size());
    for (size_t i = 0; i < keyRefs.size(); ++i) {
        uint32_t kofAt = static_cast<uint32_t>(ops.size());
        ops.push_back(DDS_OP_KOF | 1u);
        ops.push_back(keyRefs[i].adrWord);
        storage->keyNames.push_back(keyRefs[i].name);
        storage->keys.push_back({storage->keyNames.back().c_str(), kofAt,
                                 static_cast<uint32_t>(i)});
    }

    // XTypes metadata (serialized typeinfo/typemap) is generated from the
    // parsed IDL, so it must describe the wire encoding the ops actually
    // produce. Skip it when the marshalling diverges from the IDL: widened
    // primitives (float32/int16/... stored and sent at Roxal's wider width)
    // and @optional members (marshalled as plain required fields). Without
    // the blobs the type participates by name, like a NO_TYPE_INFO build.
    bool metaEligible = true;
    {
        std::unordered_set<std::string> seen { topFull };
        std::function<bool(const FieldType&)> ftClean = [&](const FieldType& ft) -> bool {
            if (ft.widened)
                return false;
            if (ft.element && !ftClean(*ft.element))
                return false;
            if (ft.kind == FieldType::Kind::StructRef) {
                const StructInfo* si = adapter->findStruct(ft.refName);
                if (!si)
                    return false;
                if (!seen.insert(si->fullName).second)
                    return true;
                for (const auto& f : si->fields)
                    if (f.isOptional || !ftClean(f.type))
                        return false;
            }
            return true;
        };
        for (const auto& f : top->fields) {
            if (f.isOptional || !ftClean(f.type)) {
                metaEligible = false;
                break;
            }
        }
    }
    if (metaEligible) {
        std::vector<unsigned char> ti, tm;
        if (adapter->typeMetaFor(topFull, ti, tm) && !ti.empty() && !tm.empty()) {
            storage->typeInfoBlob = std::move(ti);
            storage->typeMapBlob = std::move(tm);
        }
    }

    std::vector<size_t> topOffsets;
    const uint32_t sampleSize = static_cast<uint32_t>(computeLayout(*top, topOffsets));
    uint32_t align = 1;
    for (const auto& f : top->fields)
        align = std::max(align, static_cast<uint32_t>(fieldAlignInternal(f.type, this)));

    storage->typeName = topFull;
    const bool haveMeta = !storage->typeInfoBlob.empty();
    auto* desc = new dds_topic_descriptor_t{
        .m_size = sampleSize,
        .m_align = align,
        .m_flagset = haveMeta ? DDS_TOPIC_XTYPES_METADATA : 0u,
        .m_nkeys = static_cast<uint32_t>(storage->keys.size()),
        .m_typename = storage->typeName.c_str(),
        .m_keys = storage->keys.empty() ? nullptr : storage->keys.data(),
        .m_nops = static_cast<uint32_t>(ops.size()),
        .m_ops = storage->ops.data(),
        .m_meta = "",
        .type_information = { haveMeta ? storage->typeInfoBlob.data() : nullptr,
                              haveMeta ? static_cast<uint32_t>(storage->typeInfoBlob.size()) : 0u },
        .type_mapping = { haveMeta ? storage->typeMapBlob.data() : nullptr,
                          haveMeta ? static_cast<uint32_t>(storage->typeMapBlob.size()) : 0u },
        .restrict_data_representation = 0,
    };
    // The deleter captures the storage bundle, keeping ops/keys/blobs alive
    // for as long as any TopicSupport/SignalBinding references the descriptor.
    return std::shared_ptr<dds_topic_descriptor_t>(desc,
        [storage](dds_topic_descriptor_t* p) { delete p; });
}

std::shared_ptr<ModuleDDS::TopicSupport> ModuleDDS::buildDynamicTopic(Value participantVal, const std::string& topicName, const std::string& typeName, dds_qos_t* qos)
{
    dds_entity_t participant = entityFromValue(participantVal, true);
    if (participant <= 0)
        return nullptr;

    auto self = VM::instance().ddsModule;
    if (!self || !self->adapter)
        return nullptr;

    auto existing = self->supportByType->find(typeName);
    if (existing != self->supportByType->end() && existing->second && existing->second->descriptor) {
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

    // Build a complete static-style descriptor (m_ops + XTypes metadata, see
    // buildTopicDescriptor) and create the topic through the ordinary
    // static-type path -- the same code path idlc-generated applications use,
    // which registers and dedups type metadata against the process type
    // library inside CycloneDDS. The previous implementation constructed
    // types through the dds_dynamic_type_* API, whose typelib dedup frees a
    // just-constructed type out from under any second handle to it whenever a
    // matching entry already exists -- i.e. whenever the process typelib is
    // populated, e.g. by a host application's statically registered types --
    // crashing the process (use-after-free in CycloneDDS's
    // dynamic_type_complete_locked dedup; reported upstream).
    auto descriptor = self->buildTopicDescriptor(typeName);
    dds_entity_t topic = ::dds_create_topic(participant, descriptor.get(), topicName.c_str(), qos, nullptr);
    if (topic <= 0)
        throw std::runtime_error("dds_create_topic failed for " + typeName + ": " + std::string(dds_strretcode(-topic)));

    auto support = std::make_shared<TopicSupport>();
    support->descriptor = descriptor;
    support->typeName = typeName;
    support->entity = topic;
    support->handle = self->makeHandleValue(topic);
    return support;
}

Value ModuleDDS::dds_create_writer(VM&, ArgsView args)
{
    if (args.size() < 2)
        throw std::invalid_argument("dds.create_writer(participant, topic, qos=None) expects at least 2 args");
    ModuleDDS* self = VM::instance().ddsModule;
    if (!self || self->writerType->isNil())
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
            (*self->supportByEntity)[writer] = topicSupport;
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
    if (!self || self->readerType->isNil())
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
            (*self->supportByEntity)[reader] = topicSupport;
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
        auto it = supportByEntity->find(ent);
        if (it != supportByEntity->end())
            return it->second;
    }
    std::string tn = typeNameFromValue(v);
    auto it2 = supportByType->find(tn);
    if (it2 != supportByType->end())
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
    writerSignals->push_back({sigVal.weakRef(), writer, typeName, desc});
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
        if (self->defaultParticipant->isNonNil())
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
        if (self->defaultParticipant->isNonNil())
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

    // The reader's effective history QoS decides the delivery policy of the
    // reader-signal thread (see drainReaderBinding). Query it once here.
    dds_history_kind_t historyKind = DDS_HISTORY_KEEP_LAST;
    if (dds_qos_t* q = dds_create_qos()) {
        if (dds_get_qos(reader, q) == DDS_RETCODE_OK) {
            dds_history_kind_t kind;
            int32_t depth = 0;
            if (dds_qget_history(q, &kind, &depth))
                historyKind = kind;
        }
        dds_delete_qos(q);
    }

    {
        std::lock_guard<std::mutex> lock(signalMutex);
        readerSignals->push_back({sigVal.weakRef(), reader, typeName, desc, historyKind});
    }
    readerBindingsChanged.store(true);
    wakeReaderThread();
}

void ModuleDDS::unregisterSignal(const Value& sigVal)
{
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
    readerBindingsChanged.store(true);
    wakeReaderThread();
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
        case FieldType::Kind::Byte: return sizeof(uint8_t);
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
                auto sup = mod->supportByType->find(ft.refName);
                if (sup != mod->supportByType->end() && sup->second->descriptor)
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
        case FieldType::Kind::Byte: return alignof(uint8_t);
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
                auto sup = mod->supportByType->find(ft.refName);
                if (sup != mod->supportByType->end() && sup->second && sup->second->descriptor)
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
            case FieldType::Kind::Byte: {
                uint8_t v = fval.isNumber() ? static_cast<uint8_t>(fval.asInt()) : 0;
                *reinterpret_cast<uint8_t*>(target) = v;
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
                auto supIt = supportByType->find(refName);
                if (supIt != supportByType->end())
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
                            case FieldType::Kind::Byte:
                                *reinterpret_cast<uint8_t*>(elemPtr) = ev.isNumber() ? static_cast<uint8_t>(ev.asInt()) : 0;
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
                                auto sup = supportByType->find(refName);
                                if (sup != supportByType->end())
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
                    // Fast path: a packed byte list bulk-copies straight into the sequence buffer.
                    const std::vector<uint8_t>* packedSrc =
                        field.type.element->kind == FieldType::Kind::Byte ? lst->packedBytes() : nullptr;
                    if (packedSrc) {
                        std::memcpy(seq->_buffer, packedSrc->data(), len);
                    } else
                    for (size_t idx = 0; idx < len; ++idx) {
                        Value ev = lst->getElement(idx);
                        char* elemPtr = reinterpret_cast<char*>(seq->_buffer + elemSz * idx);
                        switch (field.type.element->kind) {
                            case FieldType::Kind::Bool:
                                *reinterpret_cast<bool*>(elemPtr) = ev.isBool() ? ev.asBool() : ev.isNumber() ? ev.asInt() != 0 : false;
                                break;
                            case FieldType::Kind::Byte:
                                *reinterpret_cast<uint8_t*>(elemPtr) = ev.isNumber() ? static_cast<uint8_t>(ev.asInt()) : 0;
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
                                auto sup = supportByType->find(refName);
                                if (sup != supportByType->end())
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
            case FieldType::Kind::Byte:
                val = Value::intVal(*reinterpret_cast<const uint8_t*>(src));
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
                auto sup = supportByType->find(refName);
                if (sup != supportByType->end())
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
                                case FieldType::Kind::Byte:
                                    ev = Value::intVal(*reinterpret_cast<const uint8_t*>(eptr));
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
                                    auto sup = supportByType->find(refName);
                                    if (sup != supportByType->end())
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
                            if (field.type.element->kind == FieldType::Kind::Byte) {
                                // Byte sequences (image/blob data) -> packed byte list: one bulk copy into
                                // the list's compact std::vector<uint8_t> storage. Transparent to scripts
                                // (reads still yield byte values), 1 byte/elem instead of an 8-byte boxed
                                // Value, no per-element GC churn, and zero-copy-transferable into a tensor
                                // via `tensor(..., bytes=move(data))` for the vision pipeline.
                                const uint8_t* bytes = reinterpret_cast<const uint8_t*>(seq->_buffer);
                                std::vector<uint8_t> buf(bytes, bytes + seq->_length);
                                lst->adoptPackedBytes(std::move(buf));
                            } else
                            for (uint32_t idx = 0; idx < seq->_length && elemSz > 0; ++idx) {
                                const char* eptr = reinterpret_cast<const char*>(seq->_buffer + elemSz * idx);
                                Value ev = Value::nilVal();
                                switch (field.type.element->kind) {
                                    case FieldType::Kind::Bool:
                                        ev = Value::boolVal(*reinterpret_cast<const bool*>(eptr));
                                        break;
                                    case FieldType::Kind::Byte:
                                        ev = Value::intVal(*reinterpret_cast<const uint8_t*>(eptr));
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
                                        auto sup = supportByType->find(refName);
                                        if (sup != supportByType->end())
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

// Take everything currently in the reader's cache and hand it to the signal
// according to the reader's history QoS:
//  - keep_last: the QoS contract says only the newest sample(s) matter, so the
//    cache is drained and only the newest valid sample is converted and set.
//    (For keyed topics with multiple live instances this delivers the last
//    sample in take order; per-instance freshness is not distinguished.)
//  - keep_all: the user asked for lossless delivery, so every valid sample is
//    converted and set in order. Only one batch is taken per call: the
//    level-triggered readcondition immediately re-wakes the waitset while a
//    backlog remains, which keeps per-wake work bounded and fair across
//    readers.
void ModuleDDS::drainReaderBinding(const SignalBinding& binding)
{
    if (!binding.signal.isAlive() || binding.entity <= 0)
        return;

    constexpr int kBatch = 16;
    void* samples[kBatch] = { nullptr };
    dds_sample_info_t infos[kBatch];

    const StructInfo* info = findStructInfo(binding.typeName);
    Value typeVal = resolveTypeValue(binding.typeName);
    const dds_topic_descriptor_t* desc = binding.descriptor ? binding.descriptor.get() : nullptr;
    const bool keepAll = (binding.historyKind == DDS_HISTORY_KEEP_ALL);
    const bool convertible = info && typeVal.isNonNil();

    auto deliver = [&](const Value& val) {
        Value sigStrong = binding.signal.strongRef();
        if (isSignal(sigStrong)) {
            ObjSignal* objSig = asSignal(sigStrong);
            objSig->signal->set(val);
        }
    };

    Value newest;
    bool haveNewest = false;
    for (;;) {
        dds_return_t got = ::dds_take(binding.entity, samples, infos, kBatch, kBatch);
        if (got < 0) {
            fprintf(stderr, "dds_take error: %s\n", dds_strretcode(-got));
            break;
        }
        if (got == 0)
            break;
        if (convertible) {
            if (keepAll) {
                for (int i = 0; i < got; ++i) {
                    if (infos[i].valid_data && samples[i])
                        deliver(valueFromSample(*info, desc, samples[i], typeVal));
                }
            } else {
                // Only the newest valid sample of the batch needs converting.
                for (int i = got - 1; i >= 0; --i) {
                    if (infos[i].valid_data && samples[i]) {
                        newest = valueFromSample(*info, desc, samples[i], typeVal);
                        haveNewest = true;
                        break;
                    }
                }
            }
        }
        ::dds_return_loan(binding.entity, samples, got);
        if (keepAll || got < kBatch)
            break;
    }
    if (!keepAll && haveNewest)
        deliver(newest);
}

void ModuleDDS::readerThreadLoop()
{
    // reader entity -> its readcondition attached to the waitset
    std::unordered_map<dds_entity_t, dds_entity_t> readConds;
    std::vector<SignalBinding> snapshot;
    std::unordered_map<dds_entity_t, const SignalBinding*> byEntity;

    auto resync = [&]() {
        {
            std::lock_guard<std::mutex> lock(signalMutex);
            snapshot = readerSignals;
        }
        byEntity.clear();
        for (const auto& b : snapshot) {
            if (b.entity <= 0)
                continue;
            byEntity[b.entity] = &b;
            if (readConds.count(b.entity))
                continue;
            dds_entity_t cond = dds_create_readcondition(b.entity, DDS_ANY_STATE);
            if (cond <= 0) {
                fprintf(stderr, "dds reader signal: dds_create_readcondition: %s\n",
                        dds_strretcode(-cond));
                continue;
            }
            dds_return_t rc = dds_waitset_attach(readerWaitset, cond,
                                                 static_cast<dds_attach_t>(b.entity));
            if (rc < 0) {
                fprintf(stderr, "dds reader signal: dds_waitset_attach: %s\n",
                        dds_strretcode(-rc));
                ::dds_delete(cond);
                continue;
            }
            readConds[b.entity] = cond;
        }
        for (auto it = readConds.begin(); it != readConds.end();) {
            if (byEntity.count(it->first)) {
                ++it;
            } else {
                // Detach/delete may legitimately fail if the reader (and its
                // child readcondition) was already deleted -- ignore.
                dds_waitset_detach(readerWaitset, it->second);
                ::dds_delete(it->second);
                it = readConds.erase(it);
            }
        }
    };

    constexpr int kMaxTriggers = 64;
    dds_attach_t triggered[kMaxTriggers];

    while (readerThreadRunning.load()) {
        if (readerBindingsChanged.exchange(false))
            resync();

        // Finite timeout as a safety net; wake-ups normally come from the
        // readconditions or the guard condition.
        dds_return_t n = dds_waitset_wait(readerWaitset, triggered, kMaxTriggers, DDS_SECS(1));
        if (!readerThreadRunning.load())
            break;
        if (n < 0) {
            fprintf(stderr, "dds reader signal: dds_waitset_wait: %s\n", dds_strretcode(-n));
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (n == 0)
            continue;

        // Reset the guard if it fired (membership/shutdown handled above).
        bool guardWasSet = false;
        dds_take_guardcondition(readerGuard, &guardWasSet);

        {
            // GC coverage for the delivery phase.  drainReaderBinding creates
            // GC Values (sample conversions, the ObjSignal wrapper enqueued by
            // processEventDrivenSignalUpdate's foreign path) on this otherwise
            // unregistered thread -- invisible to the collector, they can be
            // swept while still referenced here (use-after-free), and stores
            // during the mark phase can hide live objects.  A SCOPED
            // participant (not loop-persistent: nothing wakes the 1s
            // dds_waitset_wait on a GC request, a persistent one would stall
            // every collection) makes the collector wait for us / park us:
            // while unparked, no mark/sweep can start; poll ONLY between
            // bindings -- never inside drainReaderBinding, whose locals are
            // live Values.  This thread is not RT: parking briefly is fine.
            roxal::SimpleMarkSweepGC::ExternalParticipant participant(
                roxal::SimpleMarkSweepGC::instance());
            participant.pollSafepointIfRequested();  // park up-front if a barrier is forming

            int count = std::min<int>(static_cast<int>(n), kMaxTriggers);
            for (int i = 0; i < count; ++i) {
                auto entity = static_cast<dds_entity_t>(triggered[i]);
                if (entity <= 0)
                    continue; // the guard condition (attached with cookie 0)
                auto it = byEntity.find(entity);
                if (it != byEntity.end())
                    drainReaderBinding(*it->second);
                participant.pollSafepointIfRequested();  // between bindings: no Value locals live
            }
        }
    }

    for (auto& kv : readConds) {
        dds_waitset_detach(readerWaitset, kv.second);
        ::dds_delete(kv.second);
    }
}

void ModuleDDS::wakeReaderThread()
{
    if (readerGuard > 0)
        dds_set_guardcondition(readerGuard, true);
}

void ModuleDDS::startReaderThread()
{
    bool expected = false;
    if (readerThreadRunning.compare_exchange_strong(expected, true)) {
        readerWaitset = dds_create_waitset(DDS_CYCLONEDDS_HANDLE);
        readerGuard = dds_create_guardcondition(DDS_CYCLONEDDS_HANDLE);
        if (readerWaitset <= 0 || readerGuard <= 0 ||
            dds_waitset_attach(readerWaitset, readerGuard, 0) < 0) {
            fprintf(stderr, "dds reader signal: failed to create waitset/guard\n");
            readerThreadRunning.store(false);
            return;
        }
        readerBindingsChanged.store(true);
        readerThread = std::thread([this](){ readerThreadLoop(); });
    }
}

void ModuleDDS::stopReaderThread()
{
    bool expected = true;
    if (readerThreadRunning.compare_exchange_strong(expected, false)) {
        wakeReaderThread();
        if (readerThread.joinable())
            readerThread.join();
        if (readerWaitset > 0)
            ::dds_delete(readerWaitset);
        if (readerGuard > 0)
            ::dds_delete(readerGuard);
        readerWaitset = 0;
        readerGuard = 0;
    }
}

#endif // ROXAL_ENABLE_DDS
