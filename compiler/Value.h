#pragma once
#include <atomic>
#include <optional>
#include <unordered_map>
#include <functional>
#include <vector>
#include <tuple>

#include <core/common.h>

namespace roxal {



enum class ValueType {
    // Values used for NAN type tag:
    Nil   = 1, // 0001 (both a type and singular value)
    // Bool False 0010
    // Bool True  0011
    Bool  = 4, // 0100
    Byte,      // 0101
    Int,       // 0110
    Real,      // 0111
    Decimal,   // 1000
    Enum,      // 1001
    Type,      // 1010

    // Obj (pointer) types
    String,
    Range,
    List,
    Dict,
    Vector,
    Matrix,
    Tensor,
    Orient,
    Object,
    Actor,
    Module,
    Boxed = 0xff // not used with NAN tagging
};

std::string to_string(ValueType t);


struct Obj;
struct ObjString;


// If Values are real (IEEE C++ double), then no bit conversions necessary
//  IEEE Quiet NAN values are used to place type tag in mantissa (bits 46..49)
//  If type is nil or bool tag is also literal value
//  If type is other value types, value is stored in lower bits (max 32bits)
//  If type if object, sign bit is set and pointer is stored in lower 48 bits
//  The enum type is an exception - it is a value type with complex type information:
//   the lower 32bits are divided into lower 16bits holding the enum int16_t value and the
//   upper 16bits holding an enum type id, used to lookup the enum type information from a global table

// All IEEE doubles are Quiet NANs if these bits are all 1:
const uint64_t QNAN = ((uint64_t)0x7ffc000000000000);
const uint64_t SignBit = ((uint64_t)0x8000000000000000);

const int TypeTagOffset = 46;
const uint64_t TypeTag = uint64_t(0xf) << TypeTagOffset;


// Use type-tag as literal value for nil, true and false
const uint64_t TagNil   = uint64_t(ValueType::Nil) << TypeTagOffset; // 0001
const uint64_t TagFalse = uint64_t(2) << TypeTagOffset; // 0010
const uint64_t TagTrue  = uint64_t(3) << TypeTagOffset; // 0011
const uint64_t TagByte  = uint64_t(ValueType::Byte) << TypeTagOffset;
const uint64_t TagInt   = uint64_t(ValueType::Int) << TypeTagOffset;
const uint64_t TagDecimal = uint64_t(ValueType::Decimal) << TypeTagOffset;
const uint64_t TagEnum  = uint64_t(ValueType::Enum) << TypeTagOffset;
const uint64_t TagType  = uint64_t(ValueType::Type) << TypeTagOffset;


//ValueType valueType(ValueType t); // value type (ignoring boxing - so primitive type if boxed)

class Value;
bool isObjPrimitive(const Value& v); // forward from Object.h



class Value {
public:
    /// @brief Default constructor. Initializes the value to Nil.
    Value() : val(QNAN | TagNil) {}

    /// @brief Constructs a boolean value.
    /// @param b The boolean value.
    explicit Value(bool b) { val = b ? (QNAN | TagTrue) : (QNAN | TagFalse); }


    /// @brief Constructs a byte value.
    /// @param b The byte value.
    explicit Value(uint8_t b) { val = QNAN | TagByte | (0xff & *reinterpret_cast<uint8_t*>(&b)); }

    /// @brief Constructs a real value.
    /// @param r The real value.
    explicit Value(double r) { val = (*reinterpret_cast<uint64_t*>(&r)); }

    /// @brief Constructs an integer value.
    /// @param i The integer value.
    explicit Value(int32_t i) { val = QNAN | TagInt | (0xffffffff & *reinterpret_cast<uint64_t*>(&i)); }

    /// @brief Constructs a value of the specified builtin type.
    /// @param bt The builtin type.
    explicit Value(ValueType bt) { val = QNAN | TagType | uint64_t(bt); }

    /// @brief Constructs a value from an object pointer.
    /// @param o The object pointer.
    explicit Value(Obj* o);
    explicit Value(int16_t enumLabelValue, uint16_t enumTypeId)
        { val = QNAN | TagEnum | (0xffffffff & (enumLabelValue | (uint64_t(enumTypeId) << 16))); }


    /// @brief Copy constructor.
    /// @param v The value to copy.
    Value(const Value& v)
    {
        val.store(v.val.load());
        if (isObj() || isBoxed())
            incRefObj();
    }

    /// @brief Assignment operator.
    /// @param v The value to assign.
    /// @return The assigned value.
    Value& operator=(const Value& v)
    {
        if (isObj() || isBoxed())
            decRefObj();

        val.store(v.val.load());
        if (isObj() || isBoxed())
            incRefObj();

        return *this;
    }


    /// @brief Destructor. Will decrement the reference count of objects.
    ~Value() {
        if (isObj() || isBoxed())
            decRefObj();
    }


    /// @brief Boxes the value into an object if it is a primitive type.
    void box();

    /// @brief Unboxes the value from an object if it is boxed.
    void unbox();

    /// @brief Checks if the value is boxed.
    /// @return True if the value is boxed, false otherwise.
    bool isBoxed() const { return isObj() && isObjPrimitive(*this); }

    /// @brief Checks if the value is boxable (primitive type that can be boxed).
    /// @return True if the value is boxable, false otherwise.
    bool isBoxable() const { return !isBoxed() && (isBool() || isInt() || isReal()); }

    /// @brief Retrieves the value type.
    /// @return The value type.
    ValueType type() const;

    /// @brief Retrieves the type name of the value.
    /// @return The type name of the value.
    std::string typeName() const;

    /// @brief Checks if the value is Nil.
    /// @return True if the value is Nil, false otherwise.
    inline bool isNil() const { return val == (QNAN | TagNil); }

    /// @brief Checks if the value is a boolean.
    /// @return True if the value is a boolean, false otherwise.
    inline bool isBool() const { return (val|uint64_t(1)<<TypeTagOffset) == (QNAN | TagTrue); }

    /// @brief Retrieves the value as a boolean.
    /// @param strict If true, performs strict type checking. If false, allows type coercion.
    /// @return The boolean value.
    bool asBool(bool strict=true) const;

    /// @brief Checks if the value is a byte.
    /// @return True if the value is a byte, false otherwise.
    inline bool isByte() const { return (val & (QNAN | TypeTag)) == (QNAN | TagByte); }

    /// @brief Retrieves the value as a byte.
    /// @param strict If true, performs strict type checking. If false, allows type coercion.
    /// @return The byte value.
    uint8_t asByte(bool strict=true) const;

    /// @brief Checks if the value is an integer.
    /// @return True if the value is an integer, false otherwise.
    inline bool isInt() const { return (val & (QNAN | TypeTag)) == (QNAN | TagInt); }

    /// @brief Retrieves the value as an integer.
    /// @param strict If true, performs strict type checking. If false, allows type coercion.
    /// @return The integer value.
    int32_t asInt(bool strict=true) const;

    /// @brief Checks if the value is a real number.
    /// @return True if the value is a real number, false otherwise.
    inline bool isReal() const { return (val&QNAN) != QNAN; }

    /// @brief Retrieves the value as a real number.
    /// @param strict If true, performs strict type checking. If false, allows type coercion.
    /// @return The real value.
    double asReal(bool strict=true) const;

    /// @brief Checks if the value is a number (integer, real, or byte).
    /// @return True if the value is a number, false otherwise.
    inline bool isNumber() const { return isInt() || isReal() || isByte(); } // TODO: || isDecimal(v)

    inline bool isEnum() const { return (val & (QNAN | TypeTag)) == (QNAN | TagEnum); }
    int16_t asEnum() const;
    uint16_t enumTypeId() const {
        #ifdef DEBUG_BUILD
        assert(isEnum());
        #endif
        return uint16_t((val & 0xffff0000) >> 16);
    }

    /// @brief Checks if the value is a type.
    /// @return True if the value is a type, false otherwise.
    inline bool isType() const { return (val & (QNAN | TypeTag)) == (QNAN | TagType); }

    /// @brief Retrieves the value as a type.
    /// @param strict If true, performs strict type checking. If false, allows type coercion.
    /// @return The type value.
    ValueType asType(bool strict=true) const;

    /// @brief Checks if the value is a primitive type (not an object - excludes boxed ObjPrimitive).
    /// @return True if the value is a primitive type, false otherwise.
    inline bool isPrimitive() const {
        return isNil() || isBool() || isInt() || isReal() || isType();
    }

    /// @brief Checks if the value is an object.
    /// @return True if the value is an object, false otherwise.
    inline bool isObj() const { return (val & (QNAN | SignBit)) == (QNAN | SignBit); }

    /// @brief Retrieves the value as an object pointer.
    /// @return The object pointer.
    inline Obj* asObj() const {
        #ifdef DEBUG_BUILD
        assert(isObj());
        #endif
        return (Obj*)(uintptr_t)(val & ~(SignBit | QNAN));
    }


    // @brief if is ObjFuture, block waiting for value (and replace this with value)
    void resolveFuture();

    /// @brief Equality operator.
    /// @param rhs The right-hand side value to compare.
    /// @return True if the values are equal, false otherwise.
    bool operator==(const Value& rhs) const;

    static_assert(sizeof(size_t) >= sizeof(uint64_t), "size_t is not big enough for uint64_t val as hash");

    /// @brief Calculates the hash value of the value.
    /// @return The hash value.
    size_t hash() const {
        return size_t(val.load());
    }

    #ifdef DEBUG_BUILD
    uint64_t getVal() const { return val; }
    static void testPrimitiveValues();
    #endif

    /// @brief Creates a deep copy of the value.
    /// @return The cloned value.
    Value clone() const;

    /// @brief Determine if this value's type can be converted to another type.
    bool convertibleTo(ValueType to, bool strict=true) const;

protected:
    std::atomic_uint64_t val;

    void incRefObj();
    void decRefObj();
};



// value factories
inline Value nilVal() { return Value(); }

inline Value trueVal() { return Value(true); }
inline Value falseVal() { return Value(false); }
inline Value boolVal(bool b) { return Value(b); }

inline Value byteVal(uint8_t b) { return Value(b); }

inline Value intVal(int32_t i) { return Value(i); }

inline Value realVal(double r) { return Value(r); }

inline __attribute__((noinline)) Value enumVal(int16_t labelVal, uint16_t enumTypeId) { return Value(labelVal, enumTypeId); }

inline Value typeVal(ValueType bt) { return Value(bt); }


// create default value for given primitive or builtin
//  (e.g. false:bool. 0:int, 0,0:real, '':string, []:list, {}:dict, nil:object etc )
Value defaultValue(ValueType t);


Value toType(ValueType t, Value v, bool strict=true);

// construct / convert from values (e.g. 'constructor' args) a value of the specified builtin type
//  (for single arg, same as toType(*begin)) - TODO: use C++20 range?
Value construct(ValueType type, std::vector<Value>::const_iterator begin, std::vector<Value>::const_iterator end);


ValueType binaryOpType(Value l, Value r);

// check if a value of type `from` can be converted to `to` according to the
// conversion rules in conversions.md.
bool convertibleTo(ValueType from, ValueType to, bool strict=true);

// run conversion unit tests used by the runtime '_runtests' builtin
std::vector<std::tuple<std::string,bool,std::string>> testConversions();

bool isFalsey(const Value& v);
bool isTruthy(const Value& v);

bool valuesEqual(Value a, Value b);

Value negate(Value v);
Value add(Value l, Value r);
Value subtract(Value l, Value r);
Value multiply(Value l, Value r);
Value divide(Value l, Value r);
Value mod(Value l, Value r);

Value land(Value l, Value r);
Value lor(Value l, Value r);

Value greater(Value l, Value r);
Value less(Value l, Value r);

std::string toString(const Value& v);

std::ostream& operator<<(std::ostream& out, const Value& v);




// map of variables to values
//  (use for each mnodule's variables; also maintains builtin globals)
//  (all inline for performance)
class VariablesMap
{
public:
    VariablesMap() {}

    typedef std::pair<icu::UnicodeString, Value> NameValue;

    void clearGlobals()
    {
        std::lock_guard<std::mutex> lock(globalsLock);
        globals.clear();
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(varsLock);
        vars.clear();
    }

    // module or global exists?
    bool exists(int32_t nameHash) const
    {
        {
            std::lock_guard<std::mutex> lock(varsLock);
            auto it = vars.find(nameHash);
            if (it != vars.end())
                return true;
        }
        {
            std::lock_guard<std::mutex> lock(globalsLock);
            auto it = globals.find(nameHash);
            if (it != vars.end())
                return true;
        }
        return false;
    }

    // module or global exists?
    bool exists(const icu::UnicodeString& name) const
      { return exists(name.hashCode()); }

    // global only exists?
    bool existsGlobal(int32_t nameHash) const {
        std::lock_guard<std::mutex> lock(globalsLock);
        auto it = globals.find(nameHash);
        return (it != vars.end());
    }

    bool existsGlobal(const icu::UnicodeString& name) const
      { return existsGlobal(name.hashCode()); }


    // return either module or global var, or nothing if not found
    std::optional<Value> load(int32_t nameHash) const
    {
        {
            std::lock_guard<std::mutex> lock(varsLock);

            auto it = vars.find(nameHash);
            if (it != vars.end())
                return it->second.second;
        }

        // try globals
        {
            std::lock_guard<std::mutex> lock(globalsLock);

            auto it = globals.find(nameHash);
            if (it != vars.end())
                return it->second.second;
        }
        return {};
    }

    // return either module or global var, or nothing if not found
    std::optional<Value> load(const icu::UnicodeString& name) const
      { return load(name.hashCode()); }


    // store value as var
    //  (module vars only, can't update global builtins)
    // return: was stored (e.g. if exists and overwrite=false, returns false)
    bool store(int32_t nameHash, const icu::UnicodeString& name, const Value& value, bool overwrite=false)
    {
        std::lock_guard<std::mutex> lock(varsLock);
        if (overwrite)
            vars.insert_or_assign(nameHash, std::make_pair(name,value));
        else {
            auto it = vars.find(nameHash);
            if (it != vars.end())
                return false;
            vars.insert({nameHash, std::make_pair(name,value)});
        }
        return true;
    }

    bool store(const icu::UnicodeString& name, const Value& value, bool overwrite=false)
    { return store(name.hashCode(), name, value, overwrite); }

    bool store(const NameValue& nameValue, bool overwrite=false)
    { return store(nameValue.first.hashCode(), nameValue.first, nameValue.second, overwrite); }

    // only store if module var already exists
    // returns if stored (i.e. exists).  globals not considered
    bool storeIfExists(int32_t nameHash, const icu::UnicodeString& name, const Value& value)
    {
        std::lock_guard<std::mutex> lock(varsLock);
        auto it = vars.find(nameHash);
        if (it == vars.end())
            return false;
        it->second.second = value;
        return true;
    }


    // store global var value (will overwrite, module vars not considered)
    void storeGlobal(const icu::UnicodeString& name, const Value& value)
    {
        std::lock_guard<std::mutex> lock(globalsLock);
        globals.insert_or_assign(name.hashCode(), std::make_pair(name,value));
    }

    // iterate over each module var and call f (with const).  globals not considered.
    void forEach(std::function<void(const NameValue&)> f)
    {
        std::lock_guard<std::mutex> lock(varsLock);
        for(const auto& entry : vars) {
            try {
                f(entry.second);
            }
            catch (...) {}
        }
    }

    // list of module variable names (globals not considered)
    std::vector<icu::UnicodeString> variableNames() const
    {
        std::lock_guard<std::mutex> lock(varsLock);
        std::vector<icu::UnicodeString> keys;
        for(const auto& entry : vars)
            keys.push_back(entry.second.first);
        return keys;
    }

protected:
    // map from name ObjString.hash to <name, value> pair
    // TODO: use something other than UnicodeString?? (ObjString* or Value??)
    typedef std::unordered_map<int32_t, NameValue> VarsMap;

    mutable std::mutex varsLock;
    VarsMap vars;

    static std::mutex globalsLock;
    static VarsMap globals;
};



}
