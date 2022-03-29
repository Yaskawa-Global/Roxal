#include <stdexcept>
#include <unordered_map>

#include "Object.h"

using namespace roxal;


std::vector<Obj*> Obj::unrefedObjs {};

#ifdef DEBUG_TRACE_MEMORY
std::map<Obj*, const char*> Obj::allocatedObjs {};
#endif


#ifdef DEBUG_BUILD
// inline for release build

void Obj::incRef()
{
    if (addedToUnrefdObjs) {
        //!!!throw std::runtime_error("Can't incRef object already decRef()'d to 0");
        std::cout << "re-incRefing object already decRef()'d to 0:" << objTypeName(this) << std::endl;
        if (type == ObjType::String)
            std::cout << "  " << objStringToString(static_cast<ObjString*>(this)) << std::endl;//!!!
        auto it = std::find(unrefedObjs.begin(), unrefedObjs.end(), this);
        *it = nullptr;
        addedToUnrefdObjs = false;
        ++refCount;
    }
    ++refCount;
}

void Obj::decRef() 
{     
    if (--refCount <= 0) {
        if (std::find(unrefedObjs.cbegin(), unrefedObjs.cend(), this) != unrefedObjs.cend()) {
            if (this->type == ObjType::String)
                std::cout << "refCount=" << refCount << " obj=" << objStringToString(static_cast<ObjString*>(this)) << std::endl;//!!!
            ; //!!!throw std::runtime_error("Obj::decRef() - can't add object to unrefd list more than once. Obj:"+toString(this));
        }
        else {
            unrefedObjs.push_back(this);
            addedToUnrefdObjs = true;
        }
    }
}
#endif


// interned strings table
static std::unordered_map<int32_t, ObjString*> strings {};


ObjString::ObjString(const UnicodeString& us)
    :  s(us)
{
    type = ObjType::String;
    refCount = 0;
    hash = s.hashCode();

    // add ourself to the string intern table
    #ifdef DEBUG_BUILD
    if (strings.find(hash) != strings.end())
        throw std::runtime_error("Duplicate ObjString creation");
    #endif
    strings[hash] = this;
}


ObjString::~ObjString()
{
    // remove ourself from the strings intern table
    auto si = strings.find(hash);
    if (si != strings.end())
        strings.erase(si);
}



static uint32_t fnv1a32(const UnicodeString& s)
{
    // FNV-1a 32bit hash
    //  see https://en.wikipedia.org/wiki/Fowler%E2%80%93Noll%E2%80%93Vo_hash_function
    const char16_t* buf = s.getBuffer();
    uint32_t h = 2166136261u;
    for(int i=0; i<s.length();i++) {
        char16_t c = buf[i];
        h ^= uint8_t(buf[i] & 0xff);
        h *= 16777619;
        h ^= uint8_t((buf[i] >> 8) & 0xff);
        h *= 16777619;
    }
    return h;
}


ObjString* roxal::stringVal(const UnicodeString& s) 
{ 
    int32_t hash = s.hashCode();
    auto si = strings.find(hash);
    if (si == strings.end()) { // not found
        // create new
        return newObj<ObjString>(__func__,s); 
    }
    else { // found existing string
        return si->second;
    }
} 



std::string roxal::objFunctionToString(const ObjFunction* of)
{
    std::string s;
    of->name.toUTF8String(s);
    return "<func "+s+">";
}


std::string roxal::objStringToString(const ObjString* os)
{
    std::string s;
    static_cast<const ObjString*>(os)->s.toUTF8String(s);
    return s;
}


std::string roxal::objToString(const Value& v)
{
    switch(objType(v)) {
        case ObjType::Closure: {
            return objFunctionToString(asClosure(v)->function);
        }
        case ObjType::Upvalue: {
            return "upvalue";
        }
        case ObjType::Function: {
            return objFunctionToString(asFunction(v));
        }
        case ObjType::Native: {
            return "<native fn>";
        }
        case ObjType::String: {
            return toUTF8StdString(asUString(v));
        }
        case ObjType::ObjectType: {
            ObjObjectType* obj = asObjectType(v);
            return std::string("<type ")+(obj->isActor ? "actor" :"object")+" "+toUTF8StdString(obj->name)+">";
        }
        case ObjType::Instance: {
            ObjInstance* inst = asInstance(v);
            return std::string(inst->instanceType->isActor ? "actor" : "object")+" "+toUTF8StdString(inst->instanceType->name);
        }
        case ObjType::BoundMethod: {
            return objFunctionToString(asBoundMethod(v)->method->function);
        }
        default: ;
    }
    return "";
}




ObjFunction::ObjFunction()
    : arity(0), upvalueCount(0), name()
{
    type = ObjType::Function;
    chunk = std::make_shared<Chunk>();
}






ObjNative::ObjNative(NativeFn _function)
    : function(_function)
{
    type = ObjType::Native;
}


ObjNative* roxal::nativeVal(NativeFn function)
{
    return newObj<ObjNative>(__func__,function);
}



ObjObjectType* roxal::objectTypeVal(const icu::UnicodeString& typeName, bool isActor)
{
    return newObj<ObjObjectType>(__func__, typeName, isActor);
}


ObjInstance::ObjInstance(ObjObjectType* objectType) 
{ 
    type = ObjType::Instance; 
    instanceType = objectType;
    instanceType->incRef();
}

ObjInstance::~ObjInstance() 
{
    instanceType->decRef();
}




ObjInstance* roxal::objInstanceVal(ObjObjectType* objectType)
{
    return newObj<ObjInstance>(__func__, objectType);
}




ObjBoundMethod::ObjBoundMethod(const Value& instance, ObjClosure* closure)
    : receiver(instance), method(closure)
{
    type = ObjType::BoundMethod;
    method->incRef();
}

ObjBoundMethod::~ObjBoundMethod() 
{
    method->decRef();
}






bool roxal::objsEqual(const Value& l, const Value& r)
{
    if (!l.isObj() || !r.isObj())
        return false;

    if (objType(l) != objType(r))
        return false;

    switch (objType(l)) {
        case ObjType::String: {
            auto ls = asString(l);
            auto rs = asString(r);
            if (ls == rs) // identical object
                return true;            

            // Trust hash.  Possible different strings with has collision will
            //  compare as equal (low probability)
            // TODO: consider doing full char comparison for equality if hashes match
            return asString(l)->hash == asString(l)->hash;
            // if (ls->s.length() != rs->s.length())
            //     return false;
            // return ls->s == rs->s;
        } break;
        default: 
            throw std::runtime_error("Unimplemented object type for objEqual:"+std::to_string(int(objType(l))));

    }
    return false;
}


std::string roxal::objTypeName(const Obj* obj)
{
    if (obj == nullptr) return "null";

    switch (obj->type) {
    case ObjType::None: return "none";
    case ObjType::ObjectType: return static_cast<const ObjObjectType*>(obj)->isActor ? "type actor" : "type object";
    case ObjType::Instance: {
        const ObjInstance* inst = static_cast<const ObjInstance*>(obj);
        return inst->instanceType->isActor ? "actor":"object";
    }
    case ObjType::BoundMethod: return "function";
    case ObjType::Closure: return "closure";
    case ObjType::Function: return "function";
    case ObjType::Native: return "native";
    case ObjType::Upvalue: return "upvalue";
    case ObjType::Bool: return "bool";
    case ObjType::Int: return "int";
    case ObjType::Real: return "real";
    case ObjType::String: return "string";
    }
    return "unknown";
}
