#pragma once

#include <functional>
#include <map>
#include <vector>
#include <sstream>
#include <atomic>
#include <future>
#include <condition_variable>
#include <memory>
#include <utility>
#include <unicode/ustring.h>
#include <ostream>
#include <istream>
#include <fstream>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <array>

#include <core/common.h>
#include <core/AST.h>
#include <core/atomic.h>
#include <core/types.h>
#include "Chunk.h"
#include "Value.h"
#include "SimpleMarkSweepGC.h"
#include <Eigen/Dense>
#include <Eigen/Geometry>

#ifdef ROXAL_ENABLE_ONNX
#include <onnxruntime_cxx_api.h>
#endif


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
struct ObjEventType; // forward
struct ObjEventInstance; // forward
struct ObjFunction; // forward for bound native default values
struct ObjException; // forward
#ifdef ROXAL_COMPUTE_SERVER
class ComputeConnection; // forward
#endif

void visitInternedStrings(const std::function<void(ObjString*)>& fn);
void purgeDeadInternedStrings();
}


namespace roxal {

using icu::UnicodeString;

struct UnreleasedObj; // forward declaration


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
    Orient,
    Vector,
    Matrix,
    Tensor,
    Signal,
    Library,
    ForeignPtr,
    File,
    EventType,
    EventInstance,
    Exception
};

/// Returns true if the object type is user-mutable and can hold Value references
/// to other objects (container types relevant for graph isolation checking).
inline bool isMutableRefContainerType(ObjType t) {
    return t == ObjType::List || t == ObjType::Dict || t == ObjType::Instance;
}




#include "ObjControl.h"

struct Obj {
    Obj() : type(ObjType::None), control(nullptr) {}
    virtual ~Obj() {}

    ObjType type;
    ObjControl* control;


    ValueType valueType() const;

    virtual void trace(ValueVisitor& visitor) const = 0;
    // Release strong Value references owned by the object without destroying
    // it. The GC invokes this to break cycles before routing the object
    // through the existing unref queue, and builtin modules reuse it during
    // manual teardown.
    virtual void dropReferences();

    virtual unique_ptr<Obj, UnreleasedObj> clone(roxal::ptr<CloneContext> ctx) const = 0; // deep copy preserving structure
    virtual unique_ptr<Obj, UnreleasedObj> shallowClone() const; // shallow copy (copies property slots, not children); returns nullptr for types that don't support it

    // MVCC: throws if this object is a frozen clone (const snapshot).
    // Call at the top of every mutation method to prevent const violations
    // that bypass the Value-level const bit (e.g. builtin method dispatch).
    inline void ensureMutable() const {
        if (control->snapshotToken)
            throw std::runtime_error("Cannot mutate const value");
    }

    // MVCC: save current state into the version chain before mutation.
    // Only call when activeSnapshotCount > 0.
    void saveVersion();

    // MVCC: free version chain entries and release SnapshotToken.
    // Called from dropReferences() during object destruction.
    void cleanupMVCC();

    // MVCC: trim version chain entries that no active snapshot can observe.
    // Keeps the newest entry with epoch <= minEpoch as a floor version.
    // If minEpoch == UINT64_MAX (no active snapshots), clears the entire chain.
    // Called from GC sweep when all threads are at a safepoint.
    void trimVersionChain(uint64_t minEpoch);

    virtual void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const = 0;
    virtual void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) = 0;

    inline void incRef()
    {
        control->strong.fetch_add(1,std::memory_order_relaxed);
    }

    // Atomically increment strong count only if currently > 0.
    // Returns true on success (caller now holds a strong ref).
    // Returns false if the object is already dying (strong == 0).
    inline bool tryIncRef()
    {
        int32_t prev = control->strong.load(std::memory_order_relaxed);
        while (prev > 0) {
            if (control->strong.compare_exchange_weak(prev, prev + 1,
                    std::memory_order_relaxed, std::memory_order_relaxed))
                return true;
        }
        return false;
    }

    void decRef();

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

template<typename T>
inline void delObj(T* o);

// MVCC snapshot functions

/// Create a frozen snapshot of a mutable value (T → const T).
/// Shallow-clones the root object, allocates a SnapshotToken, sets ConstMask.
/// Returns the original value unchanged if it's already const or not a reference type.
Value createFrozenSnapshot(const Value& v);

/// Check whether all mutable interior objects reachable from \p root are
/// exclusively owned by the graph (no external aliases).  The root itself
/// is excluded — the caller verifies sole-ownership separately.
bool isIsolatedGraph(Obj* root);

/// Resolve a child value read through a frozen parent.
/// For primitives, returns the value directly.
/// For reference types, materializes a frozen clone at the parent's snapshot epoch,
/// caching it back into the parent's property slot via `cacheSlot` (if non-null).
/// Uses the SnapshotToken's cloneMap for alias/cycle preservation.
Value resolveConstChild(const Value& child, SnapshotToken* token, Value* cacheSlot = nullptr);

struct UnreleasedObj {
    template<typename T>
    void operator()(T* p) const {
        if (p != nullptr)
            throw std::runtime_error("newObj unique_ptr destroyed without releasing");
    }
};

// newObj returns a unique_ptr whose deleter throws if the pointer is not
// released before destruction. The caller must transfer ownership to
// Value::objVal() before the unique_ptr goes out of scope.

// For debug builds, we include the function name, file & line number
#ifdef DEBUG_BUILD
template<typename T, typename... Args>
inline unique_ptr<T, UnreleasedObj> newObj(const std::string& name, const std::string& filename, int lineNumber, Args&&... args) {
    // Allocate one contiguous block for control and object, ensuring proper alignment for T.
    constexpr std::size_t alignT = alignof(T);
    const std::size_t total = sizeof(ObjControl) + sizeof(T) + alignT - 1; // extra space for alignment adjustment
    char* mem = new char[total];

    // Place control at the start
    ObjControl* ctrl = new (mem) ObjControl();

    // Compute an aligned address for T immediately after ObjControl
    std::uintptr_t base = reinterpret_cast<std::uintptr_t>(mem);
    std::uintptr_t unaligned = base + sizeof(ObjControl);
    std::uintptr_t aligned = (unaligned + (alignT - 1)) & ~(static_cast<std::uintptr_t>(alignT) - 1);
    T* o = new (reinterpret_cast<void*>(aligned)) T(std::forward<Args>(args)...);

    ctrl->strong = 0;
    ctrl->weak   = 1;   // implicit weak ref representing strong refs
    ctrl->obj    = o;
    ctrl->allocationSize = static_cast<std::uint64_t>(total);
    ctrl->collecting.store(false, std::memory_order_relaxed);
    ctrl->markEpoch.store(0uLL, std::memory_order_relaxed);
    o->control   = ctrl;

    SimpleMarkSweepGC::instance().registerAllocation(ctrl);

    // In debug builds, verify alignment to catch subtle UB early
    debug_assert_msg(reinterpret_cast<std::uintptr_t>(o) % alignT == 0, "newObj produced misaligned object memory");

    #ifdef DEBUG_TRACE_MEMORY
    if (Obj::allocatedObjs.containsKey(o))
        throw std::runtime_error("new Obj* yielded address already allocated: " + toString(o));
    Obj::allocatedObjs.store(o, name + " @ " + filename + ":" + std::to_string(lineNumber));
    #endif

    return unique_ptr<T, UnreleasedObj>(o);
}
#else
template<typename T, typename... Args>
inline unique_ptr<T, UnreleasedObj> newObj(Args&&... args) {
    // Allocate one contiguous block for control and object, ensuring proper alignment for T.
    constexpr std::size_t alignT = alignof(T);
    const std::size_t total = sizeof(ObjControl) + sizeof(T) + alignT - 1; // extra space for alignment adjustment
    char* mem = new char[total];

    // Place control at the start
    ObjControl* ctrl = new (mem) ObjControl();

    // Compute an aligned address for T immediately after ObjControl
    std::uintptr_t base = reinterpret_cast<std::uintptr_t>(mem);
    std::uintptr_t unaligned = base + sizeof(ObjControl);
    std::uintptr_t aligned = (unaligned + (alignT - 1)) & ~(static_cast<std::uintptr_t>(alignT) - 1);
    T* o = new (reinterpret_cast<void*>(aligned)) T(std::forward<Args>(args)...);

    ctrl->strong = 0;
    ctrl->weak   = 1;   // implicit weak ref representing strong refs
    ctrl->obj    = o;
    ctrl->allocationSize = static_cast<std::uint64_t>(total);
    ctrl->collecting.store(false, std::memory_order_relaxed);
    ctrl->markEpoch.store(0uLL, std::memory_order_relaxed);
    o->control   = ctrl;

    SimpleMarkSweepGC::instance().registerAllocation(ctrl);

    #ifdef DEBUG_TRACE_MEMORY
    if (Obj::allocatedObjs.containsKey(o))
        throw std::runtime_error("new Obj* yielded address already allocated: " + toString(o));
    Obj::allocatedObjs.store(o, "");
    #endif

    return unique_ptr<T, UnreleasedObj>(o);
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
    SimpleMarkSweepGC::instance().unregisterAllocation(ctrl);
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
    ObjPrimitive(int64_t i) { type=ObjType::Int; as.integer = i; }
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
        int64_t integer;
        ValueType btype;
    } as;

    unique_ptr<Obj, UnreleasedObj> clone(roxal::ptr<CloneContext> ctx) const override;

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;

    void trace(ValueVisitor& visitor) const override { (void)visitor; }
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

inline unique_ptr<ObjPrimitive, UnreleasedObj> newBoolObj(bool b) {
#ifdef DEBUG_BUILD
    return newObj<ObjPrimitive>(__func__, __FILE__, __LINE__, b);
#else
    return newObj<ObjPrimitive>(b);
#endif
}

inline unique_ptr<ObjPrimitive, UnreleasedObj> newIntObj(int64_t i) {
#ifdef DEBUG_BUILD
    return newObj<ObjPrimitive>(__func__, __FILE__, __LINE__, i);
#else
    return newObj<ObjPrimitive>(i);
#endif
}

inline unique_ptr<ObjPrimitive, UnreleasedObj> newRealObj(double r) {
#ifdef DEBUG_BUILD
    return newObj<ObjPrimitive>(__func__, __FILE__, __LINE__, r);
#else
    return newObj<ObjPrimitive>(r);
#endif
}

inline unique_ptr<ObjPrimitive, UnreleasedObj> newTypeObj(ValueType t) {
#ifdef DEBUG_BUILD
    return newObj<ObjPrimitive>(__func__, __FILE__, __LINE__, t);
#else
    return newObj<ObjPrimitive>(t);
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
    // internKey is the value stored in the intern table; hash caches ICU hashCode().
    uint64_t internKey;
    int32_t hash;

    // number of 16bit Unicode code units
    int32_t length() const { return s.length(); }

    unique_ptr<Obj, UnreleasedObj> clone(roxal::ptr<CloneContext> ctx) const override { return unique_ptr<Obj, UnreleasedObj>(const_cast<ObjString*>(this)); } // strings are interned/immutable

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;

    // Elements are Unicode code units (not code points or characters)
    Value index(const Value& i) const;

    std::string toStdString() const
      { std::string ss; s.toUTF8String(ss); return ss; }

    void trace(ValueVisitor& visitor) const override { (void)visitor; }
};


inline bool isString(const Value& v) { return isObjType(v, ObjType::String); }
inline ObjString* asStringObj(const Value& v) { return static_cast<ObjString*>(v.asObj()); }
inline UnicodeString asUString(const Value& v) { return asStringObj(v)->s; }

// allocate new ObjString on heap and copy s (or return existing interned string).
// If wasInterned is non-null and the string was found in the intern table,
// *wasInterned is set to true and the returned object has an extra strong ref
// (via tryIncRef) that the caller must compensate for.
unique_ptr<ObjString, UnreleasedObj> newObjString(const UnicodeString& s, bool* wasInterned = nullptr);
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

    unique_ptr<Obj, UnreleasedObj> clone(roxal::ptr<CloneContext> ctx) const override;

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

    void trace(ValueVisitor& visitor) const override;
    void dropReferences() override;
};


inline bool isRange(const Value& v) { return isObjType(v, ObjType::Range); }
inline ObjRange* asRange(const Value& v) { return static_cast<ObjRange*>(v.asObj()); }

unique_ptr<ObjRange, UnreleasedObj> newRangeObj(); // empty range
unique_ptr<ObjRange, UnreleasedObj> newRangeObj(const Value& start, const Value& stop, const Value& step, bool closed);

std::string objRangeToString(const ObjRange* r);





//
// list

struct ObjList : public Obj
{
    ObjList() : elts_(make_ptr<std::vector<Value>>()) { type = ObjType::List; }
    ObjList(const ObjRange* r);
    virtual ~ObjList() {}

    int32_t length() const { return static_cast<int32_t>(elts_->size()); }
    bool empty() const { return elts_->empty(); }

    // Element access by integer index (no bounds-check Value wrapping)
    Value getElement(size_t i) const { return elts_->at(i); }
    void setElement(size_t i, const Value& v);  // MVCC-guarded

    // Element access by Value index (with bounds checking and slice support)
    Value index(const Value& i) const;
    void setIndex(const Value& i, const Value& v);

    // Bulk access: returns a snapshot copy of the elements vector
    std::vector<Value> getElements() const { return *elts_; }
    // Bulk replace: sets all elements from a plain vector
    void setElements(const std::vector<Value>& v);  // MVCC-guarded

    // Capacity
    void reserve(size_t n) { ensureUnique(); elts_->reserve(n); }

    // List operations (in-place)
    void concatenate(const ObjList* other);  // Concatenate other list to this list
    void append(const Value& value);         // Append value to this list
    void set(const ObjList* other);          // Shallow copy from other list

    bool equals(const ObjList* other) const;  // Deep equality comparison

    // Replace element at index without MVCC guards (for frozen snapshot caching)
    void cacheElement(int64_t index, const Value& val) { ensureUnique(); (*elts_)[index] = val; }

    unique_ptr<Obj, UnreleasedObj> clone(roxal::ptr<CloneContext> ctx) const override;
    unique_ptr<Obj, UnreleasedObj> shallowClone() const override;

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;

    void trace(ValueVisitor& visitor) const override;
    void dropReferences() override;

private:
    ptr<std::vector<Value>> elts_;
    void ensureUnique() {
        if (elts_.use_count() > 1)
            elts_ = make_ptr<std::vector<Value>>(*elts_);
    }
};


inline bool isList(const Value& v) { return isObjType(v, ObjType::List); }
inline ObjList* asList(const Value& v) { return static_cast<ObjList*>(v.asObj()); }

unique_ptr<ObjList, UnreleasedObj> newListObj();
unique_ptr<ObjList, UnreleasedObj> newListObj(const ObjRange* r);
unique_ptr<ObjList, UnreleasedObj> newListObj(const std::vector<Value>& elts);

std::string objListToString(const ObjList* ol);



//
// dict

struct ObjDict : public Obj
{
    struct ValueComparitor
    {
        using is_transparent = std::true_type;

        // standard comparison (between two instances of Type)
        bool operator()(const Value& lhs, const Value& rhs) const { return less(lhs, rhs).asBool(); }
    };

    struct DictData {
        std::map<Value,Value,ValueComparitor> entries;
        std::vector<Value> m_keys;
    };

    ObjDict() : data_(make_ptr<DictData>()) { type = ObjType::Dict; }
    virtual ~ObjDict() {}

    int32_t length() const {
        return data_->m_keys.size();
    }

    bool contains(const Value& key) const {
        return (data_->entries.find(key) != data_->entries.end());
    }

    Value at(const Value& key) const {
        auto it = data_->entries.find(key);
        if (it != data_->entries.end())
            return it->second;
        return Value::nilVal();
    }

    std::vector<Value> keys() const {
        return data_->m_keys;
    }

    std::vector<std::pair<Value,Value>> items() const {
        std::vector<std::pair<Value,Value>> keyvalues {};
        // can't just iterate over the entries directly, as we want to preserve order according to m_keys
        for(auto it=data_->m_keys.cbegin(); it!=data_->m_keys.cend(); it++)
            keyvalues.push_back(std::pair<Value,Value>(*it,data_->entries.at(*it)));
        return keyvalues;
    }

    void store(const Value& key, const Value& val);   // MVCC-guarded
    void erase(const Value& key);                      // MVCC-guarded

    // Replace value for existing key without MVCC guards (for frozen snapshot caching)
    void cacheValue(const Value& key, const Value& val) {
        ensureUnique();
        auto it = data_->entries.find(key);
        if (it != data_->entries.end())
            it->second = val;
    }

    void set(const ObjDict* other); // Shallow copy from other dict

    bool equals(const ObjDict* other) const;  // Deep equality comparison

    unique_ptr<Obj, UnreleasedObj> clone(roxal::ptr<CloneContext> ctx) const override;
    unique_ptr<Obj, UnreleasedObj> shallowClone() const override;

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;

    void trace(ValueVisitor& visitor) const override;
    void dropReferences() override;

private:
    ptr<DictData> data_;
    void ensureUnique() {
        if (data_.use_count() > 1)
            data_ = make_ptr<DictData>(*data_);
    }
};


inline bool isDict(const Value& v) { return isObjType(v, ObjType::Dict); }
inline ObjDict* asDict(const Value& v) { return static_cast<ObjDict*>(v.asObj()); }

unique_ptr<ObjDict, UnreleasedObj> newDictObj();
unique_ptr<ObjDict, UnreleasedObj> newDictObj(const std::vector<std::pair<Value,Value>>& entries);

std::string objDictToString(const ObjDict* od);



//
// vector

struct ObjVector : public Obj
{
    ObjVector() : vec_(make_ptr<Eigen::VectorXd>()) { type = ObjType::Vector; }
    ObjVector(const Eigen::VectorXd& values);
    ObjVector(int32_t size);
    virtual ~ObjVector() {}

    int32_t length() const { return vec_->size(); }

    Value index(const Value& i) const;
    void setIndex(const Value& i, const Value& v);

    bool equals(const ObjVector* other, double eps = 1e-15) const;

    void set(const ObjVector* other); // Shallow copy from other vector

    // COW accessors
    const Eigen::VectorXd& vec() const { return *vec_; }
    Eigen::VectorXd& vecMut();  // MVCC-guarded

    unique_ptr<Obj, UnreleasedObj> clone(roxal::ptr<CloneContext> ctx) const override;
    unique_ptr<Obj, UnreleasedObj> shallowClone() const override;

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;

    void trace(ValueVisitor& visitor) const override { (void)visitor; }

private:
    ptr<Eigen::VectorXd> vec_;
    void ensureUnique() {
        if (vec_.use_count() > 1)
            vec_ = make_ptr<Eigen::VectorXd>(*vec_);
    }
};

inline bool isVector(const Value& v) { return isObjType(v, ObjType::Vector); }
inline ObjVector* asVector(const Value& v) { return static_cast<ObjVector*>(v.asObj()); }

unique_ptr<ObjVector, UnreleasedObj> newVectorObj();
unique_ptr<ObjVector, UnreleasedObj> newVectorObj(int32_t size);
unique_ptr<ObjVector, UnreleasedObj> newVectorObj(const Eigen::VectorXd& values);

std::string objVectorToString(const ObjVector* ov);


//
// matrix

struct ObjMatrix : public Obj
{
    ObjMatrix() : mat_(make_ptr<Eigen::MatrixXd>()) { type = ObjType::Matrix; }
    ObjMatrix(const Eigen::MatrixXd& values);
    ObjMatrix(int32_t rows, int32_t cols);
    virtual ~ObjMatrix() {}

    int32_t rows() const { return mat_->rows(); }
    int32_t cols() const { return mat_->cols(); }

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

    // COW accessors
    const Eigen::MatrixXd& mat() const { return *mat_; }
    Eigen::MatrixXd& matMut();  // MVCC-guarded

    unique_ptr<Obj, UnreleasedObj> clone(roxal::ptr<CloneContext> ctx) const override;
    unique_ptr<Obj, UnreleasedObj> shallowClone() const override;

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;

    void trace(ValueVisitor& visitor) const override { (void)visitor; }

private:
    ptr<Eigen::MatrixXd> mat_;
    void ensureUnique() {
        if (mat_.use_count() > 1)
            mat_ = make_ptr<Eigen::MatrixXd>(*mat_);
    }
};

inline bool isMatrix(const Value& v) { return isObjType(v, ObjType::Matrix); }
inline ObjMatrix* asMatrix(const Value& v) { return static_cast<ObjMatrix*>(v.asObj()); }

unique_ptr<ObjMatrix, UnreleasedObj> newMatrixObj();
unique_ptr<ObjMatrix, UnreleasedObj> newMatrixObj(int32_t rows, int32_t cols);
unique_ptr<ObjMatrix, UnreleasedObj> newMatrixObj(const Eigen::MatrixXd& values);

std::string objMatrixToString(const ObjMatrix* om);


//
// orient (3D orientation, backed by unit quaternion)

struct ObjOrient : public Obj
{
    ObjOrient() : quat_(make_ptr<Eigen::Quaterniond>(Eigen::Quaterniond::Identity())) { type = ObjType::Orient; }
    ObjOrient(const Eigen::Quaterniond& q);
    virtual ~ObjOrient() {}

    bool equals(const ObjOrient* other, double eps = 1e-12) const;

    void set(const ObjOrient* other);

    // COW accessors
    const Eigen::Quaterniond& quat() const { return *quat_; }
    Eigen::Quaterniond& quatMut();  // MVCC-guarded

    unique_ptr<Obj, UnreleasedObj> clone(roxal::ptr<CloneContext> ctx) const override;
    unique_ptr<Obj, UnreleasedObj> shallowClone() const override;

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;

    void trace(ValueVisitor& visitor) const override { (void)visitor; }

private:
    ptr<Eigen::Quaterniond> quat_;
    void ensureUnique() {
        if (quat_.use_count() > 1)
            quat_ = make_ptr<Eigen::Quaterniond>(*quat_);
    }
};

inline bool isOrient(const Value& v) { return isObjType(v, ObjType::Orient); }
inline ObjOrient* asOrient(const Value& v) { return static_cast<ObjOrient*>(v.asObj()); }

unique_ptr<ObjOrient, UnreleasedObj> newOrientObj();
unique_ptr<ObjOrient, UnreleasedObj> newOrientObj(const Eigen::Quaterniond& q);

std::string objOrientToString(const ObjOrient* oo);


//
// tensor

enum class TensorDType : uint8_t {
    Float16 = 0,
    Float32 = 1,
    Float64 = 2,   // default, matches double storage
    Int8    = 3,
    Int16   = 4,
    Int32   = 5,
    Int64   = 6,
    UInt8   = 7,
    Bool    = 8
};

std::string to_string(TensorDType dtype);
TensorDType tensorDTypeFromString(const std::string& s);

struct ObjTensor : public Obj
{
    ObjTensor();
    ObjTensor(const std::vector<int64_t>& shape, TensorDType dtype = TensorDType::Float64);
    virtual ~ObjTensor() {}

#ifdef ROXAL_ENABLE_ONNX
    /// Construct from an ONNX Runtime value (takes ownership, zero-copy).
    /// Shape and dtype are read from the Ort::Value.
    explicit ObjTensor(Ort::Value&& ortValue);

    /// True when the tensor data lives inside an Ort::Value.
    bool isOrtBacked() const { return ort_value_ != nullptr; }

    /// Return a const reference to the underlying Ort::Value.
    /// Throws if not ORT-backed.
    const Ort::Value& ortValue() const;

    /// Return a mutable Ort::Value (e.g. for pre-allocated inference output).
    /// Triggers COW if shared.
    Ort::Value& ortValueMut();

    /// True when the underlying Ort::Value resides in GPU memory.
    bool isOnGpu() const;

    /// If the tensor is on GPU, copy data to CPU (lazy materialization).
    /// Logically const — tensor value unchanged, only memory location.
    void ensureCpu() const;
#else
    bool isOrtBacked() const { return false; }
#endif

    // Shape and dtype accessors
    const std::vector<int64_t>& shape() const { return shape_; }
    int64_t rank() const { return static_cast<int64_t>(shape_.size()); }
    int64_t numel() const;  // total number of elements
    TensorDType dtype() const { return dtype_; }

    // Element access (multi-dimensional indexing)
    Value index(const std::vector<Value>& indices) const;
    void setIndex(const std::vector<Value>& indices, const Value& v);

    // Single flat index access - works with both native and ORT storage.
    // Reads from ORT buffer directly without copying when ORT-backed.
    double at(int64_t flatIdx) const;
    void setAt(int64_t flatIdx, double v);

    // Reshape (returns new tensor)
    Value reshape(const std::vector<int64_t>& newShape) const;

    // Raw data pointer access (COW).
    // For ORT-backed tensors this returns the ORT buffer directly
    // (only valid when dtype is Float64; use at() for type-safe access).
    const double* data() const;
    double* dataMut();

    // Type-erased raw data access for bulk I/O (e.g., image read/write).
    // Works with any dtype. For ORT-backed tensors returns ORT buffer directly.
    const void* rawData() const;
    void* rawDataMut();  // triggers COW

    bool equals(const ObjTensor* other, double eps = 1e-15) const;
    void set(const ObjTensor* other);

    unique_ptr<Obj, UnreleasedObj> clone(roxal::ptr<CloneContext> ctx) const override;
    unique_ptr<Obj, UnreleasedObj> shallowClone() const override;

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;

    void trace(ValueVisitor& visitor) const override { (void)visitor; }  // no Value refs

private:
    std::vector<int64_t> shape_;
    std::vector<int64_t> strides_;
    TensorDType dtype_ = TensorDType::Float64;

#ifdef ROXAL_ENABLE_ONNX
    // ORT-backed storage.  Wrapped in shared_ptr for COW (Ort::Value is move-only).
    // When non-null, this is the authoritative data source.
    // Mutable so ensureCpu() can lazily materialize a CPU copy while remaining
    // logically const (the tensor value is unchanged).
    mutable std::shared_ptr<Ort::Value> ort_value_;

    /// COW guard for the Ort::Value (deep-copies if shared).
    void ensureOrtUnique();
#else
    // Native storage (used when ONNX Runtime is not available)
    ptr<std::vector<double>> data_;

    void ensureUnique() {
        if (data_.use_count() > 1)
            data_ = make_ptr<std::vector<double>>(*data_);
    }
#endif

    void computeStrides();
    int64_t flatIndex(const std::vector<int64_t>& indices) const;
};

inline bool isTensor(const Value& v) { return isObjType(v, ObjType::Tensor); }
inline ObjTensor* asTensor(const Value& v) { return static_cast<ObjTensor*>(v.asObj()); }

/// @brief Clone values with value semantics that require explicit cloning.
/// @param v The value to potentially clone.
/// @return A cloned value if v is an object with value semantics (vector/matrix/tensor),
///         otherwise returns v unchanged. Primitives and immutable objects (strings) don't need cloning.
/// @note Clone is cheap for vector/matrix/tensor due to copy-on-write (COW) -
///       data is shared until mutation, when it's copied lazily.
inline Value cloneIfValueSemantics(const Value& v) {
    // Only clone mutable objects that should have value semantics
    // Primitives are copied by value naturally, ObjPrimitive (strings) are immutable
    // Note: clone() uses COW for vector/matrix/tensor - data is shared, not copied
    // nullptr context is fine here - value semantics types (vector/matrix/tensor) don't need cycle tracking
    if (v.isObj() && v.valueSemantics() && !isObjPrimitive(v)) {
        return Value(v.asObj()->clone(nullptr));
    }
    return v;
}

unique_ptr<ObjTensor, UnreleasedObj> newTensorObj();
unique_ptr<ObjTensor, UnreleasedObj> newTensorObj(const std::vector<int64_t>& shape,
                                                   TensorDType dtype = TensorDType::Float64);
unique_ptr<ObjTensor, UnreleasedObj> newTensorObj(const std::vector<int64_t>& shape,
                                                   const std::vector<double>& data,
                                                   TensorDType dtype = TensorDType::Float64);
#ifdef ROXAL_ENABLE_ONNX
/// Create a tensor that takes ownership of an Ort::Value (zero-copy).
unique_ptr<ObjTensor, UnreleasedObj> newTensorObj(Ort::Value&& ortValue);
#endif

std::string objTensorToString(const ObjTensor* ot);


//
// signal (dataflow signal wrapper)

struct ObjSignal : public Obj {
    ObjSignal(ptr<df::Signal> s);
    virtual ~ObjSignal();
    ObjEventType* ensureChangeEventType();
    ptr<df::Signal> signal;
    df::DataflowEngine* engine;
    // Lazily initialized `SignalChanged` event type shared by all emissions.
    Value changeEventType;
    weak_ptr<df::Signal> changeEventSignal;
    bool changeEventUsesTimeSpan = false;

    // Optional callback invoked when signal.stop() is called (for gRPC streaming)
    std::function<void()> onStopCallback;

    unique_ptr<Obj, UnreleasedObj> clone(roxal::ptr<CloneContext> ctx) const override;

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;

    void trace(ValueVisitor& visitor) const override;
    void dropReferences() override;
};

inline bool isSignal(const Value& v) { return isObjType(v, ObjType::Signal); }
inline ObjSignal* asSignal(const Value& v) { return static_cast<ObjSignal*>(v.asObj()); }

unique_ptr<ObjSignal, UnreleasedObj> newSignalObj(ptr<df::Signal> s);
std::string objSignalToString(const ObjSignal* os);


//
// event types and instances

struct ObjEventType : public Obj {
    struct PayloadProperty {
        icu::UnicodeString name;
        Value type;
        Value initialValue;
    };

    struct PayloadPropertyView {
        size_t index;
        const PayloadProperty* property;
        uint16_t hash15;
    };

    explicit ObjEventType(const icu::UnicodeString& name);
    virtual ~ObjEventType() {}

    icu::UnicodeString name;
    Value superType;
    std::vector<PayloadProperty> payloadProperties;
    std::unordered_map<int32_t, size_t> propertyLookup;

    // list of subscribed handler closures (weak references)
    std::vector<Value> subscribers;

    std::vector<PayloadPropertyView> orderedPayloadProperties() const;
    std::optional<PayloadPropertyView> findPayloadPropertyByHash15(uint16_t hash15,
                                                                   bool& ambiguous) const;

    unique_ptr<Obj, UnreleasedObj> clone(roxal::ptr<CloneContext> ctx) const override;

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;

    void trace(ValueVisitor& visitor) const override;
    void dropReferences() override;
};

struct ObjEventInstance : public Obj {
    explicit ObjEventInstance(const Value& eventType);
    virtual ~ObjEventInstance() {}

    Value typeHandle;
    std::unordered_map<int32_t, Value> payload;  // keyed by property name hash

    unique_ptr<Obj, UnreleasedObj> clone(roxal::ptr<CloneContext> ctx) const override;

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;

    void trace(ValueVisitor& visitor) const override;
    void dropReferences() override;
};

inline bool isEventType(const Value& v) { return isObjType(v, ObjType::EventType); }
inline ObjEventType* asEventType(const Value& v) { return static_cast<ObjEventType*>(v.asObj()); }

inline bool isEventInstance(const Value& v) { return isObjType(v, ObjType::EventInstance); }
inline ObjEventInstance* asEventInstance(const Value& v) { return static_cast<ObjEventInstance*>(v.asObj()); }

unique_ptr<ObjEventType, UnreleasedObj> newEventTypeObj(const icu::UnicodeString& name,
                                                         Value superType = Value::nilVal());
unique_ptr<ObjEventInstance, UnreleasedObj> newEventInstanceObj(const Value& eventType,
                                                                std::unordered_map<int32_t, Value> payload = {});
std::string objEventTypeToString(const ObjEventType* ev);
std::string objEventInstanceToString(const ObjEventInstance* ev);


//
// dynamic library handle

struct ObjLibrary : public Obj {
    ObjLibrary(void* h) : handle(h) { type = ObjType::Library; }
    virtual ~ObjLibrary();
    void* handle;

    unique_ptr<Obj, UnreleasedObj> clone(roxal::ptr<CloneContext> ctx) const override;

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;

    void trace(ValueVisitor& visitor) const override { (void)visitor; }
};

inline bool isLibrary(const Value& v) { return isObjType(v, ObjType::Library); }
inline ObjLibrary* asLibrary(const Value& v) { return static_cast<ObjLibrary*>(v.asObj()); }

unique_ptr<ObjLibrary, UnreleasedObj> newLibraryObj(void* handle);
std::string objLibraryToString(const ObjLibrary* lib);

//
// opaque foreign pointer

struct ObjForeignPtr : public Obj {
    ObjForeignPtr(void* p) : ptr(p) { type = ObjType::ForeignPtr; }
    virtual ~ObjForeignPtr() { if (cleanup) cleanup(ptr); }

    void registerCleanup(std::function<void(void*)> fn) { cleanup = std::move(fn); }

    void* ptr;
    std::function<void(void*)> cleanup;

    unique_ptr<Obj, UnreleasedObj> clone(roxal::ptr<CloneContext> ctx) const override;

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;

    void trace(ValueVisitor& visitor) const override { (void)visitor; }
};

inline bool isForeignPtr(const Value& v) { return isObjType(v, ObjType::ForeignPtr); }
inline ObjForeignPtr* asForeignPtr(const Value& v) { return static_cast<ObjForeignPtr*>(v.asObj()); }

unique_ptr<ObjForeignPtr, UnreleasedObj> newForeignPtrObj(void* ptr);
std::string objForeignPtrToString(const ObjForeignPtr* fp);


//
// file handle

struct ObjFile : public Obj {
    ObjFile(roxal::ptr<std::fstream> f, bool binary = false)
        : file(std::move(f)), binary(binary) { type = ObjType::File; }
    virtual ~ObjFile() {
        std::lock_guard<std::mutex> lock(mutex);
        if (file && file->is_open()) file->close();
    }
    roxal::ptr<std::fstream> file;
    bool binary;
    mutable std::mutex mutex;

    unique_ptr<Obj, UnreleasedObj> clone(roxal::ptr<CloneContext> ctx) const override;

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;

    void trace(ValueVisitor& visitor) const override { (void)visitor; }
};

inline bool isFile(const Value& v) { return isObjType(v, ObjType::File); }
inline ObjFile* asFile(const Value& v) { return static_cast<ObjFile*>(v.asObj()); }

unique_ptr<ObjFile, UnreleasedObj> newFileObj(roxal::ptr<std::fstream> f, bool binary = false);
std::string objFileToString(const ObjFile* f);



//
// exception object

struct ObjException : public Obj {
    ObjException() { type = ObjType::Exception; }
    ObjException(Value msg,
                 Value exType = Value::nilVal(),
                 Value st = Value::nilVal(),
                 Value detail = Value::nilVal())
        : message(msg)
        , exType(exType)
        , stackTrace(st)
        , detail(detail)
    {
        type = ObjType::Exception;
    }
    virtual ~ObjException() {}

    Value message;
    Value exType; // object type of the exception

    Value stackTrace; // list of stack frames when raised
    Value detail;     // auxiliary data provided by native code

    unique_ptr<Obj, UnreleasedObj> clone(roxal::ptr<CloneContext> ctx) const override;

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;

    void trace(ValueVisitor& visitor) const override;
    void dropReferences() override;
};

inline bool isException(const Value& v) { return isObjType(v, ObjType::Exception); }
inline ObjException* asException(const Value& v) { return static_cast<ObjException*>(v.asObj()); }

unique_ptr<ObjException, UnreleasedObj> newExceptionObj(Value message = Value::nilVal(),
                                                        Value exType = Value::nilVal(),
                                                        Value stackTrace = Value::nilVal(),
                                                        Value detail = Value::nilVal());
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

// Info for functions declared in .rox with C++ native implementation attached via link()
struct BuiltinFuncInfo {
    NativeFn function;
    std::vector<Value> defaultValues;
    uint32_t resolveArgMask {0};  // bit N set → resolve future arg N before native call
    bool noMutateSelf {false};    // method doesn't mutate receiver state
    uint32_t noMutateArgs {0};    // bitmask: bit N set → arg N not mutated
    bool resolveReturn {false};   // if true, resolve returned future before caller resumes

    BuiltinFuncInfo() = default;
    BuiltinFuncInfo(NativeFn fn, std::vector<Value> defaults = {}, uint32_t mask = 0,
                    bool noMutateSelf_ = false, uint32_t noMutateArgs_ = 0)
        : function(fn), defaultValues(std::move(defaults)), resolveArgMask(mask),
          noMutateSelf(noMutateSelf_), noMutateArgs(noMutateArgs_) {}
};

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
    unique_ptr<BuiltinFuncInfo> builtinInfo;  // non-null when C++ impl attached via link()

    bool strict;        // true if function was compiled in strict mode

    FunctionType fnType { FunctionType::Function };
    Value ownerType { Value::nilVal() }; // weak ref owning type
    ast::Access access { ast::Access::Public };
    bool isImplicit { false }; // user-defined implicit conversion (init or operator T())

    // for parameters with default values that must be re-evaluated on each call
    //  this is map from param name UnicodeString::hashCode() -> Value ObjFunction
    //  (where ObjFunction is a function that takes no params and returns the default value)
    std::map<int32_t, Value> paramDefaultFunc;

    Value moduleType; // ObjModuleType

    void clear(); // reset to blank without other reference values

    unique_ptr<Obj, UnreleasedObj> clone(roxal::ptr<CloneContext> ctx) const override;

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;

    void trace(ValueVisitor& visitor) const override;
    void dropReferences() override;
};

inline bool isFunction(const Value& v) { return isObjType(v, ObjType::Function); }
inline ObjFunction* asFunction(const Value& v) {
    debug_assert_msg(isFunction(v), "Value is an ObjFunction");
    return static_cast<ObjFunction*>(v.asObj());
}


inline unique_ptr<ObjFunction, UnreleasedObj> newFunctionObj(const icu::UnicodeString& name,
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

    unique_ptr<Obj, UnreleasedObj> clone(roxal::ptr<CloneContext> ctx) const override;

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;

    void trace(ValueVisitor& visitor) const override;
    void dropReferences() override;
};

inline bool isUpvalue(const Value& v) { return isObjType(v, ObjType::Upvalue); }
inline ObjUpvalue* asUpvalue(const Value& v) { return static_cast<ObjUpvalue*>(v.asObj()); }

inline unique_ptr<ObjUpvalue, UnreleasedObj> newUpvalueObj(Value* v) {
    #ifdef DEBUG_BUILD
    return newObj<ObjUpvalue>(__func__,__FILE__,__LINE__,v);
    #else
    return newObj<ObjUpvalue>(v);
    #endif
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

    unique_ptr<Obj, UnreleasedObj> clone(roxal::ptr<CloneContext> ctx) const override;

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;

    void trace(ValueVisitor& visitor) const override;
    void dropReferences() override;
};

inline bool isClosure(const Value& v) { return isObjType(v, ObjType::Closure); }
inline ObjClosure* asClosure(const Value& v) {
    debug_assert_msg(isClosure(v), "Value is an ObjClosure");
    return static_cast<ObjClosure*>(v.asObj());
}

inline unique_ptr<ObjClosure, UnreleasedObj> newClosureObj(Value function) { // ObjFunction
    debug_assert_msg(isFunction(function), "Value is an ObjFunction");
    #ifdef DEBUG_BUILD
    return newObj<ObjClosure>(toUTF8StdString(asFunction(function)->name),__FILE__,__LINE__,function);
    #else
    return newObj<ObjClosure>(function);
    #endif
}



// future

struct ObjFuture : public Obj
{
    ObjFuture(const std::shared_future<Value>& fv,
              ptr<type::Type> promisedType = nullptr)
        : future(fv), promisedType(std::move(promisedType))
    {
        type = ObjType::Future;
    }
    virtual ~ObjFuture() {}

    Value asValue() { return future.valid() ? future.get() : Value::nilVal(); }

    std::shared_future<Value> future;
    ptr<type::Type> promisedType;  // type of the promised value (nullptr = unknown)

    mutable std::mutex waitMutex;
    std::vector<weak_ptr<Thread>> waiters;

    void addWaiter(const ptr<Thread>& t);
    void wakeWaiters();

    unique_ptr<Obj, UnreleasedObj> clone(roxal::ptr<CloneContext> ctx) const override;

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;

    void trace(ValueVisitor& visitor) const override;
    void dropReferences() override;
};

inline bool isFuture(const Value& v) { return isObjType(v, ObjType::Future); }
inline ObjFuture* asFuture(const Value& v) {
    debug_assert_msg(isFuture(v), "Value is an ObjFuture");
    return static_cast<ObjFuture*>(v.asObj());
}

inline unique_ptr<ObjFuture, UnreleasedObj> newFutureObj(const std::shared_future<Value>& fv,
                                                         ptr<type::Type> promisedType = nullptr) {
    #ifdef DEBUG_BUILD
    return newObj<ObjFuture>(__func__, __FILE__,__LINE__,fv, std::move(promisedType));
    #else
    return newObj<ObjFuture>(fv, std::move(promisedType));
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
    uint32_t resolveArgMask {0}; // bit N set → resolve arg N before call

    unique_ptr<Obj, UnreleasedObj> clone(roxal::ptr<CloneContext> ctx) const override;

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;

    void trace(ValueVisitor& visitor) const override;
    void dropReferences() override;
};

inline bool isNative(const Value& v) { return isObjType(v, ObjType::Native); }
inline ObjNative* asNative(const Value& v) { return static_cast<ObjNative*>(v.asObj()); }

unique_ptr<ObjNative, UnreleasedObj> newNativeObj(NativeFn function, void* data=nullptr,
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

    unique_ptr<Obj, UnreleasedObj> clone(roxal::ptr<CloneContext> ctx) const override;

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;

    void trace(ValueVisitor& visitor) const override { (void)visitor; }
};

inline bool isTypeSpec(const Value& v) { return isObjType(v,ObjType::Type); }
inline ObjTypeSpec* asTypeSpec(const Value& v) { return static_cast<ObjTypeSpec*>(v.asObj()); }

unique_ptr<ObjTypeSpec, UnreleasedObj> newTypeSpecObj(ValueType t); // primitive

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
        bool isConst { false };
        Value ownerType { Value::nilVal() }; // weak ref to owning type
        std::optional<icu::UnicodeString> ctype;
    };
    std::unordered_map<int32_t, Property> properties;
    std::vector<int32_t> propertyOrder;

    struct PublicPropertyView {
        int32_t key;
        const Property* property;
        uint16_t hash15;
    };

    std::vector<PublicPropertyView> orderedPublicProperties() const;
    std::optional<PublicPropertyView> findPublicPropertyByHash15(uint16_t hash15,
                                                                 bool& ambiguous) const;

    struct Method {
        icu::UnicodeString name;
        Value closure;
        ast::Access access { ast::Access::Public };
        bool isImplicit { false };
        Value ownerType { Value::nilVal() }; // weak ref to owning type
    };
    std::unordered_map<int32_t, Method> methods;

    // name -> value
    std::unordered_map<int32_t, std::pair<icu::UnicodeString, Value>> enumLabelValues;

    // nested type declarations
    struct NestedType {
        icu::UnicodeString name;
        Value type;
        ast::Access access { ast::Access::Public };
    };
    std::unordered_map<int32_t, NestedType> nestedTypes;

    // global enum type id -> ObjObjectType
    //  TODO: make thread safe?
    static std::unordered_map<uint16_t, ObjObjectType*> enumTypes;

    unique_ptr<Obj, UnreleasedObj> clone(roxal::ptr<CloneContext> ctx) const override;

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;

    void trace(ValueVisitor& visitor) const override;
    void dropReferences() override;
};


inline bool isObjectType(const Value& v) { return isObjType(v, ObjType::Type) && ((asTypeSpec(v)->typeValue == ValueType::Object) || (asTypeSpec(v)->typeValue == ValueType::Actor)); }
inline ObjObjectType* asObjectType(const Value& v) { return static_cast<ObjObjectType*>(v.asObj()); }

// Check if sourceType is the same type as targetType or a subtype (extends/implements).
// Used by Value::is() and future promised-type assignability checks.
inline bool isSubtypeOf(ObjObjectType* sourceType, ObjObjectType* targetType) {
    ObjObjectType* t = sourceType;
    while (t) {
        if (t == targetType) return true;
        if (t->superType.isNil()) break;
        t = asObjectType(t->superType);
    }
    return false;
}

inline bool isEnumType(const Value& v) { return isObjType(v, ObjType::Type) && asTypeSpec(v)->typeValue == ValueType::Enum; }

unique_ptr<ObjObjectType, UnreleasedObj> newObjectTypeObj(const icu::UnicodeString& typeName, bool isActor, bool isInterface = false, bool isEnumeration = false);


struct ObjPackageType : public ObjTypeSpec
{
    // TODO

    unique_ptr<Obj, UnreleasedObj> clone(roxal::ptr<CloneContext> ctx) const override;

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;

    void trace(ValueVisitor& visitor) const override { (void)visitor; }
};

struct ObjModuleType : public ObjTypeSpec
{
    ObjModuleType(const icu::UnicodeString& typeName);

    virtual ~ObjModuleType();

    icu::UnicodeString name;
    icu::UnicodeString fullName;
    icu::UnicodeString sourcePath;

    // variables declared at runtime via VM OpCode::DefineModuleVar
    VariablesMap vars;
    std::unordered_set<int32_t> constVars;

    void registerModuleAlias(const icu::UnicodeString& alias,
                             const icu::UnicodeString& moduleFullName);
    std::vector<std::pair<icu::UnicodeString, icu::UnicodeString>> moduleAliasSnapshot() const;
    icu::UnicodeString moduleAliasFullName(const icu::UnicodeString& alias) const;
    void clearModuleAliases();

    // Registered literal suffixes: suffix string -> function name
    // Populated during compilation of @suffix-annotated functions.
    std::unordered_map<icu::UnicodeString, icu::UnicodeString> registeredSuffixes;

    // cstruct type annotations: type name hash -> arch (32 or 64)
    std::unordered_map<int32_t, int> cstructArch;
    // property ctype annotations: type name hash -> (prop name hash -> ctype)
    std::unordered_map<int32_t, std::unordered_map<int32_t, icu::UnicodeString>> propertyCTypes;

    static atomic_vector<Value> allModules;

    unique_ptr<Obj, UnreleasedObj> clone(roxal::ptr<CloneContext> ctx) const override;

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;

    void trace(ValueVisitor& visitor) const override;
    void dropReferences() override;

private:
    std::unordered_map<int32_t, std::pair<icu::UnicodeString, icu::UnicodeString>> moduleAliases;
};

inline bool isModuleType(const Value& v) { return isObjType(v, ObjType::Type) && (asTypeSpec(v)->typeValue == ValueType::Module); }
inline ObjModuleType* asModuleType(const Value& v) { return static_cast<ObjModuleType*>(v.asObj()); }

unique_ptr<ObjModuleType, UnreleasedObj> newModuleTypeObj(const icu::UnicodeString& typeName);





//
// object instance

struct ObjectInstance : public Obj
{
    using PropertyMap = std::unordered_map<int32_t, VariablesMap::MonitoredValue>;

    ObjectInstance(const Value& objectType);
    virtual ~ObjectInstance();

    Value instanceType;

    // Property access by hash
    // Non-const version triggers ensureUnique() because callers may write through
    // the returned pointer (e.g. resolveConstChild caching).
    VariablesMap::MonitoredValue* findProperty(int32_t hash) {
        ensureUnique();
        auto it = properties_->find(hash);
        return (it != properties_->end()) ? &it->second : nullptr;
    }
    const VariablesMap::MonitoredValue* findProperty(int32_t hash) const {
        auto it = properties_->find(hash);
        return (it != properties_->end()) ? &it->second : nullptr;
    }
    bool hasProperty(int32_t hash) const { return properties_->contains(hash); }
    // Returns a reference to the slot, creating it if it doesn't exist (like operator[])
    VariablesMap::MonitoredValue& propertySlot(int32_t hash);       // MVCC-guarded
    void assignProperty(int32_t hash, const Value& value);          // MVCC-guarded
    void emplaceProperty(int32_t hash, VariablesMap::MonitoredValue mv);  // MVCC-guarded
    void clearProperties() { ensureUnique(); properties_->clear(); }

    // convenience methods for property access by name (e.g. for builtin method implementations)
    Value getProperty(const icu::UnicodeString& name) const;
    Value getProperty(const std::string& name) const { return getProperty(toUnicodeString(name)); }
    Value getProperty(const char* name) const { return getProperty(toUnicodeString(name)); }
    void setProperty(const icu::UnicodeString& name, Value value);  // MVCC-guarded
    void setProperty(const std::string& name, Value value) { setProperty(toUnicodeString(name), value); }
    void setProperty(const char* name, Value value) { setProperty(toUnicodeString(name), value); }

    Value ensurePropertySignal(int32_t nameHash, const std::string& signalName);
    Value ensurePropertySignal(const icu::UnicodeString& name, const std::string& signalName)
      { return ensurePropertySignal(name.hashCode(), signalName); }

    unique_ptr<Obj, UnreleasedObj> clone(roxal::ptr<CloneContext> ctx) const override;
    unique_ptr<Obj, UnreleasedObj> shallowClone() const override;

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;

    void trace(ValueVisitor& visitor) const override;
    void dropReferences() override;

private:
    ptr<PropertyMap> properties_;
    void ensureUnique() {
        if (properties_.use_count() > 1)
            properties_ = make_ptr<PropertyMap>(*properties_);
    }
};

inline bool isObjectInstance(const Value& v) { return isObjType(v, ObjType::Instance); }
inline ObjectInstance* asObjectInstance(const Value& v) { return static_cast<ObjectInstance*>(v.asObj()); }

unique_ptr<ObjectInstance, UnreleasedObj> newObjectInstance(const Value& objectType);

/// Quantity extraction for vector construction and orient.
/// If v is a sys.quantity object, extracts the SI canonical value into siValue
/// and the 4-element dimension vector into dims, returning true.
/// If v is a bare zero (int or real == 0), returns true with siValue=0 and dims left unchanged
/// (compatible with any dimension).
/// Otherwise returns false.
bool tryExtractQuantity(const Value& v, double& siValue, std::array<int32_t,4>& dims, bool& isDimensioned);


//
// actor instance

struct ActorInstance : public Obj
{
    struct UninitializedTag {};

    ActorInstance(UninitializedTag);
    ActorInstance(const Value& objectType);
    virtual ~ActorInstance();

    void initialize(const Value& objectType);

    Value instanceType;

    // Property access by hash (same API as ObjectInstance)
    VariablesMap::MonitoredValue* findProperty(int32_t hash) {
        auto it = properties.find(hash);
        return (it != properties.end()) ? &it->second : nullptr;
    }
    const VariablesMap::MonitoredValue* findProperty(int32_t hash) const {
        auto it = properties.find(hash);
        return (it != properties.end()) ? &it->second : nullptr;
    }
    bool hasProperty(int32_t hash) const { return properties.contains(hash); }
    VariablesMap::MonitoredValue& propertySlot(int32_t hash);       // MVCC-guarded
    void assignProperty(int32_t hash, const Value& value);          // MVCC-guarded
    void emplaceProperty(int32_t hash, VariablesMap::MonitoredValue mv);  // MVCC-guarded
    void clearProperties() { properties.clear(); }

    Value ensurePropertySignal(int32_t nameHash, const std::string& signalName);

    // Returns a future resolved with the queued method's result, or nil for proc methods
    Value queueCall(const Value& callee, const CallSpec& callSpec, Value* argsStackTop,
                    bool forceCompletionFuture = false);


    // queue of actor method calls
    //  each is a callee and parameters
    // (serviced in thread created by Thread::act() )
    struct MethodCallInfo {
        Value callee;
        std::vector<Value> args;
        ptr<std::promise<Value>> returnPromise;
        Value returnFuture;
        CallSpec callSpec;
#ifdef ROXAL_COMPUTE_SERVER
        struct PrintTarget {
            enum class Kind : std::uint8_t {
                LocalStdout,
                RemoteCall
            };

            Kind kind { Kind::LocalStdout };
            weak_ptr<ComputeConnection> remoteConn;
            uint64_t remoteCallId { 0 };

            static PrintTarget localStdout() { return {}; }

            static PrintTarget remoteCall(const ptr<ComputeConnection>& conn, uint64_t callId) {
                PrintTarget target;
                target.kind = Kind::RemoteCall;
                target.remoteConn = conn;
                target.remoteCallId = callId;
                return target;
            }

            bool routesRemotely() const { return kind == Kind::RemoteCall; }
        };

        PrintTarget printTarget;
#endif

        bool valid() const { return !callee.isNil(); }
    };
    atomic_queue<MethodCallInfo> callQueue;

    std::mutex queueMutex;
    std::condition_variable queueConditionVar;

    std::thread::id thread_id;
    weak_ptr<Thread> thread;
    std::atomic<bool> alive{false}; // true while actor OS thread is running; guards queueCall

#ifdef ROXAL_COMPUTE_SERVER
    bool isRemote { false };
    int64_t remoteActorId { 0 };
    weak_ptr<ComputeConnection> remoteConn;
    // Keep the transport alive until a connection cache/registry exists.
    ptr<ComputeConnection> remoteConnHold;
#endif

    unique_ptr<Obj, UnreleasedObj> clone(roxal::ptr<CloneContext> ctx) const override { throw std::runtime_error("cannot clone actor instances"); }

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;

    void trace(ValueVisitor& visitor) const override;
    void dropReferences() override;

private:
    std::unordered_map<int32_t, VariablesMap::MonitoredValue> properties;
};

inline bool isActorInstance(const Value& v) { return isObjType(v, ObjType::Actor); }
inline ActorInstance* asActorInstance(const Value& v) { return static_cast<ActorInstance*>(v.asObj()); }

unique_ptr<ActorInstance, UnreleasedObj> newActorInstance(const Value& objectType);
unique_ptr<ActorInstance, UnreleasedObj> newActorInstance(ActorInstance::UninitializedTag);

#ifdef ROXAL_COMPUTE_SERVER
Value makeRemoteActor(const Value& actorType, int64_t remoteId, ptr<ComputeConnection> conn);
#endif



//
// method closure bound to object|actor instance

struct ObjBoundMethod : public Obj
{
    ObjBoundMethod(const Value& instance, const Value& closure);
    virtual ~ObjBoundMethod();

    Value receiver;
    Value method;

    unique_ptr<Obj, UnreleasedObj> clone(roxal::ptr<CloneContext> ctx) const override;

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;

    void trace(ValueVisitor& visitor) const override;
    void dropReferences() override;
};

inline bool isBoundMethod(const Value& v) { return isObjType(v, ObjType::BoundMethod); }
inline ObjBoundMethod* asBoundMethod(const Value& v) { return static_cast<ObjBoundMethod*>(v.asObj()); }

inline unique_ptr<ObjBoundMethod, UnreleasedObj> newBoundMethodObj(const Value& instance, const Value& closure) {
#ifdef DEBUG_BUILD
    return newObj<ObjBoundMethod>(__func__, __FILE__, __LINE__, instance, closure);
#else
    return newObj<ObjBoundMethod>(instance, closure);
#endif
}

//
// native method bound to builtin instance

struct ObjBoundNative : public Obj
{
    ObjBoundNative(const Value& instance, NativeFn fn, bool proc = false,
                   ptr<roxal::type::Type> funcType=nullptr,
                   std::vector<Value> defaults = {},
                   Value declFunction = Value::nilVal())
      : receiver(instance), function(fn), isProc(proc),
        funcType(funcType), defaultValues(std::move(defaults)),
        declFunction(declFunction) { type = ObjType::BoundNative; }
    virtual ~ObjBoundNative() {}

    Value receiver;
    NativeFn function;
    bool isProc;  // true for proc methods, false for func methods
    ptr<roxal::type::Type> funcType;
    std::vector<Value> defaultValues;
    Value declFunction; // ObjFunction for default arg expressions

    unique_ptr<Obj, UnreleasedObj> clone(roxal::ptr<CloneContext> ctx) const override;

    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;

    void trace(ValueVisitor& visitor) const override;
    void dropReferences() override;
};

inline bool isBoundNative(const Value& v) { return isObjType(v, ObjType::BoundNative); }
inline ObjBoundNative* asBoundNative(const Value& v) { return static_cast<ObjBoundNative*>(v.asObj()); }
inline unique_ptr<ObjBoundNative, UnreleasedObj> newBoundNativeObj(const Value& instance, NativeFn fn, bool isProc = false,
                                         ptr<roxal::type::Type> funcType=nullptr,
                                         std::vector<Value> defaults = {},
                                         Value declFunction = Value::nilVal()) {
    #ifdef DEBUG_BUILD
    return newObj<ObjBoundNative>(__func__, __FILE__, __LINE__, instance, fn, isProc, funcType, std::move(defaults), declFunction);
    #else
    return newObj<ObjBoundNative>(instance, fn, isProc, funcType, std::move(defaults), declFunction);
    #endif
}






#ifdef DEBUG_BUILD
void testObjectValues();
#endif



} // namespace
