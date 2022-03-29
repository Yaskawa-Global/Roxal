#include <functional>
#include <time.h>

#include "RoxalCompiler.h"

#include "VM.h"
#include "Object.h"

using namespace roxal;


VM::VM()
    : lineMode(false)
{
    resetStack();
    defineNativeFunctions();
    initString = stringVal(UnicodeString("init"));
    initString->incRef();
}


VM::VM(std::istream& linestream)
    : lineMode(true), lineStream(&linestream)
{
    resetStack();
    defineNativeFunctions();
    initString = stringVal(UnicodeString("init"));
    initString->incRef();
}

VM::~VM()
{
    resetStack();
    globals.clear();

    initString->decRef();

    freeObjects();

    #ifdef DEBUG_TRACE_MEMORY
    outputAllocatedObjs();
    #endif
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

    ObjClosure* closure = closureVal(function);
    push(objVal(closure));

    call(closure,0);

    auto result = this->execute();

    #if defined(DEBUG_TRACE_EXECUTION)
    if (globals.size() > 0) {        
        std::cout << std::endl << "== globals ==" << std::endl;
        for(const auto& global : globals) 
            std::cout << toUTF8StdString(global.second.first) << " = " << toString(global.second.second) << std::endl;
    }
    #endif

    stack.clear();
    freeObjects();

    return result;
}



void VM::interpretLine()
{

}




void VM::push(const Value& value)
{
    #ifdef DEBUG_BUILD
    if (value.isObj() && value.asObj() == nullptr)
        throw std::runtime_error("Can't push Value with null Obj*");
    #endif

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


bool VM::call(ObjClosure* closure, int argCount)
{
    if (argCount != closure->function->arity) {
        runtimeError("Passed "+std::to_string(argCount)+" arguments for function "
                    +toUTF8StdString(closure->function->name)+" which has "
                    +std::to_string(closure->function->arity)+" parameters.");
        return false;
    }

    if (frames.size() > 128) {
        runtimeError("Stack overflow.");
        return false;
    }

    CallFrame callframe {};
    callframe.closure = closure;
    callframe.ip = closure->function->chunk->code.begin();
    callframe.slots = &(*(stackTop - argCount - 1));
    frames.push_back(callframe);
    return true;
}


bool VM::callValue(const Value& callee, int argCount)
{
    if (callee.isObj()) {
        switch (objType(callee)) {
            case ObjType::BoundMethod: {
                ObjBoundMethod* boundMethod { asBoundMethod(callee) };
                *(stackTop - argCount - 1) = boundMethod->receiver;
                return call(boundMethod->method, argCount);
            }
            case ObjType::ObjectType: {
                ObjObjectType* type = asObjectType(callee);
                Value result { objVal(objInstanceVal(type)) };
                *(stackTop - argCount - 1) = result;
                auto it = type->methods.find(initString->hash);
                if (it != type->methods.end()) {
                    Value initializer { it->second.second };
                    return call(asClosure(initializer), argCount);
                }
                else {
                    if (argCount != 0) {
                        runtimeError("Expected 0 arguments for type instantiation, provided "+std::to_string(argCount));
                        return false;
                    }
                }
                return true;
            }
            case ObjType::Closure:
                return call(asClosure(callee), argCount);
            case ObjType::Native: {
                NativeFn native = asNative(callee)->function;
                Value result { native(argCount, &(*stackTop) - argCount) };
                //stackTop -= argCount+1; 
                *(stackTop - argCount - 1) = result;
                push(result);
                return true;
            }
            default:
                break;
        }
    }
    runtimeError("Only functions, objects and actors can be called.");
    return false;
}


bool VM::invokeFromType(ObjObjectType* type, ObjString* name, int argCount)
{
    auto it = type->methods.find(name->hash);
    if (it == type->methods.end()) {
        runtimeError("Undefined property '%s'",toUTF8StdString(name->s).c_str());
        return false;
    }
    Value method { it->second.second };
    return call(asClosure(method), argCount);
}



bool VM::invoke(ObjString* name, int argCount)
{
    Value receiver { peek(argCount) };
    if (!isInstance(receiver)) {
        runtimeError("Only instances have method.");
        return false;
    }

    ObjInstance* instance = asInstance(receiver);

    // check to ensure name isn't a prop with a func in it
    auto it = instance->properties.find(name->hash);
    if (it != instance->properties.end()) { // it is a prop
        Value value { it->second };
        *(stackTop - argCount - 1) = value;
        return callValue(value, argCount);
    }

    return invokeFromType(instance->instanceType, name, argCount);
}


bool VM::bindMethod(ObjObjectType* instanceType, ObjString* name)
{
    auto it = instanceType->methods.find(name->hash);
    if (it == instanceType->methods.end())
        return false;

    Value method { it->second.second };

    ObjBoundMethod* boundMethod { boundMethodVal(peek(0), asClosure(method)) };

    pop();
    push(objVal(boundMethod));

    return true;
}



ObjUpvalue* VM::captureUpvalue(Value& local)
{
    auto begin = openUpvalues.begin();
    auto end = openUpvalues.end();
    auto it { begin };
    while ((it != end) && ((*it)->location > &local)) {
        ++it;
    }

    if (it != end && (*it)->location == &local)
        return *it;

    ObjUpvalue* createdUpvalue = upvalueVal(local);

    createdUpvalue->incRef();
    openUpvalues.insert(it, createdUpvalue);

    // TODO: add debug/test code to ensure openUpvalues are decreasing stack order

    return createdUpvalue;
}


void VM::closeUpvalues(Value* last)
{
    while (!openUpvalues.empty() && (openUpvalues.front()->location >= last)) {
        ObjUpvalue* upvalue = openUpvalues.front();
        upvalue->closed = *upvalue->location;
        upvalue->location = &upvalue->closed;
        openUpvalues.pop_front();
        upvalue->decRef();
    }
}


void VM::defineMethod(ObjString* name)
{
    Value method = peek(0);
    #ifdef DEBUG_BUILD
    if (!isClosure(method))
        throw std::runtime_error("Can't create method from non-closure");
    if (!isObjectType(peek(1)))
        throw std::runtime_error("Can't create method without object or actor type on stack");
    #endif
    ObjObjectType* type = asObjectType(peek(1));
    type->methods[name->hash] = std::make_pair(name->s,method);
    pop();
}



void VM::defineNative(const std::string& name, NativeFn function)
{
    UnicodeString uname { toUnicodeString(name) };
    Value funcVal { objVal(nativeVal(function)) };
    globals[uname.hashCode()] = std::make_pair(uname,funcVal);
}



VM::InterpretResult VM::execute()
{
    if (frames.empty() || frames.back().closure->function->chunk->code.size()==0)
        return InterpretResult::OK; // nothing to execute

    auto frame { frames.end()-1 };

    auto readByte = [&]() -> uint8_t {
        #ifdef DEBUG_BUILD
            if (frame->ip == frame->closure->function->chunk->code.end())
                throw std::runtime_error("Invalid IP");
        #endif
        return *frame->ip++;
    };

    auto readShort = [&]() -> uint16_t {
        #ifdef DEBUG_BUILD
            if (frame->ip == frame->closure->function->chunk->code.end())
                throw std::runtime_error("Invalid IP");
        #endif
        frame->ip += 2;
        return (frame->ip[-2] << 8) | frame->ip[-1];
    };

    auto readConstant = [&]() -> Value {
        #ifdef DEBUG_BUILD
            return frame->closure->function->chunk->constants.at(Chunk::size_type(readByte()));
        #else
            return frame->closure->function->chunk->constants[Chunk::size_type(readByte())];
        #endif
    };

    auto readConstant2 = [&]() -> Value {
        #ifdef DEBUG_BUILD
            return frame->closure->function->chunk->constants.at(Chunk::size_type((readByte() << 8) + readByte()));
        #else
            return frame->closure->function->chunk->constants[Chunk::size_type((readByte() << 8) + readByte())];
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
            if (!frames.empty() && (frame->ip != frame->closure->function->chunk->code.end())) {
                // and instruction
                frame->closure->function->chunk->disassembleInstruction(frame->ip - frame->closure->function->chunk->code.begin());
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
            case asByte(OpCode::GetProp): {
                if (!isInstance(peek(0))) {
                    runtimeError("Only instances have properties.");
                    return InterpretResult::RuntimeError;
                }
                ObjInstance* inst = asInstance(peek(0));
                ObjString* name = readString();
                // is it an instance property?
                auto it = inst->properties.find(name->hash);
                if (it != inst->properties.end()) { // exists
                    pop();
                    push(it->second);
                }
                else { // no
                    // check if it is a method name
                    if (bindMethod(inst->instanceType, name))
                        break;

                    runtimeError("Undefined method or property '"+toUTF8StdString(name->s)+"' for instance type '"+toUTF8StdString(inst->instanceType->name)+"'.");
                    return InterpretResult::RuntimeError;
                }
                break;
            }
            case asByte(OpCode::SetProp): {
                if (!isInstance(peek(1))) {
                    runtimeError("Only instances have properties.");
                    return InterpretResult::RuntimeError;
                }
                ObjInstance* inst = asInstance(peek(1));
                ObjString* name = readString();
                //std::cout << "setting prop " << toUTF8StdString(name->s) << " of " << toString(peek(1)) << " to " << toString(peek(0)) << std::endl;//!!!
                inst->properties[name->hash] = peek(0);
                Value value { pop() };
                pop();
                push(value);
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
            case asByte(OpCode::Modulo): {
                // TODO: support decimal
                if (!peek(0).isInt()) {
                    runtimeError("Operand must be an integer");
                    return InterpretResult::RuntimeError;
                }
                if (!peek(1).isInt()) {
                    runtimeError("Operand must be an integer");
                    return InterpretResult::RuntimeError;
                }
                binaryOp([](Value a, Value b) -> Value { return mod(a,b); });
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
            case asByte(OpCode::Invoke): {
                ObjString* method = readString();
                int argCount = readByte();
                if (!invoke(method, argCount))
                    return InterpretResult::RuntimeError;
                frame = frames.end()-1;
                break;
            }
            case asByte(OpCode::Closure): {
                ObjFunction* function = asFunction(readConstant());
                ObjClosure* closure = closureVal(function);
                push(objVal(closure));
                for (int i = 0; i < closure->upvalues.size(); i++) {
                    uint8_t isLocal = readByte();
                    uint8_t index = readByte();
                    ObjUpvalue* upvalue;
                    if (isLocal) 
                        upvalue = captureUpvalue(*(frame->slots + index));
                    else 
                        upvalue = frame->closure->upvalues[index];
                    upvalue->incRef();
                    closure->upvalues[i] = upvalue;
                }
                break;
            }
            case asByte(OpCode::CloseUpvalue): {
                closeUpvalues(&(*(stackTop -1)));
                pop();
                break;
            }
            case asByte(OpCode::Return): {
                auto returningFrame { frames.back() };

                Value result = pop();
                closeUpvalues(returningFrame.slots);

                frames.pop_back();

                if (frames.empty()) {
                    pop();
                    freeObjects();

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
                auto stackIndex = (frame->slots - &stack[0]) + slot;
                if (stackIndex >= stack.size())
                    throw std::runtime_error("Stack overflow access");
                #endif
                push(frame->slots[slot]);
                break;
            }
            case asByte(OpCode::SetLocal): {
                uint8_t slot = readByte();
                #ifdef DEBUG_BUILD
                auto stackIndex = (frame->slots - &stack[0]) + slot;
                if (stackIndex >= stack.size())
                    throw std::runtime_error("Stack overflow access");
                #endif
                frame->slots[slot] = peek(0);
                break;
            }
            case asByte(OpCode::DefineGlobal): {
                ObjString* name = readString();
                globals[name->hash] = std::make_pair(name->s,peek(0));
                pop();
                break;
            }
            case asByte(OpCode::GetGlobal): {
                ObjString* name = readString();
                auto gi = globals.find(name->hash);
                if (gi != globals.end()) { // found
                    Value value { gi->second.second };
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
                auto gi = globals.find(name->hash);
                if (gi != globals.end()) { // found
                    // set new value, but leave it on stack (as assignment is an expression)
                    globals[name->hash] = std::make_pair(name->s,peek(0));
                }
                else {
                    runtimeError("Undefined variable '"+name->toStdString()+"'");
                    return InterpretResult::RuntimeError;
                }
                break;
            }
            case asByte(OpCode::SetNewGlobal): {
                ObjString* name = readString();
                auto gi = globals.find(name->hash);
                if (gi != globals.end()) { // found
                    // set new value, but leave it on stack (as assignment is an expression)
                    globals[name->hash] = std::make_pair(name->s,peek(0));
                }
                else {
                    // only automatic declaration of globals on assignment when
                    //   at global/module level scope
                    globals[name->hash] = std::make_pair(name->s,peek(0));
                }
                break;
            }
            case asByte(OpCode::GetUpvalue): {
                uint8_t slot = readByte();
                push(*frame->closure->upvalues[slot]->location);
                break;
            }
            case asByte(OpCode::SetUpvalue): {
                uint8_t slot = readByte();
                *(frame->closure->upvalues[slot]->location) = peek(0);
                break;
            }
            case asByte(OpCode::Constant2): {
                Value constant = readConstant2();
                push(constant);
                break;
            }
            case asByte(OpCode::Print): {
                std::cout << toString(pop()) << std::endl;
                break;
            }
            case asByte(OpCode::ObjectType): {
                ObjString* name = readString();
                push(objVal(objectTypeVal(name->s, /*isActor=*/false)));
                break;
            }
            case asByte(OpCode::ActorType): {
                ObjString* name = readString();
                push(objVal(objectTypeVal(name->s, /*isActor=*/true)));
                break;
            }
            case asByte(OpCode::Extend): {
                if (!isObjectType(peek(1))) {
                    runtimeError("Can only extend another actor or object type.");
                    return InterpretResult::RuntimeError;
                }
                ObjObjectType* supertype = asObjectType(peek(1));
                ObjObjectType* subtype = asObjectType(peek(0));
// std::cout << "supertype:" << objToString(peek(1)) << " subtype:" << objToString(peek(0)) << std::endl;//!!!                
// std::cout << "supertype methods:" << std::endl;
// for(auto mi=supertype->methods.begin(); mi!=supertype->methods.end();++mi)
//   std::cout << "  " << toUTF8StdString(mi->second.first) << "=" << toString(mi->second.second) << std::endl;
// std::cout << "subtype methods:" << std::endl;
// for(auto mi=subtype->methods.begin(); mi!=subtype->methods.end();++mi)
//   std::cout << "  " << toUTF8StdString(mi->second.first) << "=" << toString(mi->second.second) << std::endl;
                // TODO: nicer STL way to do this? append?
                for(auto it = supertype->methods.cbegin(); it != supertype->methods.cend(); ++it)
                    subtype->methods[it->first] = it->second;
// std::cout << "subtype methods:" << std::endl;
// for(auto mi=subtype->methods.begin(); mi!=subtype->methods.end();++mi)
//   std::cout << "  " << toUTF8StdString(mi->second.first) << "=" << toString(mi->second.second) << std::endl;
                pop(); // subclass
                pop(); // superclass?  TODO: want this??
                break;
            }
            case asByte(OpCode::GetSuper): {
                ObjString* name = readString();
                ObjObjectType* superType = asObjectType(pop());

                if (!bindMethod(superType, name)) 
                    return InterpretResult::RuntimeError;
                break;
            }
            case asByte(OpCode::Method): {
                defineMethod(readString());
                break;
            }
            case asByte(OpCode::Nop): {
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

    openUpvalues.clear(); // TODO: need to deref or delete?
}


void VM::freeObjects()
{
    if (Obj::unrefedObjs.empty()) return;

    // free objcts who's reference count dropped to 0
    while(!Obj::unrefedObjs.empty()) {
        Obj* obj { Obj::unrefedObjs.back() };
        Obj::unrefedObjs.pop_back();

        if (obj != nullptr)
            delObj(obj);
    }
    
    Obj::unrefedObjs.clear();
}


void VM::outputAllocatedObjs()
{
    #ifdef DEBUG_TRACE_MEMORY
    if (Obj::allocatedObjs.size()>0) {
        std::cout << std::hex;
        std::cout << "== allocated Objs (" << std::to_string(Obj::allocatedObjs.size()) << ") ==" << std::endl;
        for(const auto& p : Obj::allocatedObjs) {
            std::cout << "  " << uint64_t(p.first) << " " << p.second << std::endl;
        }
        std::cout << std::dec;
    }
    #endif
}


void VM::concatenate()
{
    #ifdef DEBUG_BUILD
        if (!isString(peek(1)))
            throw std::runtime_error("concatenate called with non-String LHS");
    #endif

    Value rhs { peek(0) };
    Value lhs { peek(1) };

    UnicodeString lhsString { asUString(lhs) };
    UnicodeString rhsString {};
    
    if (!isString(rhs)) {
        // convert RHS to a string
        // TODO: use canonical type -> string conversion using unicode instead
        //  of 'internal' toString()
        rhsString = toUnicodeString(toString(rhs));
    }
    else
        rhsString = asUString(rhs);

    UnicodeString combined { lhsString + rhsString };
    pop();
    pop();
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

    size_t instruction = frame->ip - frame->closure->function->chunk->code.begin() - 1;
    int line = frame->closure->function->chunk->getLine(instruction);
    fprintf(stderr, "[line %d] in script\n", line);
    resetStack();    
}


// native

static Value clockNative(int argCount, Value* args)
{
    return realVal(double(clock())/CLOCKS_PER_SEC);
}



void VM::defineNativeFunctions()
{
    defineNative("clock", clockNative);
}
