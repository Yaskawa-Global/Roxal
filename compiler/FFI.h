#pragma once

#include <vector>
#include <string>
#include <ffi.h>

#include <core/common.h>
#include "Value.h"
#include "ArgsView.h"
#include "Chunk.h"
#include "Object.h"
#include <core/AST.h>
namespace roxal {

enum class PrimitivePtrType {
    None,
    UInt8,
    Int8,
    UInt16,
    Int16,
    UInt32,
    Int32,
    UInt64,
    Int64
};

struct FFIWrapper {
    ffi_cif cif;
    void* fn;
    std::vector<ffi_type*> argTypes;
    std::vector<bool> argIsCharPtr;
    std::vector<bool> argIsConstCharPtr;
    std::vector<bool> argIsBool;
    std::vector<ObjObjectType*> argObjTypes;
    std::vector<std::vector<ffi_type*>> argStructElems;
    std::vector<ffi_type> argStructTypes;
    std::vector<PrimitivePtrType> argPrimPtrTypes; // primitive pointer arg types
    ffi_type* retType;
    bool retIsCharPtr{false};
    bool retIsBool{false};
    Value retObjType{}; // ObjObjectType
    std::vector<ffi_type*> retStructElems;
    ffi_type retStructType;
};

struct CStructContext {
    std::vector<std::vector<uint8_t>> buffers;
    std::vector<ObjectInstance*> instances;
    std::vector<PrimitivePtrType> primPtrTypes; // primitive pointer buffer types
};

void* createFFIWrapper(void* fn, ffi_type* retType,
                       const std::vector<ffi_type*>& argTypes);

Value loadlib_native(ArgsView args);
Value ffi_native(ArgsView args);

Value callCFunc(ObjClosure* closure, const CallSpec& callSpec, Value* args);

void marshalProperty(const Value& val, const ObjObjectType::Property& prop,
                     size_t ptrSize, std::vector<uint8_t>& buffer,
                     size_t& offset, size_t& structAlign,
                     std::vector<std::string>* stringStore,
                     CStructContext* ctx = nullptr);
Value unmarshalProperty(const ObjObjectType::Property& prop, size_t ptrSize,
                        const uint8_t* bytes, size_t len,
                        size_t& offset, size_t& structAlign,
                        CStructContext* ctx = nullptr);

std::vector<uint8_t> objectToCStruct(ObjectInstance* instance,
                                     std::vector<std::string>* stringStore=nullptr,
                                     CStructContext* ctx = nullptr);
Value objectFromCStruct(const Value& type, const void* data, size_t len); // ObjectInstance
void updateObjectFromCStruct(ObjectInstance* instance, const void* data, size_t len,
                             CStructContext* ctx = nullptr);

}
