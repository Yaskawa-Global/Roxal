#include <iostream>
#include <cassert>

#include <core/types.h>
#include "Object.h"
#include "Chunk.h"

using namespace roxal;


Chunk::Chunk(const icu::UnicodeString& packageName_, const icu::UnicodeString& moduleName_,
             const icu::UnicodeString& sourceName_)
    : packageName(packageName_), moduleName(moduleName_), sourceName(sourceName_)
{
    code.reserve(8);
}


void Chunk::write(uint8_t byte, int line, int column, const std::string& comment)
{
    code.push_back(byte);
    #ifdef DEBUG_BUILD
    codeComments.push_back(comment);
    #endif
    if (lineTable.empty() || lineTable.back().line != line || lineTable.back().column != column)
        lineTable.push_back(LineEntry{code.size()-1, line, column});
}


void Chunk::writeConsant(const Value& value, int line, int column, const std::string& comment)
{
    auto constant = addConstant(value);
    if (constant > 255)
        throw std::runtime_error("maximum of 256 constants exceeded");
    write(OpCode::Constant, line, column, comment);
    write(constant, line, column);
}


uint8_t Chunk::lastByte() const
{
    if (code.empty())
        return uint8_t(OpCode::Nop);
    return code.back();
}


Chunk::size_type Chunk::addConstant(const Value& value)
{
    // TODO: perhaps seach and reuse consts?  (definitely if value types?)
    //  worth it? as they're per-chunk
    constants.push_back(value);
    return constants.size()-1;
}


int Chunk::getLine(size_type offset) const
{
    if (lineTable.empty())
        return -1;
    LineEntry entry{0, lineTable.front().line, lineTable.front().column};
    for(const auto& e : lineTable) {
        if (e.offset > offset)
            break;
        entry = e;
    }
    return entry.line;
}

int Chunk::getColumn(size_type offset) const
{
    if (lineTable.empty())
        return -1;
    LineEntry entry{0, lineTable.front().line, lineTable.front().column};
    for(const auto& e : lineTable) {
        if (e.offset > offset)
            break;
        entry = e;
    }
    return entry.column;
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


Chunk::size_type Chunk::argInstruction(const std::string& name, size_type offset, bool doubleByteArg) const
{
    uint16_t arg = doubleByteArg ? (code.at(offset+1) << 8) + code.at(offset+2) : code.at(offset+1); // LSByte last
    #ifdef DEBUG_BUILD
    auto comment { codeComments.at(offset) };
    if (comment.empty())
        std::cout << format("%-16s %4d", name.c_str(), arg) << std::endl;
    else
        std::cout << format("%-16s %4d  # %s ", name.c_str(), arg, comment.c_str()) << std::endl;
    #else
    std::cout << format("%-16s %4d", name.c_str(), arg) << std::endl;
    #endif
    return doubleByteArg ? offset+3 : offset+2;
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



Chunk::size_type Chunk::constantInstruction(const std::string& name, size_type offset, bool doubleByteArg) const
{
    uint16_t constant = doubleByteArg ? (code.at(offset+1) << 8) + code.at(offset+2) : code.at(offset+1);
    #ifdef DEBUG_BUILD
    if (constant >= constants.size())
        throw std::runtime_error("Constant instruction at "+std::to_string(offset)+" references constant "+std::to_string(constant)+" but constant table size is "+std::to_string(constants.size()));
    #endif
    auto value = constants.at(constant);
    std::cout << format("%-16s %4d '", name.c_str(), constant)
              << toString(value)
              << "':" << value.typeName() << std::endl;
    return doubleByteArg ? offset+3 : offset+2;
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
        //throw std::runtime_error("No instruction to disassemble a offset "+std::to_string(offset));

    if (offset > 0 && getLine(offset) == getLine(offset-1))
        std::cout << "   | ";
    else
        std::cout << format("%4d ", getLine(offset));

    bool doubleByteArg = false;
    uint8_t instructionByte = code.at(offset);
    OpCode instruction {};
    if ((instructionByte & DoubleByteArg) == 0)
        instruction = OpCode(instructionByte);
    else {
        instruction = OpCode(instructionByte & ~DoubleByteArg);
        doubleByteArg = true; // expects 2 bytes of argument
    }

    switch (instruction) {
        case OpCode::Constant:
            return constantInstruction("CONSTANT", offset, doubleByteArg);
        case OpCode::ConstNil:
            return simpleInstruction("CONST_NIL", offset);
        case OpCode::ConstTrue:
            return simpleInstruction("CONST_TRUE", offset);
        case OpCode::ConstFalse:
            return simpleInstruction("CONST_FALSE", offset);
        case OpCode::ConstInt0:
            return simpleInstruction("CONST_INT0", offset);
        case OpCode::ConstInt1:
            return simpleInstruction("CONST_INT1", offset);
        case OpCode::Equal:
            return simpleInstruction("EQUAL", offset);
        case OpCode::NotEqual:
            return simpleInstruction("NOT_EQUAL", offset);
        case OpCode::Is:
            return simpleInstruction("IS", offset);
        case OpCode::In:
            return simpleInstruction("IN", offset);
        case OpCode::Greater:
            return simpleInstruction("GREATER", offset);
        case OpCode::Less:
            return simpleInstruction("LESS", offset);
        case OpCode::GreaterEqual:
            return simpleInstruction("GREATER_EQUAL", offset);
        case OpCode::LessEqual:
            return simpleInstruction("LESS_EQUAL", offset);
        case OpCode::Add:
            return simpleInstruction("ADD", offset);
        case OpCode::Subtract:
            return simpleInstruction("SUBTRACT", offset);
        case OpCode::Multiply:
            return simpleInstruction("MULTIPLY", offset);
        case OpCode::Divide:
            return simpleInstruction("DIVIDE", offset);
        case OpCode::Modulo:
            return simpleInstruction("MODULO", offset);
        case OpCode::Negate:
            return simpleInstruction("NEGATE", offset);
        case OpCode::And:
            return simpleInstruction("AND", offset);
        case OpCode::Or:
            return simpleInstruction("OR", offset);
        case OpCode::BitAnd:
            return simpleInstruction("BIT_AND", offset);
        case OpCode::BitOr:
            return simpleInstruction("BIT_OR", offset);
        case OpCode::BitXor:
            return simpleInstruction("BIT_XOR", offset);
        case OpCode::BitNot:
            return simpleInstruction("BIT_NOT", offset);
        case OpCode::Pop:
            return simpleInstruction("POP", offset);
        case OpCode::PopN:
            return byteInstruction("POPN", offset);
        case OpCode::Dup:
            return simpleInstruction("DUP", offset);
        case OpCode::DupBelow:
            return simpleInstruction("DUP_BELOW", offset);
        case OpCode::Swap:
            return simpleInstruction("SWAP", offset);
        case OpCode::JumpIfFalse:
            return jumpInstruction("JUMP_IF_FALSE", 1, offset);
        case OpCode::JumpIfTrue:
            return jumpInstruction("JUMP_IF_TRUE", 1, offset);
        case OpCode::Jump:
            return jumpInstruction("JUMP", 1, offset);
        case OpCode::Loop:
            return jumpInstruction("LOOP", -1, offset);
        case OpCode::Call: {
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
        case OpCode::Index:
            return byteInstruction("INDEX", offset);
        case OpCode::SetIndex:
            return byteInstruction("SET_INDEX", offset);
        case OpCode::Invoke:
            return invokeInstruction("INVOKE", offset);
        case OpCode::Closure: {
            offset++;
            uint16_t constant = doubleByteArg ? (code.at(offset++) << 8) + code.at(offset++) : code.at(offset++);
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
        case OpCode::CloseUpvalue:
            return simpleInstruction("CLOSE_UPVALUE", offset);
        case OpCode::Return:
            return simpleInstruction("RETURN", offset);
        case OpCode::ReturnStore:
            return simpleInstruction("RETURN_STORE", offset);
        case OpCode::ObjectType:
            return constantInstruction("OBJECT_TYPE", offset, doubleByteArg);
        case OpCode::ActorType:
            return constantInstruction("ACTOR_TYPE", offset, doubleByteArg);
        case OpCode::InterfaceType:
            return constantInstruction("INTERFACE_TYPE", offset, doubleByteArg);
        case OpCode::EnumerationType:
            return constantInstruction("ENUMERATION_TYPE", offset, doubleByteArg);
        case OpCode::EventType:
            return constantInstruction("EVENT_TYPE", offset, doubleByteArg);
        case OpCode::Property:
            return constantInstruction("PROPERTY", offset, doubleByteArg);
        case OpCode::Method:
            return constantInstruction("METHOD", offset, doubleByteArg);
        case OpCode::EnumLabel:
            return constantInstruction("ENUM_LABEL", offset, doubleByteArg);
        case OpCode::EventPayload:
            return constantInstruction("EVENT_PAYLOAD", offset, doubleByteArg);
        case OpCode::EventExtend:
            return simpleInstruction("EVENT_EXTEND", offset);
        case OpCode::Extend:
            return simpleInstruction("EXTEND", offset);
        case OpCode::DefineModuleConst:
            return constantInstruction("DEFINE_MODULE_CONST", offset, doubleByteArg);
        case OpCode::DefineModuleVar:
            return constantInstruction("DEFINE_MODULE_VAR", offset, doubleByteArg);
        case OpCode::GetModuleVar:
            return constantInstruction("GET_MODULE_VAR", offset, doubleByteArg);
        case OpCode::GetModuleVarSignal:
            return constantInstruction("GET_MODULE_VAR_SIGNAL", offset, doubleByteArg);
        case OpCode::SetModuleVar:
            return constantInstruction("SET_MODULE_VAR", offset, doubleByteArg);
        case OpCode::SetNewModuleVar:
            return constantInstruction("SET_NEW_MODULE_VAR", offset, doubleByteArg);
        case OpCode::ImportModuleVars:
            return simpleInstruction("IMPORT_MODULE_VARS", offset);
        case OpCode::GetLocal:
            return argInstruction("GET_LOCAL", offset, doubleByteArg);
        case OpCode::SetLocal:
            return argInstruction("SET_LOCAL", offset, doubleByteArg);
        case OpCode::GetPropSignal:
            return constantInstruction("GET_PROP_SIGNAL", offset, doubleByteArg);
        case OpCode::GetUpvalue:
            return argInstruction("GET_UPVALUE", offset, doubleByteArg);
        case OpCode::SetUpvalue:
            return argInstruction("SET_UPVALUE", offset, doubleByteArg);
        case OpCode::SetProp:
            return constantInstruction("SET_PROP", offset, doubleByteArg);
        case OpCode::GetProp:
            return constantInstruction("GET_PROP", offset, doubleByteArg);
        case OpCode::SetPropCheck:
            return constantInstruction("SET_PROP_CHECK", offset, doubleByteArg);
        case OpCode::GetPropCheck:
            return constantInstruction("GET_PROP_CHECK", offset, doubleByteArg);
        case OpCode::GetSuper:
            return constantInstruction("GET_SUPER", offset, doubleByteArg);
        case OpCode::NewRange:
            return byteInstruction("NEWRANGE", offset);
        case OpCode::NewList:
            return byteInstruction("NEWLIST", offset);
        case OpCode::NewDict:
            return byteInstruction("NEWDICT", offset);
        case OpCode::NewVector:
            return byteInstruction("NEWVECTOR", offset);
        case OpCode::NewMatrix:
            return byteInstruction("NEWMATRIX", offset);
        case OpCode::IfDictToKeys:
            return simpleInstruction("IF_DICT_TO_KEYS", offset);
        case OpCode::IfDictToItems:
            return simpleInstruction("IF_DICT_TO_ITEMS", offset);
        case OpCode::ToType:
            return byteInstruction("TO_TYPE", offset);
        case OpCode::ToTypeStrict:
            return byteInstruction("TO_TYPE_STRICT", offset);
        case OpCode::ToTypeSpec:
            return simpleInstruction("TO_TYPE_SPEC", offset);
        case OpCode::ToTypeSpecStrict:
            return simpleInstruction("TO_TYPE_SPEC_STRICT", offset);
        case OpCode::EventOn:
            return byteInstruction("EVENT_ON", offset);
        case OpCode::EventOff:
            return simpleInstruction("EVENT_OFF", offset);
        case OpCode::SetupExcept:
            return jumpInstruction("SETUP_EXCEPT", 1, offset);
        case OpCode::EndExcept:
            return simpleInstruction("END_EXCEPT", offset);
        case OpCode::Throw:
            return simpleInstruction("THROW", offset);
        case OpCode::CopyInto:
            return simpleInstruction("COPY_INTO", offset);
        case OpCode::Nop:
            return simpleInstruction("NOP", offset);
        default:
            std::cout << "Unknown opcode " << std::to_string(int(instruction)) << std::endl;
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

    // check that all unspecified arguments have defaults declared (throw if any missing)
    // Note: variadic params (marked with -2) are allowed to have no args
    auto checkRequiredArgumentsSpecified = [&](const type::Type::FuncType& funcType) {
        #ifdef DEBUG_BUILD
        assert(argPositions.size() == funcType.params.size());
        #endif
        for(int pi=0; pi<funcType.params.size();pi++) {
            if (argPositions[pi] == -1) {
                if (funcType.params[pi].has_value()) {
                    // Variadic params are optional (empty list if no args)
                    if (funcType.params[pi].value().variadic)
                        continue;
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

        if (calleeType->builtin == type::BuiltinType::Func) {
            if (calleeType->func.has_value()) {
                const type::Type::FuncType& funcType { calleeType->func.value() };
                bool hasVariadic = funcType.hasVariadic();
                size_t regularParamCount = hasVariadic ? funcType.params.size() - 1 : funcType.params.size();

                // Fill regular params first
                for(size_t i=0; i<argCount && i<regularParamCount; i++)
                    argPositions.push_back(i);

                // For unfilled regular params, mark as not supplied
                for(size_t i=argCount; i<regularParamCount; i++)
                    argPositions.push_back(-1);

                // Handle variadic param
                if (hasVariadic) {
                    if (argCount > regularParamCount) {
                        // -2 indicates variadic param; VM will collect remaining args
                        // The value encodes: -2 means "collect args from index regularParamCount onwards"
                        argPositions.push_back(-2);
                    } else {
                        // No variadic args provided - VM will create empty list
                        argPositions.push_back(-1);
                    }
                }

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
                bool hasVariadic = funcType.hasVariadic();
                size_t regularParamCount = hasVariadic ? funcType.params.size() - 1 : funcType.params.size();

                // Find param index by name hash (excluding variadic param)
                auto paramIndex = [&](uint16_t nameHashCode) -> int8_t {
                    int8_t index=0;
                    for(const auto& param : funcType.params) {
                        if (param.has_value()) {
                            // Don't match against variadic param by name
                            if (param.value().variadic)
                                break;
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
                size_t nextUnfilledRegularParam = 0;  // For positional args when variadic exists
                bool variadicArgsStarted = false;

                // First pass: match all named arguments
                for(size_t ai = 0; ai < args.size(); ai++) {
                    if (!args[ai].positional) {
                        auto pindex = paramIndex(args[ai].paramNameHash);
                        if (pindex >= 0) {
                            argPositions[pindex] = ai;
                            callerOutOfOrderParamSeen = true;  // named arg seen
                        }
                        else
                            throw std::runtime_error("Unknown parameter name in call to "+calleeType->toString());
                    }
                }

                // Find which regular params are still unfilled after named args
                for(size_t pi = 0; pi < regularParamCount; pi++) {
                    if (argPositions[pi] == -1) {
                        nextUnfilledRegularParam = pi;
                        break;
                    }
                    nextUnfilledRegularParam = pi + 1;
                }

                // Second pass: handle positional arguments
                // Track if we've seen a named arg before current position (in call order)
                bool seenNamedArgInCallOrder = false;
                size_t positionalArgIndex = 0;  // Index among positional args only
                for(size_t ai = 0; ai < args.size(); ai++) {
                    if (!args[ai].positional) {
                        seenNamedArgInCallOrder = true;  // Mark that we've seen a named arg
                    }
                    else {
                        // This is a positional argument
                        if (hasVariadic) {
                            // With variadic: positional args after named go to variadic
                            if (seenNamedArgInCallOrder) {
                                // Positional after named -> goes to variadic
                                variadicArgsStarted = true;
                                // Don't assign to argPositions; VM will collect these
                            }
                            else {
                                // Positional before any named -> fills regular params
                                if (nextUnfilledRegularParam < regularParamCount) {
                                    argPositions[nextUnfilledRegularParam] = ai;
                                    nextUnfilledRegularParam++;
                                }
                                else {
                                    // Regular params full, rest go to variadic
                                    variadicArgsStarted = true;
                                }
                            }
                        }
                        else {
                            // No variadic: original behavior
                            if (!seenNamedArgInCallOrder) {
                                if (positionalArgIndex < regularParamCount)
                                    argPositions[positionalArgIndex] = ai;
                            }
                            else
                                throw std::runtime_error("Can't use positional arguments after named arguments in call to "+calleeType->toString());
                        }
                        positionalArgIndex++;
                    }
                }

                // Set variadic param marker if we have variadic args
                if (hasVariadic) {
                    if (variadicArgsStarted) {
                        argPositions[funcType.params.size() - 1] = -2;  // -2 = collect variadic args
                    }
                    // else remains -1, VM creates empty list
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
    ptr<type::Type> type = make_ptr<type::Type>(type::BuiltinType::Func);
    type->func = type::Type::FuncType();
    type->func.value().returnTypes.push_back(make_ptr<type::Type>(type::BuiltinType::Int));
    auto& params { type->func.value().params };
    for(int i=0; i<6; i++) {
        auto param {type::Type::FuncType::ParamType{UnicodeString::fromUTF8("p"+std::to_string(i))}};
        if (i>=4)
            param.hasDefault=true;
        if (i==2 || i == 5)
            param.type = make_ptr<type::Type>(type::BuiltinType::Real);
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

void Chunk::serialize(std::ostream& out, roxal::ptr<SerializationContext> ctx) const
{
    auto writeUS = [&](const icu::UnicodeString& us) {
        std::string s; us.toUTF8String(s);
        uint32_t len = s.size();
        out.write(reinterpret_cast<char*>(&len), 4);
        out.write(s.data(), len);
    };

    writeUS(packageName);
    writeUS(moduleName);
    writeUS(sourceName);

    uint32_t codeSize = code.size();
    out.write(reinterpret_cast<char*>(&codeSize), 4);
    if(codeSize)
        out.write(reinterpret_cast<const char*>(code.data()), codeSize);

    uint32_t constCount = constants.size();
    out.write(reinterpret_cast<char*>(&constCount), 4);
    for(const auto& v : constants)
        writeValue(out, v, ctx);

    uint32_t lineCount = lineTable.size();
    out.write(reinterpret_cast<char*>(&lineCount), 4);
    for(const auto& e : lineTable) {
        uint32_t off = e.offset;
        int32_t line = e.line;
        int32_t col = e.column;
        out.write(reinterpret_cast<char*>(&off), 4);
        out.write(reinterpret_cast<char*>(&line), 4);
        out.write(reinterpret_cast<char*>(&col), 4);
    }
}

void Chunk::deserialize(std::istream& in, roxal::ptr<SerializationContext> ctx)
{
    auto readUS = [&]() {
        uint32_t len; in.read(reinterpret_cast<char*>(&len), 4);
        std::string s(len, '\0');
        if(len) in.read(s.data(), len);
        return icu::UnicodeString::fromUTF8(s);
    };

    packageName = readUS();
    moduleName  = readUS();
    sourceName  = readUS();

    uint32_t codeSize; in.read(reinterpret_cast<char*>(&codeSize), 4);
    code.resize(codeSize);
    if(codeSize)
        in.read(reinterpret_cast<char*>(code.data()), codeSize);

    uint32_t constCount; in.read(reinterpret_cast<char*>(&constCount), 4);
    constants.clear();
    for(uint32_t i=0;i<constCount;i++)
        constants.push_back(readValue(in, ctx));

    uint32_t lineCount; in.read(reinterpret_cast<char*>(&lineCount), 4);
    lineTable.clear();
    for(uint32_t i=0;i<lineCount;i++) {
        uint32_t off; int32_t line; int32_t col;
        in.read(reinterpret_cast<char*>(&off), 4);
        in.read(reinterpret_cast<char*>(&line), 4);
        in.read(reinterpret_cast<char*>(&col), 4);
        lineTable.push_back(LineEntry{off,line,col});
    }
}
