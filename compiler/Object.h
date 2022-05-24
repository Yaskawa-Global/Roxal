#pragma once

#include <map>
#include <sstream>
#include <unordered_map>
#include <unicode/ustring.h>

#include <core/common.h>
#include "Chunk.h"
#include "Value.h"


namespace roxal {

using icu::UnicodeString;


enum class ObjType {
    None = 0,
    BoundMethod,
    ObjectType,
    Closure,
    Function,
    Instance,
    Native,
    Upvalue,
    Bool,
    Int,
    Real,
    String
};


struct Obj {
    Obj() : type(ObjType::None), refCount(0) {}
    virtual ~Obj() {}

    ObjType type;
    int32_t refCount;

    inline void incRef() { ++refCount; }
    inline void decRef() { 
        if (--refCount <= 0) {
            unrefedObjs.push_back(this);
        }
    }

    static std::vector<Obj*> unrefedObjs;

    #ifdef DEBUG_TRACE_MEMORY
    static std::map<Obj*, const char*> allocatedObjs;
    #endif
};


template<typename T, typename... Args>
inline T* newObj(const char* comment, Args&&... args) {
    #ifdef DEBUG_TRACE_MEMORY
    T* o = new T(std::forward<Args>(args)...);
    if (Obj::allocatedObjs.find(o) != Obj::allocatedObjs.end())
        throw std::runtime_error("new Obj* yielded address already allocated: "+toString(o));
    Obj::allocatedObjs[o] = comment;
    return o;
    #else
    return new T(std::forward<Args>(args)...);
    #endif
}

template<typename T>
inline void delObj(T* o) {
    #ifdef DEBUG_TRACE_MEMORY
    auto it = Obj::allocatedObjs.find(o);
    if (it == Obj::allocatedObjs.end())
        throw std::runtime_error("delete for unallocated Obj* "+toString(o)+" :"+objTypeName(o));
    Obj::allocatedObjs.erase(it);
    #endif
    delete o;
}

inline std::ostream& operator<<(std::ostream& out, const Obj* obj)
{
    out << std::hex << uint64_t(obj) << std::dec;
    return out;
}

inline std::string toString(Obj* obj) 
{
    std::stringstream ss;
    ss << obj;
    return ss.str();
}


// starts refCount at 1
inline Value objVal(Obj* o) { return Value(o); }


inline ObjType objType(const Value& v) { return v.asObj()->type; }
inline bool isObjType(const Value& v, ObjType type) 
    { return v.isObj() && v.asObj()->type == type; }


std::string objToString(const Value& v);

bool objsEqual(const Value& l, const Value& r);
std::string objTypeName(Obj* obj);


// 
// Boxed built-in primitives (bool, byte, int, real)

struct ObjPrimitive : public Obj 
{
    ObjPrimitive(bool b) { type=ObjType::Bool; as.boolean = b; }
    ObjPrimitive(double r) { type=ObjType::Real; as.real = r; }
    ObjPrimitive(int32_t i) { type=ObjType::Int; as.integer = i; }

    union {
        bool boolean;
        double real;
        int32_t integer;
    } as;
};

inline bool isPrimitive(const Value& v) { return isObjType(v, ObjType::Bool) || isObjType(v, ObjType::Int) || isObjType(v, ObjType::Real); }
inline ObjPrimitive* asPrimitive(const Value& v) { return static_cast<ObjPrimitive*>(v.asObj()); }




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
    Method,
    Initializer,
    Module
};


struct ObjFunction : public Obj
{
    ObjFunction();
    virtual ~ObjFunction() {}

    int arity;
    int upvalueCount;
    ptr<Chunk> chunk;
    UnicodeString name;
};

inline bool isFunction(const Value& v) { return isObjType(v, ObjType::Function); }
inline ObjFunction* asFunction(const Value& v) { 
    #ifdef DEBUG_BUILD
    if (!isFunction(v))
        throw std::runtime_error("Value is not an ObjFunction");
    #endif
    return static_cast<ObjFunction*>(v.asObj()); 
}


inline ObjFunction* functionVal() {
    return newObj<ObjFunction>(__func__);
}

std::string objFunctionToString(const ObjFunction* of);


//
// Upvalue

struct ObjUpvalue : public Obj {
    ObjUpvalue(Value* v) 
    {
        type = ObjType::Upvalue;
        location = v;
    }
    virtual ~ObjUpvalue() {}

    Value* location;
    Value closed;
};

inline bool isUpvalue(const Value& v) { return isObjType(v, ObjType::Upvalue); }
inline ObjUpvalue* asUpvalue(const Value& v) { return static_cast<ObjUpvalue*>(v.asObj()); }

inline ObjUpvalue* upvalueVal(Value* v) {
    return newObj<ObjUpvalue>(__func__,v);
}



//
// Closure

struct ObjClosure : public Obj
{
    ObjClosure(ObjFunction* f) : function(f) { 
        function->incRef(); 

        type = ObjType::Closure; 
        upvalues.resize(function->upvalueCount, nullptr);
    }
    virtual ~ObjClosure() {
        for(size_t i=0; i<upvalues.size();i++)
            if (upvalues[i] != nullptr) 
                upvalues[i]->decRef();
            
        function->decRef();
    }

    ObjFunction* function;
    std::vector<ObjUpvalue*> upvalues;
};

inline bool isClosure(const Value& v) { return isObjType(v, ObjType::Closure); }
inline ObjClosure* asClosure(const Value& v) { 
    #ifdef DEBUG_BUILD
    if (!isClosure(v))
        throw std::runtime_error("Value is not an ObjClosure");
    #endif
    return static_cast<ObjClosure*>(v.asObj()); 
}

inline ObjClosure* closureVal(ObjFunction* function) {
    return newObj<ObjClosure>(__func__,function);
}





//
// native function

typedef Value (*NativeFn)(int argCount, Value* args);

struct ObjNative : public Obj
{
    ObjNative(NativeFn function);
    virtual ~ObjNative() {}

    NativeFn function;
};

inline bool isNative(const Value& v) { return isObjType(v, ObjType::Native); }
inline ObjNative* asNative(const Value& v) { return static_cast<ObjNative*>(v.asObj()); }

ObjNative* nativeVal(NativeFn function);




//
// object|actor type

struct ObjObjectType : public Obj
{
    ObjObjectType(const icu::UnicodeString& typeName, bool isactor) 
        : name(typeName), isActor(isactor) 
    { 
        type = ObjType::ObjectType; 
    }
    virtual ~ObjObjectType() {}

    icu::UnicodeString name;
    bool isActor;

    std::unordered_map<int32_t, std::pair<icu::UnicodeString, Value>> methods;

};


inline bool isObjectType(const Value& v) { return isObjType(v, ObjType::ObjectType); }
inline ObjObjectType* asObjectType(const Value& v) { return static_cast<ObjObjectType*>(v.asObj()); }


ObjObjectType* objectTypeVal(const icu::UnicodeString& typeName, bool isActor);


//
// object|actor instance

struct ObjInstance : public Obj
{
    ObjInstance(ObjObjectType* objectType);
    virtual ~ObjInstance();

    ObjObjectType* instanceType;
    std::unordered_map<int32_t, Value> properties;
};

inline bool isInstance(const Value& v) { return isObjType(v, ObjType::Instance); }
inline ObjInstance* asInstance(const Value& v) { return static_cast<ObjInstance*>(v.asObj()); }

ObjInstance* objInstanceVal(ObjObjectType* objectType);



//
// method closure bound to object|actor instance

struct ObjBoundMethod : public Obj
{
    ObjBoundMethod(const Value& instance, ObjClosure* closure);
    virtual ~ObjBoundMethod();

    Value receiver;
    ObjClosure* method;
};

inline bool isBoundMethod(const Value& v) { return isObjType(v, ObjType::BoundMethod); }
inline ObjBoundMethod* asBoundMethod(const Value& v) { return static_cast<ObjBoundMethod*>(v.asObj()); }

inline ObjBoundMethod* boundMethodVal(const Value& instance, ObjClosure* closure) {
    return newObj<ObjBoundMethod>(__func__,instance, closure);
}




} // namespace





