#pragma once

#include <unicode/ustring.h>

#include "common.h"
#include "Chunk.h"
#include "Value.h"


namespace roxal {

using icu::UnicodeString;


enum class ObjType {
    None = 0,
    Function,
    Native,
    String
};


struct Obj {
    Obj() : type(ObjType::None), refCount(0) {}
    virtual ~Obj() {}

    ObjType type;
    int32_t refCount;

    inline void incRef() { ++refCount; }
    inline void decRef() { 
        if (--refCount <= 0) 
            unrefedObjs.push_back(this);
    }

    static std::vector<Obj*> unrefedObjs;
};


// starts refCount at 1
inline Value objVal(Obj* o) { return Value(o); }


inline ObjType objType(const Value& v) { return v.asObj()->type; }
inline bool isObjType(const Value& v, ObjType type) 
    { return v.isObj() && v.asObj()->type == type; }


std::string objToString(const Value& v);

bool objsEqual(const Value& l, const Value& r);




//
// string

struct ObjString : public Obj 
{
    ObjString(const UnicodeString& us);
    virtual ~ObjString();

    UnicodeString s;
    int32_t hash;

    std::string toStdString() const 
      { std::string ss; s.toUTF8String(ss); return ss; }
};


inline bool isString(const Value& v) { return isObjType(v, ObjType::String); }
inline ObjString* asString(const Value& v) { return static_cast<ObjString*>(v.asObj()); }
inline UnicodeString asUString(const Value& v) { return asString(v)->s; }

// allocate new ObjString on heap and copy s (or return existing interned string)
ObjString* stringVal(const UnicodeString& s); 

std::string objStringToString(const ObjString* os);


//
// function

enum class FunctionType {
    Function,
    Module
};


struct ObjFunction : public Obj
{
    ObjFunction();

    int arity;
    ptr<Chunk> chunk;
    UnicodeString name;
};

inline bool isFunction(const Value& v) { return isObjType(v, ObjType::Function); }
inline ObjFunction* asFunction(const Value& v) { return static_cast<ObjFunction*>(v.asObj()); }


ObjFunction* functionVal();

std::string objFunctionToString(const ObjFunction* of);


//
// native function

typedef Value (*NativeFn)(int argCount, Value* args);

struct ObjNative : public Obj
{
    ObjNative(NativeFn function);

    NativeFn function;
};

inline bool isNative(const Value& v) { return isObjType(v, ObjType::Native); }
inline ObjNative* asNative(const Value& v) { return static_cast<ObjNative*>(v.asObj()); }

ObjNative* nativeVal(NativeFn function);



}


