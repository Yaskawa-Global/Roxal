#pragma once

#include <vector>
#include <string>

#include <core/common.h>
#include "Value.h"


namespace roxal {

constexpr uint8_t DoubleByteArg = 0x80;

enum class OpCode {
    Nop,
    Constant,
    ConstNil,
    ConstTrue,
    ConstFalse,
    ConstInt0,
    ConstInt1,
    Equal,
    NotEqual,
    Is,
    In,
    Greater,
    Less,
    GreaterEqual,
    LessEqual,
    Add,
    Subtract,
    Multiply,
    Divide,
    Modulo,
    Negate,
    And,
    Or,
    BitAnd,
    BitOr,
    BitXor,
    BitNot,
    Pop,
    PopN,
    StmtAction,    // expression-statement disposition: peek top, await futures,
                   // invoke 'statement action' methods, loop until terminal then pop
    Dup,
    DupBelow,
    Swap,
    JumpIfFalse,
    JumpIfTrue,
    Jump,
    Loop,
    Call,
    RemoteCall,
    Index,
    SetIndex,
    Invoke,
    Closure,
    CloseUpvalue,
    Return,
    ReturnStore,
    ObjectType,
    ActorType,
    InterfaceType,
    EnumerationType,
    EventType,
    Property,
    Method,
    EnumLabel,
    EventPayload,
    EventExtend,
    Extend,
    Implements,
    DefineModuleVar,
    DefineModuleConst,
    GetModuleVar,
    GetModuleVarSignal,
    SetModuleVar,
    SetNewModuleVar,
    ImportModuleVars,
    GetUpvalue,
    SetUpvalue,
    GetLocal,
    SetLocal,
    GetPropSignal,
    SetProp,
    GetProp,
    SetPropCheck,
    GetPropCheck,
    GetSuper,
    NewRange,
    NewList,
    NewDict,
    NewVector,
    NewMatrix,
    IfDictToKeys,
    IfDictToItems,
    ToType,
    ToTypeStrict,
    ToTypeSpec,
    ToTypeSpecStrict,
    EventOn,
    EventOff,
    SetupExcept,
    EndExcept,
    Throw,
    CopyInto,
    MakeConst,
    MoveLocal,      // like GetLocal but nils the source slot (ownership transfer)
    MoveModuleVar,  // like GetModuleVar but nils the source variable
    MoveProp,       // like GetProp but nils the source property
    NestedType,     // associate nested type with enclosing type
    DefineModuleOverload,  // pop closure; create or append to module-scope OverloadSet bound to name
    GetOverloadAt,         // load module OverloadSet by name, push closures[index] (compile-time-resolved overload)
    DefineLocalOverload,   // pop closure; create or append to local-slot OverloadSet
    GetLocalOverloadAt,    // load local OverloadSet by slot, push closures[index] (compile-time-resolved overload)
    InvokeOverloadAt,      // like Invoke but with explicit overload index — compile-time-resolved method dispatch
    PopToCount,    // 'jump' stack cleanup: close upvalues & pop locals down to a frame-relative
                   // slot count (fixed 2-byte arg, like Jump).
    _Last
};

inline constexpr uint8_t asByte(OpCode op) { return uint8_t(op); }
inline constexpr bool isDoubleByte(OpCode op) { return (uint8_t(op) & DoubleByteArg) != 0; }

static_assert(int(OpCode::_Last) < 128);


class Chunk
{
public:
    Chunk(const icu::UnicodeString& packageName_, const icu::UnicodeString& moduleName_,
          const icu::UnicodeString& sourceName_ = icu::UnicodeString());
    virtual ~Chunk() {}

    // fully-qualified package & module name (e.g. p.q.mod)
    //  is prefix for all module-level(global) names
    icu::UnicodeString packageName;
    icu::UnicodeString moduleName;

    typedef std::vector<uint8_t> CodeType;

    CodeType code;
    #ifdef DEBUG_BUILD
    std::vector<std::string> codeComments;
    #endif

    std::vector<Value> constants;

    using size_type = CodeType::size_type;
    using iterator = CodeType::iterator;

    struct LineEntry {
        size_type offset;
        int line;
        int column;
    };

    icu::UnicodeString sourceName; // name of source file

    void write(uint8_t byte, int line, int column, const std::string& comment = "");
    void write(OpCode byte, int line, int column, const std::string& comment = "") { write(uint8_t(byte), line, column, comment); }
    void writeConsant(const Value& value, int line, int column, const std::string& comment = "");
    uint8_t lastByte() const; // last byte written

    size_type addConstant(const Value& value);

    int getLine(size_type offset) const;
    int getColumn(size_type offset) const;

    void disassemble(const icu::UnicodeString& name);
    size_type disassembleInstruction(size_type offset);

    void serialize(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const;
    void deserialize(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr);

protected:
    std::vector<LineEntry> lineTable; // sparse line/column table

    size_type constantInstruction(const std::string& name, size_type offset, bool doubleByteArg) const;
    size_type invokeInstruction(const std::string& name, size_type offset) const;
    size_type simpleInstruction(const std::string& name, size_type offset) const;
    size_type byteInstruction(const std::string& name, size_type offset) const;
    size_type argInstruction(const std::string& name, size_type offset, bool doubleByteArg) const;
    size_type jumpInstruction(const std::string& name, int sign, size_type offset) const;
    // For opcodes whose first arg is a name (single/double-byte) and second arg
    // is a 2-byte uint16_t (e.g. an overload index).
    size_type constantPlusIndexInstruction(const std::string& name, size_type offset, bool doubleByteArg) const;
    // For opcodes whose first arg is a slot (single/double-byte) and second arg
    // is a 2-byte uint16_t.
    size_type argPlusIndexInstruction(const std::string& name, size_type offset, bool doubleByteArg) const;
    // For opcodes like InvokeOverloadAt: name (variable) + 2-byte overload index
    // + 1-byte CallSpec (matches the existing Invoke disasm assumption of
    // single-byte CallSpec — fine for all-positional calls).
    size_type invokeOverloadAtInstruction(const std::string& name, size_type offset, bool doubleByteArg) const;


};


// forward decl.
namespace type {
    struct Type;
};


// CallSpec for OpCode::Call

// Since functions are a first-class type in the language and dynamic typing
//  can be used, at point of generating the Call OpCode, the compiler may not
//  know the type of the function being called.  Hence, if named arguments are
//  used, the argument name ordering must be encoded in the instruction stream
//  so that runtime the VM can compare the arg ordering with the called function
//  paramter ordering.
// The CallSpec encodes this (and for common case of all-positional arguments,
//  only takes a single byte indicating the argument count (7bits))

struct CallSpec {
    CallSpec() {}
    explicit CallSpec(uint16_t argCount) : allPositional(true), argCount(argCount) {}
    explicit CallSpec(Chunk::iterator& ci);

    uint16_t argCount;
    bool allPositional;
    struct ArgSpec {
        bool positional;
        uint16_t paramNameHash; // lower 15bits of UnicodeString::hashCode()
    };
    std::vector<ArgSpec> args; // only used if !allPositional

    std::vector<uint8_t> toBytes() const;

    bool namedArgs() const; // any named args?

    // given the type of the callee (e.g. for func this specifies the parameter name/order)
    //  for this caller CallSpec, return vector in param order of arg positions (or -1
    //   if arg value for param not supplied)
    //  (e.g. is [0]=3 -> first callee param is 4th arg pushed onto stack by caller)
    // if throwOnMissing is true and an argument is not supplied and not defaulted by the callee,
    //  throw an exception
    // throws exceptions on other errors (e.g. missing type information)
    std::vector<int8_t> paramPositions(ptr<type::Type> calleeType, bool throwOnMissing=true) const;

    #ifdef DEBUG_BUILD
    static void testParamPositions();
    #endif
};



}
