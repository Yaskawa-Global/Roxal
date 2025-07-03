#pragma once

#include <vector>
#include <string>

#include <core/common.h>
#include "Value.h"


namespace roxal {


enum class OpCode {
    Nop,
    Constant,
    Constant2,
    ConstNil,
    ConstTrue,
    ConstFalse,
    ConstInt0,
    ConstInt1,
    Equal,
    Is,
    Greater,
    Less,
    Add,
    Subtract,
    Multiply,
    Divide,
    Modulo,
    Negate,
    And,
    Or,
    Pop,
    PopN,
    Dup,
    DupBelow,
    Swap,
    JumpIfFalse,
    JumpIfTrue,
    Jump,
    Loop,
    Call,
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
    Property,
    Method,
    EnumLabel,
    Extend,
    DefineModuleVar,
    DefineModuleVar2,
    GetModuleVar,
    SetModuleVar,
    GetModuleVar2,
    SetModuleVar2,
    SetNewModuleVar,
    SetNewModuleVar2,
    ImportModuleVars,
    GetUpvalue,
    SetUpvalue,
    GetUpvalue2,
    SetUpvalue2,
    GetLocal,
    SetLocal,
    GetLocal2,
    SetLocal2,
    SetProp,
    GetProp,
    SetPropCheck,
    GetPropCheck,
    SetProp2,
    GetProp2,
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
    EventOn
};

inline constexpr uint8_t asByte(OpCode op) { return uint8_t(op); }



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

protected:
    std::vector<LineEntry> lineTable; // sparse line/column table

    size_type constantInstruction(const std::string& name, size_type offset) const;
    size_type constantInstruction2(const std::string& name, size_type offset) const;
    size_type invokeInstruction(const std::string& name, size_type offset) const;
    size_type simpleInstruction(const std::string& name, size_type offset) const;
    size_type byteInstruction(const std::string& name, size_type offset) const;
    size_type shortInstruction(const std::string& name, size_type offset) const;
    size_type jumpInstruction(const std::string& name, int sign, size_type offset) const;


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
