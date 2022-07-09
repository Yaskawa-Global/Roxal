#include <functional>
#include <time.h>
#include <chrono>
#include <thread>

#include "ASTGenerator.h"
#include "ASTGraphviz.h"
#include "RoxalCompiler.h"

#include "VM.h"
#include "Object.h"
#include "Stream.h"

using namespace roxal;


VM::VM()
    : lineMode(false), threadSleep(false)
{
    resetStack();
    defineBuiltinFunctions();
    defineNativeFunctions();
    initString = stringVal(UnicodeString("init"));
    initString->incRef();

    auto engine = StreamEngine::instance();
}


VM::VM(std::istream& linestream)
    : lineMode(true), lineStream(&linestream)
{
    resetStack();
    defineBuiltinFunctions();
    defineNativeFunctions();
    initString = stringVal(UnicodeString("init"));
    initString->incRef();
    StreamEngine::instance();
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

void VM::popN(size_t n)
{
    for(auto i=0; i<n; i++) pop();
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
    std::vector<CallFrame> defValFrames {};

    // handle execution of default param expression 'func' for params not supplied
    if (argCount < closure->function->arity) {
        assert(closure->function->funcType.has_value());
        ptr<type::Type> funcType { closure->function->funcType.value() };
        auto paramTypes { funcType->func.value().params };
        // for each missing arg
        for(size_t paramIndex = argCount; paramIndex < closure->function->arity; paramIndex++) {
            auto param { paramTypes.at(paramIndex) };
            assert(param.has_value());
            auto paramName = param.value().name;
            if (param.value().hasDefault) {
                auto funcIt = closure->function->paramDefaultFunc.find(param.value().nameHashCode);
                assert(funcIt != closure->function->paramDefaultFunc.cend());

                ObjFunction* defValFunc = funcIt->second;

                // call it, which will leave the returned default val on the stack as an arg for this call
                ObjClosure* defValClosure = closureVal(defValFunc);
                if (defValClosure->upvalues.size() > 0) {
                    runtimeError("Captured variables in default parameter '"+toUTF8StdString(paramName)+"' value expressions are not allowed"
                                +" in declaration of function '"+toUTF8StdString(closure->function->name)+"'.");
                    return false;
                }

                call(defValClosure,0);
                defValFrames.push_back(*(frames.end()-1));
                frames.pop_back();

                argCount++;
            }
            else {
                runtimeError("Call to function '"+toUTF8StdString(closure->function->name)+"'"
                             +" is missing argument for parameter '"+toUTF8StdString(paramName)+"'"
                             +" (and no default is specified)");
                return false;
            }
        }
    }
    else if (argCount > closure->function->arity) {
        runtimeError("Passed "+std::to_string(argCount)+" arguments for function "
                    +toUTF8StdString(closure->function->name)+" which has "
                    +std::to_string(closure->function->arity)+" parameters.");
        return false;
    }
    assert(argCount == closure->function->arity);

    if (frames.size() > 128) {
        runtimeError("Stack overflow.");
        return false;
    }

    CallFrame callframe {};
    callframe.closure = closure;
    callframe.ip = closure->function->chunk->code.begin();
    callframe.slots = &(*(stackTop - argCount - 1 + defValFrames.size()));
    frames.push_back(callframe);

    // the closures for default arg values must be executed before this closure call, so put
    //  them above it on the frame stack
    int f=defValFrames.size();
    for(auto fi = defValFrames.rbegin(); fi != defValFrames.rend(); fi++) {
        auto& frame { *fi };
        frame.slots = &(*(stackTop - 1 + f));
        frames.push_back(frame);
        f--;
    }

    return true;
}


bool VM::call(ValueType builtinType, int argCount)
{
    auto argBegin = stackTop - argCount;
    auto argEnd = stackTop;

    *(stackTop - argCount - 1) = construct(builtinType, argBegin, argEnd);
    popN(argCount);
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
                Value result { (this->*native)(argCount, &(*stackTop) - argCount) };
                *(stackTop - argCount - 1) = result;
                popN(argCount);
                return true;
            }
            default:
                break;
        }
    }
    else if (callee.isType()) {
        auto type { callee.asType() };
        return call(type, argCount);
    }
    runtimeError("Only functions, builtin-types, objects and actors can be called.");
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
        runtimeError("Only instances have methods.");
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


bool VM::indexValue(const Value& indexable, int subscriptCount)
{
    if (indexable.isObj()) {
        // TODO: move some per-type indexing code into Object or Value 
        switch (objType(indexable)) {
            case ObjType::String: {
                if (subscriptCount != 1) {
                    runtimeError("String indexing requires a single subscript.");
                    return false;
                }
                ObjString* str = asString(indexable);
                Value index = pop();
                if (!index.isNumber()) {
                    runtimeError("String indexing subscript must be a number.");
                    return false;
                }
                const UnicodeString& ustr { str->s };
                UnicodeString substr {};
                // TODO: bounds check
                ustr.extract(int32_t(index.asInt()),1,substr);
                auto newStrValue { objVal(stringVal(substr)) };
                pop(); // discard indexable
                push(newStrValue);
                return true;
            }
            case ObjType::List: {
                if (subscriptCount != 1) {
                    runtimeError("List indexing requires a single subscript.");
                    return false;
                }
                ObjList* list = asList(indexable);
                Value index = pop();
                if (!index.isNumber()) {
                    runtimeError("List indexing subscript must be a number.");
                    return false;
                }
                // TODO: bounds check
                Value result { list->elts.at(index.asInt()) };
                pop(); // discard indexable
                push(result);
                return true;
            }
            case ObjType::Dict: {
                if (subscriptCount != 1) {
                    runtimeError("Dict lookup requires a single key index.");
                    return false;
                }
                ObjDict* dict = asDict(indexable);
                Value index = pop();
                // TODO: bounds check
                Value result { dict->entries.at(index) };
                pop(); // discard indexable
                push(result);
                return true;
            }
            case ObjType::Stream: {
                if (subscriptCount != 1) {
                    runtimeError("Stream indexing requires a single numeric index <= 0.");
                    return false;
                }
                Value index = pop();
                if (!index.isNumber() || index.asInt() > 0) {
                    runtimeError("Stream indexing requires a single numeric index <= 0.");
                    return false;
                }
                auto n = index.asInt();
                Stream* stream = asStream(indexable);
                Value result;
                if (n==0)
                    result = indexable;
                else if (n==-1) 
                    result = Stream::prev(n,indexable);
                else {
                    runtimeError("Stream index "+std::to_string(n)+" out of range.");
                    return false;
                }
                pop(); // discard indexable
                push(result);
                return true;
            }
            // indexing a closure occurs in special case of a function that returns a stream,
            //  where the function name is used to refer to the returned stream (e.g. f[-1] is prev value)
            case ObjType::Closure: {
                if (subscriptCount != 1) {
                    runtimeError("Stream indexing requires a single numeric index <= 0.");
                    return false;
                }
                Value index = pop();
                if (!index.isNumber() || index.asInt() > 0) {
                    runtimeError("Stream indexing requires a single numeric index <= 0.");
                    return false;
                }
                auto n = index.asInt();
                if (n==0) {
                    runtimeError("Can't access the current value of a stream func in its evaluation (only previous values).");
                    return false;
                }

                // since the returned stream doesn't exist yet, create a named stream proxy
                //  using the function name
                auto frame { frames.end()-1 };
                auto func { frame->closure->function };
                // TODO: check this is a func not a proc

                // string name var/placeholder for stream as it doesn't exist yet
                Value indexable { stringVal(func->name) };

                if (n==-1) {
                    Value stream { Stream::prev(n,indexable) };
                    frame->forwardStreamRefs[func->name] = stream;
                    //std::cout << "Added forward stream ref " << toUTF8StdString(func->name) << std::endl;//!!!
                    pop(); // discard indexable
                    push(stream);
                }
                else {
                    runtimeError("Stream index "+std::to_string(n)+" out of range.");
                    return false;
                }
                return true;
            }
            default:
                break;
        }
        runtimeError("Only strings, lists, [vectors, dicts, matrices, tensors - unimplemented] and streams can be indexed, not type "+objTypeName(indexable.asObj())+".");
        return false;
    }
    runtimeError("Only strings, lists, [vectors, dicts, matrices, tensors - unimplemented] and streams can be indexed, not type "+indexable.typeName()+".");
    return false;
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

    ObjUpvalue* createdUpvalue = upvalueVal(&local);

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

    uint8_t instruction {};

    for(;;) {
        #if defined(DEBUG_TRACE_EXECUTION)
            // output stack
            if (stack.size() > 0) {
                std::cout << "          ";
                for(auto vi = stack.begin(); vi < stackTop; vi++) {
                    bool aString = vi->isObj() && isString(*vi);
                    bool aStream = vi->isObj() && isStream(*vi);
                    std::cout << "[";
                    if (frame->slots == &(*vi) )
                        std::cout << "F^"; // show frame pointer
                    std::cout << " ";
                    if (aStream)
                        std::cout << "<stream ";
                    if (aString)
                        std::cout << "'"; // quote strings
                    std::cout << toString(*vi);
                    if (aStream)
                        std::cout << ">";
                    if (aString)
                        std::cout << "'";
                    if (vi->isNumber()) // show numeric type
                        std::cout << ":" << vi->typeName().at(0);

                    std::cout << " ]";
                }
                std::cout << std::endl;
            }
            if (frame->ip != frame->closure->function->chunk->code.end()) {
                // and instruction
                frame->closure->function->chunk->disassembleInstruction(frame->ip - frame->closure->function->chunk->code.begin());
            }
            else {
                std::cout << "          <end of chunk>" << std::endl;
                return InterpretResult::RuntimeError;
            }
        #endif

        StreamEngine::instance()->updateStreamStates();


        if (threadSleep) {
            if (StreamEngine::instance()->currentTime() >= threadSleepUntil) {
                threadSleep = false;
            }
            else
                goto postInstructionDispatch;
        }

        instruction = readByte();
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
                    runtimeError("Operand to > must be a number");
                    return InterpretResult::RuntimeError;
                }
                if (!peek(1).isNumber()) {
                    runtimeError("Operand to > must be a number");
                    return InterpretResult::RuntimeError;
                }
                binaryOp([](Value a, Value b) -> Value { return greater(a,b); });
                break;
            }
            case asByte(OpCode::Less): {
                if (!peek(0).isNumber()) {
                    runtimeError("Operand to < must be a number");
                    return InterpretResult::RuntimeError;
                }
                if (!peek(1).isNumber()) {
                    runtimeError("Operand to < must be a number");
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
                else if (isStream(peek(0)) || isStream(peek(1)))
                    binaryOp([](Value a, Value b) -> Value { return add(a,b); });
                else {
                    runtimeError("Operands of + must be two numbers or a strings LHS");
                    return InterpretResult::RuntimeError;
                }
                break;
            }
            case asByte(OpCode::Subtract): {
                if (!peek(0).isNumber()) {
                    runtimeError("Operand of - must be a number");
                    return InterpretResult::RuntimeError;
                }
                if (!peek(1).isNumber()) {
                    runtimeError("Operand of - must be a number");
                    return InterpretResult::RuntimeError;
                }
                binaryOp([](Value a, Value b) -> Value { return subtract(a,b); });
                break;
            }
            case asByte(OpCode::Multiply): {
                if (!peek(0).isNumber()) {
                    runtimeError("Operand of * must be a number");
                    return InterpretResult::RuntimeError;
                }
                if (!peek(1).isNumber()) {
                    runtimeError("Operand of * must be a number");
                    return InterpretResult::RuntimeError;
                }
                binaryOp([](Value a, Value b) -> Value { return multiply(a,b); });
                break;
            }
            case asByte(OpCode::Divide): {
                if (!peek(0).isNumber()) {
                    runtimeError("Operand of / must be a number");
                    return InterpretResult::RuntimeError;
                }
                if (!peek(1).isNumber()) {
                    runtimeError("Operand of / must be a number");
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
                    runtimeError("Operand of 'not' or negation must be a number, bool or nil");
                    return InterpretResult::RuntimeError;
                }
                break;
            }
            case asByte(OpCode::Modulo): {
                // TODO: support decimal
                if (!peek(0).isInt()) {
                    runtimeError("Operand of '%' must be an integer");
                    return InterpretResult::RuntimeError;
                }
                if (!peek(1).isInt()) {
                    runtimeError("Operand of '%' must be an integer");
                    return InterpretResult::RuntimeError;
                }
                binaryOp([](Value a, Value b) -> Value { return mod(a,b); });
                break;
            }
            case asByte(OpCode::And): {
                if (!peek(0).isBool()) {
                    runtimeError("Operand of 'and' must be a bool");
                    return InterpretResult::RuntimeError;
                }
                if (!peek(1).isBool()) {
                    runtimeError("Operand of 'and' must be a bool");
                    return InterpretResult::RuntimeError;
                }
                binaryOp([](Value a, Value b) -> Value { return land(a,b); });
                break;
            }
            case asByte(OpCode::Or): {
                if (!peek(0).isBool()) {
                    runtimeError("Operand of 'or' must be a bool");
                    return InterpretResult::RuntimeError;
                }
                if (!peek(1).isBool()) {
                    runtimeError("Operand of 'or' must be a bool");
                    return InterpretResult::RuntimeError;
                }
                binaryOp([](Value a, Value b) -> Value { return lor(a,b); });
                break;
            }
            case asByte(OpCode::FollowedBy): {
                binaryOp([](Value a, Value b) -> Value { return followedBy(a,b); });
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
            case asByte(OpCode::Index): {
                uint8_t argCount = readByte();
                if (!indexValue(peek(argCount), argCount))
                    return InterpretResult::RuntimeError;
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

                if (!returningFrame.forwardStreamRefs.empty()) {
                    auto funcName { returningFrame.closure->function->name };
                    //std::cout << "returning frame "+toUTF8StdString(funcName)+" has forward stream refs" << std::endl;//!!!                    
                    for(auto& forwardStreamRef : returningFrame.forwardStreamRefs) {
                        const auto& name { forwardStreamRef.first };
                        auto& stream { forwardStreamRef.second };
                        #ifdef DEBUG_BUILD
                        assert(isStream(stream));
                        if (name != funcName) // shouldn't happen
                            throw std::runtime_error("invalid forward stream reference "+toUTF8StdString(name)+" in func "+toUTF8StdString(funcName));
                        #endif
                        if (!isStream(result)) {
                            runtimeError("Func '"+toUTF8StdString(funcName)+"' must return a stream (not type "+result.typeName()+")");
                            return InterpretResult::RuntimeError;
                        }
                        asStream(stream)->patch(name,result);
                    }
                }

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
                globals[name->hash] = std::make_pair(name->s,pop());
                break;
            }
            case asByte(OpCode::GetGlobal): {
                ObjString* name = readString();
                auto gi = globals.find(name->hash);
                if (gi != globals.end()) { // found
                    Value value = gi->second.second;
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
            case asByte(OpCode::NewList): {
                int eltCount = readByte();
                std::vector<Value> elts {};
                elts.reserve(eltCount);
                // top of stack is last list elt by index
                for(int i=0; i<eltCount;i++)
                    elts.push_back(peek(eltCount-i-1));
                for(int i=0; i<eltCount;i++) pop();
                push(objVal(listVal(elts)));
                break;
            }
            case asByte(OpCode::NewDict): {
                int entryCount = readByte();
                std::vector<std::pair<Value,Value>> entries {};
                entries.reserve(entryCount);
                // top of stack is last dict entry (the value)
                for(int i=0; i<entryCount;i++) {
                    entries.push_back(std::make_pair(peek(2*(entryCount-1-i)+1),
                                                     peek(2*(entryCount-1-i))));
                }
                for(int i=0; i<entryCount*2;i++) pop();
                push(objVal(dictVal(entries)));
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
            case asByte(OpCode::Method): {
                defineMethod(readString());
                break;
            }
            case asByte(OpCode::Nop): {
                break;
            }
        }

        postInstructionDispatch:

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

        delObj(obj);
    }
    
    Obj::unrefedObjs.clear();
}


void VM::outputAllocatedObjs()
{
    #ifdef DEBUG_TRACE_MEMORY
    if (Obj::allocatedObjs.size()>0) {
        std::cout << std::hex;
        std::cout << "== allocated Objs (" << Obj::allocatedObjs.size() << ")==" << std::endl;
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



//
// builtins

void VM::defineBuiltinFunctions()
{
    defineNative("print", &VM::print_builtin);
    defineNative("sleep", &VM::sleep_builtin);
}


Value VM::print_builtin(int argCount, Value* args)
{
    if (argCount != 1) 
        throw std::invalid_argument("print expects single argument (convertable to a string)");

    auto str = toString(args[0]);
    std::cout << str << std::endl;
    return nilVal();
}


Value VM::sleep_builtin(int argCount, Value* args)
{
    if ((argCount != 1) || !args[0].isNumber()) 
        throw std::invalid_argument("sleep expects single numeric argument (microsecs)");

    uint64_t nanosecs = uint64_t(args[0].asInt()) * 1000;

    threadSleep = true;
    threadSleepUntil = StreamEngine::instance()->currentTime() + nanosecs;

    return nilVal();
}



//
// native

void VM::defineNativeFunctions()
{
    defineNative("_clock", &VM::clock_native);
    defineNative("_ussleep", &VM::usSleep_native);
    defineNative("_mssleep", &VM::msSleep_native);
    //defineNative("_sleep", &VM::sleep_native);
}


Value VM::clock_native(int argCount, Value* args)
{
    return realVal(double(clock())/CLOCKS_PER_SEC);
}

Value VM::usSleep_native(int argCount, Value* args)
{
    if ((argCount != 1) || !args[0].isNumber()) 
        throw std::invalid_argument("ussleep expects single numeric argument");

    std::this_thread::sleep_for(std::chrono::microseconds(args[0].asInt()));

    return nilVal();
}

Value VM::msSleep_native(int argCount, Value* args)
{
    if ((argCount != 1) || !args[0].isNumber()) 
        throw std::invalid_argument("mssleep expects single numeric argument");

    std::this_thread::sleep_for(std::chrono::milliseconds(args[0].asInt()));

    return nilVal();
}

Value VM::sleep_native(int argCount, Value* args)
{
    if ((argCount != 1) || !args[0].isNumber()) 
        throw std::invalid_argument("sleep expects single numeric argument");

    std::this_thread::sleep_for(std::chrono::seconds(args[0].asInt()));

    return nilVal();
}

