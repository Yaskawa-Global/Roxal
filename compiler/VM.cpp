#include <functional>

#include "RoxalCompiler.h"

#include "VM.h"
#include "Object.h"

using namespace roxal;


VM::VM()
{
    resetStack();
    inInnerScope = false;
}


VM::InterpretResult VM::interpret(std::istream& source, const std::string& name)
{
    ObjFunction* function { nullptr };

    try {
        RoxalCompiler compiler {};

        function = compiler.compile(source, name);

    } catch (std::exception& e) {
        return InterpretResult::CompileError;
    }

    if (function == nullptr)
        return InterpretResult::CompileError;

    push(objVal(function));

    call(function,0);

    auto result = this->execute();

    #if defined(DEBUG_TRACE_EXECUTION)
    if (globals.size() > 0) {        
        std::cout << std::endl << "== globals ==" << std::endl;
        for(const auto& global : globals) 
            std::cout << objStringToString(global.first) << " = " << toString(global.second) << std::endl;
    }
    #endif

    delete function;

    return result;
}


void VM::push(const Value& value)
{
    *stackTop = value;
    stackTop++;

    #ifdef DEBUG_BUILD
    if (stackTop == stack.end())
        throw std::runtime_error("Stack overflow");
    #endif
}


Value VM::pop()
{    
    #ifdef DEBUG_BUILD
    if (stackTop == stack.begin())
        throw std::runtime_error("Stack underflow");
    #endif

    stackTop--;
    auto retValue = *stackTop; // copy (hold ref)
    
    if (stackTop->isObj())
        *stackTop = Value(); // ensure to call decRef on objects

    return retValue;
}


Value VM::peek(int distance)
{
    #ifdef DEBUG_BUILD
    if (stackTop - stack.begin() <= distance)
        throw std::runtime_error("Stack underflow access ("+std::to_string(distance)+" stacksize:"+std::to_string(stackTop - stack.begin())+")");
    #endif
    return *(stackTop - 1 - distance);
}


bool VM::call(ObjFunction* function, int argCount)
{
    if (argCount != function->arity) {
        runtimeError("Passed "+std::to_string(argCount)+" arguments for function "
                    +toUTF8StdString(function->name)+" which has "
                    +std::to_string(function->arity)+" parameters.");
        return false;
    }

    if (frames.size() > 128) {
        runtimeError("Stack overflow.");
        return false;
    }

    CallFrame callframe {};
    callframe.function = function;
    callframe.ip = function->chunk->code.begin();
    callframe.slots = &(*(stackTop - argCount - 1));
    frames.push_back(callframe);
    return true;
}


bool VM::callValue(const Value& callee, int argCount)
{
    if (callee.isObj()) {
        switch (objType(callee)) {
            case ObjType::Function:
                return call(asFunction(callee), argCount);
            default:
                break;
        }
    }
    runtimeError("Only functions, objects and actors can be called.");
    return false;
}




VM::InterpretResult VM::execute()
{
    if (frames.empty() || frames.back().function->chunk->code.size()==0)
        return InterpretResult::OK; // nothing to execute

    auto frame { frames.end()-1 };

    auto readByte = [&]() -> uint8_t {
        #ifdef DEBUG_BUILD
            if (frame->ip == frame->function->chunk->code.end())
                throw std::runtime_error("Invalid IP");
        #endif
        return *frame->ip++;
    };

    auto readShort = [&]() -> uint16_t {
        #ifdef DEBUG_BUILD
            if (frame->ip == frame->function->chunk->code.end())
                throw std::runtime_error("Invalid IP");
        #endif
        frame->ip += 2;
        return (frame->ip[-2] << 8) | frame->ip[-1];
    };

    auto readConstant = [&]() -> Value {
        #ifdef DEBUG_BUILD
            return frame->function->chunk->constants.at(Chunk::size_type(readByte()));
        #else
            return frame->function->chunk->constants[Chunk::size_type(readByte())];
        #endif
    };

    auto readConstant2 = [&]() -> Value {
        #ifdef DEBUG_BUILD
            return frame->function->chunk->constants.at(Chunk::size_type((readByte() << 8) + readByte()));
        #else
            return frame->function->chunk->constants[Chunk::size_type((readByte() << 8) + readByte())];
        #endif
    };

    auto readString = [&]() -> ObjString* {
        return asString(readConstant());
    };

    auto binaryOp = [&](std::function<Value(Value, Value)> op) {
        Value b = pop();
        Value a = pop();
        push( op(a,b) );
    };

    #if defined(DEBUG_TRACE_EXECUTION)
    std::cout << std::endl << "== executing ==" << std::endl;
    #endif


    //
    //  main dispatch loop
    for(;;) {
        #if defined(DEBUG_TRACE_EXECUTION)
            // output stack
            if (stack.size() > 0) {
                std::cout << "          ";
                for(auto vi = stack.begin(); vi < stackTop; vi++) {
                    bool aString = vi->isObj() && isString(*vi);
                    std::cout << "[";
                    if (frame->slots == &(*vi) )
                        std::cout << "F^"; // show frame pointer
                    std::cout << " ";
                    if (aString)
                        std::cout << "'"; // quote strings
                    std::cout << toString(*vi);
                    if (aString)
                        std::cout << "'";
                    if (vi->isNumber()) // show numeric type
                        std::cout << ":" << vi->typeName().at(0);

                    std::cout << " ]";
                }
                std::cout << std::endl;
            }
            if (frame->ip != frame->function->chunk->code.end()) {
                // and instruction
                frame->function->chunk->disassembleInstruction(frame->ip - frame->function->chunk->code.begin());
            }
            else {
                std::cout << "          <end of chunk>" << std::endl;
                return InterpretResult::RuntimeError;
            }
        #endif

        uint8_t instruction { readByte() };
        switch(instruction) {
            case asByte(OpCode::Constant): {
                Value constant = readConstant();
                push(constant);
                break;
            }
            case asByte(OpCode::ConstTrue): {
                push(trueVal());
                break;
            }
            case asByte(OpCode::ConstFalse): {
                push(falseVal());
                break;
            }
            case asByte(OpCode::Equal): {
                Value b = pop();
                Value a = pop();
                push(boolVal(valuesEqual(a,b)));
                break;
            }
            case asByte(OpCode::Greater): {
                if (!peek(0).isNumber()) {
                    runtimeError("Operand must be a number");
                    return InterpretResult::RuntimeError;
                }
                if (!peek(1).isNumber()) {
                    runtimeError("Operand must be a number");
                    return InterpretResult::RuntimeError;
                }
                binaryOp([](Value a, Value b) -> Value { return greater(a,b); });
                break;
            }
            case asByte(OpCode::Less): {
                if (!peek(0).isNumber()) {
                    runtimeError("Operand must be a number");
                    return InterpretResult::RuntimeError;
                }
                if (!peek(1).isNumber()) {
                    runtimeError("Operand must be a number");
                    return InterpretResult::RuntimeError;
                }
                binaryOp([](Value a, Value b) -> Value { return less(a,b); });
                break;
            }
            case asByte(OpCode::Add): {
                if (peek(0).isNumber() && peek(1).isNumber()) {
                    binaryOp([](Value a, Value b) -> Value { return add(a,b); });
                }
                else if (isString(peek(1))) {
                    concatenate();
                }
                else {
                    runtimeError("Operands must be two numbers or a strings LHS");
                    return InterpretResult::RuntimeError;
                }
                break;
            }
            case asByte(OpCode::Subtract): {
                if (!peek(0).isNumber()) {
                    runtimeError("Operand must be a number");
                    return InterpretResult::RuntimeError;
                }
                if (!peek(1).isNumber()) {
                    runtimeError("Operand must be a number");
                    return InterpretResult::RuntimeError;
                }
                binaryOp([](Value a, Value b) -> Value { return subtract(a,b); });
                break;
            }
            case asByte(OpCode::Multiply): {
                if (!peek(0).isNumber()) {
                    runtimeError("Operand must be a number");
                    return InterpretResult::RuntimeError;
                }
                if (!peek(1).isNumber()) {
                    runtimeError("Operand must be a number");
                    return InterpretResult::RuntimeError;
                }
                binaryOp([](Value a, Value b) -> Value { return multiply(a,b); });
                break;
            }
            case asByte(OpCode::Divide): {
                if (!peek(0).isNumber()) {
                    runtimeError("Operand must be a number");
                    return InterpretResult::RuntimeError;
                }
                if (!peek(1).isNumber()) {
                    runtimeError("Operand must be a number");
                    return InterpretResult::RuntimeError;
                }
                if (peek(0).asReal() == 0.0) {
                    runtimeError("Divide by 0");
                    return InterpretResult::RuntimeError;
                }
                binaryOp([](Value a, Value b) -> Value { return divide(a,b); });
                break;
            }
            case asByte(OpCode::Negate): {
                Value operand { peek(0) };

                if (operand.isNumber() || operand.isBool())
                    push(negate(pop()));
                else if (isFalsey(operand)) {
                    // if it looks like false, isFalsey() -> true, so we push
                    //   true, which is negative of false
                    push(boolVal(isFalsey(operand)));
                }
                else {
                    runtimeError("Operand must be a number, bool or nil");
                    return InterpretResult::RuntimeError;
                }
                break;
            }
            case asByte(OpCode::And): {
                if (!peek(0).isBool()) {
                    runtimeError("Operand must be a bool");
                    return InterpretResult::RuntimeError;
                }
                if (!peek(1).isBool()) {
                    runtimeError("Operand must be a bool");
                    return InterpretResult::RuntimeError;
                }
                binaryOp([](Value a, Value b) -> Value { return land(a,b); });
                break;
            }
            case asByte(OpCode::Or): {
                if (!peek(0).isBool()) {
                    runtimeError("Operand must be a bool");
                    return InterpretResult::RuntimeError;
                }
                if (!peek(0).isBool()) {
                    runtimeError("Operand must be a bool");
                    return InterpretResult::RuntimeError;
                }
                binaryOp([](Value a, Value b) -> Value { return lor(a,b); });
                break;
            }
            case asByte(OpCode::Pop): {
                pop();
                break;
            }
            case asByte(OpCode::PopN): {
                uint8_t count = readByte();
                for(auto i=0; i<count; i++)
                    pop();
                break;
            }
            case asByte(OpCode::JumpIfFalse): {
                uint16_t jumpDist = readShort();
                if (isFalsey(peek(0)))
                    frame->ip += jumpDist;
                break;
            }
            case asByte(OpCode::JumpIfTrue): {
                uint16_t jumpDist = readShort();
                if (isTruthy(peek(0)))
                    frame->ip += jumpDist;
                break;
            }
            case asByte(OpCode::Jump): {
                uint16_t jumpDist = readShort();
                frame->ip += jumpDist;
                break;
            }
            case asByte(OpCode::Loop): {
                uint16_t jumpDist = readShort();
                frame->ip -= jumpDist;
                break;
            }
            case asByte(OpCode::Call): {
                uint8_t argCount = readByte();
                if (!callValue(peek(argCount), argCount))
                    return InterpretResult::RuntimeError;
                frame = frames.end()-1;
                break;
            }
            case asByte(OpCode::Return): {
                Value result = pop();

                auto returningFrame { frames.back() };
                frames.pop_back();

                if (frames.empty()) {
                    pop();
                    return InterpretResult::OK;
                }
                else {
                    auto popCount = &(*stackTop) - returningFrame.slots;
                    //stackTop -= popCount;
                    // loop to ensure stack Values unref'd
                    // TODO: could make popn(n) method
                    for(auto i=0; i<popCount; i++)
                        pop();
                    push(result);
                    frame = frames.end() -1;
                }
                break;
            }
            case asByte(OpCode::ConstNil): {
                push(nilVal());
                break;
            }
            case asByte(OpCode::GetLocal): {
                uint8_t slot = readByte();
                #ifdef DEBUG_BUILD
                auto stackIndex = frame->slots - &stack[0];
                if (stackIndex >= stack.size())
                    throw std::runtime_error("Stack overflow access");
                #endif
                push(frame->slots[slot]);
                break;
            }
            case asByte(OpCode::SetLocal): {
                uint8_t slot = readByte();
                #ifdef DEBUG_BUILD
                auto stackIndex = frame->slots - &stack[0];
                if (stackIndex >= stack.size())
                    throw std::runtime_error("Stack overflow access");
                #endif
                frame->slots[slot] = peek(0);
                break;
            }
            case asByte(OpCode::DefineGlobal): {
                ObjString* name = readString();
                globals[name] = pop();
                break;
            }
            case asByte(OpCode::GetGlobal): {
                ObjString* name = readString();
                auto gi = globals.find(name);
                if (gi != globals.end()) { // found
                    Value value = gi->second;
                    push(value);
                }
                else {
                    runtimeError("Undefined variable '"+name->toStdString()+"'");
                    return InterpretResult::RuntimeError;
                }
                break;
            }
            case asByte(OpCode::SetGlobal): {
                ObjString* name = readString();
                auto gi = globals.find(name);
                if (gi != globals.end()) { // found
                    // set new value, but leave it on stack (as assignment is an expression)
                    globals[name] = peek(0);
                }
                else {
                    runtimeError("Undefined variable '"+name->toStdString()+"'");
                    return InterpretResult::RuntimeError;
                }
                break;
            }
            case asByte(OpCode::SetNewGlobal): {
                ObjString* name = readString();
                auto gi = globals.find(name);
                if (gi != globals.end()) { // found
                    // set new value, but leave it on stack (as assignment is an expression)
                    globals[name] = peek(0);
                }
                else {
                    // only automatic declaration of globals on assignment when
                    //   at global/module level scope
                    globals[name] = peek(0);
                }
                break;
            }
            case asByte(OpCode::Constant2): {
                Value constant = readConstant2();
                push(constant);
                break;
            }
            case asByte(OpCode::Print): {
                std::cout << toString(pop()) << std::endl;
                //return InterpretResult::OK;
                break;
            }
        }

        freeObjects();

    } // for


    return InterpretResult::OK;

}


void VM::resetStack()
{
    stack.clear();
    stack.resize(MaxStack);
    stackTop = stack.begin();
    frames.clear();
}


void VM::freeObjects()
{
    // free objcts who's reference count dropped to 0
    for(Obj* obj : Obj::unrefedObjs) 
        delete obj;
    
    Obj::unrefedObjs.clear();
}


void VM::concatenate()
{
    #ifdef DEBUG_BUILD
        if (!isString(peek(1)))
            throw std::runtime_error("concatenate called with non-String LHS");
    #endif

    Value rhs { pop() };
    Value lhs { pop() };

    UnicodeString lhsString { asUString(lhs) };
    UnicodeString rhsString {};
    
    if (!isString(rhs)) {
        // convert RHS to a string
        // TODO: use canonical type -> string conversion using unicode instead
        //  of 'internal' toString()
        rhsString = toUnicodeString(toString(rhs));
    }

    UnicodeString combined { lhsString + rhsString };
    push( objVal(stringVal(combined)) );
}


void VM::runtimeError(const std::string& format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format.c_str(), args);
    va_end(args);
    fputs("\n", stderr);

    auto frame { frames.end()-1 };

    size_t instruction = frame->ip - frame->function->chunk->code.begin() - 1;
    int line = frame->function->chunk->getLine(instruction);
    fprintf(stderr, "[line %d] in script\n", line);
    resetStack();    
}
