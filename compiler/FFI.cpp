#include "FFI.h"
#include "VM.h"

#include <cstring>
#include <sstream>
#include <stdexcept>
#include <dlfcn.h>
#include <ffi.h>
#include <filesystem>

using namespace roxal;

void* roxal::createFFIWrapper(void* fn, ffi_type* retType,
                              const std::vector<ffi_type*>& argTypes)
{
    FFIWrapper* spec = new FFIWrapper;
    spec->fn = fn;
    spec->retType = retType;
    spec->argTypes = argTypes;
    spec->argIsCharPtr.assign(argTypes.size(), false);
    spec->argIsConstCharPtr.assign(argTypes.size(), false);
    spec->argIsBool.assign(argTypes.size(), false);
    spec->argObjTypes.assign(argTypes.size(), nullptr);
    spec->argStructElems.resize(argTypes.size());
    spec->argStructTypes.resize(argTypes.size());
    spec->argObjTypes.assign(argTypes.size(), nullptr);
    spec->retIsCharPtr = false;
    spec->retIsBool = false;
    if (ffi_prep_cif(&spec->cif, FFI_DEFAULT_ABI, argTypes.size(), retType,
                     spec->argTypes.data()) != FFI_OK)
        throw std::runtime_error("ffi_prep_cif failed");
    return spec;
}

Value roxal::loadlib_native(ArgsView args)
{
    if (args.size() != 1 || !isString(args[0]))
        throw std::invalid_argument("loadlib expects single string argument");

    std::filesystem::path path = toUTF8StdString(asUString(args[0]));

    if (!path.is_absolute()) {
        if (VM::thread && !VM::thread->frames.empty()) {
            const CallFrame& frame = VM::thread->frames.back();
            ObjFunction* fn = asFunction(asClosure(frame.closure)->function);
            std::string src = toUTF8StdString(fn->chunk->sourceName);
            if (!src.empty()) {
                std::filesystem::path base = std::filesystem::path(src).parent_path();
                path = base / path;
            }
        }
    }

    void* h = dlopen(path.string().c_str(), RTLD_LAZY);
    if (!h)
        throw std::runtime_error(std::string("dlopen failed: ") + dlerror());

    return Value::libraryVal(h);
}

Value roxal::ffi_native(ArgsView args)
{
    ObjNative* native = asNative(*(args.data-1));
    FFIWrapper* spec = static_cast<FFIWrapper*>(native->data);
    if (!spec)
        throw std::runtime_error("ffi_native called without spec");
    size_t argCount = args.size();
    if (argCount != spec->argTypes.size())
        throw std::invalid_argument("invalid argument count for ffi function");

    std::vector<void*> argValues(argCount);
    std::vector<double> realVals(argCount);
    std::vector<float> floatVals(argCount);
    std::vector<int32_t> intVals(argCount);
    std::vector<uint32_t> uint32Vals(argCount);
    std::vector<int16_t> sint16Vals(argCount);
    std::vector<uint16_t> uint16Vals(argCount);
    std::vector<uint8_t> byteVals(argCount);
    std::vector<uint8_t> boolVals(argCount);

    for(size_t i=0;i<argCount;i++) {
        if (spec->argTypes[i] == &ffi_type_double || spec->argTypes[i] == &ffi_type_float) {
            if (!args[i].isNumber())
                throw std::invalid_argument("ffi argument not number");
            if (spec->argTypes[i] == &ffi_type_double) {
                realVals[i] = args[i].asReal();
                argValues[i] = &realVals[i];
            } else {
                floatVals[i] = args[i].asReal();
                argValues[i] = &floatVals[i];
            }
        } else if (spec->argTypes[i] == &ffi_type_sint32) {
            if (!args[i].isNumber())
                throw std::invalid_argument("ffi argument not int");
            intVals[i] = args[i].asInt();
            argValues[i] = &intVals[i];
        } else if (spec->argTypes[i] == &ffi_type_uint32) {
            if (!args[i].isNumber())
                throw std::invalid_argument("ffi argument not uint32_t");
            uint32Vals[i] = uint32_t(args[i].asInt());
            argValues[i] = &uint32Vals[i];
        } else if (spec->argTypes[i] == &ffi_type_sint16) {
            if (!args[i].isNumber())
                throw std::invalid_argument("ffi argument not int16_t");
            sint16Vals[i] = int16_t(args[i].asInt());
            argValues[i] = &sint16Vals[i];
        } else if (spec->argTypes[i] == &ffi_type_uint16) {
            if (!args[i].isNumber())
                throw std::invalid_argument("ffi argument not uint16_t");
            uint16Vals[i] = uint16_t(args[i].asInt());
            argValues[i] = &uint16Vals[i];
        } else if (spec->argTypes[i] == &ffi_type_sint8) {
            if (!args[i].isNumber())
                throw std::invalid_argument("ffi argument not int8_t");
            byteVals[i] = uint8_t(int8_t(args[i].asInt()));
            argValues[i] = &byteVals[i];
        } else if (spec->argTypes[i] == &ffi_type_uint8) {
            if (spec->argIsBool[i]) {
                if (!args[i].isBool())
                    throw std::invalid_argument("ffi argument not bool");
                boolVals[i] = args[i].asBool() ? 1 : 0;
                argValues[i] = &boolVals[i];
            } else {
                if (!args[i].isNumber())
                    throw std::invalid_argument("ffi argument not uint8_t");
                byteVals[i] = uint8_t(args[i].asInt());
                argValues[i] = &byteVals[i];
            }
        } else {
            throw std::runtime_error("unsupported ffi arg type");
        }
    }

    union {
        double d; float f;
        int32_t i; uint32_t ui32;
        int16_t s16; uint16_t u16;
        int8_t s8; uint8_t u8;
    } ret;

    ffi_call(&spec->cif, FFI_FN(spec->fn), &ret, argValues.data());

    if (spec->retType == &ffi_type_double)
        return Value::realVal(ret.d);
    else if (spec->retType == &ffi_type_float)
        return Value::realVal(double(ret.f));
    else if (spec->retType == &ffi_type_sint32)
        return Value::intVal(ret.i);
    else if (spec->retType == &ffi_type_uint32)
        return Value::intVal(int32_t(ret.ui32));
    else if (spec->retType == &ffi_type_sint16)
        return Value::intVal(int32_t(ret.s16));
    else if (spec->retType == &ffi_type_uint16)
        return Value::intVal(int32_t(ret.u16));
    else if (spec->retType == &ffi_type_sint8)
        return Value::byteVal(uint8_t(ret.s8));
    else if (spec->retType == &ffi_type_uint8)
        return spec->retIsBool ? Value::boolVal(ret.u8 != 0) : Value::byteVal(ret.u8);
    else
        throw std::runtime_error("unsupported ffi return type");
}

Value roxal::callCFunc(ObjClosure* closure, const CallSpec& callSpec, Value* args)
{
    ObjFunction* function = asFunction(closure->function);
    FFIWrapper* spec = static_cast<FFIWrapper*>(function->nativeSpec);

    if (!spec) {
        ptr<ast::Annotation> annot;
        for(const auto& a : function->annotations)
            if (a->name == "cfunc") { annot = a; break; }
        if (!annot)
            throw std::runtime_error("cfunc annotation missing");

        ObjModuleType* mod = asModuleType(function->moduleType);

        auto getArg = [&](const std::string& n) -> ptr<ast::Expression> {
            for(const auto& an : annot->args)
                if (toUTF8StdString(an.first) == n) return an.second;
            return nullptr;
        };

        auto libExpr = getArg("lib");
        auto cnameExpr = getArg("cname");
        auto argsExpr = getArg("args");
        auto retExpr = getArg("ret");
        if (!libExpr)
            throw std::runtime_error("cfunc annotation requires lib");

        auto evalExpr = [&](ptr<ast::Expression> expr) -> Value {
            if (auto s = dynamic_ptr_cast<ast::Str>(expr)) {
                return Value::stringVal(s->str);
            } else if (auto n = dynamic_ptr_cast<ast::Num>(expr)) {
                if (std::holds_alternative<int32_t>(n->num))
                    return Value(std::get<int32_t>(n->num));
                else
                    return Value(std::get<double>(n->num));
            } else if (auto b = dynamic_ptr_cast<ast::Bool>(expr)) {
                return Value::boolVal(b->value);
            } else if (auto v = dynamic_ptr_cast<ast::Variable>(expr)) {
                auto name = v->name;
                auto opt = mod->vars.load(name);
                if (!opt.has_value())
                    throw std::runtime_error("unknown variable in cfunc annotation: "+toUTF8StdString(name));
                return opt.value();
            } else {
                throw std::runtime_error("unsupported expression in cfunc annotation");
            }
        };

        Value libVal = evalExpr(libExpr);
        if (!isLibrary(libVal))
            throw std::runtime_error("lib argument not library handle");
        void* handle = asLibrary(libVal)->handle;

        std::string cname;
        if (cnameExpr) {
            Value cnameVal = evalExpr(cnameExpr);
            cname = toUTF8StdString(asUString(cnameVal));
        } else {
            cname = toUTF8StdString(function->name);
        }

        std::vector<ffi_type*> argTypes;
        std::vector<bool> argIsCharPtr;
        std::vector<bool> argIsConstCharPtr;
        std::vector<bool> argIsBool;
        std::vector<ObjObjectType*> argObjTypes;
        std::vector<std::vector<ffi_type*>> argStructElems;
        std::vector<ffi_type> argStructTypes;
        std::vector<PrimitivePtrType> argPrimPtrTypes;
        if (argsExpr) {
            std::string argsStr = toUTF8StdString(asUString(evalExpr(argsExpr)));
            std::stringstream ss(argsStr);
            std::string token;
            while(std::getline(ss, token, ',')) {
                token.erase(0, token.find_first_not_of(" \t"));
                size_t space = token.rfind(' ');
                std::string type = token;
                if (space != std::string::npos)
                    type = token.substr(0, space);
                type.erase(type.find_last_not_of(" \t") + 1);
                if (type=="float")
                    argTypes.push_back(&ffi_type_float);
                else if (type=="double" || type=="real")
                    argTypes.push_back(&ffi_type_double);
                else if (type=="int" || type=="int32_t")
                    argTypes.push_back(&ffi_type_sint32);
                else if (type=="uint32_t")
                    argTypes.push_back(&ffi_type_uint32);
                else if (type=="int16_t")
                    argTypes.push_back(&ffi_type_sint16);
                else if (type=="uint16_t")
                    argTypes.push_back(&ffi_type_uint16);
                else if (type=="int8_t")
                    argTypes.push_back(&ffi_type_sint8);
                else if (type=="uint8_t")
                    argTypes.push_back(&ffi_type_uint8);
                else if (type=="bool") {
                    argTypes.push_back(&ffi_type_uint8);
                    argIsBool.push_back(true);
                }
                else if (type=="char*") {
                    argTypes.push_back(&ffi_type_pointer);
                    argIsCharPtr.push_back(true);
                    argIsConstCharPtr.push_back(false);
                }
                else if (type=="const char*") {
                    argTypes.push_back(&ffi_type_pointer);
                    argIsCharPtr.push_back(true);
                    argIsConstCharPtr.push_back(true);
                }
                else if (!type.empty() && (type.back()=='*' || type.back()=='&')) {
                    std::string base = type.substr(0, type.size()-1);
                    while(!base.empty() && isspace(base.back())) base.pop_back();
                    PrimitivePtrType ppt = PrimitivePtrType::None;
                    if (base=="uint8_t") ppt = PrimitivePtrType::UInt8;
                    else if (base=="int8_t") ppt = PrimitivePtrType::Int8;
                    else if (base=="uint16_t") ppt = PrimitivePtrType::UInt16;
                    else if (base=="int16_t") ppt = PrimitivePtrType::Int16;
                    else if (base=="uint32_t") ppt = PrimitivePtrType::UInt32;
                    else if (base=="int32_t" || base=="int") ppt = PrimitivePtrType::Int32;
                    if (ppt != PrimitivePtrType::None) {
                        argTypes.push_back(&ffi_type_pointer);
                        argPrimPtrTypes.push_back(ppt);
                    } else {
                        argTypes.push_back(&ffi_type_pointer);
                        argPrimPtrTypes.push_back(PrimitivePtrType::None);
                    }
                }
                else {
                    auto opt = mod->vars.load(toUnicodeString(type));
                    if (opt.has_value() && isObjectType(opt.value())) {
                        ObjObjectType* t = asObjectType(opt.value());
                        if (!t->isCStruct)
                            throw std::runtime_error("arg type not cstruct: "+type);
                        std::vector<ffi_type*> elems;
                        std::function<void(ObjObjectType*)> appendStruct;
                        appendStruct = [&](ObjObjectType* st) {
                            for (int32_t h : st->propertyOrder) {
                                const auto& prop = st->properties.at(h);
                                std::string ct;
                                if (prop.ctype.has_value())
                                    ct = toUTF8StdString(prop.ctype.value());
                                auto byName = [&](const std::string& n) -> ffi_type* {
                                    if (n=="float") return &ffi_type_float;
                                    if (n=="double" || n=="real") return &ffi_type_double;
                                    if (n=="int" || n=="int32_t") return &ffi_type_sint32;
                                    if (n=="uint32_t") return &ffi_type_uint32;
                                    if (n=="int16_t") return &ffi_type_sint16;
                                    if (n=="uint16_t") return &ffi_type_uint16;
                                    if (n=="int8_t") return &ffi_type_sint8;
                                    if (n=="uint8_t") return &ffi_type_uint8;
                                    if (n=="bool") return &ffi_type_uint8;
                                    if (n=="char*" || n=="const char*" || !n.empty() && n.back()=='*')
                                        return &ffi_type_pointer;
                                    return nullptr;
                                };
                                auto parseArray = [&](const std::string& str, std::string& base, size_t& len) -> bool {
                                    auto lb=str.find('['); auto rb=str.find(']', lb==std::string::npos?0:lb);
                                    if(lb!=std::string::npos && rb==str.size()-1){
                                        base=str.substr(0,lb); while(!base.empty() && isspace(base.back())) base.pop_back();
                                        std::string ls=str.substr(lb+1, rb-lb-1); len=strtoul(ls.c_str(),nullptr,10); return len>0; }
                                    return false; };

                                ffi_type* et = nullptr;
                                size_t arrLen = 0; std::string arrBase;
                                bool isArr = (!ct.empty() && parseArray(ct, arrBase, arrLen));
                                if (isArr) {
                                    et = byName(arrBase);
                                } else if (!ct.empty()) {
                                    et = byName(ct);
                                    if (!et && isObjectType(prop.type) && ct.back()!='*') {
                                        ObjObjectType* nt = asObjectType(prop.type);
                                        if (!nt->isCStruct)
                                            throw std::runtime_error("nested struct field not cstruct in arg type "+type);
                                        appendStruct(nt);
                                        continue;
                                    }
                                } else if (isTypeSpec(prop.type)) {
                                    ObjTypeSpec* ts = asTypeSpec(prop.type);
                                    switch(ts->typeValue) {
                                        case ValueType::Bool: et=&ffi_type_uint8; break;
                                        case ValueType::Byte: et=&ffi_type_uint8; break;
                                        case ValueType::Int: et=&ffi_type_sint32; break;
                                        case ValueType::Real: et=&ffi_type_double; break;
                                        case ValueType::Enum: et=&ffi_type_sint32; break;
                                        default: break;
                                    }
                                }
                                if (!et)
                                    throw std::runtime_error("unsupported struct field type "+ct+" in arg type "+type);
                                if (isArr) {
                                    for(size_t ai=0; ai<arrLen; ++ai) elems.push_back(et);
                                } else {
                                    elems.push_back(et);
                                }
                            }
                        };
                        appendStruct(t);
                        elems.push_back(nullptr);
                        ffi_type st; st.size=0; st.alignment=0; st.type=FFI_TYPE_STRUCT; st.elements=elems.data();
                        argStructElems.push_back(elems);
                        argStructTypes.push_back(st);
                        argTypes.push_back(&argStructTypes.back());
                        argObjTypes.push_back(t);
                    } else {
                        throw std::runtime_error("unsupported arg type: "+type);
                    }
                }
                if (argIsCharPtr.size() < argTypes.size())
                    argIsCharPtr.push_back(false);
                if (argIsConstCharPtr.size() < argTypes.size())
                    argIsConstCharPtr.push_back(false);
                if (argIsBool.size() < argTypes.size())
                    argIsBool.push_back(false);
                if (argObjTypes.size() < argTypes.size())
                    argObjTypes.push_back(nullptr);
                if (argStructElems.size() < argTypes.size())
                    argStructElems.emplace_back();
                if (argStructTypes.size() < argTypes.size())
                    argStructTypes.emplace_back();
                if (argPrimPtrTypes.size() < argTypes.size())
                    argPrimPtrTypes.push_back(PrimitivePtrType::None);
            }
        }

        ffi_type* retType = &ffi_type_void;
        bool retIsCharPtr = false;
        bool retIsBool = false;
        Value retObjType {}; // ObjObjectType
        std::vector<ffi_type*> retElems;
        ffi_type retStruct;
        if (retExpr) {
            std::string r = toUTF8StdString(asUString(evalExpr(retExpr)));
            if (r=="float")
                retType = &ffi_type_float;
            else if (r=="double" || r=="real")
                retType = &ffi_type_double;
            else if (r=="int" || r=="int32_t")
                retType = &ffi_type_sint32;
            else if (r=="uint32_t")
                retType = &ffi_type_uint32;
            else if (r=="int16_t")
                retType = &ffi_type_sint16;
            else if (r=="uint16_t")
                retType = &ffi_type_uint16;
            else if (r=="int8_t")
                retType = &ffi_type_sint8;
            else if (r=="uint8_t")
                retType = &ffi_type_uint8;
            else if (r=="bool") {
                retType = &ffi_type_uint8;
                retIsBool = true;
            }
            else if (r=="void")
                retType = &ffi_type_void;
            else if (r=="char*") {
                retType = &ffi_type_pointer;
                retIsCharPtr = true;
            }
            else {
                auto opt = mod->vars.load(toUnicodeString(r));
                if (opt.has_value() && isObjectType(opt.value())) {
                    ObjObjectType* t = asObjectType(opt.value());
                    if (!t->isCStruct)
                        throw std::runtime_error("return type not cstruct: "+r);
                    retObjType = opt.value();
                    std::function<void(ObjObjectType*)> appendStruct;
                    appendStruct = [&](ObjObjectType* st) {
                        for (int32_t h : st->propertyOrder) {
                            const auto& prop = st->properties.at(h);
                            std::string ct;
                            if (prop.ctype.has_value())
                                ct = toUTF8StdString(prop.ctype.value());
                            auto byName = [&](const std::string& n) -> ffi_type* {
                                if (n=="float") return &ffi_type_float;
                                if (n=="double" || n=="real") return &ffi_type_double;
                                if (n=="int" || n=="int32_t") return &ffi_type_sint32;
                                if (n=="uint32_t") return &ffi_type_uint32;
                                if (n=="int16_t") return &ffi_type_sint16;
                                if (n=="uint16_t") return &ffi_type_uint16;
                                if (n=="int8_t") return &ffi_type_sint8;
                                if (n=="uint8_t") return &ffi_type_uint8;
                                if (n=="bool") return &ffi_type_uint8;
                                if (n=="char*" || n=="const char*" || !n.empty() && n.back()=='*')
                                    return &ffi_type_pointer;
                                return nullptr;
                            };
                            auto parseArray = [&](const std::string& str, std::string& base, size_t& len) -> bool {
                                auto lb=str.find('['); auto rb=str.find(']', lb==std::string::npos?0:lb);
                                if(lb!=std::string::npos && rb==str.size()-1){
                                    base=str.substr(0,lb); while(!base.empty() && isspace(base.back())) base.pop_back();
                                    std::string ls=str.substr(lb+1, rb-lb-1); len=strtoul(ls.c_str(),nullptr,10); return len>0; }
                                return false; };

                            ffi_type* et = nullptr;
                            size_t arrLen = 0; std::string arrBase;
                            bool isArr = (!ct.empty() && parseArray(ct, arrBase, arrLen));
                            if (isArr) {
                                et = byName(arrBase);
                            } else if (!ct.empty()) {
                                et = byName(ct);
                                if (!et && isObjectType(prop.type) && ct.back()!='*') {
                                    ObjObjectType* nt = asObjectType(prop.type);
                                    if (!nt->isCStruct)
                                        throw std::runtime_error("nested struct field not cstruct in return type "+r);
                                    appendStruct(nt);
                                    continue;
                                }
                            } else if (isTypeSpec(prop.type)) {
                                ObjTypeSpec* ts = asTypeSpec(prop.type);
                                switch(ts->typeValue) {
                                    case ValueType::Bool: et=&ffi_type_uint8; break;
                                    case ValueType::Byte: et=&ffi_type_uint8; break;
                                    case ValueType::Int: et=&ffi_type_sint32; break;
                                    case ValueType::Real: et=&ffi_type_double; break;
                                    case ValueType::Enum: et=&ffi_type_sint32; break;
                                    default: break;
                                }
                            }
                            if (!et)
                                throw std::runtime_error("unsupported struct field type "+ct+" in return type "+r);
                            if (isArr) {
                                for(size_t ai=0; ai<arrLen; ++ai) retElems.push_back(et);
                            } else {
                                retElems.push_back(et);
                            }
                        }
                    };
                    appendStruct(t);
                    retElems.push_back(nullptr);
                    retStruct.size = 0; retStruct.alignment = 0; retStruct.type = FFI_TYPE_STRUCT;
                    retStruct.elements = retElems.data();
                    retType = &retStruct;
                } else {
                    throw std::runtime_error("unsupported return type: "+r);
                }
            }
        }

        void* fnPtr = dlsym(handle, cname.c_str());
        if (!fnPtr)
            throw std::runtime_error(std::string("dlsym failed: ")+dlerror());

        spec = new FFIWrapper;
        spec->fn = fnPtr;
        spec->argTypes = argTypes;
        spec->argIsCharPtr = argIsCharPtr;
        spec->argIsConstCharPtr = argIsConstCharPtr;
        spec->argIsBool = argIsBool;
        spec->argObjTypes = argObjTypes;
        spec->argStructElems = argStructElems;
        spec->argStructTypes = argStructTypes;
        spec->argPrimPtrTypes = argPrimPtrTypes;
        for (size_t i=0;i<spec->argStructTypes.size();i++)
            if (spec->argObjTypes[i])
                spec->argStructTypes[i].elements = spec->argStructElems[i].data();
        for (size_t i=0;i<spec->argTypes.size();i++)
            if (spec->argObjTypes[i])
                spec->argTypes[i] = &spec->argStructTypes[i];
        spec->retType = retType;
        spec->retIsCharPtr = retIsCharPtr;
        spec->retIsBool = retIsBool;
        spec->retObjType = retObjType;
        if (!retObjType.isNil()) {
            spec->retStructElems = retElems;
            spec->retStructType = retStruct;
            spec->retStructType.elements = spec->retStructElems.data();
            spec->retType = &spec->retStructType;
        }
        if (ffi_prep_cif(&spec->cif, FFI_DEFAULT_ABI, spec->argTypes.size(), spec->retType,
                         spec->argTypes.data()) != FFI_OK)
            throw std::runtime_error("ffi_prep_cif failed");
        function->nativeSpec = spec;
    }

    if (callSpec.argCount != spec->argTypes.size()) {
        throw std::invalid_argument(
            "Invalid argument count for cfunc " + toUTF8StdString(function->name) +
            ": expected " + std::to_string(spec->argTypes.size()) +
            ", got " + std::to_string(callSpec.argCount));
    }

    std::vector<Value> argVector(callSpec.argCount);
    for(size_t i=0;i<callSpec.argCount;i++)
        argVector[i] = args[i];

    std::vector<void*> argValues(callSpec.argCount);
    std::vector<std::vector<uint8_t>> structBuffers(callSpec.argCount);
    std::vector<ObjectInstance*> structArgInstances(callSpec.argCount);
    std::vector<std::vector<std::string>> structStrings(callSpec.argCount);
    std::vector<CStructContext> structContexts(callSpec.argCount);
    std::vector<const char*> cstrPtrs(callSpec.argCount);
    std::vector<std::vector<char>> mutableBuffers(callSpec.argCount);
    std::vector<char*> mutablePtrs(callSpec.argCount);
    std::vector<ObjString*> mutableStringObjs(callSpec.argCount);
    std::vector<void*> structPtrs(callSpec.argCount);
    std::vector<double> realVals(callSpec.argCount);
    std::vector<float> floatVals(callSpec.argCount);
    std::vector<int32_t> intVals(callSpec.argCount);
    std::vector<uint32_t> uint32Vals(callSpec.argCount);
    std::vector<int16_t> sint16Vals(callSpec.argCount);
    std::vector<uint16_t> uint16Vals(callSpec.argCount);
    std::vector<uint8_t> byteVals(callSpec.argCount);
    std::vector<uint8_t> boolVals(callSpec.argCount);
    std::vector<int32_t> intPtrVals(callSpec.argCount);
    std::vector<uint32_t> uint32PtrVals(callSpec.argCount);
    std::vector<int16_t> sint16PtrVals(callSpec.argCount);
    std::vector<uint16_t> uint16PtrVals(callSpec.argCount);
    std::vector<int8_t> sint8PtrVals(callSpec.argCount);
    std::vector<uint8_t> uint8PtrVals(callSpec.argCount);
    std::vector<void*> primPtrPtrs(callSpec.argCount);

    auto funcNameAndArg = [&](int i) {
        return toUTF8StdString(function->name)+" arg"+std::to_string(i);
    };

    for(int i=0;i<callSpec.argCount;i++) {
        if (spec->argTypes[i] == &ffi_type_double || spec->argTypes[i] == &ffi_type_float) {
            if (!argVector[i].isNumber())
                throw std::invalid_argument(funcNameAndArg(i)+" not a number for C double/float");
            if (spec->argTypes[i] == &ffi_type_double) {
                realVals[i] = argVector[i].asReal();
                argValues[i] = &realVals[i];
            } else {
                floatVals[i] = argVector[i].asReal();
                argValues[i] = &floatVals[i];
            }
        } else if (spec->argTypes[i] == &ffi_type_sint32) {
            if (!argVector[i].isNumber() && !argVector[i].isEnum())
                throw std::invalid_argument(funcNameAndArg(i)+" not a number for C int32");
            intVals[i] = argVector[i].asInt();
            argValues[i] = &intVals[i];
        } else if (spec->argTypes[i] == &ffi_type_uint32) {
            if (!argVector[i].isNumber() && !argVector[i].isEnum())
                throw std::invalid_argument(funcNameAndArg(i)+" not a number for C uint32");
            uint32Vals[i] = uint32_t(argVector[i].asInt());
            argValues[i] = &uint32Vals[i];
        } else if (spec->argTypes[i] == &ffi_type_sint16) {
            if (!argVector[i].isNumber() && !argVector[i].isEnum())
                throw std::invalid_argument(funcNameAndArg(i)+" not a number for C int16");
            sint16Vals[i] = int16_t(argVector[i].asInt());
            argValues[i] = &sint16Vals[i];
        } else if (spec->argTypes[i] == &ffi_type_uint16) {
            if (!argVector[i].isNumber() && !argVector[i].isEnum())
                throw std::invalid_argument(funcNameAndArg(i)+" not a number for C uint16");
            uint16Vals[i] = uint16_t(argVector[i].asInt());
            argValues[i] = &uint16Vals[i];
        } else if (spec->argTypes[i] == &ffi_type_sint8) {
            if (!argVector[i].isNumber() && !argVector[i].isEnum())
                throw std::invalid_argument(funcNameAndArg(i)+" not a number for C int8_t");
            byteVals[i] = uint8_t(int8_t(argVector[i].asInt()));
            argValues[i] = &byteVals[i];
        } else if (spec->argTypes[i] == &ffi_type_uint8) {
            if (spec->argIsBool[i]) {
                if (!argVector[i].isBool())
                    throw std::invalid_argument(funcNameAndArg(i)+" not bool");
                boolVals[i] = argVector[i].asBool() ? 1 : 0;
                argValues[i] = &boolVals[i];
            } else {
                if (!argVector[i].isNumber() && !argVector[i].isEnum())
                    throw std::invalid_argument(funcNameAndArg(i)+" not a number for C uint8_t");
                byteVals[i] = uint8_t(argVector[i].asInt());
                argValues[i] = &byteVals[i];
            }
        } else if (spec->argPrimPtrTypes.size()>i && spec->argPrimPtrTypes[i] != PrimitivePtrType::None) {
            PrimitivePtrType pt = spec->argPrimPtrTypes[i];
            switch(pt) {
                case PrimitivePtrType::Int32:
                    intPtrVals[i] = argVector[i].asInt();
                    primPtrPtrs[i] = &intPtrVals[i];
                    break;
                case PrimitivePtrType::UInt32:
                    uint32PtrVals[i] = uint32_t(argVector[i].asInt());
                    primPtrPtrs[i] = &uint32PtrVals[i];
                    break;
                case PrimitivePtrType::Int16:
                    sint16PtrVals[i] = int16_t(argVector[i].asInt());
                    primPtrPtrs[i] = &sint16PtrVals[i];
                    break;
                case PrimitivePtrType::UInt16:
                    uint16PtrVals[i] = uint16_t(argVector[i].asInt());
                    primPtrPtrs[i] = &uint16PtrVals[i];
                    break;
                case PrimitivePtrType::Int8:
                    sint8PtrVals[i] = int8_t(argVector[i].asInt());
                    primPtrPtrs[i] = &sint8PtrVals[i];
                    break;
                case PrimitivePtrType::UInt8:
                    uint8PtrVals[i] = uint8_t(argVector[i].asInt());
                    primPtrPtrs[i] = &uint8PtrVals[i];
                    break;
                default:
                    break;
            }
            argValues[i] = &primPtrPtrs[i];
        } else if (spec->argObjTypes[i]) {
            if (!isObjectInstance(argVector[i]))
                throw std::invalid_argument(funcNameAndArg(i)+" not object instance for C struct value");
            ObjectInstance* inst = asObjectInstance(argVector[i]);
            structBuffers[i] = objectToCStruct(inst, &structStrings[i], &structContexts[i]);
            argValues[i] = structBuffers[i].data();
        } else if (spec->argTypes[i] == &ffi_type_pointer) {
            if (spec->argIsCharPtr[i]) {
                if (!isString(argVector[i]))
                    throw std::invalid_argument(funcNameAndArg(i)+" not string for C char*");
                if (spec->argIsConstCharPtr[i]) {
                    std::string s = toUTF8StdString(asUString(argVector[i]));
                    structStrings[i].push_back(std::move(s));
                    cstrPtrs[i] = structStrings[i].back().c_str();
                    argValues[i] = &cstrPtrs[i];
                } else {
                    std::string s = toUTF8StdString(asUString(argVector[i]));
                    mutableBuffers[i].assign(s.begin(), s.end());
                    mutableBuffers[i].push_back('\0');
                    mutablePtrs[i] = mutableBuffers[i].data();
                    argValues[i] = &mutablePtrs[i];
                    mutableStringObjs[i] = asStringObj(argVector[i]);
                }
            } else {
                if (!isObjectInstance(argVector[i])) {
                    if (argVector[i].isNil())
                        throw std::invalid_argument(funcNameAndArg(i)+" not object instance for C pointer (nil)");
                    else
                        throw std::invalid_argument(funcNameAndArg(i)+" not object instance for C pointer");
                }
                ObjectInstance* inst = asObjectInstance(argVector[i]);
                structArgInstances[i] = inst;
                structBuffers[i] = objectToCStruct(inst, &structStrings[i], &structContexts[i]);
                structPtrs[i] = structBuffers[i].data();
                argValues[i] = &structPtrs[i];
            }
        } else {
            throw std::runtime_error("unsupported ffi arg type");
        }
    }

    union { double d; float f; int32_t i; uint32_t ui32; int16_t s16; uint16_t u16; int8_t s8; uint8_t u8; void* p; } ret;
    void* retPtr = &ret;
    std::vector<uint8_t> retBuf;
    if (!spec->retObjType.isNil()) {
        retBuf.resize(spec->cif.rtype->size);
        retPtr = retBuf.data();
    }
    ffi_call(&spec->cif, FFI_FN(spec->fn), retPtr, argValues.data());

    for (int i=0;i<callSpec.argCount;i++)
        if (structArgInstances[i])
            updateObjectFromCStruct(structArgInstances[i], structBuffers[i].data(), structBuffers[i].size(), &structContexts[i]);

    for (int i=0;i<callSpec.argCount;i++) {
        if (mutableStringObjs[i]) {
            std::string newStr(mutableBuffers[i].data());
            if (newStr != toUTF8StdString(mutableStringObjs[i]->s))
                updateInternedString(mutableStringObjs[i], toUnicodeString(newStr));
        }
        if (spec->argPrimPtrTypes.size()>i && spec->argPrimPtrTypes[i] != PrimitivePtrType::None) {
            PrimitivePtrType pt = spec->argPrimPtrTypes[i];
            switch(pt) {
                case PrimitivePtrType::Int32:
                    argVector[i] = Value::intVal(intPtrVals[i]);
                    break;
                case PrimitivePtrType::UInt32:
                    argVector[i] = Value::intVal(int32_t(uint32PtrVals[i]));
                    break;
                case PrimitivePtrType::Int16:
                    argVector[i] = Value::intVal(int32_t(sint16PtrVals[i]));
                    break;
                case PrimitivePtrType::UInt16:
                    argVector[i] = Value::intVal(int32_t(uint16PtrVals[i]));
                    break;
                case PrimitivePtrType::Int8:
                    argVector[i] = Value::byteVal(uint8_t(sint8PtrVals[i]));
                    break;
                case PrimitivePtrType::UInt8:
                    argVector[i] = Value::byteVal(uint8PtrVals[i]);
                    break;
                default: break;
            }
            args[i] = argVector[i];
        }
    }

    if (!spec->retObjType.isNil()) {
        return objectFromCStruct(spec->retObjType, retBuf.data(), retBuf.size());
    } else if (spec->retType == &ffi_type_double)
        return Value(ret.d);
    else if (spec->retType == &ffi_type_float)
        return Value(double(ret.f));
    else if (spec->retType == &ffi_type_sint32)
        return Value::intVal(ret.i);
    else if (spec->retType == &ffi_type_uint32)
        return Value::intVal(int32_t(ret.ui32));
    else if (spec->retType == &ffi_type_sint16)
        return Value::intVal(int32_t(ret.s16));
    else if (spec->retType == &ffi_type_uint16)
        return Value::intVal(int32_t(ret.u16));
    else if (spec->retType == &ffi_type_sint8)
        return Value::byteVal(uint8_t(ret.s8));
    else if (spec->retType == &ffi_type_uint8)
        return spec->retIsBool ? Value::boolVal(ret.u8 != 0) : Value::byteVal(ret.u8);
    else
        return Value::nilVal();
}

void roxal::marshalProperty(const Value& val, const ObjObjectType::Property& prop,
                            size_t ptrSize, std::vector<uint8_t>& buffer,
                            size_t& offset, size_t& structAlign,
                            std::vector<std::string>* stringStore,
                            CStructContext* ctx)
{
    std::string ctypeStr;
    if (prop.ctype.has_value())
        ctypeStr = toUTF8StdString(prop.ctype.value());

    bool isArray = false;
    std::string arrayBase;
    size_t arrayLen = 0;
    auto lb = ctypeStr.find('[');
    auto rb = ctypeStr.find(']', lb == std::string::npos ? 0 : lb);
    if (lb != std::string::npos && rb == ctypeStr.size() - 1) {
        arrayBase = ctypeStr.substr(0, lb);
        while(!arrayBase.empty() && isspace(arrayBase.back())) arrayBase.pop_back();
        std::string lenStr = ctypeStr.substr(lb+1, rb-lb-1);
        arrayLen = std::strtoul(lenStr.c_str(), nullptr, 10);
        if (arrayLen > 0) {
            isArray = true;
        }
    }

    auto appendPadded = [&](const void* data, size_t size, size_t align) {
        size_t padding = (align - (offset % align)) % align;
        buffer.insert(buffer.end(), padding, 0);
        const uint8_t* p = reinterpret_cast<const uint8_t*>(data);
        buffer.insert(buffer.end(), p, p + size);
        offset += padding + size;
        structAlign = std::max(structAlign, align);
    };

    auto writeByNameVal = [&](const std::string& ctype, const Value& v) -> bool {
        if (ctype == "float") { float f = float(toType(ValueType::Real, v, false).asReal()); appendPadded(&f, sizeof(f), 4); return true; }
        if (ctype == "double" || ctype == "real") { double d = toType(ValueType::Real, v, false).asReal(); appendPadded(&d, sizeof(d), 8); return true; }
        if (ctype == "int" || ctype=="int32_t") { int32_t i = toType(ValueType::Int, v, false).asInt(); appendPadded(&i, sizeof(i), 4); return true; }
        if (ctype == "uint32_t") { uint32_t u = uint32_t(toType(ValueType::Int, v, false).asInt()); appendPadded(&u, sizeof(u), 4); return true; }
        if (ctype == "int16_t") { int16_t s = int16_t(toType(ValueType::Int, v, false).asInt()); appendPadded(&s, sizeof(s), 2); return true; }
        if (ctype == "uint16_t") { uint16_t u = uint16_t(toType(ValueType::Int, v, false).asInt()); appendPadded(&u, sizeof(u), 2); return true; }
        if (ctype == "int8_t") { int8_t s = int8_t(toType(ValueType::Byte, v, false).asByte()); appendPadded(&s, sizeof(s),1); return true; }
        if (ctype == "uint8_t") { uint8_t u = toType(ValueType::Byte, v, false).asByte(); appendPadded(&u, sizeof(u),1); return true; }
        if (ctype == "bool") { uint8_t b = toType(ValueType::Bool, v, false).asBool()?1:0; appendPadded(&b, sizeof(b),1); return true; }
        if (ctype == "char*") {
            if (!isString(v))
                throw std::runtime_error("char* field expects string value");
            std::string s = toUTF8StdString(asUString(v));
            stringStore->push_back(std::move(s));
            const char* p = stringStore->back().c_str();
            appendPadded(&p, ptrSize, ptrSize);
            return true;
        }
        if (!ctype.empty() && ctype.back()=='*') {
            if (!ctx)
                throw std::runtime_error("marshalProperty missing context for primitive pointer");
            std::string base = ctype.substr(0, ctype.size()-1);
            while(!base.empty() && isspace(base.back())) base.pop_back();
            PrimitivePtrType ppt = PrimitivePtrType::None;
            if (base=="uint8_t") ppt = PrimitivePtrType::UInt8;
            else if (base=="int8_t") ppt = PrimitivePtrType::Int8;
            else if (base=="uint16_t") ppt = PrimitivePtrType::UInt16;
            else if (base=="int16_t") ppt = PrimitivePtrType::Int16;
            else if (base=="uint32_t") ppt = PrimitivePtrType::UInt32;
            else if (base=="int32_t" || base=="int") ppt = PrimitivePtrType::Int32;
            if (ppt != PrimitivePtrType::None) {
                size_t sz = (ppt==PrimitivePtrType::UInt8 || ppt==PrimitivePtrType::Int8)?1:
                             (ppt==PrimitivePtrType::UInt16 || ppt==PrimitivePtrType::Int16?2:4);
                ctx->buffers.emplace_back(sz);
                ctx->instances.push_back(nullptr);
                ctx->primPtrTypes.push_back(ppt);
                void* pbuf = ctx->buffers.back().data();
                switch(ppt) {
                    case PrimitivePtrType::UInt8: ctx->buffers.back()[0] = toType(ValueType::Byte,v,false).asByte(); break;
                    case PrimitivePtrType::Int8: ctx->buffers.back()[0] = uint8_t(int8_t(toType(ValueType::Byte,v,false).asByte())); break;
                    case PrimitivePtrType::UInt16: { uint16_t u = uint16_t(toType(ValueType::Int,v,false).asInt()); memcpy(pbuf,&u,2); break; }
                    case PrimitivePtrType::Int16: { int16_t s = int16_t(toType(ValueType::Int,v,false).asInt()); memcpy(pbuf,&s,2); break; }
                    case PrimitivePtrType::UInt32: { uint32_t u = uint32_t(toType(ValueType::Int,v,false).asInt()); memcpy(pbuf,&u,4); break; }
                    case PrimitivePtrType::Int32: { int32_t s = toType(ValueType::Int,v,false).asInt(); memcpy(pbuf,&s,4); break; }
                    default: break;
                }
                appendPadded(&pbuf, ptrSize, ptrSize);
                return true;
            }
        }
        if (ctype == "void*" || (!ctype.empty() && ctype.back()=='*')) {
            void* p = nullptr;
            if (!v.isNil()) {
                if (!isForeignPtr(v))
                    throw std::runtime_error("void* field expects foreignptr value");
                p = asForeignPtr(v)->ptr;
            }
            appendPadded(&p, ptrSize, ptrSize);
            return true;
        }
        return false;
    };

    if (isArray) {
        if (!isList(val))
            throw std::runtime_error("ctype array field " + toUTF8StdString(prop.name) + " expects list value");
        ObjList* list = asList(val);
        size_t len = list->length();
        for (size_t i = 0; i < arrayLen; ++i) {
            Value elem = (i < len) ? list->elts.at(i) : Value(0);
            if (!writeByNameVal(arrayBase, elem))
                throw std::runtime_error("unsupported ctype annotation: " + ctypeStr+" for field '"+toUTF8StdString(prop.name)+"'");
        }
        return;
    }

    if (isObjectType(prop.type) && !ctypeStr.empty() && ctypeStr.back()=='*') {
        void* p = nullptr;
        if (!val.isNil()) {
            if (!isObjectInstance(val))
                throw std::runtime_error("struct pointer field '"+toUTF8StdString(prop.name)+"' expects object instance of type "+toString(prop.type));
            if (!ctx)
                throw std::runtime_error("marshalProperty missing context for nested struct pointer");
            ObjectInstance* inst = asObjectInstance(val);
            ctx->buffers.push_back(objectToCStruct(inst, stringStore, ctx));
            ctx->instances.push_back(inst);
            ctx->primPtrTypes.push_back(PrimitivePtrType::None);
            p = ctx->buffers.back().data();
        }
        if (ctx && ctx->primPtrTypes.size() < ctx->buffers.size())
            ctx->primPtrTypes.push_back(PrimitivePtrType::None);
        appendPadded(&p, ptrSize, ptrSize);
        return;
    }

    if (isObjectType(prop.type) && !ctypeStr.empty() && ctypeStr.back() != '*') {
        if (!isObjectInstance(val))
            throw std::runtime_error("struct field '"+toUTF8StdString(prop.name)+"' expects object instance of type "+toString(prop.type));
        ObjectInstance* inst = asObjectInstance(val);
        ObjObjectType* t = asObjectType(inst->instanceType);
        if (!t->isCStruct)
            throw std::runtime_error("nested struct field not cstruct type");
        size_t startOffset = offset;
        size_t nestedAlign = 1;
        for (int32_t h : t->propertyOrder) {
            const auto& subProp = t->properties.at(h);
            auto it = inst->properties.find(subProp.name.hashCode());
            if (it == inst->properties.end())
                throw std::runtime_error("instance missing property in nested struct");
            try {
                marshalProperty(it->second, subProp, ptrSize, buffer, offset, nestedAlign, stringStore, ctx);
            } catch (const std::runtime_error& e) {
                throw std::runtime_error("Error marshalling nested struct property '" + toUTF8StdString(subProp.name) + "' of type " + toString(subProp.type) + ": " + e.what());
            }
        }
        size_t finalPad = (nestedAlign - ((offset - startOffset) % nestedAlign)) % nestedAlign;
        buffer.insert(buffer.end(), finalPad, 0);
        offset += finalPad;
        structAlign = std::max(structAlign, nestedAlign);
        return;
    }

    auto writeByName = [&](const std::string& ctype) -> bool { return writeByNameVal(ctype, val); };

    if (!ctypeStr.empty()) {
        if (!writeByName(ctypeStr))
            throw std::runtime_error("unsupported ctype annotation: " + ctypeStr + " for field '" + toUTF8StdString(prop.name) + "'");
        return;
    }

    if (isTypeSpec(prop.type)) {
        ObjTypeSpec* ts = asTypeSpec(prop.type);
        switch (ts->typeValue) {
            case ValueType::Bool: {
                uint8_t b = val.asBool();
                appendPadded(&b, sizeof(b), 1);
                break; }
            case ValueType::Byte: {
                uint8_t v = val.asByte();
                appendPadded(&v, sizeof(v), 1);
                break; }
            case ValueType::Int: {
                int32_t i = val.asInt();
                appendPadded(&i, sizeof(i), 4);
                break; }
            case ValueType::Real: {
                double d = val.asReal();
                appendPadded(&d, sizeof(d), 8);
                break; }
            case ValueType::Enum: {
                int32_t e = val.asEnum();
                appendPadded(&e, sizeof(e), 4);
                break; }
            default:
                throw std::runtime_error("unsupported struct property type");
        }
    } else {
        throw std::runtime_error("struct property has no builtin type");
    }
}

Value roxal::unmarshalProperty(const ObjObjectType::Property& prop, size_t ptrSize,
                               const uint8_t* bytes, size_t len,
                               size_t& offset, size_t& structAlign,
                               CStructContext* ctx)
{
    auto readPadded = [&](void* out, size_t size, size_t align) {
        size_t padding = (align - (offset % align)) % align;
        if (offset + padding + size > len)
            throw std::runtime_error("buffer too small for cstruct unmarshalling");
        offset += padding;
        memcpy(out, bytes + offset, size);
        offset += size;
        structAlign = std::max(structAlign, align);
    };

    Value val;
    std::string ctypeStr;
    if (prop.ctype.has_value())
        ctypeStr = toUTF8StdString(prop.ctype.value());

    bool isArray = false;
    std::string arrayBase;
    size_t arrayLen = 0;
    auto lb = ctypeStr.find('[');
    auto rb = ctypeStr.find(']', lb == std::string::npos ? 0 : lb);
    if (lb != std::string::npos && rb == ctypeStr.size() - 1) {
        arrayBase = ctypeStr.substr(0, lb);
        while(!arrayBase.empty() && isspace(arrayBase.back())) arrayBase.pop_back();
        std::string lenStr = ctypeStr.substr(lb+1, rb-lb-1);
        arrayLen = std::strtoul(lenStr.c_str(), nullptr, 10);
        if (arrayLen > 0)
            isArray = true;
    }

    if (isObjectType(prop.type) && !ctypeStr.empty() && ctypeStr.back()=='*') {
        void* p = nullptr;
        readPadded(&p, ptrSize, ptrSize);
        if (!p)
            return Value::nilVal();
        if (ctx) {
            for (size_t i = 0; i < ctx->buffers.size(); i++) {
                if (ctx->buffers[i].data() == p && ctx->instances[i]) {
                    updateObjectFromCStruct(ctx->instances[i], ctx->buffers[i].data(), ctx->buffers[i].size(), ctx);
                    return Value::objRef(ctx->instances[i]);
                }
            }
        }
        return Value::nilVal();
    }


    if (!ctypeStr.empty() && ctypeStr.back()=='*') {
        std::string base = ctypeStr.substr(0, ctypeStr.size()-1);
        while(!base.empty() && isspace(base.back())) base.pop_back();
        PrimitivePtrType ppt = PrimitivePtrType::None;
        if (base=="uint8_t") ppt = PrimitivePtrType::UInt8;
        else if (base=="int8_t") ppt = PrimitivePtrType::Int8;
        else if (base=="uint16_t") ppt = PrimitivePtrType::UInt16;
        else if (base=="int16_t") ppt = PrimitivePtrType::Int16;
        else if (base=="uint32_t") ppt = PrimitivePtrType::UInt32;
        else if (base=="int32_t" || base=="int") ppt = PrimitivePtrType::Int32;
        if (ppt != PrimitivePtrType::None) {
            void* p = nullptr;
            readPadded(&p, ptrSize, ptrSize);
            if (!p) {
                val = (ppt==PrimitivePtrType::UInt8||ppt==PrimitivePtrType::Int8)?Value::byteVal(0):Value::intVal(0);
                return val;
            }
            if (ctx) {
                for (size_t i=0;i<ctx->buffers.size();i++) {
                    if (ctx->buffers[i].data()==p && ctx->primPtrTypes[i]==ppt) {
                        const uint8_t* buf = ctx->buffers[i].data();
                        switch(ppt) {
                            case PrimitivePtrType::UInt8: val = Value::byteVal(buf[0]); break;
                            case PrimitivePtrType::Int8: val = Value::byteVal(uint8_t(*(int8_t*)buf)); break;
                            case PrimitivePtrType::UInt16: { uint16_t u; memcpy(&u,buf,2); val=Value::intVal(int32_t(u)); break; }
                            case PrimitivePtrType::Int16: { int16_t s; memcpy(&s,buf,2); val=Value::intVal(int32_t(s)); break; }
                            case PrimitivePtrType::UInt32: { uint32_t u; memcpy(&u,buf,4); val=Value::intVal(int32_t(u)); break; }
                            case PrimitivePtrType::Int32: { int32_t s; memcpy(&s,buf,4); val=Value::intVal(s); break; }
                            default: break;
                        }
                        return val;
                    }
                }
            }
            val = (ppt==PrimitivePtrType::UInt8||ppt==PrimitivePtrType::Int8)?Value::byteVal(0):Value::intVal(0);
            return val;
        }
    }
    auto readByNameVal = [&](const std::string& ctype, Value& out) -> bool {
        if (ctype == "float") { float f; readPadded(&f, sizeof(f), 4); out = Value(double(f)); return true; }
        if (ctype == "double" || ctype == "real") { double d; readPadded(&d, sizeof(d), 8); out = Value::realVal(d); return true; }
        if (ctype == "int" || ctype == "int32_t") { int32_t i; readPadded(&i, sizeof(i), 4); out = Value::intVal(i); return true; }
        if (ctype == "uint32_t") { uint32_t u; readPadded(&u, sizeof(u), 4); out = Value::intVal(int32_t(u)); return true; }
        if (ctype == "int16_t") { int16_t s; readPadded(&s, sizeof(s), 2); out = Value::intVal(int32_t(s)); return true; }
        if (ctype == "uint16_t") { uint16_t u; readPadded(&u, sizeof(u), 2); out = Value::intVal(int32_t(u)); return true; }
        if (ctype == "int8_t") { int8_t s; readPadded(&s, sizeof(s),1); out = Value::byteVal(uint8_t(s)); return true; }
        if (ctype == "uint8_t") { uint8_t u; readPadded(&u, sizeof(u),1); out = Value::byteVal(u); return true; }
        if (ctype == "bool") { uint8_t b; readPadded(&b, sizeof(b),1); out = Value::boolVal(b!=0); return true; }
        if (ctype == "char*") { const char* p; readPadded(&p, ptrSize, ptrSize); out = Value::stringVal(toUnicodeString(p?p:"")); return true; }
        if (ctype == "void*" || (!ctype.empty() && ctype.back()=='*')) { void* p; readPadded(&p, ptrSize, ptrSize); out = p ? Value::foreignPtrVal(p) : Value::nilVal(); return true; }
        return false;
    };

    if (isObjectType(prop.type) && !ctypeStr.empty() && ctypeStr.back() != '*') {
        ObjObjectType* t = asObjectType(prop.type);
        Value instVal { Value::objectInstanceVal(prop.type) };
        ObjectInstance* inst = asObjectInstance(instVal);
        size_t startOffset = offset;
        size_t nestedAlign = 1;
        for (int32_t h : t->propertyOrder) {
            const auto& subProp = t->properties.at(h);
            Value subVal = unmarshalProperty(subProp, ptrSize, bytes, len, offset, nestedAlign, ctx);
            inst->properties[subProp.name.hashCode()] = subVal;
        }
        size_t finalPad = (nestedAlign - ((offset - startOffset) % nestedAlign)) % nestedAlign;
        if (offset + finalPad > len)
            throw std::runtime_error("buffer too small for cstruct unmarshalling");
        offset += finalPad;
        structAlign = std::max(structAlign, nestedAlign);
        return instVal;
    }

    auto readByName = [&](const std::string& ctype) -> bool { return readByNameVal(ctype, val); };

    if (isArray) {
        std::vector<Value> elements;
        elements.reserve(arrayLen);
        for (size_t i = 0; i < arrayLen; ++i) {
            Value elem;
            if (!readByNameVal(arrayBase, elem))
                throw std::runtime_error("unsupported ctype annotation: " + ctypeStr);
            elements.push_back(elem);
        }
        val = Value::listVal(elements);
        return val;
    }

    if (!ctypeStr.empty()) {
        if (!readByName(ctypeStr))
            throw std::runtime_error("unsupported ctype annotation: " + ctypeStr);
    } else if (isTypeSpec(prop.type)) {
        ObjTypeSpec* ts = asTypeSpec(prop.type);
        switch (ts->typeValue) {
            case ValueType::Bool: { uint8_t b; readPadded(&b, sizeof(b), 1); val = Value::boolVal(b != 0); break; }
            case ValueType::Byte: { uint8_t v; readPadded(&v, sizeof(v), 1); val = Value::byteVal(v); break; }
            case ValueType::Int: { int32_t i; readPadded(&i, sizeof(i), 4); val = Value::intVal(i); break; }
            case ValueType::Real: { double d; readPadded(&d, sizeof(d), 8); val = Value::realVal(d); break; }
            case ValueType::Enum: { int32_t e; readPadded(&e, sizeof(e), 4); val = Value::intVal(e); break; }
            default:
                throw std::runtime_error("unsupported struct property type");
        }
    } else {
        throw std::runtime_error("struct property has no builtin type");
    }

    return val;
}

std::vector<uint8_t> roxal::objectToCStruct(ObjectInstance* instance, std::vector<std::string>* stringStore,
                                            CStructContext* ctx)
{
    if (!instance)
        throw std::invalid_argument("objectToCStruct null instance");

    ObjObjectType* type = asObjectType(instance->instanceType);
    if (!type->isCStruct)
        throw std::runtime_error("cannot convert non-cstruct type "+toUTF8StdString(type->name)+" to a cstruct");

    std::vector<uint8_t> buffer;
    size_t ptrSize = (type->cstructArch==64)?8:4;
    std::vector<std::string> localStrings;
    if (!stringStore) stringStore = &localStrings;
    size_t offset = 0;
    size_t structAlign = 1;

    for (int32_t hash : type->propertyOrder) {
        const auto& prop = type->properties.at(hash);
        auto it = instance->properties.find(prop.name.hashCode());
        if (it == instance->properties.end())
            throw std::runtime_error("instance missing property in objectToCStruct");
        try {
            marshalProperty(it->second, prop, ptrSize, buffer, offset, structAlign, stringStore, ctx);
        } catch (const std::exception& e) {
            throw std::runtime_error("Error marshalling type '"+toUTF8StdString(type->name)+"': " + e.what());
        }
    }

    size_t finalPad = (structAlign - (buffer.size() % structAlign)) % structAlign;
    buffer.insert(buffer.end(), finalPad, 0);

    return buffer;
}

Value roxal::objectFromCStruct(const Value& type, const void* data, size_t len)
{
    if (!isObjectType(type) || !data)
        throw std::invalid_argument("objectFromCStruct non-object type or null data");

    if (!asObjectType(type)->isCStruct)
        throw std::runtime_error("objectFromCStruct called on non-cstruct type");

    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);
    size_t offset = 0;
    size_t structAlign = 1;
    size_t ptrSize = (asObjectType(type)->cstructArch==64)?8:4;

    Value instVal = Value::objectInstanceVal(type);
    ObjectInstance* inst = asObjectInstance(instVal);

    ObjObjectType* objType = asObjectType(type);

    for (int32_t hash : objType->propertyOrder) {
        const auto& prop = objType->properties.at(hash);
        Value val = unmarshalProperty(prop, ptrSize, bytes, len, offset, structAlign, nullptr);
        inst->properties[prop.name.hashCode()] = val;
    }

    size_t finalPad = (structAlign - (offset % structAlign)) % structAlign;
    if (offset + finalPad > len)
        throw std::runtime_error("buffer too small for objectFromCStruct");

    return instVal;
}

void roxal::updateObjectFromCStruct(ObjectInstance* instance, const void* data, size_t len,
                                    CStructContext* ctx)
{
    if (!instance || !data)
        throw std::invalid_argument("updateObjectFromCStruct null instance or data");

    ObjObjectType* type = asObjectType(instance->instanceType);
    if (!type->isCStruct)
        throw std::runtime_error("updateObjectFromCStruct called on non-cstruct type");

    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);
    size_t offset = 0;
    size_t structAlign = 1;
    size_t ptrSize = (type->cstructArch==64)?8:4;

    for (int32_t hash : type->propertyOrder) {
        const auto& prop = type->properties.at(hash);
        Value val = unmarshalProperty(prop, ptrSize, bytes, len, offset, structAlign, ctx);
        instance->properties[prop.name.hashCode()] = val;
    }

    size_t finalPad = (structAlign - (offset % structAlign)) % structAlign;
    if (offset + finalPad > len)
        throw std::runtime_error("buffer too small for updateObjectFromCStruct");
}
