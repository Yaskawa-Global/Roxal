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
#include <ostream>
#include <istream>
#include <fstream>

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

namespace df { class Signal; class DataflowEngine; }


namespace roxal {
struct ObjObjectType; // forward
class Thread; // forward declaration for handler threads
struct ObjEvent; // forward
struct ObjException; // forward
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
    ForeignPtr,
    File,
    Event,
    Exception
};






#include "ObjControl.h"

struct Obj {
    Obj() : type(ObjType::None), control(nullptr) {}
    virtual ~Obj() {}

    ObjType type;
    ObjControl* control;


    ValueType valueType() const;

    Obj* clone() const; // deep copy

    virtual void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const = 0;
    virtual void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) = 0;

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
    static atomic_map<Obj*, std::string> allocatedObjs;
    #endif
};


// For debug builds, we include the function name, file & line number
#ifdef DEBUG_BUILD
template<typename T, typename... Args>
inline T* newObj(const std::string& name, const std::string& filename, int lineNumber, Args&&... args) {
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
    Obj::allocatedObjs.store(o, name + " @ " + filename + ":" + std::to_string(lineNumber));
    #endif

    return o;
}
#else
template<typename T, typename... Args>
inline T* newObj(Args&&... args) {
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
    Obj::allocatedObjs.store(o, "");
    #endif

    return o;
}
#endif

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

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;
};

inline bool isObjPrimitive(const Value& v)
{
    if (!v.isObj())
        return false;
    ObjType t = objType(v);
    if (t == ObjType::Bool || t == ObjType::Int || t == ObjType::Real)
        return true;
    if (t == ObjType::Type)
        return dynamic_cast<ObjPrimitive*>(v.asObj()) != nullptr;
    return false;
}
inline ObjPrimitive* asObjPrimitive(const Value& v) { return static_cast<ObjPrimitive*>(v.asObj()); }

inline ObjPrimitive* cloneObjPrimitive(const ObjPrimitive* op) {
    #ifdef DEBUG_BUILD
    if (op->type == ObjType::Bool)
        return newObj<ObjPrimitive>(__func__,__FILE__,__LINE__,op->as.boolean);
    else if (op->type == ObjType::Int)
        return newObj<ObjPrimitive>(__func__,__FILE__,__LINE__,op->as.integer);
    else if (op->type == ObjType::Real)
        return newObj<ObjPrimitive>(__func__,__FILE__,__LINE__,op->as.real);
    else if (op->type == ObjType::Type)
        return newObj<ObjPrimitive>(__func__,__FILE__,__LINE__,op->as.btype);
    throw std::runtime_error("Unsupported ObjPrimitive Type "+std::to_string(int(op->type)));
    #else
    if (op->type == ObjType::Bool)
        return newObj<ObjPrimitive>(op->as.boolean);
    else if (op->type == ObjType::Int)
        return newObj<ObjPrimitive>(op->as.integer);
    else if (op->type == ObjType::Real)
        return newObj<ObjPrimitive>(op->as.real);
    else if (op->type == ObjType::Type)
        return newObj<ObjPrimitive>(op->as.btype);
    return newObj<ObjPrimitive>(false);
    #endif
}




//
// string

struct ObjString : public Obj
{
    ObjString();
    ObjString(const UnicodeString& us);
    virtual ~ObjString();

    UnicodeString s;
    int32_t hash;

    // number of 16bit Unicode code units
    int32_t length() const { return s.length(); }

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;

    // Elements are Unicode code units (not code points or characters)
    Value index(const Value& i) const;

    std::string toStdString() const
      { std::string ss; s.toUTF8String(ss); return ss; }
};


inline bool isString(const Value& v) { return isObjType(v, ObjType::String); }
inline ObjString* asStringObj(const Value& v) { return static_cast<ObjString*>(v.asObj()); }
inline UnicodeString asUString(const Value& v) { return asStringObj(v)->s; }

// allocate new ObjString on heap and copy s (or return existing interned string)
ObjString* newObjString(const UnicodeString& s);
void updateInternedString(ObjString* obj, const UnicodeString& newVal);

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

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;

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

ObjRange* newRangeObj(); // empty range
ObjRange* newRangeObj(const Value& start, const Value& stop, const Value& step, bool closed);

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
    void set(const ObjList* other);          // Shallow copy from other list

    bool equals(const ObjList* other) const;  // Deep equality comparison

    atomic_vector<Value> elts;

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;
};


inline bool isList(const Value& v) { return isObjType(v, ObjType::List); }
inline ObjList* asList(const Value& v) { return static_cast<ObjList*>(v.asObj()); }

ObjList* newListObj();
ObjList* newListObj(const ObjRange* r);
ObjList* newListObj(const std::vector<Value>& elts);
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
        return Value::nilVal();
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

    void set(const ObjDict* other); // Shallow copy from other dict

    bool equals(const ObjDict* other) const;  // Deep equality comparison

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

public:
    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;
};


inline bool isDict(const Value& v) { return isObjType(v, ObjType::Dict); }
inline ObjDict* asDict(const Value& v) { return static_cast<ObjDict*>(v.asObj()); }

ObjDict* newDictObj();
ObjDict* newDictObj(const std::vector<std::pair<Value,Value>>& entries);
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

    void set(const ObjVector* other); // Shallow copy from other vector

    Eigen::VectorXd vec;

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;
};

inline bool isVector(const Value& v) { return isObjType(v, ObjType::Vector); }
inline ObjVector* asVector(const Value& v) { return static_cast<ObjVector*>(v.asObj()); }

ObjVector* newVectorObj();
ObjVector* newVectorObj(int32_t size);
ObjVector* newVectorObj(const Eigen::VectorXd& values);
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

    void set(const ObjMatrix* other); // Shallow copy from other matrix

    Eigen::MatrixXd mat;

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;
};

inline bool isMatrix(const Value& v) { return isObjType(v, ObjType::Matrix); }
inline ObjMatrix* asMatrix(const Value& v) { return static_cast<ObjMatrix*>(v.asObj()); }

ObjMatrix* newMatrixObj();
ObjMatrix* newMatrixObj(int32_t rows, int32_t cols);
ObjMatrix* newMatrixObj(const Eigen::MatrixXd& values);
ObjMatrix* cloneMatrix(const ObjMatrix* m);

std::string objMatrixToString(const ObjMatrix* om);

//
// signal (dataflow signal wrapper)

struct ObjSignal : public Obj {
    ObjSignal(ptr<df::Signal> s);
    virtual ~ObjSignal();
    ObjEvent* ensureChangeEvent();
    ptr<df::Signal> signal;
    df::DataflowEngine* engine;
    Value changeEvent;

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;
};

inline bool isSignal(const Value& v) { return isObjType(v, ObjType::Signal); }
inline ObjSignal* asSignal(const Value& v) { return static_cast<ObjSignal*>(v.asObj()); }

ObjSignal* newSignalObj(ptr<df::Signal> s);
std::string objSignalToString(const ObjSignal* os);


//
// event

struct ObjEvent : public Obj {
    ObjEvent() { type = ObjType::Event; }
    virtual ~ObjEvent() {}

    // list of subscribed handler closures (weak references)
    std::vector<Value> subscribers;

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;
};

inline bool isEvent(const Value& v) { return isObjType(v, ObjType::Event); }
inline ObjEvent* asEvent(const Value& v) { return static_cast<ObjEvent*>(v.asObj()); }

ObjEvent* newEventObj();
std::string objEventToString(const ObjEvent* ev);


//
// dynamic library handle

struct ObjLibrary : public Obj {
    ObjLibrary(void* h) : handle(h) { type = ObjType::Library; }
    virtual ~ObjLibrary();
    void* handle;

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;
};

inline bool isLibrary(const Value& v) { return isObjType(v, ObjType::Library); }
inline ObjLibrary* asLibrary(const Value& v) { return static_cast<ObjLibrary*>(v.asObj()); }

ObjLibrary* newLibraryObj(void* handle);
std::string objLibraryToString(const ObjLibrary* lib);

//
// opaque foreign pointer

struct ObjForeignPtr : public Obj {
    ObjForeignPtr(void* p) : ptr(p) { type = ObjType::ForeignPtr; }
    virtual ~ObjForeignPtr() { if (cleanup) cleanup(ptr); }

    void registerCleanup(std::function<void(void*)> fn) { cleanup = std::move(fn); }

    void* ptr;
    std::function<void(void*)> cleanup;

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;
};

inline bool isForeignPtr(const Value& v) { return isObjType(v, ObjType::ForeignPtr); }
inline ObjForeignPtr* asForeignPtr(const Value& v) { return static_cast<ObjForeignPtr*>(v.asObj()); }

ObjForeignPtr* newForeignPtrObj(void* ptr);
std::string objForeignPtrToString(const ObjForeignPtr* fp);


//
// file handle

struct ObjFile : public Obj {
    ObjFile(roxal::ptr<std::fstream> f, bool binary = false)
        : file(std::move(f)), binary(binary) { type = ObjType::File; }
    virtual ~ObjFile() { if (file && file->is_open()) file->close(); }
    roxal::ptr<std::fstream> file;
    bool binary;

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;
};

inline bool isFile(const Value& v) { return isObjType(v, ObjType::File); }
inline ObjFile* asFile(const Value& v) { return static_cast<ObjFile*>(v.asObj()); }

ObjFile* newFileObj(roxal::ptr<std::fstream> f, bool binary = false);
std::string objFileToString(const ObjFile* f);



//
// exception object

struct ObjException : public Obj {
    ObjException() { type = ObjType::Exception; }
    ObjException(Value msg, Value exType = Value::nilVal(), Value st = Value::nilVal())
        : message(msg), exType(exType), stackTrace(st) { type = ObjType::Exception; }
    virtual ~ObjException() {}

    Value message;
    Value exType; // object type of the exception

    Value stackTrace; // list of stack frames when raised

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;
};

inline bool isException(const Value& v) { return isObjType(v, ObjType::Exception); }
inline ObjException* asException(const Value& v) { return static_cast<ObjException*>(v.asObj()); }

ObjException* newExceptionObj(Value message = Value::nilVal(), Value exType = Value::nilVal(), Value stackTrace = Value::nilVal());
std::string objExceptionToString(const ObjException* ex);
std::string objExceptionStackTraceToString(const ObjException* ex);
std::string stackTraceToString(Value frames);


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

class VM; // forward for native functions
struct ArgsView; // forward for native functions
//using NativeFn = std::function<Value(VM&, ArgsView)>;

struct ObjFunction : public Obj
{
    ObjFunction(const icu::UnicodeString& name,
                const icu::UnicodeString& packageName,
                const icu::UnicodeString& moduleName,
                const icu::UnicodeString& sourceName);
    virtual ~ObjFunction();

    UnicodeString name;
    std::optional<ptr<roxal::type::Type>> funcType;
    int arity;
    int upvalueCount;
    ptr<Chunk> chunk;
    std::vector<ptr<ast::Annotation>> annotations;
    icu::UnicodeString doc;
    void* nativeSpec { nullptr }; // for ffi or other native info
    NativeFn nativeImpl;
    std::vector<Value> nativeDefaults;

    bool strict;        // true if function was compiled in strict mode

    FunctionType fnType { FunctionType::Function };
    Value ownerType { Value::nilVal() }; // weak ref owning type
    ast::Access access { ast::Access::Public };

    // for parameters with default values that must be re-evaluated on each call
    //  this is map from param name UnicodeString::hashCode() -> Value ObjFunction
    //  (where ObjFunction is a function that takes no params and returns the default value)
    std::map<int32_t, Value> paramDefaultFunc;

    Value moduleType; // ObjModuleType

    void clear(); // reset to blank without other reference values

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;
};

inline bool isFunction(const Value& v) { return isObjType(v, ObjType::Function); }
inline ObjFunction* asFunction(const Value& v) {
    debug_assert_msg(isFunction(v), "Value is an ObjFunction");
    return static_cast<ObjFunction*>(v.asObj());
}


inline ObjFunction* newFunctionObj(const icu::UnicodeString& name,
                                   const icu::UnicodeString& packageName,
                                   const icu::UnicodeString& moduleName,
                                   const icu::UnicodeString& sourceName) {
    #ifdef DEBUG_BUILD
    return newObj<ObjFunction>(toUTF8StdString(name), __FILE__, __LINE__, name, packageName, moduleName, sourceName);
    #else
    return newObj<ObjFunction>(name, packageName, moduleName, sourceName);
    #endif
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

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;
};

inline bool isUpvalue(const Value& v) { return isObjType(v, ObjType::Upvalue); }
inline ObjUpvalue* asUpvalue(const Value& v) { return static_cast<ObjUpvalue*>(v.asObj()); }

inline ObjUpvalue* newUpvalueObj(Value* v) {
    #ifdef DEBUG_BUILD
    return newObj<ObjUpvalue>(__func__,__FILE__,__LINE__,v);
    #else
    return newObj<ObjUpvalue>(v);
    #endif
}

inline ObjUpvalue* cloneUpvalue(const ObjUpvalue* u) {
    #ifdef DEBUG_BUILD
    ObjUpvalue* newup = newObj<ObjUpvalue>(__func__,__FILE__, __LINE__,u->location);
    #else
    ObjUpvalue* newup = newObj<ObjUpvalue>(u->location);
    #endif
    // clone value (this Upvalue is now closed)
    newup->closed = newup->location->clone();
    newup->location = &newup->closed;
    return newup;
}


//
// Closure

struct ObjClosure : public Obj
{
    ObjClosure(const Value& f = Value::nilVal()) : function(f) {
        type = ObjType::Closure;
        if (function.isNonNil()) {
            debug_assert_msg(isFunction(function), "Value is an ObjFunction");
            upvalues.resize(asFunction(function)->upvalueCount);
        }
    }
    virtual ~ObjClosure() {
        upvalues.clear();
    }

    Value function; // ObjFunction
    std::vector<Value> upvalues; // ObjUpvalue

    // thread expected to execute this closure when used as an event handler
    weak_ptr<Thread> handlerThread;

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;
};

inline bool isClosure(const Value& v) { return isObjType(v, ObjType::Closure); }
inline ObjClosure* asClosure(const Value& v) {
    debug_assert_msg(isClosure(v), "Value is an ObjClosure");
    return static_cast<ObjClosure*>(v.asObj());
}

inline ObjClosure* newClosureObj(Value function) { // ObjFunction
    debug_assert_msg(isFunction(function), "Value is an ObjFunction");
    #ifdef DEBUG_BUILD
    return newObj<ObjClosure>(toUTF8StdString(asFunction(function)->name),__FILE__,__LINE__,function);
    #else
    return newObj<ObjClosure>(function);
    #endif
}

inline ObjClosure* cloneClosure(const ObjClosure* c) {
    #ifdef DEBUG_BUILD
    ObjClosure* newc = newObj<ObjClosure>(__func__,__FILE__,__LINE__,c->function);
    #else
    ObjClosure* newc = newObj<ObjClosure>(c->function);
    #endif
    newc->upvalues.resize(c->upvalues.size());
    for(auto i=0; i<c->upvalues.size();i++)
        newc->upvalues[i] = c->upvalues.at(i).clone();
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

    Value asValue() { return future.valid() ? future.get() : Value::nilVal(); }

    std::shared_future<Value> future;

    mutable std::mutex waitMutex;
    std::vector<weak_ptr<Thread>> waiters;

    void addWaiter(const ptr<Thread>& t);
    void wakeWaiters();

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;
};

inline bool isFuture(const Value& v) { return isObjType(v, ObjType::Future); }
inline ObjFuture* asFuture(const Value& v) {
    debug_assert_msg(isFuture(v), "Value is an ObjFuture");
    return static_cast<ObjFuture*>(v.asObj());
}

inline ObjFuture* newFutureObj(const std::shared_future<Value>& fv) {
    #ifdef DEBUG_BUILD
    return newObj<ObjFuture>(__func__, __FILE__,__LINE__,fv);
    #else
    return newObj<ObjFuture>(fv);
    #endif
}



//
// native function

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

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;
};

inline bool isNative(const Value& v) { return isObjType(v, ObjType::Native); }
inline ObjNative* asNative(const Value& v) { return static_cast<ObjNative*>(v.asObj()); }

ObjNative* newNativeObj(NativeFn function, void* data=nullptr,
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

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;
};

inline bool isTypeSpec(const Value& v) { return isObjType(v,ObjType::Type); }
inline ObjTypeSpec* asTypeSpec(const Value& v) { return static_cast<ObjTypeSpec*>(v.asObj()); }

ObjTypeSpec* newTypeSpecObj(ValueType t); // primitive

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
    Value superType { Value::nilVal() }; // parent type
    bool isCStruct { false };
    int cstructArch { hostArch };
    uint16_t enumTypeId;

    // name -> type, initial value
    struct Property {
        icu::UnicodeString name;
        Value type;
        Value initialValue;
        ast::Access access { ast::Access::Public };
        Value ownerType { Value::nilVal() }; // weak ref to owning type
        std::optional<icu::UnicodeString> ctype;
    };
    std::unordered_map<int32_t, Property> properties;
    std::vector<int32_t> propertyOrder;

    struct Method {
        icu::UnicodeString name;
        Value closure;
        ast::Access access { ast::Access::Public };
        Value ownerType { Value::nilVal() }; // weak ref to owning type
    };
    std::unordered_map<int32_t, Method> methods;

    // name -> value
    std::unordered_map<int32_t, std::pair<icu::UnicodeString, Value>> enumLabelValues;


    // global enum type id -> ObjObjectType
    //  TODO: make thread safe?
    static std::unordered_map<uint16_t, ObjObjectType*> enumTypes;

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;
};


inline bool isObjectType(const Value& v) { return isObjType(v, ObjType::Type) && ((asTypeSpec(v)->typeValue == ValueType::Object) || (asTypeSpec(v)->typeValue == ValueType::Actor)); }
inline ObjObjectType* asObjectType(const Value& v) { return static_cast<ObjObjectType*>(v.asObj()); }

inline bool isEnumType(const Value& v) { return isObjType(v, ObjType::Type) && asTypeSpec(v)->typeValue == ValueType::Enum; }

ObjObjectType* newObjectTypeObj(const icu::UnicodeString& typeName, bool isActor, bool isInterface = false, bool isEnumeration = false);


struct ObjPackageType : public ObjTypeSpec
{
    // TODO

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;
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

    static atomic_vector<Value> allModules;

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;
};

inline bool isModuleType(const Value& v) { return isObjType(v, ObjType::Type) && (asTypeSpec(v)->typeValue == ValueType::Module); }
inline ObjModuleType* asModuleType(const Value& v) { return static_cast<ObjModuleType*>(v.asObj()); }

ObjModuleType* newModuleTypeObj(const icu::UnicodeString& typeName);





//
// object instance

struct ObjectInstance : public Obj
{
    ObjectInstance(ObjObjectType* objectType);
    virtual ~ObjectInstance();

    ObjObjectType* instanceType;
    std::unordered_map<int32_t, Value> properties;

    // convenience methods for property access (e.g. for builtin method implementations)
    Value getProperty(const icu::UnicodeString& name) const;
    Value getProperty(const std::string& name) const { return getProperty(toUnicodeString(name)); }
    Value getProperty(const char* name) const { return getProperty(toUnicodeString(name)); }
    void setProperty(const icu::UnicodeString& name, Value value);
    void setProperty(const std::string& name, Value value) { setProperty(toUnicodeString(name), value); }
    void setProperty(const char* name, Value value) { setProperty(toUnicodeString(name), value); }

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;
};

inline bool isObjectInstance(const Value& v) { return isObjType(v, ObjType::Instance); }
inline ObjectInstance* asObjectInstance(const Value& v) { return static_cast<ObjectInstance*>(v.asObj()); }

ObjectInstance* newObjectInstance(ObjObjectType* objectType);
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
        Value returnFuture;
        CallSpec callSpec;

        bool valid() const { return !callee.isNil(); }
    };
    atomic_queue<MethodCallInfo> callQueue;

    std::mutex queueMutex;
    std::condition_variable queueConditionVar;

    std::thread::id thread_id;
    weak_ptr<Thread> thread;

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;
};

inline bool isActorInstance(const Value& v) { return isObjType(v, ObjType::Actor); }
inline ActorInstance* asActorInstance(const Value& v) { return static_cast<ActorInstance*>(v.asObj()); }

ActorInstance* newActorInstance(ObjObjectType* objectType);



//
// method closure bound to object|actor instance

struct ObjBoundMethod : public Obj
{
    ObjBoundMethod(const Value& instance, const Value& closure);
    virtual ~ObjBoundMethod();

    Value receiver;
    Value method;

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;
};

inline bool isBoundMethod(const Value& v) { return isObjType(v, ObjType::BoundMethod); }
inline ObjBoundMethod* asBoundMethod(const Value& v) { return static_cast<ObjBoundMethod*>(v.asObj()); }

inline ObjBoundMethod* newBoundMethodObj(const Value& instance, const Value& closure) {
    #ifdef DEBUG_BUILD
    return newObj<ObjBoundMethod>(__func__, __FILE__, __LINE__, instance, closure);
    #else
    return newObj<ObjBoundMethod>(instance, closure);
    #endif
}

inline ObjBoundMethod* cloneBoundMethod(const ObjBoundMethod* bm) {
    #ifdef DEBUG_BUILD
    auto newmb = newObj<ObjBoundMethod>(toUTF8StdString(asFunction(asClosure(bm->method)->function)->name),__FILE__,__LINE__,bm->receiver, bm->method);
    #else
    auto newmb = newObj<ObjBoundMethod>(bm->receiver, bm->method);
    #endif

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

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;
};

inline bool isBoundNative(const Value& v) { return isObjType(v, ObjType::BoundNative); }
inline ObjBoundNative* asBoundNative(const Value& v) { return static_cast<ObjBoundNative*>(v.asObj()); }
inline ObjBoundNative* newBoundNativeObj(const Value& instance, NativeFn fn, bool isProc = false,
                                         ptr<roxal::type::Type> funcType=nullptr,
                                         std::vector<Value> defaults = {}) {
    #ifdef DEBUG_BUILD
    return newObj<ObjBoundNative>(__func__, __FILE__, __LINE__, instance, fn, isProc, funcType, std::move(defaults));
    #else
    return newObj<ObjBoundNative>(instance, fn, isProc, funcType, std::move(defaults));
    #endif
}

inline ObjBoundNative* cloneBoundNative(const ObjBoundNative* bm) {
    #ifdef DEBUG_BUILD
    auto newbm = newObj<ObjBoundNative>(__func__, __FILE__, __LINE__, bm->receiver, bm->function, bm->isProc, bm->funcType, bm->defaultValues);
    #else
    auto newbm = newObj<ObjBoundNative>(bm->receiver, bm->function, bm->isProc, bm->funcType, bm->defaultValues);
    #endif
    newbm->receiver = newbm->receiver.clone();
    return newbm;
}






#ifdef DEBUG_BUILD
void testObjectValues();
#endif



} // namespace
