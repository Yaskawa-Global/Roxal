#pragma once

#include <core/common.h>

namespace roxal {



enum class ValueType {
    Nil   = 1, // 0001 (both a type and singular value)
    Bool  = 4, // 0100
    Byte,  
    Int,
    Real,
    Decimal,
    String,
    Type,
    List,
    Dict,
    Vector,
    Matrix,
    Tensor,
    Orient,
    Stream,
    Object,
    Actor,
    Boxed = 0xff
};

std::string to_string(ValueType t);

inline ValueType valueType(ValueType t) { return ValueType(int(t) & 0x7f); }
inline bool isBoxed(ValueType t) { return (int(t) & 0x80) != 0; }

struct Obj;
struct ObjString;


#if defined(NAN_TAGGING)

const uint64_t QNAN = ((uint64_t)0x7ffc000000000000);

const uint8_t TagNil   = int(ValueType::Nil); 
const uint8_t TagFalse = 2; // 0010
const uint8_t TagTrue  = 3; // 0011

typedef uint64_t Value;

inline Value nilVal() { return ((Value)(uint64_t)(QNAN | TagNil)); }
inline bool isNil(Value v) { return v==nilVal(); }

inline Value trueVal() { return ((Value)(uint64_t)(QNAN | TagTrue )); }
inline Value falseVal() { return ((Value)(uint64_t)(QNAN | TagFalse )); }
inline Value boolVal(bool b) { return b ? trueVal() : falseVal(); }
inline bool isBool(Value v) { return (v|1) == trueVal(); }
inline bool asBool(Value v) { return v == trueVal(); }

inline Value realVal(double v) { return *reinterpret_cast<uint64_t*>(&v); }
inline bool isReal(Value v) { return (v&QNAN) != QNAN; }
inline double asReal(Value v) { return *reinterpret_cast<double*>(&v); }

//inline Value objVal()



#else


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


// value factories
inline Value nilVal() { return Value(); }

inline Value trueVal() { return Value(true); }
inline Value falseVal() { return Value(false); }
inline Value boolVal(bool b) { return Value(b); }

inline Value intVal(int32_t i) { return Value(i); }

inline Value realVal(double r) { return Value(r); }

inline Value typeVal(ValueType bt) { return Value(bt); }


#endif

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