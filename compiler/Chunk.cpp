#include <iostream>


#include "Object.h"
#include "Chunk.h"

using namespace roxal;


Chunk::Chunk()
{
    code.reserve(8);
}


void Chunk::write(uint8_t byte, int line, const std::string& comment)
{
    code.push_back(byte);
    #ifdef DEBUG_BUILD
    codeComments.push_back(comment);
    #endif
    lines.push_back(line);
}


void Chunk::writeConsant(const Value& value, int line, const std::string& comment)
{
    auto constant = addConstant(value);
    if (constant > 255)
        throw std::runtime_error("maximum of 256 constants exceeded");
    write(OpCode::Constant, line, comment);
    write(constant, line);
}


Chunk::size_type Chunk::addConstant(const Value& value)
{
    constants.push_back(value);
    return constants.size()-1;
}


int Chunk::getLine(size_type offset) const
{
    if (offset < lines.size())
        return lines.at(offset);
    return -1;
}



void Chunk::disassemble(const icu::UnicodeString& name)
{
    std::cout << "== " << toUTF8StdString(name) << " ==" << std::endl;
    for(size_type offset=0; offset < code.size();) {
        offset = disassembleInstruction(offset);
    }
}


Chunk::size_type Chunk::simpleInstruction(const std::string& name, size_type offset) const
{
    #ifdef DEBUG_BUILD
    auto comment { codeComments.at(offset) };
    if (comment.empty())
        std::cout << name << std::endl;
    else
        std::cout << format("%-16s # %s",name.c_str(),comment.c_str()) << std::endl;
    #else
    std::cout << name << std::endl;
    #endif
    return offset+1;
}


Chunk::size_type Chunk::byteInstruction(const std::string& name, size_type offset) const
{
    uint8_t arg = code.at(offset+1);
    #ifdef DEBUG_BUILD
    auto comment { codeComments.at(offset) };
    if (comment.empty())
        std::cout << format("%-16s %4d", name.c_str(), arg) << std::endl;
    else
        std::cout << format("%-16s %4d  # %s", name.c_str(), arg, comment.c_str()) << std::endl;
    #else
    std::cout << format("%-16s %4d", name.c_str(), arg) << std::endl;
    #endif
    return offset+2;
}


Chunk::size_type Chunk::shortInstruction(const std::string& name, size_type offset) const
{
    uint16_t arg = (code.at(offset+1) << 8) + code.at(offset+2); // LSByte last
    #ifdef DEBUG_BUILD
    auto comment { codeComments.at(offset) };
    if (comment.empty())
        std::cout << format("%-16s %4d", name.c_str(), arg) << std::endl;
    else
        std::cout << format("%-16s %4d  # %s ", name.c_str(), arg, comment.c_str()) << std::endl;
    #else
    std::cout << format("%-16s %4d", name.c_str(), arg) << std::endl;
    #endif
    return offset+3;
}


Chunk::size_type Chunk::jumpInstruction(const std::string& name, int sign, size_type offset) const
{
    uint16_t arg = (code.at(offset+1) << 8) + code.at(offset+2); // LSByte last
    size_type jumpTarget = offset + 3 + sign*arg;
    #ifdef DEBUG_BUILD
    auto comment { codeComments.at(offset) };
    if (comment.empty())
        std::cout << format("%-16s %4d (-> %d)", name.c_str(), arg, jumpTarget) << std::endl;
    else
        std::cout << format("%-16s %4d (-> %d)  # %s", name.c_str(), arg, jumpTarget, comment.c_str()) << std::endl;
    #else
    std::cout << format("%-16s %4d (-> %d)", name.c_str(), arg, jumpTarget) << std::endl;
    #endif
    return offset+3;
}



Chunk::size_type Chunk::constantInstruction(const std::string& name, size_type offset) const
{
    uint8_t constant = code.at(offset+1);
    auto value = constants.at(constant);
    std::cout << format("%-16s %4d '", name.c_str(), constant) 
              << toString(value)
              << "':" << value.typeName() << std::endl;
    return offset+2;
}


Chunk::size_type Chunk::constantInstruction2(const std::string& name, size_type offset) const
{
    uint16_t constant = (code.at(offset+1) << 8) + code.at(offset+2); // LSByte last
    std::cout << format("%-16s %4d '", name.c_str(), constant) 
              << toString(constants.at(constant))
              << "'" << std::endl;
    return offset+2;
}



Chunk::size_type Chunk::disassembleInstruction(size_type offset)
{
    std::cout << format("%04d ", offset);

    if (offset >= code.size())
        return offset;
        //throw std::runtime_error("No instruction to dissasemble a offset "+std::to_string(offset));

    if (offset > 0 && lines.at(offset) == lines.at(offset-1))
        std::cout << "   | ";
    else
        std::cout << format("%4d ", lines.at(offset));

    uint8_t instruction = code.at(offset);

    switch (instruction) {
        case asByte(OpCode::Constant):
            return constantInstruction("CONSTANT", offset);
        case asByte(OpCode::Constant2):
            return constantInstruction2("CONSTANT2", offset);
        case asByte(OpCode::ConstTrue):
            return simpleInstruction("CONST_TRUE", offset);
        case asByte(OpCode::ConstFalse):
            return simpleInstruction("CONST_FALSE", offset);
        case asByte(OpCode::ConstNil):
            return simpleInstruction("CONST_NIL", offset);
        case asByte(OpCode::Equal):
            return simpleInstruction("EQUAL", offset);
        case asByte(OpCode::Greater):
            return simpleInstruction("GREATER", offset);
        case asByte(OpCode::Less):
            return simpleInstruction("LESS", offset);
        case asByte(OpCode::Add):
            return simpleInstruction("ADD", offset);
        case asByte(OpCode::Subtract):
            return simpleInstruction("SUBTRACT", offset);
        case asByte(OpCode::Multiply):
            return simpleInstruction("MULTIPLY", offset);
        case asByte(OpCode::Divide):
            return simpleInstruction("DIVIDE", offset);
        case asByte(OpCode::Modulo):
            return simpleInstruction("MODULO", offset);
        case asByte(OpCode::Negate):
            return simpleInstruction("NEGATE", offset);
        case asByte(OpCode::And):
            return simpleInstruction("AND", offset);
        case asByte(OpCode::Or):
            return simpleInstruction("OR", offset);
        case asByte(OpCode::Pop):
            return simpleInstruction("POP", offset);
        case asByte(OpCode::PopN):
            return byteInstruction("POPN", offset);
        case asByte(OpCode::JumpIfFalse):
            return jumpInstruction("JUMP_IF_FALSE", 1, offset);
        case asByte(OpCode::JumpIfTrue):
            return jumpInstruction("JUMP_IF_TRUE", 1, offset);
        case asByte(OpCode::Jump):
            return jumpInstruction("JUMP", 1, offset);
        case asByte(OpCode::Loop):
            return jumpInstruction("LOOP", -1, offset);
        case asByte(OpCode::Call):
            return byteInstruction("CALL", offset);
        case asByte(OpCode::Closure): {
            offset++;
            uint8_t constant = code.at(offset++);
            std::cout << format("%-16s %4d ","CLOSURE", constant);
            std::cout << toString(constants.at(constant)) << std::endl;

            ObjFunction* function = asFunction(constants.at(constant));
            for (int j=0; j < function->upvalueCount; j++) {
                int isLocal = code.at(offset++);
                int index = code.at(offset++);
                std::cout << format("%04d      |                     %s %d",
                                    offset - 2, isLocal ? "local" : "upvalue", index)
                          << std::endl;
            }

            return offset;
        }
        case asByte(OpCode::CloseUpvalue): 
            return simpleInstruction("CLOSE_UPVALUE", offset);
        case asByte(OpCode::Return):
            return simpleInstruction("RETURN", offset);
        case asByte(OpCode::ObjectType):
            return constantInstruction("OBJECT_TYPE", offset);
        case asByte(OpCode::ActorType):
            return constantInstruction("ACTOR_TYPE", offset);
        case asByte(OpCode::Method):
            return constantInstruction("METHOD", offset);
        case asByte(OpCode::Print):
            return simpleInstruction("PRINT", offset);
        case asByte(OpCode::DefineGlobal):
            return constantInstruction("DEFINE_GLOBAL", offset);
        case asByte(OpCode::GetGlobal):
            return constantInstruction("GET_GLOBAL", offset);
        case asByte(OpCode::SetGlobal):
            return constantInstruction("SET_GLOBAL", offset);
        case asByte(OpCode::SetNewGlobal):
            return constantInstruction("SET_NEW_GLOBAL", offset);
        case asByte(OpCode::GetLocal):
            return byteInstruction("GET_LOCAL", offset);
        case asByte(OpCode::SetLocal):
            return byteInstruction("SET_LOCAL", offset);
        case asByte(OpCode::GetUpvalue):
            return byteInstruction("GET_UPVALUE", offset);
        case asByte(OpCode::SetUpvalue):
            return byteInstruction("SET_UPVALUE", offset);
        case asByte(OpCode::SetProp):
            return constantInstruction("SET_PROP", offset);
        case asByte(OpCode::GetProp):
            return constantInstruction("GET_PROP", offset);
        case asByte(OpCode::Nop):
            return simpleInstruction("NOP", offset);
        default:
            std::cout << "Unknown opcode " << std::to_string(instruction) << std::endl;
            return offset+1;
    }
}
