#pragma once

#include <vector>

#include "common.h"
#include "Value.h"


namespace roxal {


enum class OpCode {
    Constant,
    Constant2, 
    ConstNil,
    ConstTrue,
    ConstFalse,
    Equal,
    Greater,
    Less,
    Add,
    Subtract,
    Multiply,
    Divide,
    Negate,
    And,
    Or,
    Pop,
    PopN,
    JumpIfFalse,
    JumpIfTrue,
    Jump,
    Loop,
    Call,
    Return,
    Print,
    DefineGlobal,
    GetGlobal,
    SetGlobal,
    SetNewGlobal,
    GetLocal,
    SetLocal
};

inline constexpr uint8_t asByte(OpCode op) { return uint8_t(op); }



class Chunk 
{
public:
    Chunk();

    typedef std::vector<uint8_t> CodeType;

    CodeType code;
    std::vector<Value> constants;

    using size_type = CodeType::size_type;
    using iterator = CodeType::iterator;

    void write(uint8_t byte, int line);
    void write(OpCode byte, int line) { write(uint8_t(byte), line); }
    void writeConsant(const Value& value, int line);

    size_type addConstant(const Value& value);

    int getLine(size_type offset) const;

    void disassemble(const icu::UnicodeString& name);
    size_type disassembleInstruction(size_type offset);

protected:
    std::vector<int> lines; // TODO: implement more efficient method

    size_type constantInstruction(const std::string& name, size_type offset) const;
    size_type constantInstruction2(const std::string& name, size_type offset) const;
    size_type simpleInstruction(const std::string& name, size_type offset) const;
    size_type byteInstruction(const std::string& name, size_type offset) const;
    size_type shortInstruction(const std::string& name, size_type offset) const;
    size_type jumpInstruction(const std::string& name, int sign, size_type offset) const;


};



}