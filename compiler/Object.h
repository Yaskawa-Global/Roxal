#pragma once

#include <functional>
#include <map>
#include <vector>
#include <sstream>
#include <atomic>
#include <future>
#include <condition_variable>
#include <memory>
#include <unicode/ustring.h>

#include <core/common.h>
#include <core/AST.h>
#include <core/atomic.h>
#include <core/types.h>
#include "Chunk.h"
#include "Value.h"
#include <Eigen/Dense>


// forward decls
namespace roxal::type {
    struct Type;
}
namespace roxal::ast {
    struct Annotation;
}

namespace df { class Signal; }


namespace roxal {
struct ObjObjectType; // forward
class Thread; // forward declaration for handler threads
struct ObjEvent; // forward
}


namespace roxal {

using icu::UnicodeString;


enum class ObjType {
    None = 0,
    BoundMethod,
    BoundNative,
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
    Vector,
    Matrix,
    Signal,
    Library,
    Event
};






#include "ObjControl.h"

struct Obj {
    Obj() : type(ObjType::None), control(nullptr) {}
    virtual ~Obj() {}

    ObjType type;
    ObjControl* control;


    ValueType valueType() const;

    Obj* clone() const; // deep copy

    inline void incRef()
    {
        control->strong.fetch_add(1,std::memory_order_relaxed);
    }

    inline void decRef()
    {
        auto prevCount = control->strong.fetch_sub(1,std::memory_order_relaxed);
        if (prevCount <= 1) {
            control->obj = nullptr;
            unrefedObjs.push_back(this);
        }
    }

    inline void incWeak()
    {
        control->weak.fetch_add(1,std::memory_order_relaxed);
    }

    inline void decWeak()
    {
        if (control->weak.fetch_sub(1,std::memory_order_relaxed) == 1)
            delete[] reinterpret_cast<char*>(control);
    }


    static atomic_vector<Obj*> unrefedObjs;

    #ifdef DEBUG_TRACE_MEMORY
    static atomic_map<Obj*, const char*> allocatedObjs;
    #endif
};


template<typename T, typename... Args>
inline T* newObj(const char* comment, Args&&... args) {
    // allocate one contiguous block for control and object
    char* mem = new char[sizeof(ObjControl) + sizeof(T)];
    ObjControl* ctrl = new (mem) ObjControl();
    T* o = new (mem + sizeof(ObjControl)) T(std::forward<Args>(args)...);

    ctrl->strong = 0;
    ctrl->weak   = 1;   // implicit weak ref representing strong refs
    ctrl->obj    = o;
    o->control   = ctrl;

    #ifdef DEBUG_TRACE_MEMORY
    if (Obj::allocatedObjs.containsKey(o))
        throw std::runtime_error("new Obj* yielded address already allocated: " + toString(o));
    Obj::allocatedObjs.store(o, comment);
    #endif

    return o;
}

template<typename T>
inline void delObj(T* o) {
    #ifdef DEBUG_TRACE_MEMORY
    if (!Obj::allocatedObjs.containsKey(o))
        throw std::runtime_error("delete for unallocated Obj* "+toString(o)+" :"+objTypeName(o));
    Obj::allocatedObjs.erase(o);
    #endif
    ObjControl* ctrl = o->control;
    o->~T();
    if (ctrl->weak.fetch_sub(1, std::memory_order_relaxed) == 1)
        delete[] reinterpret_cast<char*>(ctrl);
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


// create Value from Obj (increments strong ref)
inline Value objVal(Obj* o) { return Value(o); }


inline ObjType objType(const Value& v) { return v.asObj() ? v.asObj()->type : ObjType::None; }
inline bool isObjType(const Value& v, ObjType type)
{
    if (!v.isObj()) return false;
    Obj* o = v.asObj();
    return o != nullptr && o->type == type;
}


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

    // List operations (in-place)
    void concatenate(const ObjList* other);  // Concatenate other list to this list
    void append(const Value& value);         // Append value to this list

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

    std::vector<std::pair<Value,Value>> items() const {
        std::lock_guard<std::mutex> lock(m);
        std::vector<std::pair<Value,Value>> keyvalues {};
        // can't just iterate over the entries directly, as we want to preserve order according to m_keys
        for(auto it=m_keys.cbegin(); it!=m_keys.cend(); it++)
            keyvalues.push_back(std::pair<Value,Value>(*it,entries.at(*it)));
        return keyvalues;
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
    // TODO: transition unordered map (since m_keys provides ordering) - Value hash?
    std::map<Value,Value,ValueComparitor> entries;
};


inline bool isDict(const Value& v) { return isObjType(v, ObjType::Dict); }
inline ObjDict* asDict(const Value& v) { return static_cast<ObjDict*>(v.asObj()); }

ObjDict* dictVal();
ObjDict* dictVal(const std::vector<std::pair<Value,Value>>& entries);
ObjDict* cloneDict(const ObjDict* d);

std::string objDictToString(const ObjDict* od);



//
// vector

struct ObjVector : public Obj
{
    ObjVector() { type = ObjType::Vector; }
    ObjVector(const Eigen::VectorXd& values);
    ObjVector(int32_t size);
    virtual ~ObjVector() {}

    int32_t length() const { return vec.size(); }

    Value index(const Value& i) const;
    void setIndex(const Value& i, const Value& v);

    bool equals(const ObjVector* other, double eps = 1e-15) const;

    Eigen::VectorXd vec;
};

inline bool isVector(const Value& v) { return isObjType(v, ObjType::Vector); }
inline ObjVector* asVector(const Value& v) { return static_cast<ObjVector*>(v.asObj()); }

ObjVector* vectorVal();
ObjVector* vectorVal(int32_t size);
ObjVector* vectorVal(const Eigen::VectorXd& values);
ObjVector* cloneVector(const ObjVector* v);

std::string objVectorToString(const ObjVector* ov);


//
// matrix

struct ObjMatrix : public Obj
{
    ObjMatrix() { type = ObjType::Matrix; }
    ObjMatrix(const Eigen::MatrixXd& values);
    ObjMatrix(int32_t rows, int32_t cols);
    virtual ~ObjMatrix() {}

    int32_t rows() const { return mat.rows(); }
    int32_t cols() const { return mat.cols(); }

    // single index returns a row (or range of rows)
    Value index(const Value& row) const;
    // two indices return an element, vector or submatrix depending on
    // whether row and/or col are ranges
    Value index(const Value& row, const Value& col) const;

    // assign to row(s) (or submatrix if range supplied)
    void setIndex(const Value& row, const Value& value);
    // assign to element, row/column vector or submatrix
    void setIndex(const Value& row, const Value& col, const Value& value);

    bool equals(const ObjMatrix* other, double eps = 1e-15) const;

    Eigen::MatrixXd mat;
};

inline bool isMatrix(const Value& v) { return isObjType(v, ObjType::Matrix); }
inline ObjMatrix* asMatrix(const Value& v) { return static_cast<ObjMatrix*>(v.asObj()); }

ObjMatrix* matrixVal();
ObjMatrix* matrixVal(int32_t rows, int32_t cols);
ObjMatrix* matrixVal(const Eigen::MatrixXd& values);
ObjMatrix* cloneMatrix(const ObjMatrix* m);

std::string objMatrixToString(const ObjMatrix* om);

//
// signal (dataflow signal wrapper)

struct ObjSignal : public Obj {
    ObjSignal(ptr<df::Signal> s);
    virtual ~ObjSignal() {}
    ObjEvent* ensureChangeEvent();
    ptr<df::Signal> signal;
    Value changeEvent;
};

inline bool isSignal(const Value& v) { return isObjType(v, ObjType::Signal); }
inline ObjSignal* asSignal(const Value& v) { return static_cast<ObjSignal*>(v.asObj()); }

ObjSignal* signalVal(ptr<df::Signal> s);
std::string objSignalToString(const ObjSignal* os);


//
// event

struct ObjEvent : public Obj {
    ObjEvent() { type = ObjType::Event; }
    virtual ~ObjEvent() {}

    // list of subscribed handler closures (weak references)
    std::vector<Value> subscribers;
};

inline bool isEvent(const Value& v) { return isObjType(v, ObjType::Event); }
inline ObjEvent* asEvent(const Value& v) { return static_cast<ObjEvent*>(v.asObj()); }

ObjEvent* eventVal();
std::string objEventToString(const ObjEvent* ev);


//
// dynamic library handle

struct ObjLibrary : public Obj {
    ObjLibrary(void* h) : handle(h) { type = ObjType::Library; }
    virtual ~ObjLibrary();
    void* handle;
};

inline bool isLibrary(const Value& v) { return isObjType(v, ObjType::Library); }
inline ObjLibrary* asLibrary(const Value& v) { return static_cast<ObjLibrary*>(v.asObj()); }

ObjLibrary* libraryVal(void* handle);
std::string objLibraryToString(const ObjLibrary* lib);


//
// function

enum class FunctionType {
    Function,
    Method,
    Initializer,
    Module
};

std::string toString(FunctionType ft);

struct ObjModuleType; // forward

struct ObjFunction : public Obj
{
    ObjFunction(const icu::UnicodeString& packageName, const icu::UnicodeString& moduleName,
                const icu::UnicodeString& sourceName);
    virtual ~ObjFunction();

    UnicodeString name;
    std::optional<ptr<roxal::type::Type>> funcType;
    int arity;
    int upvalueCount;
    ptr<Chunk> chunk;
    std::vector<ptr<ast::Annotation>> annotations;
    void* nativeSpec { nullptr }; // for ffi or other native info

    bool strict;        // true if function was compiled in strict mode

    FunctionType fnType { FunctionType::Function };
    Value ownerType { nilVal() }; // weak ref owning type
    ast::Access access { ast::Access::Public };

    // for parameters with default values that must be re-evaluated on each call
    //  this is map from param name UnicodeString::hashCode() -> ObjFunction
    //  (where ObjFunction is a function that takes no params and returns the default value)
    std::map<int32_t, ObjFunction*> paramDefaultFunc;

    Value moduleType; // ObjModuleType
};

inline bool isFunction(const Value& v) { return isObjType(v, ObjType::Function); }
inline ObjFunction* asFunction(const Value& v) {
    #ifdef DEBUG_BUILD
    if (!isFunction(v))
        throw std::runtime_error("Value is not an ObjFunction");
    #endif
    return static_cast<ObjFunction*>(v.asObj());
}


inline ObjFunction* functionVal(const icu::UnicodeString& packageName,
                                const icu::UnicodeString& moduleName,
                                const icu::UnicodeString& sourceName) {
    return newObj<ObjFunction>(__func__, packageName, moduleName, sourceName);
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

    // thread expected to execute this closure when used as an event handler
    std::weak_ptr<Thread> handlerThread;
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
    ObjNative(NativeFn function, void* data=nullptr,
              ptr<roxal::type::Type> funcType=nullptr,
              std::vector<Value> defaults = {});
    virtual ~ObjNative() {}

    NativeFn function;
    void* data;
    ptr<roxal::type::Type> funcType;
    std::vector<Value> defaultValues;
};

inline bool isNative(const Value& v) { return isObjType(v, ObjType::Native); }
inline ObjNative* asNative(const Value& v) { return static_cast<ObjNative*>(v.asObj()); }

ObjNative* nativeVal(NativeFn function, void* data=nullptr,
                     ptr<roxal::type::Type> funcType=nullptr,
                     std::vector<Value> defaults = {});




//
// runtime type

//FIXME!!!: collision exists for Obj::type == ObjType::Type - it is used
//       both by ObjTypeSpec and by ObjPrimitive for builtin type
struct ObjTypeSpec : public Obj
{
    ObjTypeSpec() {
        type = ObjType::Type;
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
// object|actor|interface|enum type
// TODO: separate enum out into its own ObjTypeSpec subclass

struct ObjObjectType : public ObjTypeSpec
{
    ObjObjectType(const icu::UnicodeString& typeName, bool isactor = false, bool isinterface = false, bool isenumeration = false);

    virtual ~ObjObjectType()
    {
        if (isEnumeration) {
            //std::cout << "unregistering enum id " << enumTypeId << std::endl << std::flush;
            enumTypes.erase(enumTypeId);
        }
    }

    icu::UnicodeString name;
    bool isActor;
    bool isInterface;
    bool isEnumeration;
    Value superType { nilVal() }; // parent type
    bool isCStruct { false };
    int cstructArch { hostArch };
    uint16_t enumTypeId;

    // name -> type, initial value
    struct Property {
        icu::UnicodeString name;
        Value type;
        Value initialValue;
        ast::Access access { ast::Access::Public };
        Value ownerType { nilVal() }; // weak ref to owning type
        std::optional<icu::UnicodeString> ctype;
    };
    std::unordered_map<int32_t, Property> properties;
    std::vector<int32_t> propertyOrder;

    struct Method {
        icu::UnicodeString name;
        Value closure;
        ast::Access access { ast::Access::Public };
        Value ownerType { nilVal() }; // weak ref to owning type
    };
    std::unordered_map<int32_t, Method> methods;

    // name -> value
    std::unordered_map<int32_t, std::pair<icu::UnicodeString, Value>> enumLabelValues;


    // global enum type id -> ObjObjectType
    //  TODO: make thread safe?
    static std::unordered_map<uint16_t, ObjObjectType*> enumTypes;
};


inline bool isObjectType(const Value& v) { return isObjType(v, ObjType::Type) && ((asTypeSpec(v)->typeValue == ValueType::Object) || (asTypeSpec(v)->typeValue == ValueType::Actor)); }
inline ObjObjectType* asObjectType(const Value& v) { return static_cast<ObjObjectType*>(v.asObj()); }

inline bool isEnumType(const Value& v) { return isObjType(v, ObjType::Type) && asTypeSpec(v)->typeValue == ValueType::Enum; }

ObjObjectType* objectTypeVal(const icu::UnicodeString& typeName, bool isActor, bool isInterface = false, bool isEnumeration = false);


struct ObjPackageType : public ObjTypeSpec
{
    // TODO
};

struct ObjModuleType : public ObjTypeSpec
{
    ObjModuleType(const icu::UnicodeString& typeName);

    virtual ~ObjModuleType();

    icu::UnicodeString name;

    // variables declared at runtime via VM OpCode::DefineModuleVar
    VariablesMap vars;

    // cstruct type annotations: type name hash -> arch (32 or 64)
    std::unordered_map<int32_t, int> cstructArch;
    // property ctype annotations: type name hash -> (prop name hash -> ctype)
    std::unordered_map<int32_t, std::unordered_map<int32_t, icu::UnicodeString>> propertyCTypes;

    static atomic_vector<ObjModuleType*> allModules;
};

inline bool isModuleType(const Value& v) { return isObjType(v, ObjType::Type) && (asTypeSpec(v)->typeValue == ValueType::Module); }
inline ObjModuleType* asModuleType(const Value& v) { return static_cast<ObjModuleType*>(v.asObj()); }

ObjModuleType* moduleTypeVal(const icu::UnicodeString& typeName);





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
    std::weak_ptr<Thread> thread;
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

//
// native method bound to builtin instance

struct ObjBoundNative : public Obj
{
    ObjBoundNative(const Value& instance, NativeFn fn, bool proc = false,
                   ptr<roxal::type::Type> funcType=nullptr,
                   std::vector<Value> defaults = {})
      : receiver(instance), function(fn), isProc(proc),
        funcType(funcType), defaultValues(std::move(defaults)) { type = ObjType::BoundNative; }
    virtual ~ObjBoundNative() {}

    Value receiver;
    NativeFn function;
    bool isProc;  // true for proc methods, false for func methods
    ptr<roxal::type::Type> funcType;
    std::vector<Value> defaultValues;
};

inline bool isBoundNative(const Value& v) { return isObjType(v, ObjType::BoundNative); }
inline ObjBoundNative* asBoundNative(const Value& v) { return static_cast<ObjBoundNative*>(v.asObj()); }
inline ObjBoundNative* boundNativeVal(const Value& instance, NativeFn fn, bool isProc = false,
                                      ptr<roxal::type::Type> funcType=nullptr,
                                      std::vector<Value> defaults = {}) { return newObj<ObjBoundNative>(__func__, instance, fn, isProc, funcType, std::move(defaults)); }
inline ObjBoundNative* cloneBoundNative(const ObjBoundNative* bm) {
    auto newbm = newObj<ObjBoundNative>(__func__, bm->receiver, bm->function, bm->isProc, bm->funcType, bm->defaultValues);
    newbm->receiver = newbm->receiver.clone();
    return newbm;
}






#ifdef DEBUG_BUILD
void testObjectValues();
#endif



} // namespace
