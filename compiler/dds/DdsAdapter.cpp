#ifdef ROXAL_ENABLE_DDS

#include "dds/DdsAdapter.h"
#include "Object.h"

extern "C" {
#include <idl/processor.h>
#include <idl/visit.h>
#include <idl/descriptor_type_meta.h>
}

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

using namespace roxal;

namespace {

struct ParseResult {
    std::string package;
    std::vector<StructInfo> structs;
    std::vector<EnumInfo> enums;
    const idl_node_t* root{nullptr};
    idl_pstate_t* pstate{nullptr};
};

std::string joinScope(const std::vector<std::string>& scope, const std::string& name)
{
    std::string full;
    for (size_t i = 0; i < scope.size(); ++i) {
        if (i > 0) full += "::";
        full += scope[i];
    }
    if (!full.empty())
        full += "::";
    full += name;
    return full;
}

FieldType::Kind mapBaseType(idl_type_t t)
{
    switch (t) {
        case IDL_BOOL: return FieldType::Kind::Bool;
        case IDL_FLOAT:
        case IDL_DOUBLE:
        case IDL_LDOUBLE: return FieldType::Kind::Float64;
        case IDL_SHORT:
        case IDL_USHORT:
        case IDL_LONG:
        case IDL_ULONG:
        case IDL_INT8:
        case IDL_UINT8:
        case IDL_INT16:
        case IDL_UINT16:
        case IDL_INT32:
        case IDL_UINT32:
        case IDL_CHAR:
        case IDL_WCHAR:
        case IDL_OCTET:
            return FieldType::Kind::Int32;
        case IDL_LLONG:
        case IDL_ULLONG:
        case IDL_INT64:
        case IDL_UINT64:
            return (t == IDL_ULLONG || t == IDL_UINT64) ? FieldType::Kind::UInt64 : FieldType::Kind::Int64;
        default:
            return FieldType::Kind::Unsupported;
    }
}

FieldType classifyType(const void* typeSpec)
{
    FieldType ft;
    const void* stripped = idl_strip(typeSpec, IDL_STRIP_ALIASES | IDL_STRIP_FORWARD);
    idl_type_t t = idl_type(stripped);

    if (idl_is_sequence(stripped)) {
        ft.kind = FieldType::Kind::List;
        const idl_sequence_t* seq = static_cast<const idl_sequence_t*>(stripped);
        ft.element = std::make_shared<FieldType>(classifyType(seq->type_spec));
        if (seq->maximum > 0) {
            ft.bounded = true;
            ft.bound = seq->maximum;
        }
    } else if (idl_is_array(stripped)) {
        ft.kind = FieldType::Kind::List;
        // treat arrays similarly to sequences
    } else if (idl_is_string(stripped) || idl_is_wstring(stripped))
        ft.kind = FieldType::Kind::String;
    else if (idl_is_enum(stripped)) {
        ft.kind = FieldType::Kind::EnumRef;
        const char* name = idl_identifier(stripped);
        if (name)
            ft.refName = name;
    } else if (idl_is_struct(stripped)) {
        ft.kind = FieldType::Kind::StructRef;
        const char* name = idl_identifier(stripped);
        if (name)
            ft.refName = name;
    } else {
        ft.kind = mapBaseType(t);
    }
    return ft;
}

Value makeTypeSpec(FieldType::Kind kind) {
    switch (kind) {
        case FieldType::Kind::Int32: return Value::typeSpecVal(ValueType::Int);
        case FieldType::Kind::Bool: return Value::typeSpecVal(ValueType::Bool);
        case FieldType::Kind::Float64: return Value::typeSpecVal(ValueType::Real);
        case FieldType::Kind::String: return Value::typeSpecVal(ValueType::String);
        case FieldType::Kind::List: return Value::typeSpecVal(ValueType::List);
        case FieldType::Kind::Int64:
        case FieldType::Kind::UInt64: return Value::typeSpecVal(ValueType::Int);
        default: return Value::nilVal();
    }
}

Value makeDefault(FieldType::Kind kind) {
    switch (kind) {
        case FieldType::Kind::Int32: return defaultValue(ValueType::Int);
        case FieldType::Kind::Bool: return defaultValue(ValueType::Bool);
        case FieldType::Kind::Float64: return defaultValue(ValueType::Real);
        case FieldType::Kind::String: return defaultValue(ValueType::String);
        case FieldType::Kind::List: return Value::listVal();
        case FieldType::Kind::Int64:
        case FieldType::Kind::UInt64:
            return Value::intVal(0);
        default: return Value::nilVal();
    }
}

struct VisitorState {
    std::vector<std::string> scope;
    ParseResult result;
};

idl_retcode_t onModule(const idl_pstate_t*, bool revisit, const idl_path_t*, const void* node, void* user)
{
    auto* st = static_cast<VisitorState*>(user);
    const idl_module_t* mod = static_cast<const idl_module_t*>(node);
    if (!revisit) {
        const char* name = idl_identifier(mod);
        if (name)
            st->scope.push_back(name);
        if (st->result.package.empty() && name)
            st->result.package = name;
    } else {
        if (!st->scope.empty())
            st->scope.pop_back();
    }
    return IDL_RETCODE_OK;
}

idl_retcode_t onEnum(const idl_pstate_t*, bool revisit, const idl_path_t*, const void* node, void* user)
{
    if (revisit) return IDL_RETCODE_OK;
    auto* st = static_cast<VisitorState*>(user);
    const idl_enum_t* en = static_cast<const idl_enum_t*>(node);
    EnumInfo info;
    const char* name = idl_identifier(en);
    if (!name) return IDL_RETCODE_OK;
    info.fullName = joinScope(st->scope, name);
    info.node = reinterpret_cast<const idl_node_t*>(en);
    int32_t current = 0;
    bool first = true;
    const idl_enumerator_t* enumerator = en->enumerators;
    while (enumerator) {
        const char* label = idl_identifier(enumerator);
        if (!label) continue;
        if (!first)
            current += 1;
        first = false;
        info.values.emplace_back(label, current);
        enumerator = static_cast<const idl_enumerator_t*>(idl_next(enumerator));
    }
    st->result.enums.push_back(std::move(info));
    return IDL_RETCODE_OK;
}

idl_retcode_t onStruct(const idl_pstate_t*, bool revisit, const idl_path_t*, const void* node, void* user)
{
    if (revisit) return IDL_RETCODE_OK;
    auto* st = static_cast<VisitorState*>(user);
    const idl_struct_t* stNode = static_cast<const idl_struct_t*>(node);
    const char* name = idl_identifier(stNode);
    if (!name) return IDL_RETCODE_OK;
    StructInfo s;
    s.fullName = joinScope(st->scope, name);
    s.node = reinterpret_cast<const idl_node_t*>(stNode);

    const idl_member_t* member = stNode->members;
    while (member) {
        const idl_declarator_t* decl = member->declarators;
        while (decl) {
            const char* fname = idl_identifier(decl);
            if (!fname) {
                decl = static_cast<const idl_declarator_t*>(idl_next(decl));
                continue;
            }
            FieldInfo fi;
            fi.name = fname;
            FieldType ft = classifyType(member->type_spec);
            if (idl_is_array(decl) && ft.kind != FieldType::Kind::List)
                ft.kind = FieldType::Kind::List;
            fi.type = ft;
            const idl_member_t* mnode = static_cast<const idl_member_t*>(member);
            fi.isKey = mnode && mnode->key.value;
            fi.isOptional = idl_is_optional(reinterpret_cast<const idl_node_t*>(member));
            s.fields.push_back(std::move(fi));
            decl = static_cast<const idl_declarator_t*>(idl_next(decl));
        }
        member = static_cast<const idl_member_t*>(idl_next(member));
    }

    st->result.structs.push_back(std::move(s));
    return IDL_RETCODE_OK;
}

ParseResult parseWithIdl(const std::string& content)
{
    idl_pstate_t* ps = nullptr;
    if (idl_create_pstate(IDL_FLAG_ANNOTATIONS | IDL_FLAG_EXTENDED_DATA_TYPES | IDL_FLAG_ANONYMOUS_TYPES, nullptr, &ps) != IDL_RETCODE_OK)
        throw std::runtime_error("IDL parser initialization failed");
    auto guard = std::unique_ptr<idl_pstate_t, decltype(&idl_delete_pstate)>(ps, &idl_delete_pstate);

    idl_retcode_t rc = idl_parse_string(ps, content.c_str());
    if (rc != IDL_RETCODE_OK) {
        throw std::runtime_error("IDL parse failed");
    }

    VisitorState state;
    idl_visitor_t vis{};
    vis.revisit = IDL_VISIT_REVISIT;
    for (int i = 0; i <= IDL_ACCEPT; ++i)
        vis.accept[i] = nullptr;
    vis.accept[IDL_ACCEPT_MODULE] = onModule;
    vis.accept[IDL_ACCEPT_ENUM] = onEnum;
    vis.accept[IDL_ACCEPT_STRUCT] = onStruct;
    vis.visit = IDL_DECLARATION | IDL_MODULE | IDL_ENUM | IDL_STRUCT;

    rc = idl_visit(ps, ps->root, &vis, &state);
    if (rc != IDL_RETCODE_OK)
        throw std::runtime_error("IDL visitor failed");

    ParseResult res = state.result;
    res.root = ps->root;
    res.pstate = ps;
    guard.release(); // keep ps alive for caller to extract typeinfo
    return res;
}

} // namespace

DdsAdapter::DdsAdapter() = default;
DdsAdapter::~DdsAdapter() = default;

std::vector<Value> DdsAdapter::allocateTypes(const std::string& idlFile)
{
    std::ifstream in(idlFile);
    if (!in.is_open())
        throw std::runtime_error("Unable to open IDL file: " + idlFile);
    std::stringstream buffer;
    buffer << in.rdbuf();

    ParseResult parsed = parseWithIdl(buffer.str());
    if (!parsed.package.empty())
        lastPackage_ = parsed.package;
    else
        lastPackage_.clear();
    rootNode_ = const_cast<idl_node_t*>(parsed.root);
    parserState_ = parsed.pstate;

    // generate typeinfo/typemap blobs for CycloneDDS topic descriptors
    typeInfo_.clear();
    typeMap_.clear();
    if (parsed.pstate && parsed.root) {
        idl_typeinfo_typemap_t tim{};
        if (generate_type_meta_ser(reinterpret_cast<idl_pstate_t*>(parsed.pstate),
                                   reinterpret_cast<const idl_node_t*>(parsed.root),
                                   &tim) == IDL_RETCODE_OK) {
            typeInfo_.assign(tim.typeinfo, tim.typeinfo + tim.typeinfo_size);
            typeMap_.assign(tim.typemap, tim.typemap + tim.typemap_size);
            if (tim.typeinfo) free(tim.typeinfo);
            if (tim.typemap) free(tim.typemap);
        }
    }

    auto shortName = [](const std::string& full) -> std::string {
        auto pos = full.rfind("::");
        if (pos == std::string::npos)
            return full;
        return full.substr(pos + 2);
    };

    std::unordered_map<std::string, Value> declByName;
    std::unordered_map<std::string, ObjObjectType*> structByName;
    std::vector<Value> out;

    for (const auto& e : parsed.enums) {
        enumsByFullName_.emplace(e.fullName, e);
        enumsByName_.emplace(shortName(e.fullName), e);
        Value declVal = Value::objectTypeVal(toUnicodeString(shortName(e.fullName)), false, false, true);
        ObjObjectType* enumObj = asObjectType(declVal);
        for (const auto& val : e.values) {
            icu::UnicodeString labelName = toUnicodeString(val.first);
            Value enumValue = Value::enumVal(static_cast<int16_t>(val.second), enumObj->enumTypeId);
            enumObj->enumLabelValues[labelName.hashCode()] = std::make_pair(labelName, enumValue);
        }
        declByName.emplace(e.fullName, declVal);
        declByName.emplace(shortName(e.fullName), declVal);
        fullNameByType_.emplace(enumObj, e.fullName);
        out.push_back(declVal);
    }

    for (const auto& s : parsed.structs) {
        structsByFullName_.emplace(s.fullName, s);
        structsByName_.emplace(shortName(s.fullName), s);
        Value declVal = Value::objectTypeVal(toUnicodeString(shortName(s.fullName)), false);
        ObjObjectType* obj = asObjectType(declVal);
        structByName.emplace(s.fullName, obj);
        declByName.emplace(s.fullName, declVal);
        declByName.emplace(shortName(s.fullName), declVal);
        fullNameByType_.emplace(obj, s.fullName);
        out.push_back(declVal);
    }

    for (const auto& s : parsed.structs) {
        auto structIt = declByName.find(s.fullName);
        if (structIt == declByName.end())
            continue;
        ObjObjectType* obj = structByName[s.fullName];
        for (const auto& f : s.fields) {
            ObjObjectType::Property prop;
            prop.name = toUnicodeString(f.name);
            prop.ownerType = structIt->second.weakRef();
            if (f.type.kind == FieldType::Kind::EnumRef || f.type.kind == FieldType::Kind::StructRef) {
                auto it = declByName.find(f.type.refName);
                if (it != declByName.end())
                    prop.type = it->second;
                else
                    prop.type = Value::nilVal();
                prop.initialValue = Value::nilVal();
            } else {
                prop.type = makeTypeSpec(f.type.kind);
                prop.initialValue = makeDefault(f.type.kind);
            }
            auto hash = prop.name.hashCode();
            obj->properties.emplace(hash, prop);
            obj->propertyOrder.push_back(hash);
            if (f.isKey)
                isKeyByFieldType_[obj] = true;
        }
    }

    return out;
}

std::string DdsAdapter::fullNameForType(const Value& v) const
{
    const ObjObjectType* obj = nullptr;
    if (isObjectType(v))
        obj = asObjectType(v);
    else if (isObjectInstance(v))
        obj = asObjectType(asObjectInstance(v)->instanceType);
    if (obj) {
        auto it = fullNameByType_.find(obj);
        if (it != fullNameByType_.end())
            return it->second;
    }
    return "";
}

const StructInfo* DdsAdapter::findStruct(const std::string& fullName) const
{
    auto it = structsByFullName_.find(fullName);
    if (it != structsByFullName_.end())
        return &it->second;
    auto it2 = structsByName_.find(fullName);
    return it2 == structsByName_.end() ? nullptr : &it2->second;
}

const EnumInfo* DdsAdapter::findEnum(const std::string& fullName) const
{
    auto it = enumsByFullName_.find(fullName);
    if (it != enumsByFullName_.end())
        return &it->second;
    auto it2 = enumsByName_.find(fullName);
    return it2 == enumsByName_.end() ? nullptr : &it2->second;
}

bool DdsAdapter::typeMetaFor(const std::string& fullName,
                             std::vector<unsigned char>& outInfo,
                             std::vector<unsigned char>& outMap) const
{
    const StructInfo* s = findStruct(fullName);
    if (!s || !s->node || !parserState_)
        return false;

    idl_typeinfo_typemap_t tim{};
    if (generate_type_meta_ser(reinterpret_cast<idl_pstate_t*>(parserState_),
                               s->node,
                               &tim) != IDL_RETCODE_OK)
        return false;
    outInfo.assign(tim.typeinfo, tim.typeinfo + tim.typeinfo_size);
    outMap.assign(tim.typemap, tim.typemap + tim.typemap_size);
    if (tim.typeinfo) free(tim.typeinfo);
    if (tim.typemap) free(tim.typemap);
    return true;
}

#endif // ROXAL_ENABLE_DDS
