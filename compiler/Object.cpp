#include <stdexcept>
#include <unordered_map>

#include "Object.h"

using namespace roxal;


std::vector<Obj*> Obj::unrefedObjs {};


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
        return new ObjString(s); 
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
        case ObjType::Function: {
            return objFunctionToString(asFunction(v));
        }
        case ObjType::Native: {
            return "<native fn>";
        }
        case ObjType::String: {
            std::string s;
            asUString(v).toUTF8String(s);
            return s;
        }
        default: ;
    }
    return "";
}




ObjFunction::ObjFunction()
    : arity(0), name()
{
    type = ObjType::Function;
    chunk = std::make_shared<Chunk>();
}



ObjFunction* roxal::functionVal()
{
    return new ObjFunction();
}



ObjNative::ObjNative(NativeFn _function)
    : function(_function)
{
    type = ObjType::Native;
}


ObjNative* roxal::nativeVal(NativeFn function)
{
    return new ObjNative(function);
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
