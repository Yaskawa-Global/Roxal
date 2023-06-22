#pragma once

#include <functional>
#include <map>
#include <vector>
#include <sstream>
#include <unordered_map>
#include <atomic>
#include <future>
#include <condition_variable>
#include <unicode/ustring.h>

#include <core/common.h>
#include <core/atomic.h>
#include "Chunk.h"
#include "Value.h"


// forward decls
namespace roxal::type {
    struct Type;
}
namespace roxal::ast {
    struct Annotation;
}


namespace roxal {

using icu::UnicodeString;


enum class ObjType {
    None = 0,
    BoundMethod,
    Closure,
    Function,
    Instance,
    Actor,
    Native,
    Upvalue,
    Future,
    Bool,
    Int,
    Real,
    String,
    Range,
    Type,
    List,
    Dict,
    Stream
};


struct Obj {
    Obj() : type(ObjType::None), refCount(0) {}
    virtual ~Obj() {}

    ObjType type;
    std::atomic_int32_t refCount;


    ValueType valueType() const;

    Obj* clone() const; // deep copy

    inline void incRef() 
    {
        auto prevCount = refCount.fetch_add(1,std::memory_order_relaxed); 
        if (prevCount==0 && type==ObjType::Stream)
            registerStream();         
    }

    inline void decRef() 
    { 
        auto prevCount = refCount.fetch_sub(1,std::memory_order_relaxed);
        if (prevCount <= 1) {
            if (type==ObjType::Stream)
                unregisterStream();
            unrefedObjs.push_back(this);
        }
    }

    void registerStream();
    void unregisterStream();

    static atomic_vector<Obj*> unrefedObjs;

    #ifdef DEBUG_TRACE_MEMORY
    static atomic_map<Obj*, const char*> allocatedObjs;
    #endif
};


template<typename T, typename... Args>
inline T* newObj(const char* comment, Args&&... args) {
    #ifdef DEBUG_TRACE_MEMORY
    T* o = new T(std::forward<Args>(args)...);
    if (Obj::allocatedObjs.containsKey(o))
        throw std::runtime_error("new Obj* yielded address already allocated: "+toString(o));
    Obj::allocatedObjs.store(o, comment);
    return o;
    #else
    return new T(std::forward<Args>(args)...);
    #endif
}

template<typename T>
inline void delObj(T* o) {
    #ifdef DEBUG_TRACE_MEMORY
    if (!Obj::allocatedObjs.containsKey(o))
        throw std::runtime_error("delete for unallocated Obj* "+toString(o)+" :"+objTypeName(o));
    Obj::allocatedObjs.erase(o);
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
// Boxed built-in primitives (bool, byte, int, real, type)

struct ObjPrimitive : public Obj 
{
    ObjPrimitive(bool b) { type=ObjType::Bool; as.boolean = b; }
    ObjPrimitive(double r) { type=ObjType::Real; as.real = r; }
    ObjPrimitive(int32_t i) { type=ObjType::Int; as.integer = i; }
    ObjPrimitive(ValueType bt) { type=ObjType::Type; as.btype = bt; }

    ValueType valueType() const {
        switch (type) {
            case ObjType::Bool: return ValueType::Bool;
            case ObjType::Int: return ValueType::Int;
            case ObjType::Real: return ValueType::Real;
            case ObjType::Type: return ValueType::Type;
            default: 
            #ifdef DEBUG_BUILD
              throw std::runtime_error("Unsupported ObjPrimitive Type "+std::to_string(int(type)));
            #else
              return ValueType::Nil;
            #endif
        }
    }

    bool isBool() const { return type==ObjType::Bool; }
    bool isInt() const { return type==ObjType::Int; }
    bool isReal() const { return type==ObjType::Real; }
    bool isType() const { return type==ObjType::Type; }

    union {
        bool boolean;
        double real;
        int32_t integer;
        ValueType btype;
    } as;
};

inline bool isObjPrimitive(const Value& v) { return isObjType(v, ObjType::Bool) || isObjType(v, ObjType::Int) || isObjType(v, ObjType::Real) || isObjType(v,ObjType::Type); }
inline ObjPrimitive* asObjPrimitive(const Value& v) { return static_cast<ObjPrimitive*>(v.asObj()); }

inline ObjPrimitive* cloneObjPrimitive(const ObjPrimitive* op) {    
    if (op->type == ObjType::Bool)
        return newObj<ObjPrimitive>(__func__,op->as.boolean);
    else if (op->type == ObjType::Int)
        return newObj<ObjPrimitive>(__func__,op->as.integer);
    else if (op->type == ObjType::Real)
        return newObj<ObjPrimitive>(__func__,op->as.real);
    else if (op->type == ObjType::Type)
        return newObj<ObjPrimitive>(__func__,op->as.btype);
    #ifdef DEBUG_BUILD
    throw std::runtime_error("Unsupported ObjPrimitive Type "+std::to_string(int(op->type)));
    #endif
    return newObj<ObjPrimitive>(__func__,false);
}




//
// string

struct ObjString : public Obj 
{
    ObjString(const UnicodeString& us);
    virtual ~ObjString();

    UnicodeString s;
    int32_t hash;

    // number of 16bit Unicode code units
    int32_t length() const { return s.length(); }

    // Elements are Unicode code units (not code points or characters)
    Value index(const Value& i) const;

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
// range

struct ObjRange : public Obj 
{
    ObjRange(); // empty range
    ObjRange(const Value& start, const Value& stop, const Value& step, bool closed);
    virtual ~ObjRange() {}

    Value start;
    Value stop;
    Value step;
    bool closed;

    // length can depend on target length due to use of -ve offsets from end
    int32_t length(int32_t targetLen) const;

    // valid only if start & stop are positive (and stop must be supplied, -1 otherwise)
    int32_t length() const;

    // map index in range to values in range
    //  if targetLenb specified (not -1) it then
    //  range is interpreted as over a target of that length, with
    //   special case of start,stop being negative being interpreted
    //   as counting back the end of the target
    //  e.g. if target is list 3 elemnent list and range is [1:] then
    //   targetIndex(1) = 2
    int32_t targetIndex(int32_t index, int32_t targetLen=-1) const;

};


inline bool isRange(const Value& v) { return isObjType(v, ObjType::Range); }
inline ObjRange* asRange(const Value& v) { return static_cast<ObjRange*>(v.asObj()); }

ObjRange* rangeVal(); // empty range
ObjRange* rangeVal(const Value& start, const Value& stop, const Value& step, bool closed); 

std::string objRangeToString(const ObjRange* r);
ObjRange* cloneRange(const ObjRange* r); // deep copy





//
// list

struct ObjList : public Obj
{
    ObjList() { type = ObjType::List; }
    ObjList(const ObjRange* r);
    virtual ~ObjList() {}

    int32_t length() const { return elts.size(); }

    Value index(const Value& i) const;
    void setIndex(const Value& i, const Value& v);

    atomic_vector<Value> elts;
};


inline bool isList(const Value& v) { return isObjType(v, ObjType::List); }
inline ObjList* asList(const Value& v) { return static_cast<ObjList*>(v.asObj()); }

ObjList* listVal(); 
ObjList* listVal(const ObjRange* r); 
ObjList* listVal(const std::vector<Value>& elts); 
ObjList* cloneList(const ObjList* l); // deep copy

std::string objListToString(const ObjList* ol);



//
// dict

struct ObjDict : public Obj
{
    ObjDict() { type = ObjType::Dict; }
    virtual ~ObjDict() {}

    int32_t length() const {
        std::lock_guard<std::mutex> lock(m);
        return m_keys.size();
    }

    bool contains(const Value& key) const {
        std::lock_guard<std::mutex> lock(m);
        return (entries.find(key) != entries.end());
    }

    Value at(const Value& key) const {
        std::lock_guard<std::mutex> lock(m);
        auto it = entries.find(key);
        if (it != entries.end())
            return it->second;
        return nilVal();
    }

    std::vector<Value> keys() const {
        std::lock_guard<std::mutex> lock(m);
        return m_keys;
    }

    void store(const Value& key, const Value& val) {
        std::lock_guard<std::mutex> lock(m);
        if (entries.find(key) == entries.end()) // key exists?
            m_keys.push_back(key); // no, add to keys list
        entries[key] = val; // insert or replace 
    }

    struct ValueComparitor
    {
        using is_transparent = std::true_type;

        // standard comparison (between two instances of Type)
        bool operator()(const Value& lhs, const Value& rhs) const { return less(lhs, rhs).asBool(); }
    };

private:
    mutable std::mutex m;
    std::vector<Value> m_keys;
    // TODO: transition to unordered_map by defining Value hash
    std::map<Value,Value,ValueComparitor> entries;
};


inline bool isDict(const Value& v) { return isObjType(v, ObjType::Dict); }
inline ObjDict* asDict(const Value& v) { return static_cast<ObjDict*>(v.asObj()); }

ObjDict* dictVal(); 
ObjDict* dictVal(const std::vector<std::pair<Value,Value>>& entries); 
ObjDict* cloneDict(const ObjDict* d);

std::string objDictToString(const ObjDict* od);



//
// function

enum class FunctionType {
    Function,
    Method,
    Initializer,
    Module
};

std::string toString(FunctionType ft);


struct ObjFunction : public Obj
{
    ObjFunction();
    virtual ~ObjFunction() {}

    UnicodeString name;
    std::optional<ptr<roxal::type::Type>> funcType;
    int arity;
    int upvalueCount;
    ptr<Chunk> chunk;
    std::vector<ptr<ast::Annotation>> annotations;

    // for parameters with default values that must be re-evaluated on each call
    //  this is map from param name UnicodeString::hashCode() -> ObjFunction
    //  (where ObjFunction is a function that takes no params and returns the default value)
    std::map<int32_t, ObjFunction*> paramDefaultFunc;
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

inline ObjUpvalue* cloneUpvalue(const ObjUpvalue* u) {
    ObjUpvalue* newup = newObj<ObjUpvalue>(__func__,u->location);
    // clone value (this Upvalue is now closed)
    newup->closed = newup->location->clone();
    newup->location = &newup->closed;
    return newup;
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

inline ObjClosure* cloneClosure(const ObjClosure* c) {
    ObjClosure* newc = newObj<ObjClosure>(__func__,c->function);
    newc->upvalues.resize(c->upvalues.size());
    for(auto i=0; i<c->upvalues.size();i++)
        newc->upvalues[i] = cloneUpvalue(c->upvalues.at(i));
    return newc;
}



// future

struct ObjFuture : public Obj
{
    ObjFuture(const std::shared_future<Value>& fv) 
        : future(fv)
    {
        type = ObjType::Future;
    }
    virtual ~ObjFuture() {}

    Value asValue() { return future.valid() ? future.get() : nilVal(); }

    std::shared_future<Value> future;
};

inline bool isFuture(const Value& v) { return isObjType(v, ObjType::Future); }
inline ObjFuture* asFuture(const Value& v) {
    #ifdef DEBUG_BUILD
    if (!isFuture(v))
        throw std::runtime_error("Value is not an ObjFuture");
    #endif
    return static_cast<ObjFuture*>(v.asObj());
}

inline ObjFuture* futureVal(const std::shared_future<Value>& fv) {
    return newObj<ObjFuture>(__func__, fv);
}






//
// native function

//typedef Value (*NativeFn)(int argCount, Value* args);
//typedef std::function<Value(int argCount, Value* args)> NativeFn;
class VM;
typedef Value (VM::*NativeFn)(int,Value*);

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
// runtime type 

//FIXME!!!: collision exists for Obj::type == ObjType::Type - it is used
//       both by ObjTypeSpec and by ObjPrimitive for builtin type
struct ObjTypeSpec : public Obj
{
    ObjTypeSpec() {
        type = ObjType::Type;
        refCount = 0;
        typeValue = ValueType::Nil;
    }
    virtual ~ObjTypeSpec() {}

    ValueType typeValue;
};

inline bool isTypeSpec(const Value& v) { return isObjType(v,ObjType::Type); }
inline ObjTypeSpec* asTypeSpec(const Value& v) { return static_cast<ObjTypeSpec*>(v.asObj()); }

ObjTypeSpec* typeSpecVal(ValueType t); // primitive

std::string objTypeSpecToString(const ObjTypeSpec* ots);


//
// object|actor type

struct ObjObjectType : public ObjTypeSpec
{
    ObjObjectType(const icu::UnicodeString& typeName, bool isactor) 
        : name(typeName), isActor(isactor) 
    { 
        typeValue = isactor ? ValueType::Actor : ValueType::Object;
    }
    virtual ~ObjObjectType() {}

    icu::UnicodeString name;
    bool isActor;

    // name -> type, initial value
    std::unordered_map<int32_t, std::tuple<icu::UnicodeString, Value, Value>> properties;

    // name -> closure
    std::unordered_map<int32_t, std::pair<icu::UnicodeString, Value>> methods;
};


inline bool isObjectType(const Value& v) { return isObjType(v, ObjType::Type) && ((asTypeSpec(v)->typeValue == ValueType::Object) || (asTypeSpec(v)->typeValue == ValueType::Actor)); }
inline ObjObjectType* asObjectType(const Value& v) { return static_cast<ObjObjectType*>(v.asObj()); }


ObjObjectType* objectTypeVal(const icu::UnicodeString& typeName, bool isActor);


//
// object instance

struct ObjectInstance : public Obj
{
    ObjectInstance(ObjObjectType* objectType);
    virtual ~ObjectInstance();

    ObjObjectType* instanceType;
    std::unordered_map<int32_t, Value> properties;
};

inline bool isObjectInstance(const Value& v) { return isObjType(v, ObjType::Instance); }
inline ObjectInstance* asObjectInstance(const Value& v) { return static_cast<ObjectInstance*>(v.asObj()); }

ObjectInstance* objectInstanceVal(ObjObjectType* objectType);
ObjectInstance* cloneObjectInstance(const ObjectInstance* obj); // deep copy


//
// actor instance

struct ActorInstance : public Obj
{
    ActorInstance(ObjObjectType* objectType);
    virtual ~ActorInstance();

    ObjObjectType* instanceType;
    std::unordered_map<int32_t, Value> properties;

    // returns Value of ObjFuture or nil
    Value queueCall(const Value& callee, const CallSpec& callSpec, Value* argsStackTop);


    // queue of actor method calls
    //  each is a callee and parameters
    // (serviced in thread created by Thread::act() )
    struct MethodCallInfo {
        Value callee;
        std::vector<Value> args;
        ptr<std::promise<Value>> returnPromise; 
        CallSpec callSpec;

        bool valid() const { return !callee.isNil(); }
    };
    atomic_queue<MethodCallInfo> callQueue;

    std::mutex queueMutex;
    std::condition_variable queueConditionVar;

    std::thread::id thread_id;
};

inline bool isActorInstance(const Value& v) { return isObjType(v, ObjType::Actor); }
inline ActorInstance* asActorInstance(const Value& v) { return static_cast<ActorInstance*>(v.asObj()); }

ActorInstance* actorInstanceVal(ObjObjectType* objectType);



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

inline ObjBoundMethod* cloneBoundMethod(const ObjBoundMethod* bm) {
    auto newmb = newObj<ObjBoundMethod>(__func__,bm->receiver, bm->method);
    newmb->receiver = newmb->receiver.clone();
    return newmb;
}






#ifdef DEBUG_BUILD
void testObjectValues();
#endif



} // namespace





