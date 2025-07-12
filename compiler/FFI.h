#pragma once

#include <vector>
#include <string>
#include <ffi.h>

#include <core/common.h>
#include "Value.h"
#include "Chunk.h"
#include "Object.h"
#include <core/AST.h>

namespace roxal {

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
    ffi_type* retType;
    bool retIsCharPtr{false};
    bool retIsBool{false};
    ObjObjectType* retObjType{nullptr};
    std::vector<ffi_type*> retStructElems;
    ffi_type retStructType;
};

struct CStructContext {
    std::vector<std::vector<uint8_t>> buffers;
    std::vector<ObjectInstance*> instances;
};

void* createFFIWrapper(void* fn, ffi_type* retType,
                       const std::vector<ffi_type*>& argTypes);

Value loadlib_native(int argCount, Value* args);
Value ffi_native(int argCount, Value* args);

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
ObjectInstance* objectFromCStruct(ObjObjectType* type, const void* data, size_t len);
void updateObjectFromCStruct(ObjectInstance* instance, const void* data, size_t len,
                             CStructContext* ctx = nullptr);

}
