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
    : lineMode(false)
{
    thread = nullptr;
    defineBuiltinFunctions();
    defineNativeFunctions();
    initString = stringVal(UnicodeString("init"));
    initString->incRef();

    auto engine = StreamEngine::instance();
    //CallSpec::testParamPositions();
    //Value::testPrimitiveValues();
    //testObjectValues();
}



VM::~VM()
{
    globals.clear();

    initString->decRef();

    freeObjects();

    #ifdef DEBUG_TRACE_MEMORY
    outputAllocatedObjs();
    #endif
}


void VM::setDissasemblyOutput(bool outputBytecodeDissasembly)
{
    this->outputBytecodeDissasembly = outputBytecodeDissasembly;
}




VM::InterpretResult VM::interpret(std::istream& source, const std::string& name)
{
    ObjFunction* function { nullptr };

    try {
        RoxalCompiler compiler {};
        compiler.setOutputBytecodeDissasembly(outputBytecodeDissasembly);

        function = compiler.compile(source, name);

    } catch (std::exception& e) {
        return InterpretResult::CompileError;
    }

    if (function == nullptr)
        return InterpretResult::CompileError;

    ObjClosure* closure = closureVal(function);
    Value closureValue { objVal(closure) };

    auto firstThread = std::make_shared<Thread>();     
    threads.store(firstThread->id(), firstThread);
    firstThread->spawn(closureValue);

    // join all threads 
    //  (note: threads being waited on may create additional threads)
    while (threads.size() > 0) {
        for(auto thread : threads.get()) {
            thread.second->join();
            threads.erase(thread.first);
        }
    }

    threads.clear();

    InterpretResult result = firstThread->result; 

    #if defined(DEBUG_TRACE_EXECUTION)
    if (globals.size() > 0) {        
        std::cout << std::endl << "== globals ==" << std::endl;
        for(const auto& global : globals.get()) 
            std::cout << toUTF8StdString(global.second.first) << " = " << toString(global.second.second) << std::endl;
    }
    #endif

    freeObjects();

    return result;
}


VM::InterpretResult VM::interpretLine(std::istream& linestream)
{
    lineMode = true;
    lineStream = &linestream;
    //...
    return InterpretResult::OK;
}



thread_local ptr<VM::Thread> VM::thread;


void VM::Thread::spawn(Value closure)
{
    assert(isClosure(closure));

    state = State::Spawned;
    osthread = std::make_shared<std::thread>([this,closure]() { 
        auto& vm { VM::instance() };

        vm.thread = shared_from_this(); // set thread local storage member

        vm.resetStack();
        push(closure);
        vm.call(asClosure(closure),CallSpec(0));

        vm.execute();
        // (ignoring result, as thread is terminating anyway)

        stack.clear();

        state = State::Completed;
    });
}

void VM::Thread::join()
{
    if (state == State::Constructed)
        throw std::runtime_error("Can't join Thread that hasn't completed spawning yet. id:"+std::to_string(thisid));

    if ((osthread == nullptr) || !osthread->joinable())
        throw std::runtime_error("Can't join Thread that isn't joinable. id:"+std::to_string(thisid));

    if (actor) {
        auto inst { asActorInstance(actorInstance) };
        std::lock_guard<std::mutex> lock { inst->queueMutex };
        quit = true;
        inst->queueConditionVar.notify_one();
    }

    osthread->join();
    osthread = nullptr;
    actorInstance = nilVal();

    state = State::Completed;
}


void VM::Thread::act(Value actorInstance)
{
    assert(isActorInstance(actorInstance));
    this->actorInstance = actorInstance;

    actor = true;
    state = State::Spawned;

    osthread = std::make_shared<std::thread>([this,actorInstance]() { 
        auto& vm { VM::instance() };

        vm.thread = shared_from_this(); // set thread local storage member

        ActorInstance* actorInst = asActorInstance(actorInstance);

        vm.resetStack();
        // frame local 0 is actor 'this' instance for actor method (as for object methods)
        push(actorInstance);

        do {

            ActorInstance::MethodCallInfo callInfo {};
            {
                std::unique_lock<std::mutex> lock { actorInst->queueMutex };
                actorInst->queueConditionVar.wait(lock,[&]()
                {
                    // Acquire the lock only if we should quit or the queue has pending calls
                    return quit || !actorInst->callQueue.empty();
                });
                if (!actorInst->callQueue.empty()) {
                    callInfo = actorInst->callQueue.pop();
                }
                if (quit)
                    break;
            }

            if (callInfo.valid()) {
                
                auto closure = asBoundMethod(callInfo.callee)->method;


                for(auto it = callInfo.args.rbegin(); it != callInfo.args.rend(); ++it) 
                    push(*it);
                
                vm.call(closure,callInfo.callSpec);

                auto result = vm.execute();

                if (result.first == InterpretResult::OK) {

                    if (callInfo.returnPromise != nullptr) 
                        callInfo.returnPromise->set_value(result.second);

                }
                else {
                    // error occured, terminate actor (thread)
                    if (callInfo.returnPromise != nullptr)
                        callInfo.returnPromise->set_value(nilVal());
                    quit = true;
                    break;
                }

                    
            }

        } while (true);

        stack.clear();

        state = State::Completed;
    });

}


void VM::Thread::detach()
{
    assert(state != State::Constructed);

    if (osthread != nullptr)
        osthread->detach();
}




void VM::Thread::push(const Value& value)
{
    *stackTop = value;
    stackTop++;

    #ifdef DEBUG_BUILD
    if (stackTop == stack.end())
        throw std::runtime_error("Stack overflow");
    #endif
}


Value VM::Thread::pop()
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

void VM::Thread::popN(size_t n)
{
    for(auto i=0; i<n; i++) pop();
}



Value& VM::Thread::peek(int distance)
{
    #ifdef DEBUG_BUILD
    if (stackTop - stack.begin() <= distance)
        throw std::runtime_error("Stack underflow access ("+std::to_string(distance)+" stacksize:"+std::to_string(stackTop - stack.begin())+")");
    #endif
    return *(stackTop - 1 - distance);
}


std::atomic<uint64_t> VM::Thread::nextId = 1;


void VM::Thread::outputStack()
{
    // output stack
    if (stack.size() > 0) {

        std::cout << "          ";
        for(auto vi = stack.begin(); vi < stackTop; vi++) {
            bool aString = vi->isObj() && isString(*vi);
            bool aStream = vi->isObj() && isStream(*vi);
            std::cout << "[";
            if (!frames.empty() && (frames.end()-1)->slots == &(*vi) )
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
}





bool VM::call(ObjClosure* closure, const CallSpec& callSpec)
{
    // closure,frame pair for any param default value 'func' calls
    std::vector<std::pair<Value,CallFrame>> defValFrames {};

    // fast-path: if callee supplied all all arguments by position and none are missing,
    //  nothing special to do
    bool paramDefaultAndArgsReorderNeeded = !(callSpec.allPositional && callSpec.argCount == closure->function->arity);

    CallFrame callframe {};
    auto argCount = callSpec.argCount;

    if (paramDefaultAndArgsReorderNeeded) {

        assert(closure->function->funcType.has_value());
        ptr<type::Type> calleeType { closure->function->funcType.value() };

        callframe.reorderArgs = callSpec.paramPositions(calleeType, true);        

        // handle execution of default param expression 'func' for params not supplied
        if (argCount < closure->function->arity) {
            auto paramTypes { calleeType->func.value().params };
            // for each missing arg
            for(int16_t paramIndex = 0; paramIndex < callframe.reorderArgs.size(); paramIndex++) {
                if (callframe.reorderArgs[paramIndex] == -1) { // -1 -> not supplied in callSpec

                    // lookup param name hash
                    auto param { paramTypes.at(paramIndex) };
                    #ifdef DEBUG_BUILD
                    assert(param.has_value());
                    #endif
                    //auto paramName = param.value().name;

                    auto funcIt = closure->function->paramDefaultFunc.find(param.value().nameHashCode);
                    #ifdef DEBUG_BUILD
                    assert(funcIt != closure->function->paramDefaultFunc.cend());
                    #endif

                    ObjFunction* defValFunc = funcIt->second;

                    // call it, which will leave the returned default val on the stack as an arg for this call
                    ObjClosure* defValClosure = closureVal(defValFunc);

                    // normal after emit Op Closure in compiler
                    // for (int i = 0; i < function->upvalueCount; i++) {
                    //         emitByte(functionScope.upvalues[i].isLocal ? 1 : 0);
                    //         emitByte(functionScope.upvalues[i].index);
                    //     }

                    // normal on exec Closure Op in VM
                    // for (int i = 0; i < closure->upvalues.size(); i++) {
                    //     uint8_t isLocal = readByte();
                    //     uint8_t index = readByte();
                    //     ObjUpvalue* upvalue;
                    //     if (isLocal) 
                    //         upvalue = captureUpvalue(*(frame->slots + index));
                    //     else 
                    //         upvalue = frame->closure->upvalues[index];
                    //     upvalue->incRef();
                    //     closure->upvalues[i] = upvalue;
                    // }

                    if (defValClosure->upvalues.size() > 0) {
                        auto paramName = param.value().name;
                        runtimeError("Captured variables in default parameter '"+toUTF8StdString(paramName)+"' value expressions are not allowed"
                                    +" in declaration of function '"+toUTF8StdString(closure->function->name)+"'.");
                        return false;
                    }

                    call(defValClosure,CallSpec(0));
                    defValFrames.push_back(std::make_pair(objVal(defValClosure) ,*(thread->frames.end()-1)) );
                    thread->popFrame();

                    // push a place-holder (nil) value onto the stack for the value
                    //  (since caller didn't push it before the call)
                    push(nilVal());

                    // record ...


                    // now add the map from param index to arg where it will be on the stack
                    //  once the default value func returns
                    callframe.reorderArgs[paramIndex] = argCount;
                    argCount++;

                }
            }

            // if the final arg ordering matches parameter ordering (i.e. in-order)
            //  then no need to reorder stack later
            bool argsInOrder = true;
            for(int16_t i=0; i<callframe.reorderArgs.size();i++)
                if (callframe.reorderArgs[i] != i) {
                    argsInOrder=false;
                    break;
                }
            if (argsInOrder)
                callframe.reorderArgs.clear();
        }
        else if (argCount > closure->function->arity) {
            runtimeError("Passed "+std::to_string(argCount)+" arguments for function "
                        +toUTF8StdString(closure->function->name)+" which has "
                        +std::to_string(closure->function->arity)+" parameters.");
            return false;
        }
        assert(argCount == closure->function->arity);
    }

    if (thread->frames.size() > 128) {
        runtimeError("Stack overflow.");
        return false;
    }


    callframe.closure = closure;
    callframe.startIp = callframe.ip = closure->function->chunk->code.begin();
    callframe.slots = &(*(thread->stackTop - argCount - 1));
    thread->pushFrame(callframe);
    thread->frameStart = true;

    // the closures for default arg values must be executed before this closure call, so put
    //  them above it on the frame stack
    // NB: although these default value call frames are all stacked one upon another, they
    //     logically have the main callframe as their parent (so we set it as such)
    auto numDefaultValueFrames = defValFrames.size();
    if (numDefaultValueFrames>0) {
        CallFrames::iterator parentCallFrame = thread->frames.end()-1;
        for(auto fi = defValFrames.rbegin(); fi != defValFrames.rend(); fi++) {
            auto& closureFrame { *fi };
            push(closureFrame.first); // push closure value for def val func
            auto& frame { closureFrame.second };
            frame.parent = parentCallFrame;
            frame.slots = &(*(thread->stackTop - 1));    
            thread->pushFrame(frame); 
            // reset parent
            (thread->frames.end()-1)->parent = parentCallFrame;

            thread->frameStart = true;
            numDefaultValueFrames--;
        }

        if (thread->frames.size() > 128) {
            runtimeError("Stack overflow.");
            return false;
        }
    }

    return true;
}



bool VM::call(ValueType builtinType, const CallSpec& callSpec)
{
    if (!callSpec.allPositional)
        throw std::runtime_error("Named parameters unsupported in constructor for "+to_string(builtinType));
    auto argBegin = thread->stackTop - callSpec.argCount;
    auto argEnd = thread->stackTop;

    *(thread->stackTop - callSpec.argCount - 1) = construct(builtinType, argBegin, argEnd);
    popN(callSpec.argCount);
    return true;
}


bool VM::callValue(const Value& callee, const CallSpec& callSpec)
{
    if (callee.isObj()) {
        switch (objType(callee)) {
            case ObjType::BoundMethod: {
                ObjBoundMethod* boundMethod { asBoundMethod(callee) };

                if (!isActorInstance(boundMethod->receiver)) {
                    *(thread->stackTop - callSpec.argCount - 1) = boundMethod->receiver;
                    return call(boundMethod->method, callSpec);
                }
                else {
                    // call to actor method.  Instead of calling on this thread,
                    //  queue the call for the actor thread to handle

                    ActorInstance* inst = asActorInstance(boundMethod->receiver);
                    Value future = inst->queueCall(callee, callSpec, &(*thread->stackTop) );

                    popN(callSpec.argCount + 1); // args & callee

                    push(future);

                    return true;
                }
            }
            case ObjType::ObjectType: {
                ObjObjectType* type = asObjectType(callee);
                if (!type->isActor) {
                    Value inst { objectInstanceVal(type) };
                    *(thread->stackTop - callSpec.argCount - 1) = inst;
                }
                else {
                    Value inst { actorInstanceVal(type) };

                    // spawn Thread to handle actor method calls
                    auto newThread = std::make_shared<Thread>();
                    threads.store(newThread->id(), newThread);
                    newThread->act(inst);

                    *(thread->stackTop - callSpec.argCount - 1) = inst;
                }
                auto it = type->methods.find(initString->hash);
                if (it != type->methods.end()) {
                    Value initializer { it->second.second };
                    if (!type->isActor)
                        return call(asClosure(initializer), callSpec);
                    else
                        throw std::runtime_error("queueing actor init params unimplemented");//!!!
                }
                else {
                    if (callSpec.argCount != 0) {
                        runtimeError("Expected 0 arguments for type instantiation, provided "+std::to_string(callSpec.argCount));
                        return false;
                    }
                }
                return true;
            }
            case ObjType::Closure:
                return call(asClosure(callee), callSpec);
            case ObjType::Native: {
                NativeFn native = asNative(callee)->function;
                Value result { (this->*native)(callSpec.argCount, &(*thread->stackTop) - callSpec.argCount) };
                *(thread->stackTop - callSpec.argCount - 1) = result;
                popN(callSpec.argCount);
                return true;
            }
            case ObjType::Instance: {
                runtimeError("object instances are not callable.");
                return false;
            }
            case ObjType::Actor: {
                runtimeError("actor instances are not callable.");
                return false;
            }
            default:
                break;
        }
    }
    else if (callee.isType()) {
        auto type { callee.asType() };
        return call(type, callSpec);
    }
    runtimeError("Only functions, builtin-types, objects and actors can be called.");
    return false;
}



bool VM::invokeFromType(ObjObjectType* type, ObjString* name, const CallSpec& callSpec)
{
    auto it = type->methods.find(name->hash);
    if (it == type->methods.end()) {
        runtimeError("Undefined property '%s'",toUTF8StdString(name->s).c_str());
        return false;
    }
    Value method { it->second.second };
    return call(asClosure(method), callSpec);
}



bool VM::invoke(ObjString* name, const CallSpec& callSpec)
{
    Value receiver { peek(callSpec.argCount) };

    if (isObjectInstance(receiver)) {

        ObjectInstance* instance = asObjectInstance(receiver);

        // check to ensure name isn't a prop with a func in it
        auto it = instance->properties.find(name->hash);
        if (it != instance->properties.end()) { // it is a prop
            Value value { it->second };
            *(thread->stackTop - callSpec.argCount - 1) = value;
            return callValue(value, callSpec);
        }

        return invokeFromType(instance->instanceType, name, callSpec);
    }
    else if (isActorInstance(receiver)) {
        throw std::runtime_error("invoke() for actor instance unimplemented");//!!!
    }
    else {
        runtimeError("Only object or actor instances have methods.");
        return false;
    }
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
                auto frame { thread->frames.end()-1 };
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


// execute OpCode::Return 
//  returns the call frame's result Value (doesn't push on stack, or update current frame)
Value VM::opReturn()
{
    auto returningFrame { thread->frames.back() };

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
            if (!isStream(result)) 
                throw std::runtime_error("Func '"+toUTF8StdString(funcName)+"' must return a stream (not type "+result.typeName()+")");            
            asStream(stream)->patch(name,result);
        }
    }

    thread->popFrame();

    if (!thread->frames.empty()) {
        auto popCount = &(*thread->stackTop) - returningFrame.slots;
        //stackTop -= popCount;
        // loop to ensure stack Values unref'd
        // TODO: could make popn(n) method
        for(auto i=0; i<popCount; i++)
            pop();
    }

    return result;
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
    globals.store(uname.hashCode(), std::make_pair(uname,funcVal));
}



std::pair<VM::InterpretResult,Value> VM::execute()
{
    if (thread->frames.empty() || thread->frames.back().closure->function->chunk->code.size()==0)
        return std::make_pair(InterpretResult::OK,nilVal()); // nothing to execute

    auto frame { thread->frames.end()-1 };

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

    auto errorReturn = std::make_pair(InterpretResult::RuntimeError,nilVal());


    //
    //  main dispatch loop

    uint8_t instruction {};

    for(;;) {
        #if defined(DEBUG_TRACE_EXECUTION)
            // output stack
            thread->outputStack();
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


        if (thread->threadSleep) {
            if (StreamEngine::instance()->currentTime() >= thread->threadSleepUntil) {
                thread->threadSleep = false;
            }
            else
                goto postInstructionDispatch;
        }


        if (thread->frameStart) {
            // handle assignment of default param values to tail of args slots
            //  (hence, this must happen before reordering below)
            if (!frame->tailArgValues.empty()) {
                int16_t argIndex = frame->closure->function->arity - frame->tailArgValues.size();
                for(const auto& argValue : frame->tailArgValues) {
                    *(frame->slots + 1 + argIndex) = argValue;
                    argIndex++;
                }
                frame->tailArgValues.clear();
            }

            // handle re-ordering arguments on top of stack 
            //  (to reorder from caller argument order to callee parameter order)
            if (!frame->reorderArgs.empty()) {

                const auto& reorder { frame->reorderArgs };
                auto argCount { reorder.size() };
                Value args[argCount];
                // pop args from stack (they're in reverse order from top)
                for(int16_t ai=argCount-1;ai>=0;ai--) 
                    args[ai] = pop();            
                // re-push in callee parameter order
                for(auto pi=0; pi<argCount;pi++) {
                    #ifdef DEBUG_BUILD
                    assert(reorder[pi] != -1);
                    #endif
                    push(args[reorder[pi]]);
                }
                
                frame->reorderArgs.clear();
            }


        }


        instruction = readByte();
        thread->frameStart = false;

        // TODO: consider if using gcc/clang extension will help performance:
        //   https://stackoverflow.com/questions/8019849/labels-as-values-vs-switch-statement
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
                Value& inst { peek(0) };
                inst.resolveFuture();
                if (isObjectInstance(inst)) {
                    ObjectInstance* objInst = asObjectInstance(inst);
                    ObjString* name = readString();
                    // is it an instance property?
                    auto it = objInst->properties.find(name->hash);
                    if (it != objInst->properties.end()) { // exists
                        pop();
                        push(it->second);
                        break;
                    }
                    else { // no
                        // check if it is a method name
                        if (bindMethod(objInst->instanceType, name))
                            break;

                        runtimeError("Undefined method or property '"+toUTF8StdString(name->s)+"' for instance type '"+toUTF8StdString(objInst->instanceType->name)+"'.");
                        return errorReturn;
                    }
                } else if (isActorInstance(inst)) {
                    ActorInstance* actorInst = asActorInstance(inst);
                    ObjString* name = readString();
                    // check if it is a method name
                    if (bindMethod(actorInst->instanceType, name))
                        break;

                    runtimeError("Undefined method '"+toUTF8StdString(name->s)+"' for instance type '"+toUTF8StdString(actorInst->instanceType->name)+"'.");
                    return errorReturn;

                }

                runtimeError("Only object and actor instances have methods and only objects instances have properties.");
                return errorReturn;
                break;
            }
            case asByte(OpCode::SetProp): {
                Value& inst { peek(1) };
                inst.resolveFuture();
                if (!isObjectInstance(inst)) {
                    runtimeError("Only instances have properties.");
                    return errorReturn;
                }
                ObjectInstance* objInst = asObjectInstance(inst);
                ObjString* name = readString();
                //std::cout << "setting prop " << toUTF8StdString(name->s) << " of " << toString(inst) << " to " << toString(peek(0)) << std::endl;//!!!
                objInst->properties[name->hash] = peek(0);
                Value value { pop() };
                pop(); // instance
                push(value);
                break;
            }
            case asByte(OpCode::Equal): {
                Value b = pop();
                Value a = pop();
                a.resolveFuture();
                b.resolveFuture();
                push(boolVal(valuesEqual(a,b)));
                break;
            }
            case asByte(OpCode::Greater): {
                peek(0).resolveFuture();
                peek(1).resolveFuture();
                if (!peek(0).isNumber()) {
                    runtimeError("Operand to > must be a number");
                    return errorReturn;
                }
                if (!peek(1).isNumber()) {
                    runtimeError("Operand to > must be a number");
                    return errorReturn;
                }
                binaryOp([](Value a, Value b) -> Value { return greater(a,b); });
                break;
            }
            case asByte(OpCode::Less): {
                peek(0).resolveFuture();
                peek(1).resolveFuture();
                if (!peek(0).isNumber()) {
                    runtimeError("Operand to < must be a number");
                    return errorReturn;
                }
                if (!peek(1).isNumber()) {
                    runtimeError("Operand to < must be a number");
                    return errorReturn;
                }
                binaryOp([](Value a, Value b) -> Value { return less(a,b); });
                break;
            }
            case asByte(OpCode::Add): {
                peek(0).resolveFuture();
                peek(1).resolveFuture();
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
                    return errorReturn;
                }
                break;
            }
            case asByte(OpCode::Subtract): {
                peek(0).resolveFuture();
                peek(1).resolveFuture();
                if (!peek(0).isNumber()) {
                    runtimeError("Operand of - must be a number");
                    return errorReturn;
                }
                if (!peek(1).isNumber()) {
                    runtimeError("Operand of - must be a number");
                    return errorReturn;
                }
                binaryOp([](Value a, Value b) -> Value { return subtract(a,b); });
                break;
            }
            case asByte(OpCode::Multiply): {
                peek(0).resolveFuture();
                peek(1).resolveFuture();
                if (!peek(0).isNumber()) {
                    runtimeError("Operand of * must be a number");
                    return errorReturn;
                }
                if (!peek(1).isNumber()) {
                    runtimeError("Operand of * must be a number");
                    return errorReturn;
                }
                binaryOp([](Value a, Value b) -> Value { return multiply(a,b); });
                break;
            }
            case asByte(OpCode::Divide): {
                peek(0).resolveFuture();
                peek(1).resolveFuture();
                if (!peek(0).isNumber()) {
                    runtimeError("Operand of / must be a number");
                    return errorReturn;
                }
                if (!peek(1).isNumber()) {
                    runtimeError("Operand of / must be a number");
                    return errorReturn;
                }
                if (peek(0).asReal() == 0.0) {
                    runtimeError("Divide by 0");
                    return errorReturn;
                }
                binaryOp([](Value a, Value b) -> Value { return divide(a,b); });
                break;
            }
            case asByte(OpCode::Negate): {
                Value& operand { peek(0) };
                operand.resolveFuture();

                if (operand.isNumber() || operand.isBool())
                    push(negate(pop()));
                else if (isFalsey(operand)) {
                    // if it looks like false, isFalsey() -> true, so we push
                    //   true, which is negative of false
                    push(boolVal(isFalsey(operand)));
                }
                else {
                    runtimeError("Operand of 'not' or negation must be a number, bool or nil");
                    return errorReturn;
                }
                break;
            }
            case asByte(OpCode::Modulo): {
                // TODO: support decimal
                peek(0).resolveFuture();
                peek(1).resolveFuture();
                if (!peek(0).isInt()) {
                    runtimeError("Operand of '%' must be an integer");
                    return errorReturn;
                }
                if (!peek(1).isInt()) {
                    runtimeError("Operand of '%' must be an integer");
                    return errorReturn;
                }
                binaryOp([](Value a, Value b) -> Value { return mod(a,b); });
                break;
            }
            case asByte(OpCode::And): {
                peek(0).resolveFuture();
                peek(1).resolveFuture();
                if (!peek(0).isBool()) {
                    runtimeError("Operand of 'and' must be a bool");
                    return errorReturn;
                }
                if (!peek(1).isBool()) {
                    runtimeError("Operand of 'and' must be a bool");
                    return errorReturn;
                }
                binaryOp([](Value a, Value b) -> Value { return land(a,b); });
                break;
            }
            case asByte(OpCode::Or): {
                peek(0).resolveFuture();
                peek(1).resolveFuture();
                if (!peek(0).isBool()) {
                    runtimeError("Operand of 'or' must be a bool");
                    return errorReturn;
                }
                if (!peek(1).isBool()) {
                    runtimeError("Operand of 'or' must be a bool");
                    return errorReturn;
                }
                binaryOp([](Value a, Value b) -> Value { return lor(a,b); });
                break;
            }
            case asByte(OpCode::FollowedBy): {
                peek(0).resolveFuture();
                peek(1).resolveFuture();
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
                peek(0).resolveFuture();
                if (isFalsey(peek(0)))
                    frame->ip += jumpDist;
                break;
            }
            case asByte(OpCode::JumpIfTrue): {
                uint16_t jumpDist = readShort();
                peek(0).resolveFuture();
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
                CallSpec callSpec{frame->ip};
                Value& callee { peek(callSpec.argCount) };
                callee.resolveFuture();
                if (!callValue(callee, callSpec))
                    return errorReturn;
                frame = thread->frames.end()-1;
                break;
            }
            case asByte(OpCode::Index): {
                uint8_t argCount = readByte();
                peek(argCount).resolveFuture();
                if (!indexValue(peek(argCount), argCount))
                    return errorReturn;
                break;
            }
            // TODO: reimplement optimization to use Invoke as single step for object.method()
            //  instead of current two step push & call (see original Antlr visitor compiler impl)
            case asByte(OpCode::Invoke): {
                ObjString* method = readString();
                CallSpec callSpec{frame->ip};
                if (!invoke(method, callSpec))
                    return errorReturn;
                frame = thread->frames.end()-1;
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
                closeUpvalues(&(*(thread->stackTop -1)));
                pop();
                break;
            }
            case asByte(OpCode::Return): {

                try {
                    Value result = opReturn();

                    push(result);

                    if (thread->frames.empty()) {
                        Value returnVal = pop();

                        freeObjects();

                        return std::make_pair(InterpretResult::OK,returnVal);
                    }

                    frame = thread->frames.end() -1;
                    if (frame->ip == frame->startIp)
                        thread->frameStart = true;

                } catch (std::runtime_error& e) {
                    runtimeError(std::string(e.what()));
                    return errorReturn;
                }

                break;
            }
            case asByte(OpCode::ReturnStore): {

                try {
                    Value result = opReturn();
                
                    if (thread->frames.empty()) {
                        freeObjects();

                        return std::make_pair(InterpretResult::OK,result);
                    }

                    CallFrames::iterator parentFrame = frame->parent;        
                    #ifdef DEBUG_BUILD
                    assert(parentFrame != thread->frames.end());
                    #endif            
                    parentFrame->tailArgValues.push_back(result);

                    frame = thread->frames.end() -1;
                    if (frame->ip == frame->startIp)
                        thread->frameStart = true;

                } catch (std::runtime_error& e) {
                    runtimeError(std::string(e.what()));
                    return errorReturn;
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
                auto stackIndex = (frame->slots - &thread->stack[0]) + slot;
                if (stackIndex >= thread->stack.size())
                    throw std::runtime_error("Stack overflow access");
                #endif
                push(frame->slots[slot]);
                break;
            }
            case asByte(OpCode::SetLocal): {
                uint8_t slot = readByte();
                #ifdef DEBUG_BUILD
                auto stackIndex = (frame->slots - &thread->stack[0]) + slot;
                if (stackIndex >= thread->stack.size())
                    throw std::runtime_error("Stack overflow access");
                #endif
                frame->slots[slot] = peek(0);
                break;
            }
            case asByte(OpCode::DefineGlobal): {
                ObjString* name = readString();
                globals.store(name->hash, std::make_pair(name->s,pop()));
                break;
            }
            case asByte(OpCode::GetGlobal): {
                ObjString* name = readString();
                if (globals.containsKey(name->hash)) {
                    Value value = globals.at(name->hash).second;
                    push(value);
                }
                else {
                    runtimeError("Undefined variable '"+name->toStdString()+"'");
                    return errorReturn;
                }
                break;
            }
            case asByte(OpCode::SetGlobal): {
                ObjString* name = readString();
                if (globals.containsKey(name->hash)) { // found
                    // set new value, but leave it on stack (as assignment is an expression)
                    globals.store(name->hash, std::make_pair(name->s,peek(0)));
                }
                else {
                    runtimeError("Undefined variable '"+name->toStdString()+"'");
                    return errorReturn;
                }
                break;
            }
            case asByte(OpCode::SetNewGlobal): {
                ObjString* name = readString();
                if (globals.containsKey(name->hash)) { // found
                    // set new value, but leave it on stack (as assignment is an expression)
                    globals.store(name->hash, std::make_pair(name->s,peek(0)));
                }
                else {
                    // only automatic declaration of globals on assignment when
                    //   at global/module level scope
                    globals.store(name->hash, std::make_pair(name->s,peek(0)));
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


    return std::make_pair(InterpretResult::OK,nilVal());

}


void VM::resetStack()
{
    thread->stack.clear();
    thread->stack.resize(MaxStack);
    thread->stackTop = thread->stack.begin();

    thread->frames.clear();
    thread->frames.reserve(128);
    thread->frameStart = false;

    openUpvalues.clear(); // TODO: need to deref or delete?
}


void VM::freeObjects()
{
    if (Obj::unrefedObjs.empty()) return;

    // free objcts who's reference count dropped to 0
    while(!Obj::unrefedObjs.empty()) {
        Obj::unrefedObjs.pop_back_and_apply([](Obj* obj) {
            delObj(obj);
        });
    }    
}


void VM::outputAllocatedObjs()
{
    #ifdef DEBUG_TRACE_MEMORY
    if (Obj::allocatedObjs.size()>0) {
        std::cout << std::hex;
        std::cout << "== allocated Objs (" << Obj::allocatedObjs.size() << ")==" << std::endl;
        for(const auto& p : Obj::allocatedObjs.get()) {
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

    auto frame { thread->frames.end()-1 };

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
    defineNative("fork", &VM::fork_builtin);
    defineNative("join", &VM::join_builtin);
    defineNative("_threadid", &VM::threadid_builtin);
    defineNative("_wait", &VM::wait_builtin);
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

    thread->threadSleep = true;
    thread->threadSleepUntil = StreamEngine::instance()->currentTime() + nanosecs;

    return nilVal();
}


Value VM::fork_builtin(int argCount, Value* args)
{
    if ((argCount != 1) || !isClosure(args[0])) 
        throw std::invalid_argument("fork expects single callable argument (e.g. func or proc)");

    auto newThread = std::make_shared<Thread>();
    threads.store(newThread->id(), newThread);
    newThread->spawn(args[0]);

    int32_t id = int32_t(newThread->id()); // FIXME: id is uint64

    return intVal(id);
}


Value VM::join_builtin(int argCount, Value* args)
{
    if ((argCount != 1) || !args[0].isNumber())
        throw std::invalid_argument("join expects single numeric argument (thread id)");

    uint64_t id = args[0].asInt(); // FIXME

    auto count = threads.erase_and_apply(id, [](ptr<Thread> t){
        t->join();
    });

    return count > 0 ? trueVal() : falseVal();
}


Value VM::threadid_builtin(int argCount, Value* args)
{
    if (argCount != 0) 
        throw std::invalid_argument("_threadid takes no arguments");

    int32_t id = int32_t(thread->id()); // FIXME: id is uint64
    return intVal(id);
}




Value VM::wait_builtin(int argCount, Value* args)
{
    // iterate over all the argumens, and for each if it is an ObjFuture,
    //  resolve it (which wil block until the promise is fulfilled unless it was already)

    // as special case, if a single arg and it is a list, iterate over the list elements and resolve them

    int32_t numFuturesResolved { 0 };
    if (argCount == 1) {
        if (isFuture(args[0])) {
            args[0].resolveFuture();
            numFuturesResolved++;
        }
        else if (isList(args[0])) {

            ObjList* l = asList(args[0]);
            for(auto& v : l->elts) {
                v.resolveFuture();
                numFuturesResolved++;
            }
        }
    }
    else {
        for(auto i=0; i<argCount; i++) {
            if (isFuture(args[i])) {
                args[i].resolveFuture(); // may block
                numFuturesResolved++;
            }
        }
    }

    return intVal(numFuturesResolved);
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

