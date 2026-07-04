#pragma once

#ifdef ROXAL_ENABLE_DDS

#include <string>
#include <vector>
#include <unordered_map>
#include <optional>

extern "C" {
#include "idl/tree.h"
}

#include "Value.h"

namespace roxal {

struct ObjObjectType;

struct FieldType {
    enum class Kind {
        Int32,
        Byte,   // 1-byte int (octet/uint8/int8/char) -- distinct wire width, needed for ROS interop
        Bool,
        Float64,
        String,
        List,
        EnumRef,
        StructRef,
        Int64,
        UInt64,
        Unsupported
    } kind { Kind::Unsupported };
    std::string refName; // for enums/structs
    std::shared_ptr<FieldType> element; // for sequences/lists
    bool bounded{false};
    uint32_t bound{0};
    bool isArray{false}; // true when declared as a fixed-size array
    // The IDL declared a narrower/other-width primitive (float32, int16, ...)
    // that Roxal widens to its nearest FieldType kind. The in-memory and wire
    // width then differ from the IDL, so XTypes metadata generated from the
    // IDL must not be published for types containing such fields.
    bool widened{false};
};

struct FieldInfo {
    std::string name;
    FieldType type;
    bool isKey{false};
    bool isOptional{false};
};

struct StructInfo {
    std::string fullName;
    std::vector<FieldInfo> fields;
    const idl_node_t* node{nullptr};
    int extensibility{IDL_APPENDABLE};
};

struct EnumInfo {
    std::string fullName;
    std::vector<std::pair<std::string, int32_t>> values;
    const idl_node_t* node{nullptr};
};

struct ConstInfo {
    std::string fullName;
    FieldType type;
    Value value;
};

struct TypedefInfo {
    std::string fullName;
    FieldType aliasedType;
};

class DdsAdapter {
public:
    DdsAdapter();
    ~DdsAdapter();

    // rosProfile: apply ROS 2 (rmw_cyclonedds) wire-name mangling to all parsed
    // types (pkg::msg::Type -> pkg::msg::dds_::Type_) before types are built.
    std::vector<Value> allocateTypes(const std::string& idlFile,
                                     const std::vector<std::string>& includeSearchPaths = {},
                                     bool rosProfile = false);
    std::string packageName() const { return lastPackage_; }
    // Every distinct top-level module from the last allocateTypes parse (in order).
    const std::vector<std::string>& topModules() const { return topModules_; }
    // First module declared in the main file's own text (pre-splice scan).
    const std::string& mainFirstModule() const { return mainFirstModule_; }
    // Serialized XTypes type information and typemap for the last parsed IDL root.
    const std::vector<unsigned char>& typeInfo() const { return typeInfo_; }
    const std::vector<unsigned char>& typeMap() const { return typeMap_; }
    const void* rootNode() const { return rootNode_; }
    const void* parserState() const { return parserState_; }
    const std::vector<ConstInfo>& constants() const { return consts_; }
    const std::vector<TypedefInfo>& typedefs() const { return typedefs_; }
    std::string fullNameForType(const Value& v) const;
    const StructInfo* findStruct(const std::string& fullName) const;
    const EnumInfo* findEnum(const std::string& fullName) const;
    bool typeMetaFor(const std::string& fullName,
                     std::vector<unsigned char>& outInfo,
                     std::vector<unsigned char>& outMap) const;
    // (original fullName, ROS-mangled fullName) pairs from the last
    // allocateTypes call with rosProfile=true (empty otherwise).
    const std::vector<std::pair<std::string, std::string>>& rosAliases() const { return rosAliases_; }

private:
    struct ParsedType {
        std::string fullName;
        Value value;
    };

    std::string lastPackage_;
    std::vector<std::string> topModules_;
    std::string mainFirstModule_;
    std::vector<std::pair<std::string, std::string>> rosAliases_;
    std::vector<unsigned char> typeInfo_;
    std::vector<unsigned char> typeMap_;
    void* rootNode_{nullptr};
    void* parserState_{nullptr};
    std::unordered_map<const ObjObjectType*, std::string> fullNameByType_;
    std::unordered_map<const ObjObjectType*, bool> isKeyByFieldType_;
    std::vector<ConstInfo> consts_;
    std::vector<TypedefInfo> typedefs_;
    std::unordered_map<std::string, StructInfo> structsByFullName_;
    std::unordered_map<std::string, StructInfo> structsByName_;
    std::unordered_map<std::string, EnumInfo> enumsByFullName_;
    std::unordered_map<std::string, EnumInfo> enumsByName_;
};

} // namespace roxal

#endif // ROXAL_ENABLE_DDS
