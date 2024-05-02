#include <iostream>
#include <cassert>

#include <core/types.h>
#include "Object.h"
#include "Chunk.h"

using namespace roxal;


Chunk::Chunk(const icu::UnicodeString& packageName_, const icu::UnicodeString& moduleName_)
    : packageName(packageName_), moduleName(moduleName_)
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
    #ifdef DEBUG_BUILD
    if (constant >= constants.size()) 
        throw std::runtime_error("Constant instruction at "+std::to_string(offset)+" references constant "+std::to_string(constant)+" but constant table size is "+std::to_string(constants.size()));
    #endif
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
    return offset+3;
}


Chunk::size_type Chunk::invokeInstruction(const std::string& name, size_type offset) const
{
    uint8_t constant = code.at(offset+1);
    uint8_t argCount = code.at(offset+2);
    std::cout << format("%-16s (%d args) %4d '",name.c_str(),argCount,constant);
    auto value = constants.at(constant);
    std::cout << toString(value) << "'" << std::endl;
    return offset+3;
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
        case asByte(OpCode::FollowedBy):
            return simpleInstruction("FOLLOWEDBY", offset);
        case asByte(OpCode::Pop):
            return simpleInstruction("POP", offset);
        case asByte(OpCode::PopN):
            return byteInstruction("POPN", offset);
        case asByte(OpCode::Dup):
            return simpleInstruction("DUP", offset);
        case asByte(OpCode::Swap):
            return simpleInstruction("SWAP", offset);
        case asByte(OpCode::JumpIfFalse):
            return jumpInstruction("JUMP_IF_FALSE", 1, offset);
        case asByte(OpCode::JumpIfTrue):
            return jumpInstruction("JUMP_IF_TRUE", 1, offset);
        case asByte(OpCode::Jump):
            return jumpInstruction("JUMP", 1, offset);
        case asByte(OpCode::Loop):
            return jumpInstruction("LOOP", -1, offset);
        case asByte(OpCode::Call): {
            byteInstruction("CALL", offset);
            // compute num of bytes to skip over CallSpec            
            auto ip = code.begin()+offset+1;
            CallSpec callSpec{ip};
            if (!callSpec.allPositional) {
                std::cout << format("%04d      |                 %3d  ",offset+2,callSpec.argCount);
                for(auto arg : callSpec.args)
                    if (arg.positional)
                        std::cout << "p ";
                    else
                        std::cout << "n ";
                std::cout << std::endl;
            }
            return offset+1+callSpec.toBytes().size();
        }
        case asByte(OpCode::Index):
            return byteInstruction("INDEX", offset);
        case asByte(OpCode::SetIndex):
            return byteInstruction("SET_INDEX", offset);
        case asByte(OpCode::Invoke):
            return invokeInstruction("INVOKE", offset);
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
        case asByte(OpCode::ReturnStore):
            return simpleInstruction("RETURN_STORE", offset);
        case asByte(OpCode::ObjectType):
            return constantInstruction("OBJECT_TYPE", offset);
        case asByte(OpCode::ActorType):
            return constantInstruction("ACTOR_TYPE", offset);
        case asByte(OpCode::Property):
            return constantInstruction("PROPERTY", offset);
        case asByte(OpCode::Method):
            return constantInstruction("METHOD", offset);
        case asByte(OpCode::Extend):
            return simpleInstruction("EXTEND", offset);
        case asByte(OpCode::DefineGlobal):
            return constantInstruction("DEFINE_GLOBAL", offset);
        case asByte(OpCode::DefineGlobal2):
            return constantInstruction2("DEFINE_GLOBAL2", offset);
        case asByte(OpCode::GetGlobal):
            return constantInstruction("GET_GLOBAL", offset);
        case asByte(OpCode::GetGlobal2):
            return constantInstruction2("GET_GLOBAL2", offset);
        case asByte(OpCode::SetGlobal):
            return constantInstruction("SET_GLOBAL", offset);
        case asByte(OpCode::SetGlobal2):
            return constantInstruction2("SET_GLOBAL2", offset);
        case asByte(OpCode::SetNewGlobal):
            return constantInstruction("SET_NEW_GLOBAL", offset);
        case asByte(OpCode::SetNewGlobal2):
            return constantInstruction2("SET_NEW_GLOBAL2", offset);
        case asByte(OpCode::GetLocal):
            return byteInstruction("GET_LOCAL", offset);
        case asByte(OpCode::GetLocal2):
            return shortInstruction("GET_LOCAL2", offset);
        case asByte(OpCode::SetLocal):
            return byteInstruction("SET_LOCAL", offset);
        case asByte(OpCode::SetLocal2):
            return shortInstruction("SET_LOCAL2", offset);
        case asByte(OpCode::GetUpvalue):
            return byteInstruction("GET_UPVALUE", offset);
        case asByte(OpCode::GetUpvalue2):
            return shortInstruction("GET_UPVALUE2", offset);
        case asByte(OpCode::SetUpvalue):
            return byteInstruction("SET_UPVALUE", offset);
        case asByte(OpCode::SetUpvalue2):
            return shortInstruction("SET_UPVALUE2", offset);
        case asByte(OpCode::SetProp):
            return constantInstruction("SET_PROP", offset);
        case asByte(OpCode::GetProp):
            return constantInstruction("GET_PROP", offset);
        case asByte(OpCode::SetProp2):
            return constantInstruction2("SET_PROP", offset);
        case asByte(OpCode::GetProp2):
            return constantInstruction2("GET_PROP", offset);
        case asByte(OpCode::GetSuper):
            return constantInstruction("GET_SUPER", offset);
        case asByte(OpCode::NewRange):
            return byteInstruction("NEWRANGE", offset);
        case asByte(OpCode::NewList):
            return byteInstruction("NEWLIST", offset);
        case asByte(OpCode::NewDict):
            return byteInstruction("NEWDICT", offset);
        case asByte(OpCode::Nop):
            return simpleInstruction("NOP", offset);
        default:
            std::cout << "Unknown opcode " << std::to_string(instruction) << std::endl;
            return offset+1;
    }
}




CallSpec::CallSpec(Chunk::iterator& ci)
{
    allPositional = ( *ci & 0x80) == 0;
    argCount = *ci & 0x7f;
    #ifdef DEBUG_BUILD
    if (argCount>127)
        throw std::runtime_error("Invalid byte codes - Call cannot specify > 127 args");
    #endif
    ci++;
    if (!allPositional) {
        for(auto i=0; i<argCount; i++) {
            ArgSpec aspec {};
            if (*ci == 0) {
                aspec.positional = true;
                ci++;
            }
            else {
                aspec.paramNameHash = (uint16_t(*ci++) << 8) | uint16_t(*ci++);
            }
            args.push_back(aspec);
        }
    }
}


/**
 * @brief Serializes the CallSpec into a vector of bytes.
 *
 * The function serializes the `CallSpec` object into a byte vector format that can be used by the `Chunk` bytecode.
 * If `allPositional` is true, it means all the arguments are positional and the byte vector starts with
 * the argument count (masked with 0x7F to ensure it's within 7 bits). If `allPositional` is false,
 * it means there are named arguments and each argument will be represented with either one or two bytes
 * following the argument count byte (which has the MSB set to 1).
 *
 * For positional arguments, a single byte with the value 0 is used. For named arguments, two bytes are used
 * to represent the 15-bit hash code of the argument name. The first byte is the higher part of the hash code,
 * and the second byte is the lower part. The MSB of the first byte is set to 1 to indicate that this is a named argument.
 *
 * @return A vector of bytes representing the serialized CallSpec.
 * @throw std::runtime_error If the argument count exceeds 127 in a debug build.
 */
std::vector<uint8_t> CallSpec::toBytes() const
{
    std::vector<uint8_t> bytes {};

    #ifdef DEBUG_BUILD
    assert(allPositional == !namedArgs());
    if (!allPositional)
        assert(argCount == args.size());
    #endif

    if (allPositional) { 
        // common (optimized) case of all-positional args
        if (argCount>127)
            throw std::runtime_error("Maximum of 127 call arguments exceeded");
        // 0 MSB -> only position & 7bit arg count
        bytes.push_back(0x7f & uint8_t(argCount));
    }
    else {
        // 1 MSB -> 1,2 byte arg spec (one per arg) follows
        bytes.push_back(0x80 | (0x7f & uint8_t(argCount)));
        for(const auto& arg : args) {
            if (arg.positional)
                bytes.push_back(0);
            else {
                uint16_t argNameHash = 0x8000 | (arg.paramNameHash & 0x7fff);
                bytes.push_back((argNameHash & 0xff00)>>8);
                bytes.push_back(argNameHash & 0xff);
            }

        }
    }
    return bytes;
}


/**
 * @brief A function that checks if any argument in the CallSpec object is not positional (i.e. is named).
 *
 * @return true if there is at least one argument that is not positional, false otherwise
 */
bool CallSpec::namedArgs() const
{
    for(auto& arg : args)
        if (!arg.positional)
            return true;
    return false;
}


std::vector<int8_t> CallSpec::paramPositions(ptr<type::Type> calleeType, bool throwOnMissing) const
{
    std::vector<int8_t> argPositions {};

    // check that all uunspecified arguments have defaults declared (thow if any missing)
    auto checkRequiredArgumentsSpecified = [&](const type::Type::FuncType& funcType) {
        #ifdef DEBUG_BUILD
        assert(argPositions.size() == funcType.params.size());
        #endif
        for(int pi=0; pi<funcType.params.size();pi++) {
            if (argPositions[pi] == -1) {
                if (funcType.params[pi].has_value()) {
                    if (!funcType.params[pi].value().hasDefault)
                        throw std::runtime_error("Argument for parameter "+toUTF8StdString(funcType.params[pi].value().name)+" in call to "+calleeType->toString()+" not provided.");
                }
                else
                    throw std::runtime_error("Argument for parameter "+std::to_string(pi+1)+" in call to "+calleeType->toString()+" not provided.");
            }
        }
    };


    if (allPositional) {
        // just in-order
        if (argCount > 2)
            argPositions.reserve(argCount+2);
        for(auto i=0; i<argCount; i++)
            argPositions.push_back(i);

        if (calleeType->builtin == type::BuiltinType::Func) {
            if (calleeType->func.has_value()) {
                const type::Type::FuncType& funcType { calleeType->func.value() };
                for(auto i=argCount; i<funcType.params.size(); i++)
                    argPositions.push_back(-1); // not supplied
                if (throwOnMissing)
                    checkRequiredArgumentsSpecified(funcType);
            }
        }
        else // TODO: handle other callables (e.g. by listing & comparing their func signature)
            throw std::runtime_error("Unable to determine paramters from arguments in call to type "+calleeType->toString());
    }
    else {
        if (calleeType->builtin == type::BuiltinType::Func) {

            if (calleeType->func.has_value()) {
                const type::Type::FuncType& funcType { calleeType->func.value() };

                auto paramIndex = [&](uint16_t nameHashCode) -> int8_t {
                    int8_t index=0;
                    for(const auto& param : funcType.params) {
                        if (param.has_value()) {
                            if ((param.value().nameHashCode & 0x7fff) == (nameHashCode & 0x7fff))
                                return index;
                        }
                        index++;
                    }
                    return -1;
                };

                argPositions = std::vector<int8_t>(funcType.params.size(),int8_t(-1));
                bool callerOutOfOrderParamSeen = false;
                size_t argIndex = 0;
                for(const auto& arg : args) {
                    if (arg.positional) {
                        if (!callerOutOfOrderParamSeen)
                            argPositions[argIndex] = argIndex;
                        else // TODO: runtime error? (or handle in caller?)
                            throw std::runtime_error("Can't use positional arguments after named arguments in call to "+calleeType->toString());
                    }
                    else {
                        // if the arg is named but is in position, treat like positional
                        auto pindex = paramIndex(arg.paramNameHash);
                        if (pindex >= 0) {
                            if (pindex == argIndex) { // named, but in correct position
                                argPositions[pindex] = argIndex;
                            }
                            else { // named, out of position
                                argPositions[pindex] = argIndex;
                                callerOutOfOrderParamSeen = true;
                            }
                        }
                        else // FIXME: should this be runtime error (and how can we list the call arg name?)
                            throw std::runtime_error("Function call parameter "+std::to_string(argIndex)+" in call to "+calleeType->toString());

                    }
                    argIndex++;
                }

                if (throwOnMissing)
                    checkRequiredArgumentsSpecified(funcType);
            }
            else {
                throw std::runtime_error("Callee func type not specified");
            }
        }
        else {
            throw std::runtime_error("Unable to determine paramters from arguments in call to type "+calleeType->toString());
            //throw std::runtime_error(std::string(__FUNCTION__)+" unimplemented for type: "+to_string(calleeType->builtin));
        }
    }
    return argPositions;
}


#ifdef DEBUG_BUILD
void CallSpec::testParamPositions()
{
    // create type for func (p0,p1,p2:real,p3,p4=,p5:real=) -> int
    auto type { std::make_shared<type::Type>(type::BuiltinType::Func) };
    type->func = type::Type::FuncType();
    type->func.value().returnType = std::make_shared<type::Type>(type::BuiltinType::Int);
    auto& params { type->func.value().params };
    for(int i=0; i<6; i++) {
        auto param {type::Type::FuncType::ParamType{UnicodeString::fromUTF8("p"+std::to_string(i))}};
        if (i>=4)
            param.hasDefault=true;
        if (i==2 || i == 5)
            param.type = std::make_shared<type::Type>(type::BuiltinType::Real);
        params.push_back(param);
    }


    // all positional
    CallSpec callSpec {};
    callSpec.allPositional = true;
    callSpec.argCount=6;

    auto pp = callSpec.paramPositions(type);
    assert((pp == std::vector<int8_t>{0,1,2,3,4,5}));

    auto argNameHash = [](const std::string& p) {
        return uint16_t(toUnicodeString(p).hashCode() & 0x7fff);
    };

    // f(a0,p1=,p2=,p3=) - in order, p4,p5 omitted
    callSpec.allPositional=false;
    callSpec.argCount=4;
    callSpec.args.resize(4);
    callSpec.args[0] = {true,0};
    callSpec.args[1] = {false,argNameHash("p1")};
    callSpec.args[2] = {false,argNameHash("p2")};
    callSpec.args[3] = {false,argNameHash("p3")};
    try {
        pp = callSpec.paramPositions(type);
        assert((pp == std::vector<int8_t>{0,1,2,3,-1,-1}));
    } catch (std::exception& e) {
        std::cout << "Exception in test call f(a0,p1=,p2=,p3=) - "+std::string(e.what()) << std::endl;
    }

    // f(a0,p2=,p1=,p3=) - out of order, p4,p5 omitted
    callSpec.args[1] = {false,argNameHash("p2")};
    callSpec.args[2] = {false,argNameHash("p1")};
    pp = callSpec.paramPositions(type);
    assert((pp == std::vector<int8_t>{0,2,1,3,-1,-1}));

    // f(a0,p1=,p2,p3=) - in-order, p4,p5 omitted, p2 unnamed 
    callSpec.args[1] = {false,argNameHash("p1")};
    callSpec.args[2] = {true,0};
    callSpec.args[3] = {false,argNameHash("p3")};
    try {
        pp = callSpec.paramPositions(type);
        assert((pp == std::vector<int8_t>{0,1,2,3,-1,-1}));
    } catch (std::exception& e) {
        std::cout << "Exception in test call f(a0,p1=,p2,p3=) - "+std::string(e.what()) << std::endl;
    }


    // f(p1=,p0=,p2=,p5=,p4=) - out-of-order, p3 omitted
    callSpec.argCount=5;
    callSpec.args.resize(5);
    callSpec.args[0] = {false,argNameHash("p1")};;
    callSpec.args[1] = {false,argNameHash("p0")};
    callSpec.args[2] = {false,argNameHash("p2")};
    callSpec.args[3] = {false,argNameHash("p5")};
    callSpec.args[4] = {false,argNameHash("p4")};
    try {
        pp = callSpec.paramPositions(type, false);
        assert((pp == std::vector<int8_t>{1,0,2,-1,4,3}));
    } catch (std::exception& e) {
        std::cout << "Exception in test call f(p1=,p0=,p2=,p5=,p4=) - "+std::string(e.what()) << std::endl;
    }


    // f(p0,p2=,p1=,p3) - out-of-order, p3 unnamed, p4,p5 omitted (error)
    callSpec.argCount=4;
    callSpec.args.resize(4);
    callSpec.args[0] = {true,0};;
    callSpec.args[1] = {false,argNameHash("p2")};
    callSpec.args[2] = {false,argNameHash("p1")};
    callSpec.args[3] = {true,0};
    try {
        pp = callSpec.paramPositions(type);
        assert(false); // should have thrown above
    } catch (...) {}
}
#endif