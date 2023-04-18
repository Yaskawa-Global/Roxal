#pragma once
#include <atomic>
#include <optional>
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
    Type,      // 1011

    // Obj (pointer) types 
    String,
    List,
    Dict,
    Vector,
    Matrix,
    Tensor,
    Orient,
    Stream,
    Object,
    Actor,
    Boxed = 0xff // not used with NAN tagging 
};

std::string to_string(ValueType t);


struct Obj;
struct ObjString;
class StreamEngine;


#if defined(NAN_TAGGING)

// If Values are real (IEEE C++ double), then no bit conversions necessary
//  IEEE Quiet NAN values are used to place type tag in mantissa (bits 46..49) 
//  If type is nil or bool tag is also literal value
//  If type is other value types, value is stored in lower bits (max 32bits)
//  If type if object, sign bit is set and pointer is stored in lower 48 bits

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
const uint64_t TagType   = uint64_t(ValueType::Type) << TypeTagOffset;


//ValueType valueType(ValueType t); // value type (ignoring boxing - so primitive type if boxed)

class Value;
bool isPrimitive(const Value& v); // forward from Object.h



class Value {
public:
    Value() : val(QNAN | TagNil) {}  

    explicit Value(bool b) { val = b ? (QNAN | TagTrue) : (QNAN | TagFalse); }
    explicit Value(uint8_t b) { val = QNAN | TagByte | (0xff & *reinterpret_cast<uint8_t*>(&b)); }
    explicit Value(double r) { val = (*reinterpret_cast<uint64_t*>(&r)); }
    explicit Value(int32_t i) { val = QNAN | TagInt | (0xffffffff & *reinterpret_cast<uint64_t*>(&i)); }
    explicit Value(ValueType bt) { val = QNAN | TagType | uint64_t(bt); }
    explicit Value(Obj* o);


    Value(const Value& v) 
    {
        val.store(v.val.load());
        if (isObj() || isBoxed())
            incRefObj();    
    }


    Value& operator=(const Value& v)
    {
        if (isObj() || isBoxed())
            decRefObj();

        val.store(v.val.load());
        if (isObj() || isBoxed())
            incRefObj();

        return *this;
    }


    ~Value() {
        if (isObj() || isBoxed())
            decRefObj();
    }


    void box();
    void unbox();
    bool isBoxed() const { return isObj() && isPrimitive(*this); }
    bool isBoxable() const { return !isBoxed() && (isBool() || isInt() || isReal()); }

    ValueType type() const;
    std::string typeName() const;

    inline bool isNil() const { return val == (QNAN | TagNil); }

    inline bool isBool() const { return (val|uint64_t(1)<<TypeTagOffset) == (QNAN | TagTrue); }
    bool asBool(bool strict=true) const;

    inline bool isInt() const { return (val & (QNAN | TypeTag)) == (QNAN | TagInt); }
    int32_t asInt(bool strict=true) const;

    inline bool isReal() const { return (val&QNAN) != QNAN; }
    double asReal(bool strict=true) const; 

    inline bool isNumber() const { return isInt() || isReal(); } // TODO: || isByte() || isDecimal(v)

    inline bool isType() const { return (val & (QNAN | TypeTag)) == (QNAN | TagType); }
    ValueType asType(bool strict=true) const;

    inline bool isObj() const { return (val & (QNAN | SignBit)) == (QNAN | SignBit); }
    inline Obj* asObj() const { 
        #ifdef DEBUG_BUILD
        assert(isObj());
        #endif
        return (Obj*)(uintptr_t)(val & ~(SignBit | QNAN)); 
    }


    // if is ObjFuture, block waiting for value (and replace this with value) 
    void resolveFuture();

    bool operator==(const Value& rhs) const;

    static_assert(sizeof(size_t) >= sizeof(uint64_t), "size_t is not big enough for uint64_t val as hash");

    size_t hash() const {
        return size_t(val.load());
    }

    #ifdef DEBUG_BUILD
    uint64_t getVal() const { return val; }
    static void testPrimitiveValues();
    #endif

protected:
    std::atomic_uint64_t val;

    void incRefObj();
    void decRefObj();
};


#else

inline ValueType valueType(ValueType t) { return ValueType(int(t) & 0x7f); }
inline bool isBoxed(ValueType t) { return (int(t) & 0x80) != 0; }


class Value {
public:
    Value() : _type(ValueType::Nil) {}

    explicit Value(bool b) : _type(ValueType::Bool) { as.boolean = b; }
    explicit Value(double r) : _type(ValueType::Real) { as.real=r; }
    explicit Value(int32_t i) : _type(ValueType::Int) { as.integer=i; }
    explicit Value(ValueType bt) : _type(ValueType::Type) { as.btype=bt; }
    explicit Value(Obj* o);

    Value(const Value& v) 
    {
        copyFrom(v);
    }


    Value& operator=(const Value& v)
    {
        if (isObj() || isBoxed())
            decRefObj();

        copyFrom(v);

        return *this;
    }


    ~Value() {
        if (isObj() || isBoxed())
            decRefObj();
    }


    void box();
    void unbox();
    bool isBoxed() const { return roxal::isBoxed(_type); }
    bool isBoxable() const { return !isBoxed() && (isBool() || isInt() || isReal()); }


    inline ValueType type() const { return _type; }
    std::string typeName() const;

    inline bool isNil() const { return _type == ValueType::Nil; }

    inline bool isBool() const { return valueType(_type) == ValueType::Bool; }
    bool asBool(bool strict=true) const;

    inline bool isInt() const { return valueType(_type) == ValueType::Int; }
    int32_t asInt(bool strict=true) const;

    inline bool isReal() const { return valueType(_type)==ValueType::Real; }
    double asReal(bool strict=true) const; 

    inline bool isNumber() const { return isInt() || isReal(); } // TODO: || isByte() || isDecimal(v)

    inline bool isType() const { return valueType(_type)==ValueType::Type; }
    ValueType asType(bool strict=true) const;

    inline bool isObj() const { return _type == ValueType::Object; }
    inline Obj* asObj() const { return as.obj; }


    bool operator==(const Value& rhs) const;

protected:
    ValueType _type;
    union {
        bool boolean;
        double real;
        int32_t integer;
        ValueType btype; // Value is a type (builtins only)
        Obj* obj;
    } as;

    void copyFrom(const Value& v) {


        // TODO: optimize with union copy (memcpy?)
        _type = v._type;

        if (roxal::isBoxed(_type)) {
            as.obj = v.as.obj;
            incRefObj();
        }
        else {
            switch(valueType(_type)) {            
                case ValueType::Nil: break;
                case ValueType::Bool: as.boolean = v.as.boolean; break;
                case ValueType::Int: as.integer = v.as.integer; break;
                case ValueType::Real: as.real = v.as.real; break;
                case ValueType::Type: as.btype = v.as.btype; break;
                case ValueType::Object: {
                    #ifdef DEBUG_TRACE_MEMORY
                    if (v.as.obj == nullptr)
                        throw std::runtime_error("Value constructed of assigned from invalid null Obj*");
                    #endif
                    as.obj = v.as.obj;
                    incRefObj();
                } break;
                default: throw std::runtime_error("unhandled Value type copy "+typeName());
            }
        }
    }

    void incRefObj();
    void decRefObj();
};


#endif


// value factories
inline Value nilVal() { return Value(); }

inline Value trueVal() { return Value(true); }
inline Value falseVal() { return Value(false); }
inline Value boolVal(bool b) { return Value(b); }

inline Value byteVal(uint8_t b) { return Value(b); }

inline Value intVal(int32_t i) { return Value(i); }

inline Value realVal(double r) { return Value(r); }

inline Value typeVal(ValueType bt) { return Value(bt); }


// create default value for given primitive or builtin
//  (e.g. false:bool. 0:int, 0,0:real, '':string, []:list, {}:dict, nil:object etc )
Value defaultValue(ValueType t);


<<<<<<< HEAD
=======
// create default value for given primitive or builtin
//  (e.g. false:bool. 0:int, 0,0:real, '':string, []:list, {}:dict, nil:object etc )
Value defaultValue(ValueType t);

>>>>>>> e8c8fb8 (object type property type declarations & initializers now stored at runtime (only builtins))

Value toType(ValueType t, Value v, bool strict=true);

// construct / convert from values (e.g. 'constructor' args) a value of the specified builtin type
//  (for single arg, same as toType(*begin)) - TODO: use C++20 range?
Value construct(ValueType type, std::vector<Value>::const_iterator begin, std::vector<Value>::const_iterator end);


ValueType binaryOpType(Value l, Value r);

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


}
