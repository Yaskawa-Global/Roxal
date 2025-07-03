#include <functional>
#include <time.h>
#include <math.h>
#include <chrono>
#include <thread>
#include <utility>
#include <algorithm>
#include <memory>
#include <ffi.h>
#include <dlfcn.h>


#include "ASTGenerator.h"
#include "ASTGraphviz.h"
#include "RoxalCompiler.h"
#include "dataflow/Signal.h"
#include "dataflow/DataflowEngine.h"
#include "dataflow/FuncNode.h"

#include <core/TimePoint.h>
#include "VM.h"
#include "Object.h"
#include <Eigen/Dense>
#include <core/types.h>
#include <core/common.h>
#include <core/AST.h>


using namespace roxal;

static ValueType builtinToValueType(type::BuiltinType bt)
{
    switch (bt) {
        case type::BuiltinType::Nil:     return ValueType::Nil;
        case type::BuiltinType::Bool:    return ValueType::Bool;
        case type::BuiltinType::Byte:    return ValueType::Byte;
        case type::BuiltinType::Int:     return ValueType::Int;
        case type::BuiltinType::Real:    return ValueType::Real;
        case type::BuiltinType::Decimal: return ValueType::Decimal;
        case type::BuiltinType::String:  return ValueType::String;
        case type::BuiltinType::Range:   return ValueType::Range;
        case type::BuiltinType::List:    return ValueType::List;
        case type::BuiltinType::Dict:    return ValueType::Dict;
        case type::BuiltinType::Vector:  return ValueType::Vector;
        case type::BuiltinType::Matrix:  return ValueType::Matrix;
        case type::BuiltinType::Tensor:  return ValueType::Tensor;
        case type::BuiltinType::Orient:  return ValueType::Orient;
        case type::BuiltinType::Event:   return ValueType::Event;
        default:
            throw std::runtime_error("unhandled builtin type");
    }
}

void roxal::scheduleEventHandlers(Value eventWeak, ObjEvent* ev, TimePoint when)
{
    Thread::PendingEvent pe; pe.when = when; pe.event = eventWeak;
    for (auto it = ev->subscribers.begin(); it != ev->subscribers.end(); ) {
        Value handlerVal = *it;
        if (!handlerVal.isAlive()) {
            it = ev->subscribers.erase(it);
            continue;
        }
        auto closure = asClosure(handlerVal);
        auto handlerThread = closure->handlerThread.lock();
        if (!handlerThread) {
            it = ev->subscribers.erase(it);
            continue;
        }
        handlerThread->pendingEvents.push(pe);
        handlerThread->wake();
        ++it;
    }
}

static uint64_t currentTimeNs() {
    struct timespec tp;
    clock_gettime(CLOCK_MONOTONIC,&tp);
    return uint64_t(tp.tv_sec)*1000000000ull + uint64_t(tp.tv_nsec);
}

void* VM::createFFIWrapper(void* fn, ffi_type* retType,
                           const std::vector<ffi_type*>& argTypes)
{
    FFIWrapper* spec = new FFIWrapper;
    spec->fn = fn;
    spec->retType = retType;
    spec->argTypes = argTypes;
    if (ffi_prep_cif(&spec->cif, FFI_DEFAULT_ABI, argTypes.size(), retType,
                     spec->argTypes.data()) != FFI_OK)
        throw std::runtime_error("ffi_prep_cif failed");
    return spec;
}


VM::VM()
    : lineMode(false)
{
    thread = nullptr;
    initString = stringVal(UnicodeString("init"));
    initString->incRef();

    sysModule = moduleTypeVal(UnicodeString("sys"));
    mathModule = moduleTypeVal(UnicodeString("math"));

    sysModule->incRef();
    mathModule->incRef();

    // Initialize dataflow engine as builtin actor
    dataflowEngine = df::DataflowEngine::instance();
    ObjObjectType* dataflowType = objectTypeVal(toUnicodeString("_DataflowEngine"), true);
    dataflowEngineActor = objVal(actorInstanceVal(dataflowType));
    dataflowEngineThread = std::make_shared<Thread>();
    dataflowEngineThread->act(dataflowEngineActor);

    // Start the dataflow engine run loop on its actor thread
    {
        ActorInstance* inst = asActorInstance(dataflowEngineActor);
        CallSpec cs{}; cs.argCount = 0; cs.allPositional = true;
        Value callee = objVal(boundNativeVal(dataflowEngineActor, &VM::dataflow_run_native, true));
        inst->queueCall(callee, cs, nullptr);
    }

    // Make dataflow engine available as global variable
    globals.storeGlobal(toUnicodeString("_dataflow"), dataflowEngineActor);

    defineBuiltinFunctions();
    defineBuiltinMethods();
    defineBuiltinProperties();
    defineNativeFunctions();

    //CallSpec::testParamPositions();
    //Value::testPrimitiveValues();
    //testObjectValues();
}



VM::~VM()
{
    for(auto moduleType : ObjModuleType::allModules.get())
        moduleType->vars.clear();

    // Clean up dataflow engine resources before globals cleanup
    // First stop the dataflow engine and its actor thread properly
    if (dataflowEngine) {
        dataflowEngine->stop(); // Stop the engine's run loop
    }
    if (dataflowEngineThread) {
        dataflowEngineThread->join(); // This will set quit=true and wait for thread to finish
        dataflowEngineThread.reset(); // Release the thread
    }
    dataflowEngineActor = nilVal();  // This will call decRef() via Value destructor

    globals.clearGlobals();

    initString->decRef();
    sysModule->decRef();
    mathModule->decRef();

    df::DataflowEngine::instance()->clear();


    // Release REPL thread resources before reporting potential leaks
    replThread.reset();

    freeObjects();

    // Final cleanup pass for any objects that became unreferenced during destructor
    freeObjects();

    #ifdef DEBUG_TRACE_MEMORY
    // Try one more cleanup pass right before reporting
    freeObjects();
    size_t activeThreads = threads.size();
    if (activeThreads > 0)
        std::cout << "== active threads: " << activeThreads << std::endl;
    outputAllocatedObjs();
    #endif
}


void VM::setDisassemblyOutput(bool outputBytecodeDisassembly)
{
    this->outputBytecodeDisassembly = outputBytecodeDisassembly;
}

void VM::appendModulePaths(const std::vector<std::string>& modulePaths)
{
    // insert into modulePaths, except if already present

    for (const std::string& path : modulePaths) {
        if (std::find(this->modulePaths.begin(), this->modulePaths.end(), path) == this->modulePaths.end())
            this->modulePaths.push_back(path);
    }
}




InterpretResult VM::interpret(std::istream& source, const std::string& name)
{
    ObjFunction* function { nullptr };

    try {
        RoxalCompiler compiler {};
        compiler.setOutputBytecodeDisassembly(outputBytecodeDisassembly);
        compiler.setModulePaths(modulePaths);

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

    // go
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


InterpretResult VM::interpretLine(std::istream& linestream)
{
    ObjFunction* function { nullptr };

    static RoxalCompiler compiler {};
    static ObjModuleType* replModule { nullptr };
    compiler.setOutputBytecodeDisassembly(outputBytecodeDisassembly);
    compiler.setReplMode(true);

    try {
        function = compiler.compile(linestream, "cli", replModule);

    } catch (std::exception& e) {
        return InterpretResult::CompileError;
    }

    if (function == nullptr)
        return InterpretResult::CompileError;

    if (replModule == nullptr)
        replModule = asModuleType(function->moduleType);

    lineMode = true;
    lineStream = &linestream;
    compiler.setReplMode(false);

    ObjClosure* closure = closureVal(function);

    if (!replThread) {
        replThread = std::make_shared<Thread>();
    }

    thread = replThread;
    resetStack();

    auto resultPair = callAndExec(closure, {});
    InterpretResult result = resultPair.first;

    #if defined(DEBUG_TRACE_EXECUTION)
    if (globals.size() > 0) {
        std::cout << std::endl << "== globals ==" << std::endl;
        for(const auto& global : globals.get())
            std::cout << toUTF8StdString(global.second.first) << " = " << toString(global.second.second) << std::endl;
    }
    #endif

    return result;
}

thread_local ptr<Thread> VM::thread;

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
    callframe.strict = closure->function->strict;
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
    try {
        *(thread->stackTop - callSpec.argCount - 1) = construct(builtinType, argBegin, argEnd);
        popN(callSpec.argCount);
        return true;
    } catch (std::exception& e) {
        runtimeError(e.what());
    }
    return false;
}


bool VM::callValue(const Value& callee, const CallSpec& callSpec)
{
    bool signalArg = false;
    for(int i=0;i<callSpec.argCount;i++)
        if (isSignal(peek(i))) { signalArg = true; break; }

    if (signalArg && callee.isObj() && objType(callee) == ObjType::Closure) {
        Value closureVal = callee;
        std::vector<ptr<df::Signal>> sigArgs;
        df::FuncNode::ConstArgMap constArgs;

        if (roxal::asClosure(closureVal)->function->funcType.has_value()) {
            auto calleeType = roxal::asClosure(closureVal)->function->funcType.value();
            auto paramPositions = callSpec.paramPositions(calleeType, true);
            const auto& funcType = calleeType->func.value();
            for (size_t pi = 0; pi < paramPositions.size(); ++pi) {
                int argIndex = paramPositions[pi];
                if (argIndex == -1) continue;
                Value arg = peek(callSpec.argCount - 1 - argIndex);
                const auto& param = funcType.params[pi];
                std::string pname = param.has_value() ?
                                    toUTF8StdString(param->name) : std::to_string(pi);
                if (isSignal(arg))
                    sigArgs.push_back(asSignal(arg)->signal);
                else {
                    arg.resolve();
                    constArgs[pname] = arg;
                }
            }
        }

        auto name = toUTF8StdString(roxal::asClosure(closureVal)->function->name);
        auto node = roxal::make_ptr<df::FuncNode>(name, closureVal, constArgs, sigArgs);
        node->addToEngine();
        auto outputs = node->outputs(); // creates output signals if they don't exist
        dataflowEngine->evaluate(); // Initialize signal values for new node
        popN(callSpec.argCount + 1);
        if (outputs.size() == 1) {
            push(objVal(signalVal(outputs[0])));
        } else if (outputs.empty()) {
            push(nilVal());
        } else {
            std::vector<Value> outVals;
            outVals.reserve(outputs.size());
            for(const auto& s : outputs)
                outVals.push_back(objVal(signalVal(s)));
            push(objVal(listVal(outVals)));
        }
        return true;
    }

    if (callee.isObj()) {
        switch (objType(callee)) {
            case ObjType::BoundMethod: {
                ObjBoundMethod* boundMethod { asBoundMethod(callee) };

                if (!isActorInstance(boundMethod->receiver)) {
                    *(thread->stackTop - callSpec.argCount - 1) = boundMethod->receiver;
                    return call(boundMethod->method, callSpec);
                }
                else {
                    // call to actor method.
                    //  If the caller is the same actor, treat like regular method call
                    //  otherwise, instead of calling on this thread,
                    //  queue the call for the actor thread to handle

                    ActorInstance* inst = asActorInstance(boundMethod->receiver);

                    if (std::this_thread::get_id() == inst->thread_id) {
                        // actor to this/self method call
                        *(thread->stackTop - callSpec.argCount - 1) = boundMethod->receiver; // FIXME: or inst??
                        return call(boundMethod->method, callSpec);
                    } else {
                        // call to other actor
                        Value future = inst->queueCall(callee, callSpec, &(*thread->stackTop) );

                        popN(callSpec.argCount + 1); // args & callee

                        push(future);
                    }

                    return true;
                }
            }
            case ObjType::Type: {
                ObjTypeSpec* ts = asTypeSpec(callee);
                if ((ts->typeValue == ValueType::Object) || (ts->typeValue == ValueType::Actor)) {
                    ObjObjectType* type = asObjectType(callee);
                    Value inst {};
                    if (!type->isActor) {
                        inst = Value(objectInstanceVal(type));
                        *(thread->stackTop - callSpec.argCount - 1) = inst;
                    }
                    else {
                        inst = Value(actorInstanceVal(type));

                        // spawn Thread to handle actor method calls
                        auto newThread = std::make_shared<Thread>();
                        threads.store(newThread->id(), newThread);
                        newThread->act(inst);

                        *(thread->stackTop - callSpec.argCount - 1) = inst;
                    }
                    ObjObjectType* tInit = type;
                    const ObjObjectType::Method* initMethod = nullptr;
                    while (tInit != nullptr && initMethod == nullptr) {
                        auto it = tInit->methods.find(initString->hash);
                        if (it != tInit->methods.end())
                            initMethod = &it->second;
                        else
                            tInit = tInit->superType.isNil() ? nullptr : asObjectType(tInit->superType);
                    }
                    if (initMethod != nullptr) {
                        Value initializer { initMethod->closure };
                        if (!type->isActor)
                            return call(asClosure(initializer), callSpec);
                        else {
                            ObjBoundMethod* boundInit = boundMethodVal(inst, asClosure(initializer));
                            Value calleeVal = objVal(boundInit);
                            ActorInstance* actorInst = asActorInstance(inst);
                            actorInst->queueCall(calleeVal, callSpec, &(*thread->stackTop));
                            // leave the new actor instance on the stack as the
                            // constructor return value (queueCall returns nil
                            // for proc init). Only the init arguments are
                            // popped here.
                            popN(callSpec.argCount); // remove init args
                        }
                    } else {
                        if (callSpec.argCount != 0) {
                            runtimeError("Expected 0 arguments for type instantiation, provided "+std::to_string(callSpec.argCount));
                            return false;
                        }
                    }
                    return true;
                }
                else if (ts->typeValue == ValueType::Enum) {
                    // construct a default enum value for this enum type
                    //  either the label corresponding to 0, or if none, any label
                    //  TODO: add member to type for default value (in OpCode::EnumLabel can store value of first or 0 if declared)
                    ObjObjectType* type = asObjectType(callee);
                    #ifdef DEBUG_BUILD
                    assert(type->isEnumeration);
                    #endif

                    Value value { nilVal() };

                    if (callSpec.argCount == 0) {

                        for(const auto& hashLabelValue : type->enumLabelValues) {
                            const auto& labelValue { hashLabelValue.second };
                            if (labelValue.second.asEnum() == 0) {
                                value = labelValue.second;
                                break;
                            }
                        }
                        if (value.isNil()) { // didn't find an enum label with value 0
                            if (!type->enumLabelValues.empty())
                                //  TODO: consider storing the label ordering so we can select the first one if none has value 0
                                value = type->enumLabelValues.begin()->second.second;
                            else {
                                runtimeError("enum type '"+toUTF8StdString(type->name)+"' has no labels");
                                return false;
                            }
                        }
                    }
                    else if (callSpec.argCount == 1) {
                        Value arg { peek(0) };
                        if (arg.isInt() || arg.isByte()) {
                            int intVal = arg.asInt();
                            auto it = std::find_if(type->enumLabelValues.begin(), type->enumLabelValues.end(),
                                                   [intVal](const auto& p){ return p.second.second.asInt() == intVal; });
                            if (it == type->enumLabelValues.end()) {
                                runtimeError("enum type '"+toUTF8StdString(type->name)+"' has no label with value "+std::to_string(intVal));
                                return false;
                            }
                            value = it->second.second;
                        }
                        // if single arg is an enum of the same type, that is ok, copy it
                        else if (arg.isEnum() && (arg.enumTypeId() == type->enumTypeId)) {
                            value = arg; // fall through to storing return & poping arg below
                        }
                        else if (isString(arg)) {
                            auto hash = asString(arg)->hash;
                            auto it = type->enumLabelValues.find(hash);
                            if (it == type->enumLabelValues.end() || it->second.first != asString(arg)->s) {
                                runtimeError("enum type '"+toUTF8StdString(type->name)+"' has no label '"+toUTF8StdString(asString(arg)->s)+"'");
                                return false;
                            }
                            value = it->second.second;
                        }
                        else if (isSignal(arg)) {
                            // if a signal, sample it and see if we can construct an enum from that
                            auto sample = asSignal(arg)->signal->lastValue();
                            pop(); // switch the arg for the sample
                            push(sample);
                            return callValue(callee, callSpec); // re-call with the sample value
                        }
                        else {
                            runtimeError("Type enum '"+toUTF8StdString(type->name)+"' instantiation requires an int, byte or string label (not "+arg.typeName()+").");
                            return false;
                        }
                    }
                    else {
                        runtimeError("Expected 0 or 1 argument for enum '"+toUTF8StdString(type->name)+"' type instantiation, provided "+std::to_string(callSpec.argCount));
                        return false;
                    }

                    *(thread->stackTop - callSpec.argCount - 1) = value;
                    popN(callSpec.argCount);

                    return true;
                }
                else if (ts->typeValue == ValueType::Vector) {
                    return call(ValueType::Vector, callSpec);
                }
                else {
                    throw std::runtime_error("unimplemented construction for type '"+to_string(ts->typeValue)+"'");
                }
            }
            case ObjType::Closure: {
                ObjClosure* closure = asClosure(callee);
                bool cfunc = false;
                for(const auto& annot : closure->function->annotations) {
                    if (annot->name == "cfunc") { cfunc = true; break; }
                }
                if (cfunc) {
                    Value result { callCFunc(closure, callSpec) };
                    *(thread->stackTop - callSpec.argCount - 1) = result;
                    popN(callSpec.argCount);
                    return true;
                }
                return call(closure, callSpec);
            }
            case ObjType::Native: {
                NativeFn native = asNative(callee)->function;
                try {
                    Value result { (this->*native)(callSpec.argCount, &(*thread->stackTop) - callSpec.argCount) };
                    *(thread->stackTop - callSpec.argCount - 1) = result;
                    popN(callSpec.argCount);
                    return true;
                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return false;
                }
            }
            case ObjType::BoundNative: {
                ObjBoundNative* bound { asBoundNative(callee) };

                if (!isActorInstance(bound->receiver)) {
                    *(thread->stackTop - callSpec.argCount - 1) = bound->receiver;
                    NativeFn native = bound->function;
                    try {
                        Value result { (this->*native)(callSpec.argCount+1, &(*thread->stackTop) - callSpec.argCount - 1) };
                        *(thread->stackTop - callSpec.argCount - 1) = result;
                        popN(callSpec.argCount);
                        return true;
                    } catch (std::exception& e) {
                        runtimeError(e.what());
                        return false;
                    }
                }
                else {
                    // call to actor native method.
                    //  If the caller is the same actor, treat like regular method call
                    //  otherwise, instead of calling on this thread,
                    //  queue the call for the actor thread to handle

                    ActorInstance* inst = asActorInstance(bound->receiver);

                    if (std::this_thread::get_id() == inst->thread_id) {
                        // actor to this/self native method call
                        *(thread->stackTop - callSpec.argCount - 1) = bound->receiver;
                        NativeFn native = bound->function;
                        try {
                            Value result { (this->*native)(callSpec.argCount+1, &(*thread->stackTop) - callSpec.argCount - 1) };
                            *(thread->stackTop - callSpec.argCount - 1) = result;
                            popN(callSpec.argCount);
                            return true;
                        } catch (std::exception& e) {
                            runtimeError(e.what());
                            return false;
                        }
                    } else {
                        // call to other actor
                        Value future = inst->queueCall(callee, callSpec, &(*thread->stackTop) );

                        popN(callSpec.argCount + 1); // args & callee

                        push(future);
                        return true;
                    }
                }
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

std::pair<InterpretResult,Value> VM::callAndExec(ObjClosure* closure, const std::vector<Value>& args)
{

    // Push closure first, then arguments (to match OpCode::Call stack layout)
    push(objVal(closure));
    for(const auto& a : args)
        push(a);
    CallSpec spec(args.size());
    if(!call(closure, spec))
        return { InterpretResult::RuntimeError, nilVal() };

    auto result = execute();

    // Note: execute() should have handled the cleanup when the function returned,
    // but for safety in nested calls, we don't need additional cleanup here
    // since the call() and execute() sequence manages the stack properly

    return result;
}



bool VM::invokeFromType(ObjObjectType* type, ObjString* name, const CallSpec& callSpec)
{
    ObjObjectType* t = type;
    const ObjObjectType::Method* methodPtr = nullptr;
    while (t != nullptr && methodPtr == nullptr) {
        auto it = t->methods.find(name->hash);
        if (it != t->methods.end())
            methodPtr = &it->second;
        else
            t = t->superType.isNil() ? nullptr : asObjectType(t->superType);
    }

    if (methodPtr == nullptr) {
        runtimeError("Undefined property '%s'", toUTF8StdString(name->s).c_str());
        return false;
    }
    const auto& methodInfo = *methodPtr;
    if (!isAccessAllowed(methodInfo.ownerType, methodInfo.access)) {
        runtimeError("Cannot access private member '%s'", toUTF8StdString(name->s).c_str());
        return false;
    }
    Value method { methodInfo.closure };
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
        ActorInstance* instance = asActorInstance(receiver);

        // check to ensure name isn't a prop with a func in it
        auto propIt = instance->properties.find(name->hash);
        if (propIt != instance->properties.end()) { // it is a prop
            Value value { propIt->second };
            *(thread->stackTop - callSpec.argCount - 1) = value;
            return callValue(value, callSpec);
        }

        // Try to invoke from the actor's type (user-defined methods)
        auto methodIt = instance->instanceType->methods.find(name->hash);
        if (methodIt != instance->instanceType->methods.end()) {
            const auto& methodInfo = methodIt->second;
            if (!isAccessAllowed(methodInfo.ownerType, methodInfo.access)) {
                runtimeError("Cannot access private member '%s'", toUTF8StdString(name->s).c_str());
                return false;
            }
            Value method { methodInfo.closure };
            return call(asClosure(method), callSpec);
        }

        // Check builtin methods (actors, vectors, matrices, etc.)
        auto vt = receiver.type();
        auto mit = builtinMethods.find(vt);
        if (mit != builtinMethods.end()) {
            auto it = mit->second.find(name->hash);
            if (it != mit->second.end()) {
                const BuiltinMethodInfo& methodInfo = it->second;
                NativeFn fn = methodInfo.function;

                if (std::this_thread::get_id() == instance->thread_id) {
                    // Same thread - call directly
                    Value result { (this->*fn)(callSpec.argCount+1, &(*thread->stackTop) - callSpec.argCount - 1) };
                    *(thread->stackTop - callSpec.argCount - 1) = result;
                    popN(callSpec.argCount);
                    return true;
                } else {
                    // Different thread - queue the call
                    ObjBoundNative* boundNative = boundNativeVal(receiver, fn, methodInfo.isProc);
                    Value callee = objVal(boundNative);
                    Value future = instance->queueCall(callee, callSpec, &(*thread->stackTop));

                    popN(callSpec.argCount + 1); // args & receiver
                    push(future);
                    return true;
                }
            }
        }

        runtimeError("Undefined method or property '%s' for actor instance.", toUTF8StdString(name->s).c_str());
        return false;
    }
    else {
        if (receiver.isObj()) {
            auto vt = receiver.type();
            auto mit = builtinMethods.find(vt);
            if (mit != builtinMethods.end()) {
                auto it = mit->second.find(name->hash);
                if (it != mit->second.end()) {
                    const BuiltinMethodInfo& methodInfo = it->second;
                    NativeFn fn = methodInfo.function;
                    Value result { (this->*fn)(callSpec.argCount+1, &(*thread->stackTop) - callSpec.argCount - 1) };
                    *(thread->stackTop - callSpec.argCount - 1) = result;
                    popN(callSpec.argCount);
                    return true;
                }
            }
        }
        runtimeError("Only object or actor instances have methods.");
        return false;
    }
}


bool VM::indexValue(const Value& indexable, int subscriptCount)
{
    if (indexable.isObj()) {
        // TODO: move some per-type indexing code into Object or Value
        switch (objType(indexable)) {
            case ObjType::Range: {
                if (subscriptCount != 1) {
                    runtimeError("Range indexing requires a single index.");
                    return false;
                }
                ObjRange* range = asRange(indexable);
                Value index = pop();
                if (!index.isInt()) { // TODO number?
                    runtimeError("Range indexing requires int index.");
                    return false;
                }

                auto rangeLen = range->length();
                if (rangeLen == -1) {
                    runtimeError("Range indexing requires a range with definite limits.");
                    return false;
                }

                if (index.asInt() >= range->length()) {
                    runtimeError("Range index "+toString(index)+" out of bounds.");
                    return false;
                }

                Value value = Value(range->targetIndex(index.asInt()));
                pop(); // discard indexable
                push(value);
                return true;
            }
            case ObjType::String: {
                if (subscriptCount != 1) {
                    runtimeError("String indexing requires a single index.");
                    return false;
                }
                ObjString* str = asString(indexable);
                Value index = pop();
                //std::cout << "VM::indexValue indexable="+toString(indexable)+" index="+toString(index) << std::endl << std::flush;
                try {
                    Value substr { str->index(index) };
                    pop(); // discard indexable
                    push(substr);

                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return false;
                }

                return true;
            }
            case ObjType::List: {
                if (subscriptCount != 1) {
                    runtimeError("List indexing requires a single index.");
                    return false;
                }
                ObjList* list = asList(indexable);
                Value index = pop();
                try {
                    Value sublist { list->index(index) };
                    pop(); // discard indexable
                    push(sublist);

                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return false;
                }
                return true;
            }
            case ObjType::Vector: {
                if (subscriptCount != 1) {
                    runtimeError("Vector indexing requires a single index.");
                    return false;
                }
                ObjVector* vec = asVector(indexable);
                Value index = pop();
                try {
                    Value subvec { vec->index(index) };
                    pop(); // discard indexable
                    push(subvec);

                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return false;
                }
                return true;
            }
            case ObjType::Matrix: {
                if (subscriptCount == 1) {
                    ObjMatrix* mat = asMatrix(indexable);
                    Value r = pop();
                    try {
                        Value row { mat->index(r) };
                        pop();
                        push(row);
                    } catch (std::exception& e) {
                        runtimeError(e.what());
                        return false;
                    }
                    return true;
                } else if (subscriptCount == 2) {
                    ObjMatrix* mat = asMatrix(indexable);
                    Value col = pop();
                    Value row = pop();
                    try {
                        Value elt { mat->index(row, col) };
                        pop();
                        push(elt);
                    } catch (std::exception& e) {
                        runtimeError(e.what());
                        return false;
                    }
                    return true;
                } else {
                    runtimeError("Matrix indexing requires one or two indices.");
                    return false;
                }
            }
            case ObjType::Dict: {
                if (subscriptCount != 1) {
                    runtimeError("Dict lookup requires a single key index.");
                    return false;
                }
                ObjDict* dict = asDict(indexable);
                Value index = pop();
                if (!dict->contains(index)) {
                    runtimeError("KeyError: key '" + toString(index) + "' not found in dict.");
                    return false;
                }
                Value result { dict->at(index) };
                pop(); // discard indexable
                push(result);
                return true;
            }
            case ObjType::Signal: {
                if (subscriptCount != 1) {
                    runtimeError("Signal indexing requires a single index.");
                    return false;
                }
                Value base = indexable; // copy since we'll pop indexable
                ObjSignal* sig = asSignal(base);
                Value indexVal = pop();
                if (!indexVal.isInt()) {
                    runtimeError("Signal index must be an int.");
                    return false;
                }
                int idx = indexVal.asInt();
                try {
                    if (idx == 0) {
                        pop();
                        push(base);
                    } else if (idx < 0) {
                        auto newSig = sig->signal->indexedSignal(idx);
                        pop();
                        push(objVal(signalVal(newSig)));
                    } else {
                        runtimeError("Signal index must be 0 or negative.");
                        return false;
                    }
                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return false;
                }
                return true;
            }
            case ObjType::Closure: {
                // indexing a closure occurs in special case of a function that returns a list or dict etc.
                // currently unsupported
                break;
            }
            default:
                break;
        }
        runtimeError("Only strings, lists, ranges, [vectors, dicts, matrices, tensors - unimplemented], and signals can be indexed, not type "+objTypeName(indexable.asObj())+".");
        return false;
    }
    runtimeError("Only strings, lists, ranges,[vectors, dicts, matrices, tensors - unimplemented], and signals can be indexed, not type "+indexable.typeName()+".");
    return false;
}


bool VM::setIndexValue(const Value& indexable, int subscriptCount, Value& value)
{
    if (indexable.isObj()) {
        // TODO: move some per-type indexing code into Object or Value
        switch (objType(indexable)) {
            case ObjType::Range: {
                runtimeError("Ranges are immutable - cannot be modified.");
                return false;
            }
            case ObjType::String: {
                runtimeError("Strings are immutable - content cannot be modified.");
                return false;
            } break;
            case ObjType::List: {
                if (subscriptCount != 1) {
                    runtimeError("List indexing requires a single index.");
                    return false;
                }
                ObjList* list = asList(indexable);
                Value index = pop();
                try {
                    if (isRange(index) && !isList(value)) value.resolve();
                    list->setIndex(index, value);
                    pop(); // discard indexable
                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return false;
                }
                return true;
            } break;
            case ObjType::Vector: {
                if (subscriptCount != 1) {
                    runtimeError("Vector indexing requires a single index.");
                    return false;
                }
                ObjVector* vec = asVector(indexable);
                Value index = pop();
                try {
                    if (isRange(index) && !isVector(value)) value.resolve();
                    vec->setIndex(index, value);
                    pop(); // discard indexable
                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return false;
                }
                return true;
            } break;
            case ObjType::Matrix: {
                if (subscriptCount == 1) {
                    ObjMatrix* mat = asMatrix(indexable);
                    Value r = pop();
                    try {
                        if (isRange(r) && !isMatrix(value)) value.resolve();
                        mat->setIndex(r, value);
                        pop();
                    } catch (std::exception& e) {
                        runtimeError(e.what());
                        return false;
                    }
                    return true;
                } else if (subscriptCount == 2) {
                    ObjMatrix* mat = asMatrix(indexable);
                    Value col = pop();
                    Value row = pop();
                    try {
                        if ((isRange(row) || isRange(col)) && !isMatrix(value)) value.resolve();
                        mat->setIndex(row, col, value);
                        pop();
                    } catch (std::exception& e) {
                        runtimeError(e.what());
                        return false;
                    }
                    return true;
                } else {
                    runtimeError("Matrix indexing requires one or two indices.");
                    return false;
                }
            } break;
            case ObjType::Dict: {
                if (subscriptCount != 1) {
                    runtimeError("Dict indexing requires a single index.");
                    return false;
                }
                ObjDict* dict = asDict(indexable);
                Value index = pop();
                try {
                    dict->store(index, value);
                    pop(); // discard indexable
                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return false;
                }
                return true;
            } break;
            default:
                break;
        }
        runtimeError("Only strings, lists, [vectors, dicts, matrices, tensors - unimplemented], and signals can be indexed for assignment, not type "+objTypeName(indexable.asObj())+".");
        return false;
    }

    runtimeError("Only strings, lists, [vectors, dicts, matrices, tensors - unimplemented], and signals can be indexed for assignment, not type "+indexable.typeName()+".");
    return false;
}


VM::BindResult VM::bindMethod(ObjObjectType* instanceType, ObjString* name)
{
    ObjObjectType* t = instanceType;
    const ObjObjectType::Method* methodPtr = nullptr;
    while (t != nullptr && methodPtr == nullptr) {
        auto it = t->methods.find(name->hash);
        if (it != t->methods.end())
            methodPtr = &it->second;
        else
            t = t->superType.isNil() ? nullptr : asObjectType(t->superType);
    }

    if (methodPtr == nullptr)
        return BindResult::NotFound;

    const auto& methodInfo = *methodPtr;
    if (!isAccessAllowed(methodInfo.ownerType, methodInfo.access)) {
        runtimeError("Cannot access private member '%s'", toUTF8StdString(name->s).c_str());
        return BindResult::Private;
    }

    Value method { methodInfo.closure };

    ObjBoundMethod* boundMethod { boundMethodVal(peek(0), asClosure(method)) };

    pop();
    push(objVal(boundMethod));

    return BindResult::Bound;
}



ObjUpvalue* VM::captureUpvalue(Value& local)
{
    auto& openUpvalues = thread->openUpvalues;
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
    auto& openUpvalues = thread->openUpvalues;
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


bool VM::isAccessAllowed(const Value& ownerType, ast::Access access)
{
    if (access == ast::Access::Public)
        return true;

    for(auto it = thread->frames.rbegin(); it != thread->frames.rend(); ++it) {
        ObjFunction* fn = it->closure->function;
        if (!fn->ownerType.isNil() && fn->ownerType.isAlive()) {
            if (fn->ownerType == ownerType)
                return true;
        }
    }
    return false;
}



void VM::defineProperty(ObjString* name)
{
    #ifdef DEBUG_BUILD
    if (!isObjectType(peek(3)))
        throw std::runtime_error("Can't create property without object or actor type on stack");
    #endif
    ObjObjectType* objType = asObjectType(peek(3));
    #ifdef DEBUG_BUILD
    if (objType->isInterface)
        throw std::runtime_error("Can't create property for an interface");
    #endif

    if (objType->properties.contains(name->hash))
        throw std::runtime_error("Duplicate property '"+name->toStdString()+"' declared in type "+(objType->isActor?"actor":"object")+" "+toUTF8StdString(objType->name));

    const Value& propertyType { peek(2) };
    Value propertyInitial { peek(1) };
    Value accessVal { peek(0) };

    if (!propertyInitial.isNil()) {
        // if the property type is specified, convert the initial value (if given) to the declared propType
        if (!propertyType.isNil() && isTypeSpec(propertyType)) {
            ObjTypeSpec* typeSpec = asTypeSpec(propertyType);
            if (typeSpec->typeValue != ValueType::Nil)
                // TODO: implement & use a canConvertToType()
                propertyInitial = toType(typeSpec->typeValue,propertyInitial,/*strict=*/false);
        }
    }

    ast::Access access = (!accessVal.isNil() && accessVal.isBool() && accessVal.asBool()) ? ast::Access::Private : ast::Access::Public;
    objType->properties[name->hash] = {name->s, propertyType, propertyInitial,
                                      access, objVal(objType).weakRef()};
    objType->propertyOrder.push_back(name->hash);

    // check module annotations for ctype
    if (!thread->frames.empty()) {
        auto frame = thread->frames.end()-1;
        ObjModuleType* mod = asModuleType(frame->closure->function->moduleType);
        auto itType = mod->propertyCTypes.find(objType->name.hashCode());
        if (itType != mod->propertyCTypes.end()) {
            auto itProp = itType->second.find(name->hash);
            if (itProp != itType->second.end())
                objType->properties[name->hash].ctype = itProp->second;
        }
    }
    popN(3);
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

    if (type->methods.contains(name->hash))
        throw std::runtime_error("Duplicate method '"+name->toStdString()+"' declared in type "+(type->isActor?"actor":"object")+" '"+toUTF8StdString(type->name)+"'");

    ObjClosure* closure = asClosure(method);
    closure->function->ownerType = objVal(type).weakRef();

    type->methods[name->hash] = {name->s, method, closure->function->access,
                                 objVal(type).weakRef()};
    pop();
}


void VM::defineEnumLabel(ObjString* name)
{
    Value value = peek(0);
    #ifdef DEBUG_BUILD
    if (!value.isInt() && !value.isByte())
        throw std::runtime_error("Can only create enum value from int or byte");
    if (!isEnumType(peek(1)))
        throw std::runtime_error("Can't create enum value without enum type on stack");
    #endif
    ObjObjectType* type = asObjectType(peek(1));

     if (type->enumLabelValues.contains(name->hash))
         throw std::runtime_error("Duplicate enum label '"+name->toStdString()+"' declared in type '"+toUTF8StdString(type->name)+"'");

    // convert the value from byte or int to enum
    int32_t intVal = value.asInt();
    if (intVal < std::numeric_limits<int16_t>::min() || intVal > std::numeric_limits<int16_t>::max())
        throw std::runtime_error("Enum label '"+toUTF8StdString(name->s)+"' value out of range for type '"+toUTF8StdString(type->name)+"'");
    Value enumValue {int16_t(intVal), type->enumTypeId};

    type->enumLabelValues[name->hash] = std::make_pair(name->s,enumValue);
    pop();
}




void VM::defineNative(const std::string& name, NativeFn function)
{
    UnicodeString uname { toUnicodeString(name) };
    Value funcVal { objVal(nativeVal(function)) };
    globals.storeGlobal(uname,funcVal);
}


std::pair<InterpretResult,Value> VM::execute()
{
    if (thread->frames.empty() || thread->frames.back().closure->function->chunk->code.size()==0)
        return std::make_pair(InterpretResult::OK,nilVal()); // nothing to execute

    // Track execution depth for nested calls
    thread->execute_depth++;
    size_t frame_depth_on_entry = thread->frames.size();

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
            auto index { Chunk::size_type(readByte()) };
            if (index >= frame->closure->function->chunk->constants.size())
                throw std::runtime_error("Chunk instruction read constant invalid index into constants table");
            return frame->closure->function->chunk->constants.at(index);
        #else
            return frame->closure->function->chunk->constants[Chunk::size_type(readByte())];
        #endif
    };

    auto readConstant2 = [&]() -> Value {
        #ifdef DEBUG_BUILD
            auto index { Chunk::size_type((readByte() << 8) + readByte()) };
            if (index >= frame->closure->function->chunk->constants.size())
                throw std::runtime_error("Chunk instruction read constant invalid index into constants table");
            return frame->closure->function->chunk->constants.at(index);
        #else
            return frame->closure->function->chunk->constants[Chunk::size_type((readByte() << 8) + readByte())];
        #endif
    };

    auto readString = [&]() -> ObjString* {
        return asString(readConstant());
    };

    auto readString2 = [&]() -> ObjString* {
        return asString(readConstant2());
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

        // if we're 'sleeping' don't execute any instructions
        //  (we may have been woken up by an event or a spurious wakeup, in which case we'll re-block below)
        if (thread->threadSleep)
           goto postInstructionDispatch;


        #if defined(DEBUG_TRACE_EXECUTION)
            // output stack
            thread->outputStack();
            if (frame->ip != frame->closure->function->chunk->code.end()) {
                // and instruction
                frame->closure->function->chunk->disassembleInstruction(frame->ip - frame->closure->function->chunk->code.begin());
            }
            else {
                std::cout << "          <end of chunk>" << std::endl;
                return std::make_pair(InterpretResult::RuntimeError,nilVal());
            }
        #endif


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

            // convert arguments to parameter types if specified
            if (frame->closure->function->funcType.has_value()) {
                const auto& params = frame->closure->function->funcType.value()->func.value().params;
                bool strictConv = false;
                if (thread->frames.size() >= 2)
                    strictConv = (thread->frames.end()-2)->strict;
                for(size_t pi=0; pi<params.size(); ++pi) {
                    const auto& paramOpt = params[pi];
                    if (paramOpt.has_value() && paramOpt->type.has_value()) {
                        ValueType vt = builtinToValueType(paramOpt->type.value()->builtin);
                        try {
                            *(frame->slots + 1 + pi) = toType(vt, *(frame->slots + 1 + pi), strictConv);
                        } catch(std::exception& e) {
                            runtimeError(e.what());
                            return std::make_pair(InterpretResult::RuntimeError,nilVal());
                        }
                    }
                }
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
            case asByte(OpCode::ConstInt0): {
                push(intVal(0));
                break;
            }
            case asByte(OpCode::ConstInt1): {
                push(intVal(1));
                break;
            }
            case asByte(OpCode::GetProp): {
                Value& inst { peek(0) };
                ObjString* name = readString();

                // Check for signal properties BEFORE resolving
                if (isSignal(inst)) {
                    // Handle signal builtin properties
                    auto vt = inst.type();
                    auto pit = builtinProperties.find(vt);
                    if (pit != builtinProperties.end()) {
                        auto propIt = pit->second.find(name->hash);
                        if (propIt != pit->second.end()) {
                            const BuiltinPropertyInfo& propInfo = propIt->second;
                            Value result = (this->*(propInfo.getter))(inst);
                            pop();
                            push(result);
                            break;
                        }
                    }

                    // Handle signal builtin methods
                    auto mit = builtinMethods.find(vt);
                    if (mit != builtinMethods.end()) {
                        auto methodIt = mit->second.find(name->hash);
                        if (methodIt != mit->second.end()) {
                            const BuiltinMethodInfo& methodInfo = methodIt->second;
                            ObjBoundNative* bm = boundNativeVal(inst, methodInfo.function, methodInfo.isProc);
                            pop();
                            push(objVal(bm));
                            break;
                        }
                    }

                    runtimeError("Undefined property '"+toUTF8StdString(name->s)+"' for signal.");
                    return errorReturn;
                }

                inst.resolve();
                if (isObjectInstance(inst)) {
                    ObjectInstance* objInst = asObjectInstance(inst);
                    // is it an instance property?
                    auto it = objInst->properties.find(name->hash);
                    if (it != objInst->properties.end()) { // exists
                        pop();
                        push(it->second);
                        break;
                    }
                    else { // no
                        // check if it is a method name
                        auto br = bindMethod(objInst->instanceType, name);
                        if (br == BindResult::Bound)
                            break;
                        if (br == BindResult::Private)
                            return errorReturn;

                        runtimeError("Undefined method or property '"+toUTF8StdString(name->s)+"' for instance type '"+toUTF8StdString(objInst->instanceType->name)+"'.");
                        return errorReturn;
                    }
                } else if (isActorInstance(inst)) {
                    ActorInstance* actorInst = asActorInstance(inst);
                    auto it = actorInst->properties.find(name->hash);
                    if (it != actorInst->properties.end()) {
                        pop();
                        push(it->second);
                        break;
                    } else {
                        auto br = bindMethod(actorInst->instanceType, name);
                        if (br == BindResult::Bound)
                            break;
                        if (br == BindResult::Private)
                            return errorReturn;

                        // Check builtin methods (actors, vectors, matrices, etc.)
                        auto vt = inst.type();
                        auto mit = builtinMethods.find(vt);
                        if (mit != builtinMethods.end()) {
                            auto methodIt = mit->second.find(name->hash);
                            if (methodIt != mit->second.end()) {
                                const BuiltinMethodInfo& methodInfo = methodIt->second;
                                NativeFn fn = methodInfo.function;
                                ObjBoundNative* boundNative = boundNativeVal(inst, fn, methodInfo.isProc);
                                pop();
                                push(objVal(boundNative));
                                break;
                            }
                        }

                        runtimeError("Undefined method or property '"+toUTF8StdString(name->s)+"' for instance type '"+toUTF8StdString(actorInst->instanceType->name)+"'.");
                        return errorReturn;
                    }

                }
                else if (isEnumType(inst)) {
                    auto enumObjType = asObjectType(inst);
                    // is it an existing enum label?
                    auto it = enumObjType->enumLabelValues.find(name->hash);
                    if (it != enumObjType->enumLabelValues.end()) { // exists
                        pop();
                        push(it->second.second);
                        break;
                    }

                    runtimeError("Undefined enum label '"+toUTF8StdString(name->s)+"' for enum type '"+toUTF8StdString(enumObjType->name)+"'.");
                    return errorReturn;
                }
                else if (isModuleType(inst)) {
                    auto moduleType = asModuleType(inst);

                    auto optValue { moduleType->vars.load(name->hash) };
                    if (optValue.has_value()) {
                        Value value = optValue.value();
                        pop();
                        push(value);
                        break;
                    }
                    else {
                        runtimeError("Undefined variable '"+name->toStdString()+"' in module "+toUTF8StdString(moduleType->name)+".");
                        return errorReturn;
                    }
                }

                if (inst.isObj()) {
                    auto vt = inst.type();
                    // Check builtin methods
                    auto mit = builtinMethods.find(vt);
                    if (mit != builtinMethods.end()) {
                        auto it2 = mit->second.find(name->hash);
                        if (it2 != mit->second.end()) {
                            const BuiltinMethodInfo& methodInfo = it2->second;
                            ObjBoundNative* bm = boundNativeVal(inst, methodInfo.function, methodInfo.isProc);
                            pop();
                            push(objVal(bm));
                            break;
                        }
                    }
                }

                runtimeError("Only object and actor instances have methods and only objects instances have properties.");
                return errorReturn;
                break;
            }
            case asByte(OpCode::GetPropCheck): {
                Value& inst { peek(0) };
                ObjString* name = readString();

                // Check for signal properties BEFORE resolving
                if (isSignal(inst)) {
                    // Handle signal builtin properties
                    auto vt = inst.type();
                    auto pit = builtinProperties.find(vt);
                    if (pit != builtinProperties.end()) {
                        auto propIt = pit->second.find(name->hash);
                        if (propIt != pit->second.end()) {
                            const BuiltinPropertyInfo& propInfo = propIt->second;
                            Value result = (this->*(propInfo.getter))(inst);
                            pop();
                            push(result);
                            break;
                        }
                    }

                    // Handle signal builtin methods
                    auto mit = builtinMethods.find(vt);
                    if (mit != builtinMethods.end()) {
                        auto methodIt = mit->second.find(name->hash);
                        if (methodIt != mit->second.end()) {
                            const BuiltinMethodInfo& methodInfo = methodIt->second;
                            ObjBoundNative* bm = boundNativeVal(inst, methodInfo.function, methodInfo.isProc);
                            pop();
                            push(objVal(bm));
                            break;
                        }
                    }

                    runtimeError("Undefined property '"+toUTF8StdString(name->s)+"' for signal.");
                    return errorReturn;
                }

                inst.resolve();
                if (isObjectInstance(inst)) {
                    ObjectInstance* objInst = asObjectInstance(inst);
                    auto it = objInst->properties.find(name->hash);
                    if (it != objInst->properties.end()) {
                        auto pit = objInst->instanceType->properties.find(name->hash);
                        ast::Access propAccess = ast::Access::Public;
                        Value ownerT = objVal(objInst->instanceType).weakRef();
                        if (pit != objInst->instanceType->properties.end()) {
                            propAccess = pit->second.access;
                            ownerT = pit->second.ownerType;
                        }
                        if (!isAccessAllowed(ownerT, propAccess)) {
                            runtimeError("Cannot access private member '%s'", toUTF8StdString(name->s).c_str());
                            return errorReturn;
                        }
                        pop();
                        push(it->second);
                        break;
                    } else {
                        auto br = bindMethod(objInst->instanceType, name);
                        if (br == BindResult::Bound)
                            break;
                        if (br == BindResult::Private)
                            return errorReturn;

                        runtimeError("Undefined method or property '"+toUTF8StdString(name->s)+"' for instance type '"+toUTF8StdString(objInst->instanceType->name)+"'.");
                        return errorReturn;
                    }
                } else if (isActorInstance(inst)) {
                    ActorInstance* actorInst = asActorInstance(inst);
                    auto pit = actorInst->instanceType->properties.find(name->hash);
                    auto it = actorInst->properties.find(name->hash);
                    if (it != actorInst->properties.end()) {
                        ast::Access propAccess = ast::Access::Public;
                        Value ownerT = objVal(actorInst->instanceType).weakRef();
                        if (pit != actorInst->instanceType->properties.end()) {
                            propAccess = pit->second.access;
                            ownerT = pit->second.ownerType;
                        }
                        if (!isAccessAllowed(ownerT, propAccess)) {
                            runtimeError("Cannot access private member '%s'", toUTF8StdString(name->s).c_str());
                            return errorReturn;
                        }
                        pop();
                        push(it->second);
                        break;
                    } else {
                        auto br = bindMethod(actorInst->instanceType, name);
                        if (br == BindResult::Bound)
                            break;
                        if (br == BindResult::Private)
                            return errorReturn;

                        // Check builtin methods (actors, vectors, matrices, etc.)
                        auto vt = inst.type();
                        auto mit = builtinMethods.find(vt);
                        if (mit != builtinMethods.end()) {
                            auto methodIt = mit->second.find(name->hash);
                            if (methodIt != mit->second.end()) {
                                const BuiltinMethodInfo& methodInfo = methodIt->second;
                                NativeFn fn = methodInfo.function;
                                ObjBoundNative* boundNative = boundNativeVal(inst, fn, methodInfo.isProc);
                                pop();
                                push(objVal(boundNative));
                                break;
                            }
                        }

                        runtimeError("Undefined method or property '"+toUTF8StdString(name->s)+"' for instance type '"+toUTF8StdString(actorInst->instanceType->name)+"'.");
                        return errorReturn;
                    }

                } else if (isEnumType(inst)) {
                    auto enumObjType = asObjectType(inst);
                    auto it = enumObjType->enumLabelValues.find(name->hash);
                    if (it != enumObjType->enumLabelValues.end()) {
                        pop();
                        push(it->second.second);
                        break;
                    }

                    runtimeError("Undefined enum label '"+toUTF8StdString(name->s)+"' for enum type '"+toUTF8StdString(enumObjType->name)+"'.");
                    return errorReturn;
                } else if (isModuleType(inst)) {
                    auto moduleType = asModuleType(inst);

                    auto optValue { moduleType->vars.load(name->hash) };
                    if (optValue.has_value()) {
                        Value value = optValue.value();
                        pop();
                        push(value);
                        break;
                    } else {
                        runtimeError("Undefined variable '"+name->toStdString()+"' in module "+toUTF8StdString(moduleType->name)+".");
                        return errorReturn;
                    }
                }

                if (inst.isObj()) {
                    auto vt = inst.type();
                    // Check builtin methods
                    auto mit = builtinMethods.find(vt);
                    if (mit != builtinMethods.end()) {
                        auto it2 = mit->second.find(name->hash);
                        if (it2 != mit->second.end()) {
                            const BuiltinMethodInfo& methodInfo = it2->second;
                            ObjBoundNative* bm = boundNativeVal(inst, methodInfo.function, methodInfo.isProc);
                            pop();
                            push(objVal(bm));
                            break;
                        }
                    }
                }

                runtimeError("Only object and actor instances have methods and only objects instances have properties.");
                return errorReturn;
                break;
            }
            case asByte(OpCode::SetProp): {
                Value& inst { peek(1) };
                inst.resolve();
                if (isObjectInstance(inst)) {
                    ObjectInstance* objInst = asObjectInstance(inst);
                    ObjString* name = readString();
                    //std::cout << "setting prop " << toUTF8StdString(name->s) << " of " << toString(inst) << " to " << toString(peek(0)) << std::endl;

                    Value value { peek(0) };

                    if (!value.isNil()) {
                        bool strictConv = frame->closure->function->strict;
                        // if type object specified the property type in the declaration,
                        //  convert the value to that type (if possible)
                        const auto& properties { objInst->instanceType->properties };
                        const auto& property = properties.find(name->hash);
                        if (property != properties.end()) {
                            const auto& prop { property->second };
                            if (!prop.type.isNil() && isTypeSpec(prop.type)) {
                                ObjTypeSpec* typeSpec = asTypeSpec(prop.type);
                                if (typeSpec->typeValue != ValueType::Nil)
                                    try {
                                        // TODO: implement & use a canConvertToType()
                                        value = toType(typeSpec->typeValue,value, strictConv);
                                    } catch(std::exception& e) {
                                        runtimeError(e.what());
                                        return errorReturn;
                                    }
                            }
                        }
                    }


                    objInst->properties[name->hash] = value;
                    popN(2); // pop original value & instance
                    push(value); // value (possibly converted)
                    break;
                } else if (isActorInstance(inst)) {
                    ActorInstance* actorInst = asActorInstance(inst);
                    ObjString* name = readString();

                    Value value { peek(0) };

                    if (!value.isNil()) {
                        bool strictConv = frame->closure->function->strict;
                        const auto& properties { actorInst->instanceType->properties };
                        const auto& property = properties.find(name->hash);
                        if (property != properties.end()) {
                            const auto& prop { property->second };
                            if (!prop.type.isNil() && isTypeSpec(prop.type)) {
                                ObjTypeSpec* typeSpec = asTypeSpec(prop.type);
                                if (typeSpec->typeValue != ValueType::Nil)
                                    try {
                                        value = toType(typeSpec->typeValue,value, strictConv);
                                    } catch(std::exception& e) {
                                        runtimeError(e.what());
                                        return errorReturn;
                                    }
                            }
                        }
                    }

                    actorInst->properties[name->hash] = value;
                    popN(2);
                    push(value);
                    break;
                } else if (isModuleType(inst)) {
                    auto moduleType = asModuleType(inst);

                    ObjString* name = readString();

                    auto& vars { moduleType->vars };

                    // TODO: consider if we should allow setting module vars from another module
                    //  (maybe only if !strict?)

                    if (vars.exists(name->hash)) {
                        Value value { peek(0) };

                        vars.store(name->hash, name->s, value, /*overwrite=*/true);
                        popN(2); // pop original value & instance
                        push(value); // value (possibly converted)
                    }
                    else {
                        runtimeError("Declaring new module variables in another module ('"+toUTF8StdString(moduleType->name)+"') is not allowed.");
                        return errorReturn;
                    }
                    break;
                }
                runtimeError("Only instances have properties.");
                return errorReturn;
                break;
            }
            case asByte(OpCode::SetPropCheck): {
                Value& inst { peek(1) };
                inst.resolve();
                if (isObjectInstance(inst)) {
                    ObjectInstance* objInst = asObjectInstance(inst);
                    ObjString* name = readString();

                    Value value { peek(0) };

                    if (!value.isNil()) {
                        bool strictConv = frame->closure->function->strict;
                        const auto& properties { objInst->instanceType->properties };
                        const auto& property = properties.find(name->hash);
                        if (property != properties.end()) {
                            const auto& prop { property->second };
                            if (!prop.type.isNil() && isTypeSpec(prop.type)) {
                                ObjTypeSpec* typeSpec = asTypeSpec(prop.type);
                                if (typeSpec->typeValue != ValueType::Nil)
                                    try {
                                        value = toType(typeSpec->typeValue,value, strictConv);
                                    } catch(std::exception& e) {
                                        runtimeError(e.what());
                                        return errorReturn;
                                    }
                            }
                        }
                    }

                    auto pit = objInst->instanceType->properties.find(name->hash);
                    ast::Access propAccess = ast::Access::Public;
                    Value ownerT = objVal(objInst->instanceType).weakRef();
                    if (pit != objInst->instanceType->properties.end()) {
                        propAccess = pit->second.access;
                        ownerT = pit->second.ownerType;
                    }
                    if (!isAccessAllowed(ownerT, propAccess)) {
                        runtimeError("Cannot access private member '%s'", toUTF8StdString(name->s).c_str());
                        return errorReturn;
                    }

                    objInst->properties[name->hash] = value;
                    popN(2);
                    push(value);
                    break;
                } else if (isActorInstance(inst)) {
                    ActorInstance* actorInst = asActorInstance(inst);
                    ObjString* name = readString();

                    Value value { peek(0) };

                    if (!value.isNil()) {
                        bool strictConv = frame->closure->function->strict;
                        const auto& properties { actorInst->instanceType->properties };
                        const auto& property = properties.find(name->hash);
                        if (property != properties.end()) {
                            const auto& prop { property->second };
                            if (!prop.type.isNil() && isTypeSpec(prop.type)) {
                                ObjTypeSpec* typeSpec = asTypeSpec(prop.type);
                                if (typeSpec->typeValue != ValueType::Nil)
                                    try {
                                        value = toType(typeSpec->typeValue,value, strictConv);
                                    } catch(std::exception& e) {
                                        runtimeError(e.what());
                                        return errorReturn;
                                    }
                            }
                        }
                    }

                    auto pit = actorInst->instanceType->properties.find(name->hash);
                    ast::Access propAccess = ast::Access::Public;
                    Value ownerT = objVal(actorInst->instanceType).weakRef();
                    if (pit != actorInst->instanceType->properties.end()) {
                        propAccess = pit->second.access;
                        ownerT = pit->second.ownerType;
                    }
                    if (!isAccessAllowed(ownerT, propAccess)) {
                        runtimeError("Cannot access private member '%s'", toUTF8StdString(name->s).c_str());
                        return errorReturn;
                    }

                    actorInst->properties[name->hash] = value;
                    popN(2);
                    push(value);
                    break;
                } else if (isModuleType(inst)) {
                    auto moduleType = asModuleType(inst);

                    ObjString* name = readString();

                    auto& vars { moduleType->vars };

                    if (vars.exists(name->hash)) {
                        Value value { peek(0) };

                        vars.store(name->hash, name->s, value, /*overwrite=*/true);
                        popN(2);
                        push(value);
                    } else {
                        runtimeError("Declaring new module variables in another module ('"+toUTF8StdString(moduleType->name)+"') is not allowed.");
                        return errorReturn;
                    }
                    break;
                }
                runtimeError("Only instances have properties.");
                return errorReturn;
                break;
            }
            case asByte(OpCode::GetProp2): {
                throw std::runtime_error("unimplemented");
                break;
            }
            case asByte(OpCode::SetProp2): {
                throw std::runtime_error("unimplemented");
                break;
            }
            case asByte(OpCode::GetSuper): {
                ObjString* name = readString();
                #ifdef DEBUG_BUILD
                if (!isTypeSpec(peek(0)) && !isObjectType(peek(0)))
                    throw std::runtime_error("super doesn't reference an object or actor type.");
                #endif

                ObjObjectType* superType = asObjectType(pop());
                auto br = bindMethod(superType,name);
                if (br != BindResult::Bound)
                    return std::make_pair(InterpretResult::RuntimeError,nilVal());

                break;
            }
            case asByte(OpCode::Equal): {
                Value b = pop();
                Value a = pop();
                a.resolve();
                b.resolve();
                push(boolVal(a.equals(b, frame->strict)));
                break;
            }
            case asByte(OpCode::Is): {
                Value b = pop();
                Value a = pop();
                a.resolve();
                b.resolve();
                push(boolVal(a.is(b, frame->strict)));
                break;
            }
            case asByte(OpCode::Greater): {
                peek(0).resolve();
                peek(1).resolve();
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
                peek(0).resolve();
                peek(1).resolve();
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
                peek(0).resolve();
                peek(1).resolve();
                if ((peek(0).isNumber() && peek(1).isNumber()) ||
                    (isVector(peek(0)) && isVector(peek(1))) ||
                    (isList(peek(1)) && isList(peek(0))) ||
                    isList(peek(1))) {
                    binaryOp([](Value a, Value b) -> Value { return add(a,b); });
                }
                else if (isString(peek(1))) {
                    concatenate();
                }
                else {
                    runtimeError("Operands of + must be two numbers, two vectors, two lists, list + value, or strings LHS");
                    return errorReturn;
                }
                break;
            }
            case asByte(OpCode::Subtract): {
                peek(0).resolve();
                peek(1).resolve();
                if (isVector(peek(0)) && isVector(peek(1))) {
                    binaryOp([](Value a, Value b) -> Value { return subtract(a,b); });
                } else if (peek(0).isNumber() && peek(1).isNumber()) {
                    binaryOp([](Value a, Value b) -> Value { return subtract(a,b); });
                } else {
                    runtimeError("Operands of - must be two numbers or two vectors");
                    return errorReturn;
                }
                break;
            }
            case asByte(OpCode::Multiply): {
                peek(0).resolve();
                peek(1).resolve();
                if ( (isVector(peek(0)) && isVector(peek(1))) ||
                     (isVector(peek(0)) && peek(1).isNumber()) ||
                     (peek(0).isNumber() && isVector(peek(1))) ) {
                    binaryOp([](Value a, Value b) -> Value { return multiply(a,b); });
                } else if (peek(0).isNumber() && peek(1).isNumber()) {
                    binaryOp([](Value a, Value b) -> Value { return multiply(a,b); });
                } else {
                    runtimeError("Operands of * must be numbers or vectors with scalar");
                    return errorReturn;
                }
                break;
            }
            case asByte(OpCode::Divide): {
                peek(0).resolve();
                peek(1).resolve();
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
                operand.resolve();

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
                peek(0).resolve();
                peek(1).resolve();
                if (!peek(0).isNumber() && !peek(0).isBool()) {
                    runtimeError("Operand of '%' must be an integer");
                    return errorReturn;
                }
                if (!peek(1).isNumber() && !peek(1).isBool()) {
                    runtimeError("Operand of '%' must be an integer");
                    return errorReturn;
                }
                binaryOp([](Value a, Value b) -> Value { return mod(a,b); });
                break;
            }
            case asByte(OpCode::And): {
                peek(0).resolve();
                peek(1).resolve();
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
                peek(0).resolve();
                peek(1).resolve();
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
            case asByte(OpCode::Dup): {
                auto value = peek(0);
                push(value);
                break;
            }
            case asByte(OpCode::DupBelow): {
                auto value = peek(1);
                push(value);
                break;
            }
            case asByte(OpCode::Swap): {
                std::swap(peek(0), peek(1));
                break;
            }
            case asByte(OpCode::JumpIfFalse): {
                uint16_t jumpDist = readShort();
                peek(0).resolve();
                if (isFalsey(peek(0)))
                    frame->ip += jumpDist;
                break;
            }
            case asByte(OpCode::JumpIfTrue): {
                uint16_t jumpDist = readShort();
                peek(0).resolve();
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
                callee.resolve();
                if (!callValue(callee, callSpec))
                    return errorReturn;
                frame = thread->frames.end()-1;
                break;
            }
            case asByte(OpCode::Index): {
                uint8_t argCount = readByte();
                peek(argCount).resolveFuture(); // don't resolve signals here
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
                if (function->ownerType.isNil() && !frame->closure->function->ownerType.isNil())
                    function->ownerType = frame->closure->function->ownerType;
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

                    // For nested execute() calls, only terminate when we return to entry depth
                    if (thread->execute_depth > 1 && thread->frames.size() <= frame_depth_on_entry) {
                        Value returnVal = pop();

                        freeObjects(); // Always cleanup objects on scope exit for deterministic destruction

                        if (thread->execute_depth > 0) thread->execute_depth--;
                        return std::make_pair(InterpretResult::OK,returnVal);
                    }

                    // For top-level execute(), use original termination logic
                    if (thread->execute_depth == 1 && thread->frames.empty()) {
                        Value returnVal = pop();

                        freeObjects();

                        if (thread->execute_depth > 0) thread->execute_depth--;
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

                    // For nested execute() calls, only terminate when we return to entry depth
                    if (thread->execute_depth > 1 && thread->frames.size() <= frame_depth_on_entry) {
                        freeObjects(); // Always cleanup objects on scope exit for deterministic destruction

                        if (thread->execute_depth > 0) thread->execute_depth--;
                        return std::make_pair(InterpretResult::OK,result);
                    }

                    // For top-level execute(), use original termination logic
                    if (thread->execute_depth == 1 && thread->frames.empty()) {
                        freeObjects();

                        if (thread->execute_depth > 0) thread->execute_depth--;
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
            case asByte(OpCode::SetIndex): {
                uint8_t argCount = readByte();
                peek(argCount).resolveFuture(); // don't resolve signals here
                try {
                    Value& indexable { peek(argCount) };
                    Value& value { peek(argCount+1) };
                    setIndexValue(indexable, argCount, value);
                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return errorReturn;
                }
                break;
            }
            case asByte(OpCode::DefineModuleVar): {
                ObjString* name = readString();
                moduleVars().store(name->hash, name->s,pop());
                break;
            }
            case asByte(OpCode::DefineModuleVar2): {
                ObjString* name = readString2();
                moduleVars().store(name->hash, name->s,pop());
                break;
            }
            case asByte(OpCode::GetModuleVar): {
                ObjString* name = readString();
                auto& vars { moduleVars() };
                auto optValue { vars.load(name->hash) };
                if (optValue.has_value()) {
                    Value value = optValue.value();
                    push(value);
                }
                else {
                    #ifdef DEBUG_BUILD
                    runtimeError("Undefined variable '"+name->toStdString()+"' in module "+toUTF8StdString(moduleType()->name));
                    #else
                    runtimeError("Undefined variable '"+name->toStdString()+"'");
                    #endif
                    return errorReturn;
                }
                break;
            }
            case asByte(OpCode::GetModuleVar2): {
                ObjString* name = readString2();
                const auto& vars { moduleVars() };
                auto optValue { vars.load(name->hash) };
                if (optValue.has_value()) {
                    Value value = optValue.value();
                    push(value);
                }
                else {
                    runtimeError("Undefined variable '"+name->toStdString()+"'");
                    return errorReturn;
                }
                break;
            }
            case asByte(OpCode::SetModuleVar): {
                ObjString* name = readString();
                auto& vars { moduleVars() };
                // set new value, but leave it on stack (as assignment is an expression)
                bool stored = vars.storeIfExists(name->hash, name->s,peek(0));
                if (!stored) { // not stored, since not existing
                    runtimeError("Undefined variable '"+name->toStdString()+"'");
                    return errorReturn;
                }
                break;
            }
            case asByte(OpCode::SetModuleVar2): {
                ObjString* name = readString2();
                auto& vars { moduleVars() };
                // set new value, but leave it on stack (as assignment is an expression)
                bool stored = vars.storeIfExists(name->hash, name->s,peek(0));
                if (!stored) { // not stored, since not existing
                    runtimeError("Undefined variable '"+name->toStdString()+"'");
                    return errorReturn;
                }
                break;
            }
            case asByte(OpCode::SetNewModuleVar): {
                ObjString* name = readString();
                auto& vars { moduleVars() };

                // only automatic declaration of globals on assignment when
                //   at module level scope
                bool moduleScope = true; // FIXME: set false if not in module scope

                bool exists = vars.exists(name->hash);
                if (!exists && !moduleScope) {
                    runtimeError("Undefined variable '"+name->toStdString()+"'");
                    return errorReturn;
                }

                vars.store(name->hash, name->s,peek(0), /*overwrite=*/true);

                break;
            }
            case asByte(OpCode::SetNewModuleVar2): {
                ObjString* name = readString2();
                auto& vars { moduleVars() };

                // only automatic declaration of globals on assignment when
                //   at module level scope
                bool moduleScope = true; // FIXME: set false if not in module scope

                bool exists = vars.exists(name->hash);
                if (!exists && !moduleScope) {
                    runtimeError("Undefined variable '"+name->toStdString()+"'");
                    return errorReturn;
                }

                vars.store(name->hash, name->s,peek(0), /*overwrite=*/true);
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
            case asByte(OpCode::NewRange): {
                bool closed = ((readByte() & 1) > 0);
                if (!peek(2).isNil() && !peek(2).isNumber())
                    runtimeError("The start bound of a range must be a number");
                if (!peek(1).isNil() && !peek(1).isNumber())
                    runtimeError("The stop bound of a range must be a number");
                if (!peek(0).isNil() && !peek(0).isNumber())
                    runtimeError("The step of a range must be a number");
                auto rangeObj = rangeVal(peek(2),peek(1),peek(0),closed);
                popN(3);
                push(objVal(rangeObj));
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
            case asByte(OpCode::NewVector): {
                int eltCount = readByte();
                Eigen::VectorXd vals(eltCount);
                for(int i=0; i<eltCount; i++)
                    vals[i] = toType(ValueType::Real, peek(eltCount-i-1), false).asReal();
                for(int i=0; i<eltCount; i++) pop();
                push(objVal(vectorVal(vals)));
                break;
            }
            case asByte(OpCode::NewMatrix): {
                int rowCount = readByte();
                if (rowCount == 0) {
                    push(objVal(matrixVal()));
                    break;
                }
                if (!isVector(peek(rowCount-1))) {
                    runtimeError("matrix literal rows must be vectors");
                    return errorReturn;
                }
                int colCount = asVector(peek(rowCount-1))->length();
                Eigen::MatrixXd mat(rowCount, colCount);
                for(int r=0; r<rowCount; ++r) {
                    Value rowVal = peek(rowCount - r - 1);
                    if (!isVector(rowVal)) {
                        runtimeError("matrix literal rows must be vectors");
                        return errorReturn;
                    }
                    ObjVector* vec = asVector(rowVal);
                    if (vec->length() != colCount) {
                        runtimeError("matrix rows must have equal length");
                        return errorReturn;
                    }
                    for(int c=0; c<colCount; ++c)
                        mat(r,c) = vec->vec[c];
                }
                for(int i=0; i<rowCount; ++i) pop();
                push(objVal(matrixVal(mat)));
                break;
            }
            case asByte(OpCode::IfDictToKeys): {
                Value& maybeDict = peek(0);
                if (!isDict(maybeDict))
                    maybeDict.resolve();
                if (isDict(maybeDict)) {
                    Value d { maybeDict };
                    pop();
                    auto keys { asDict(d)->keys() };
                    push(objVal(listVal(keys)));
                }
                break;
            }
            case asByte(OpCode::IfDictToItems): {
                Value& maybeDict = peek(0);
                if (!isDict(maybeDict))
                    maybeDict.resolve();
                if (isDict(maybeDict)) {
                    Value d { maybeDict };
                    pop();
                    auto vecItemPairs { asDict(d)->items() };
                    ObjList* listItems { listVal() };
                    for(const auto& item : vecItemPairs) {
                        ObjList* itemList = listVal();
                        itemList->elts.push_back(item.first);
                        itemList->elts.push_back(item.second);
                        listItems->elts.push_back(objVal(itemList));
                    }
                    push(objVal(listItems));
                }
                break;
            }
            case asByte(OpCode::ToType): {
                uint8_t typeByte = readByte();
                try {
                    peek(0) = toType(ValueType(typeByte), peek(0), /*strict=*/false);
                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return errorReturn;
                }
                break;
            }
            case asByte(OpCode::ToTypeStrict): {
                uint8_t typeByte = readByte();
                try {
                    peek(0) = toType(ValueType(typeByte), peek(0), /*strict=*/true);
                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return errorReturn;
                }
                break;
            }
            case asByte(OpCode::EventOn): {
                Value closureVal = pop();
                Value eventVal = pop();
                if (!isClosure(closureVal) || !(isEvent(eventVal) || isSignal(eventVal))) {
                    runtimeError("EVENT_ON expects event/signal and closure");
                    return errorReturn;
                }

                ObjEvent* ev = nullptr;
                if (isEvent(eventVal)) {
                    ev = asEvent(eventVal);
                } else {
                    ObjSignal* sigObj = asSignal(eventVal);
                    ev = sigObj->ensureChangeEvent();
                    eventVal = sigObj->changeEvent;
                }

                // record this handler on the current thread
                Value key = eventVal.weakRef();
                thread->eventHandlers[key].push_back(closureVal);

                // track the handler thread and subscribe the closure to the event
                auto* closure = asClosure(closureVal);
                closure->handlerThread = thread;
                ev->subscribers.push_back(closureVal.weakRef());
                break;
            }
            case asByte(OpCode::ObjectType): {
                ObjString* name = readString();
                ObjObjectType* t = objectTypeVal(name->s, false);
                if (!thread->frames.empty()) {
                    auto frame = thread->frames.end()-1;
                    ObjModuleType* mod = asModuleType(frame->closure->function->moduleType);
                    auto it = mod->cstructArch.find(name->hash);
                    if (it != mod->cstructArch.end()) {
                        t->isCStruct = true;
                        t->cstructArch = it->second;
                    }
                }
                push(objVal(t));
                break;
            }
            case asByte(OpCode::ActorType): {
                ObjString* name = readString();
                ObjObjectType* t = objectTypeVal(name->s, true);
                if (!thread->frames.empty()) {
                    auto frame = thread->frames.end()-1;
                    ObjModuleType* mod = asModuleType(frame->closure->function->moduleType);
                    auto it = mod->cstructArch.find(name->hash);
                    if (it != mod->cstructArch.end()) {
                        t->isCStruct = true;
                        t->cstructArch = it->second;
                    }
                }
                push(objVal(t));
                break;
            }
            case asByte(OpCode::InterfaceType): {
                // interface types are represented as object types (but are abstract - all abstract methods)
                ObjString* name = readString();
                ObjObjectType* t = objectTypeVal(name->s, false, true);
                if (!thread->frames.empty()) {
                    auto frame = thread->frames.end()-1;
                    ObjModuleType* mod = asModuleType(frame->closure->function->moduleType);
                    auto it = mod->cstructArch.find(name->hash);
                    if (it != mod->cstructArch.end()) {
                        t->isCStruct = true;
                        t->cstructArch = it->second;
                    }
                }
                push(objVal(t));
                break;
            }
            case asByte(OpCode::EnumerationType): {
                ObjString* name = readString();
                ObjObjectType* t = objectTypeVal(name->s, false, false, true);
                if (!thread->frames.empty()) {
                    auto frame = thread->frames.end()-1;
                    ObjModuleType* mod = asModuleType(frame->closure->function->moduleType);
                    auto it = mod->cstructArch.find(name->hash);
                    if (it != mod->cstructArch.end()) {
                        t->isCStruct = true;
                        t->cstructArch = it->second;
                    }
                }
                push(objVal(t));
                break;
            }
            case asByte(OpCode::Property): {
                try {
                    defineProperty(readString());
                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return errorReturn;
                }
                break;
            }
            case asByte(OpCode::Method): {
                try {
                    defineMethod(readString());
                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return errorReturn;
                }
                break;
            }
            case asByte(OpCode::EnumLabel): {
                try {
                    defineEnumLabel(readString());
                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return errorReturn;
                }
                break;
            }
            case asByte(OpCode::Extend): {
                if (!isObjectType(peek(1))) {
                    runtimeError("Super type to extend must be an object or actor type");
                    return errorReturn;
                }
                ObjObjectType* superType = asObjectType(peek(1));
                ObjObjectType* subType = asObjectType(peek(0));

                // object cannot extend an actor
                if (superType->isActor && !subType->isActor) {
                    runtimeError("A type object cannot extend an actor, only another object type");
                    return errorReturn;
                }

                // record inheritance relationship and copy properties
                subType->superType = objVal(superType);
                subType->properties.insert(superType->properties.cbegin(), superType->properties.cend());
                subType->propertyOrder.insert(subType->propertyOrder.end(),
                                             superType->propertyOrder.begin(),
                                             superType->propertyOrder.end());
                pop();
                break;
            }
            case asByte(OpCode::ImportModuleVars): {
                // given a list of var identifiers and two module types, copy the list of vars from
                //  one module's vars to the other (copy the declarations, not deep copying values)

                Value symbolsList { peek(2) };
                Value fromModule { peek(1) };
                Value toModule { peek(0) };

                //std::cout << "importing module vars " << objListToString(asList(symbolsList)) << " from " << fromModule << " to " << toModule << std::endl;

                #ifdef DEBUG_BUILD
                assert(isList(symbolsList));
                assert(isModuleType(fromModule));
                assert(isModuleType(toModule));
                #endif

                auto symbolsListObj { asList(symbolsList) };

                if (symbolsListObj->length() > 0) {

                    auto fromModuleType { asModuleType(fromModule) };
                    auto toModuleType { asModuleType(toModule) };

                    // special case, if list is just [*], then import all symbols
                    const auto& firstElement { symbolsListObj->elts.at(0) };
                    if (isString(firstElement) && asString(firstElement)->s == "*") {
                        fromModuleType->vars.forEach(
                            [&](const VariablesMap::NameValue& nameValue) {
                                //const icu::UnicodeString& name { nameValue.first };
                                toModuleType->vars.store(nameValue);
                                //std::cout << "declaring " << toUTF8StdString(nameValue.first) << " into " << toUTF8StdString(toModuleType->name) << std::endl;
                            });
                    }
                    else { // import the symbols explicitly listed
                        for(const auto& symbol : symbolsListObj->elts.get()) {
                            const auto& symbolString { asString(symbol) };
                            auto optValue { fromModuleType->vars.load(symbolString->hash) };
                            const auto& name { symbolString->s };

                            if (!optValue.has_value()) {
                                runtimeError("Symbol '"+toUTF8StdString(name)+"' not found in imported module "+toUTF8StdString(fromModuleType->name));
                                return errorReturn;
                            }
                            toModuleType->vars.store(symbolString->hash, name, optValue.value());
                            //std::cout << "declaring " << toUTF8StdString(name) << " into " << toUTF8StdString(toModuleType->name) << std::endl;
                        }
                    }
                }

                popN(3);
                break;
            }
            case asByte(OpCode::Nop): {
                break;
            }
            default:
                #ifdef DEBUG_BUILD
                runtimeError("Invalid instruction "+std::to_string(instruction));
                #endif
                return std::make_pair(InterpretResult::RuntimeError,nilVal());
                break;
        }

        postInstructionDispatch:

        // are we supposed to be sleeping?  If so, block until the sleep time is over
        //  or until we get a wakeup signal (for a possible event)
        // if we've slept for long enough, reset the flag and continue execution
        if (thread->threadSleep) {
            if (TimePoint::currentTime() >= thread->threadSleepUntil) {
                thread->threadSleep = false;
            }
            else {
                auto remainingSleepTime = thread->threadSleepUntil - TimePoint::currentTime();
                std::unique_lock<std::mutex> lk(thread->sleepMutex);
                bool timedout = (thread->sleepCondVar.wait_for(lk, std::chrono::microseconds(remainingSleepTime.microSecs())) == std::cv_status::timeout);
            }
        }

        if (!processPendingEvents())
            return errorReturn;

        freeObjects();

    } // for

    if (thread->execute_depth > 0) thread->execute_depth--;
    return std::make_pair(InterpretResult::OK,nilVal());

}


bool VM::processPendingEvents()
{
    if (thread->eventHandlers.empty()) return true;

    Thread::PendingEvent tev;

    // Drop events that are no longer alive or have no handlers
    while(thread->pendingEvents.pop_if([&](const Thread::PendingEvent& e){
                return !e.event.isAlive() ||
                        thread->eventHandlers.count(e.event) == 0;
            }, tev)) {
        thread->eventHandlers.erase(tev.event);
    }

    if (thread->eventHandlers.empty()) return true;

    auto now = TimePoint::currentTime();
    if (thread->pendingEvents.pop_if([&](const Thread::PendingEvent& e){
            return e.when <= now && e.event.isAlive() &&
                    thread->eventHandlers.count(e.event) > 0;
        }, tev)) {
        auto handlersIt = thread->eventHandlers.find(tev.event);
        if (handlersIt != thread->eventHandlers.end()) {
            for(const auto& handler : handlersIt->second) {
                auto closure = asClosure(handler);

                auto prevThreadSleep = thread->threadSleep.load();
                auto prevThreadSleepUntil = thread->threadSleepUntil.load();

                thread->threadSleep = false;
                auto result = callAndExec(closure, {});
                assert(!thread->threadSleep);

                thread->threadSleep = prevThreadSleep;
                thread->threadSleepUntil = prevThreadSleepUntil;

                if (result.first != InterpretResult::OK)
                    return false;
            }
        }
    }
    return true;
}



void VM::resetStack()
{
    thread->stack.clear();
    thread->stack.resize(MaxStack);
    thread->stackTop = thread->stack.begin();

    thread->frames.clear();
    thread->frames.reserve(128);
    thread->frameStart = false;

    for (auto* upvalue : thread->openUpvalues) {
        upvalue->decRef();
    }
    thread->openUpvalues.clear();
}


void VM::freeObjects()
{
    if (!Obj::unrefedObjs.empty()) {
        // free objects who's reference count dropped to 0
        while(!Obj::unrefedObjs.empty()) {
            Obj::unrefedObjs.pop_back_and_apply([](Obj* obj) {
                delObj(obj);
            });
        }
    }

    if (thread) {
        // remove event handlers referencing destroyed events or closures
        for(auto it = thread->eventHandlers.begin(); it != thread->eventHandlers.end(); ) {
            if (!it->first.isAlive()) {
                it = thread->eventHandlers.erase(it);
                continue;
            }

            ObjEvent* ev = asEvent(it->first);
            auto& handlers = it->second;
            for(auto hit = handlers.begin(); hit != handlers.end(); ) {
                if (!hit->isAlive()) {
                    for(auto es = ev->subscribers.begin(); es != ev->subscribers.end(); ) {
                        if (!es->isAlive() || asClosure(*es) == asClosure(*hit))
                            es = ev->subscribers.erase(es);
                        else
                            ++es;
                    }
                    hit = handlers.erase(hit);
                } else {
                    ++hit;
                }
            }

            if (handlers.empty())
                it = thread->eventHandlers.erase(it);
            else
                ++it;
        }
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
    int column = frame->closure->function->chunk->getColumn(instruction);
    int span = frame->closure->function->chunk->getSpan(instruction);
    fprintf(stderr, "[line %d] in script\n", line);

    auto linesPtr = frame->closure->function->chunk->sourceLines;
    if (linesPtr && line > 0 && line <= linesPtr->size()) {
        const std::string& srcLine = linesPtr->at(line-1);
        fprintf(stderr, "%s\n", srcLine.c_str());
        if (column >= 0) {
            int len = span > 0 ? span : 1;
            if (column + len > (int)srcLine.size())
                len = std::max(1, (int)srcLine.size() - column);
            std::string caret(column, ' ');
            caret += std::string(len, '^');
            fprintf(stderr, "%s\n", caret.c_str());
        }
    }
    resetStack();
}



//
// builtins

void VM::defineBuiltinFunctions()
{
    if (globals.existsGlobal(toUnicodeString("print")))
        return; // already defined

    auto addSys = [&](const std::string& name, NativeFn fn){
        defineNative(name, fn);
        sysModule->vars.store(toUnicodeString(name), objVal(nativeVal(fn)));
    };

    addSys("print", &VM::print_builtin);
    addSys("len", &VM::len_builtin);
    addSys("clone", &VM::clone_builtin);
    addSys("sleep", &VM::sleep_builtin);
    addSys("fork", &VM::fork_builtin);
    addSys("join", &VM::join_builtin);
    addSys("_threadid", &VM::threadid_builtin);
    addSys("_wait", &VM::wait_builtin);
    addSys("_runtests", &VM::runtests_builtin);
    addSys("_weakref", &VM::weakref_builtin);
    addSys("_weak_alive", &VM::weak_alive_builtin);
    addSys("_strongref", &VM::strongref_builtin);
}

void VM::defineBuiltinMethods()
{
    if (!builtinMethods.empty())
        return;

    defineBuiltinMethod(ValueType::Vector, "norm", &VM::vector_norm_builtin);
    defineBuiltinMethod(ValueType::Vector, "sum", &VM::vector_sum_builtin);
    defineBuiltinMethod(ValueType::Vector, "normalized", &VM::vector_normalized_builtin);
    defineBuiltinMethod(ValueType::Vector, "dot", &VM::vector_dot_builtin);

    defineBuiltinMethod(ValueType::Matrix, "rows", &VM::matrix_rows_builtin);
    defineBuiltinMethod(ValueType::Matrix, "cols", &VM::matrix_cols_builtin);
    defineBuiltinMethod(ValueType::Matrix, "transpose", &VM::matrix_transpose_builtin);
    defineBuiltinMethod(ValueType::Matrix, "determinant", &VM::matrix_determinant_builtin);
    defineBuiltinMethod(ValueType::Matrix, "inverse", &VM::matrix_inverse_builtin);
    defineBuiltinMethod(ValueType::Matrix, "trace", &VM::matrix_trace_builtin);
    defineBuiltinMethod(ValueType::Matrix, "norm", &VM::matrix_norm_builtin);
    defineBuiltinMethod(ValueType::Matrix, "sum", &VM::matrix_sum_builtin);

    defineBuiltinMethod(ValueType::List, "append", &VM::list_append_builtin);

    defineBuiltinMethod(ValueType::Signal, "run", &VM::signal_run_builtin);
    defineBuiltinMethod(ValueType::Signal, "stop", &VM::signal_stop_builtin);
    defineBuiltinMethod(ValueType::Signal, "tick", &VM::signal_tick_builtin);

    defineBuiltinMethod(ValueType::Event, "emit", &VM::event_emit_builtin, true);
    defineBuiltinMethod(ValueType::Event, "on", &VM::event_on_builtin, true);

    defineBuiltinMethod(ValueType::Actor, "tick", &VM::dataflow_tick_native, true);  // proc
    defineBuiltinMethod(ValueType::Actor, "run", &VM::dataflow_run_native, true);   // proc
    defineBuiltinMethod(ValueType::Actor, "runFor", &VM::dataflow_run_for_native, true);  // proc
}

void VM::defineBuiltinMethod(ValueType type, const std::string& name, NativeFn fn, bool isProc)
{
    auto us = toUnicodeString(name);
    builtinMethods[type][us.hashCode()] = BuiltinMethodInfo(fn, isProc);
}

void VM::defineBuiltinProperties()
{
    if (!builtinProperties.empty())
        return;

    // Signal properties
    defineBuiltinProperty(ValueType::Signal, "value", &VM::signal_value_getter);
}

void VM::defineBuiltinProperty(ValueType type, const std::string& name, NativePropertyGetter getter, NativePropertySetter setter)
{
    auto us = toUnicodeString(name);
    builtinProperties[type][us.hashCode()] = BuiltinPropertyInfo(getter, setter);
}

Value VM::signal_value_getter(Value& receiver)
{
    #ifdef DEBUG_BUILD
    if (!isSignal(receiver))
        throw std::invalid_argument("signal.value property called on non-signal value");
    #endif

    ObjSignal* objSignal = asSignal(receiver);
    return objSignal->signal->lastValue();
}


Value VM::print_builtin(int argCount, Value* args)
{
    if (argCount == 0) {
        std::cout << std::endl;
        return nilVal();
    }
    else if (argCount != 1)
        throw std::invalid_argument("print expects either no arguments (to output a newline) or single argument convertable to a string");

    auto str = toString(args[0]);
    std::cout << str << std::endl;
    return nilVal();
}


Value VM::len_builtin(int argCount, Value* args)
{
    if (argCount != 1)
        throw std::invalid_argument("len expects single argument");

    const Value& v (args[0]);
    int32_t len {1};

    switch (v.type()) {
        case ValueType::String: len = asString(v)->length(); break;
        case ValueType::List: len = asList(v)->length(); break;
        case ValueType::Dict: len = asDict(v)->length(); break;
        case ValueType::Vector: len = asVector(v)->length(); break;
        case ValueType::Range: {
            len = asRange(v)->length();
            if (len<0) return nilVal(); // has no defined length
        } break;
        default:
        #ifdef DEBUG_BUILD
        std::cerr << "Unhandled type in len():" << v.typeName() << std::endl;
        #endif
        ;
    }

    return intVal(len);
}



Value VM::clone_builtin(int argCount, Value* args)
{
    if (argCount != 1)
        throw std::invalid_argument("clone takes a single argument (the value to deep-copy)");

    return args[0].clone();
}


Value VM::sleep_builtin(int argCount, Value* args)
{
    if ((argCount != 1) || !args[0].isNumber())
        throw std::invalid_argument("sleep expects single numeric argument (microsecs)");

    auto microSecs { TimeDuration::microSecs(args[0].asInt()) };

    thread->threadSleep = true;
    thread->threadSleepUntil = TimePoint::currentTime() + microSecs;

    return nilVal();
}


Value VM::fork_builtin(int argCount, Value* args)
{
    if ((argCount != 1) || !isClosure(args[0]))
        throw std::invalid_argument("fork expects single callable argument (e.g. func or proc)");

    ObjClosure* closure = asClosure(args[0]);

    // Check if closure captures any outer variables (has upvalues)
    if (!closure->upvalues.empty()) {
        throw std::runtime_error("fork cannot execute functions that capture variables from outer scopes. "
                                "The function must only use its parameters and global variables.");
    }

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
            args[0].resolve();
            numFuturesResolved++;
        }
        else if (isList(args[0])) {

            ObjList* l = asList(args[0]);
            for(auto& v : l->elts.get()) {
                v.resolve();
                numFuturesResolved++;
            }
        }
    }
    else {
        for(auto i=0; i<argCount; i++) {
            if (isFuture(args[i])) {
                args[i].resolve(); // may block
                numFuturesResolved++;
            }
        }
    }

    return intVal(numFuturesResolved);
}


Value VM::runtests_builtin(int argCount, Value* args)
{
    if (argCount != 1 || !isString(args[0]))
        throw std::invalid_argument("_runtests expects single string argument");

    auto suite = toUTF8StdString(asString(args[0])->s);

    if (suite == "dataflow") {
        // TODO: Dataflow tests have been moved out - need to implement new roxal-based tests
        std::cout << "Dataflow tests temporarily disabled during Func class elimination" << std::endl;
        df::DataflowEngine::instance()->clear();
    }
    else if (suite == "conversions") {
        auto results = testConversions();

        int passes = 0;
        int fails = 0;
        for (const auto& result : results) {
            std::cout << "Test: " << std::get<0>(result) << " ";
            bool passed = std::get<1>(result);
            if (passed) {
                std::cout << "passed";
                passes++;
            }
            else {
                std::cout << "failed";
                fails++;
            }
            std::cout << " " << std::get<2>(result) << std::endl;
        }

        std::cout << "Passed " << passes << " failed " << fails << std::endl;
    }

    return nilVal();
}

Value VM::event_emit_builtin(int argCount, Value* args)
{
    if (argCount > 2 || !isEvent(args[0]))
        throw std::invalid_argument("event.emit expects optional time argument in microseconds");

    TimePoint when = TimePoint::currentTime();
    if (argCount == 2) {
        if (!args[1].isNumber())
            throw std::invalid_argument("event.emit time argument must be numeric microseconds");
        when = TimePoint::microSecs(args[1].asInt());
    }

    Value eventWeak = args[0].weakRef();
    ObjEvent* ev = asEvent(args[0]);
    if (ev->subscribers.empty())
        return nilVal();

    scheduleEventHandlers(eventWeak, ev, when);

    return nilVal();
}

Value VM::event_on_builtin(int argCount, Value* args)
{
    if (argCount != 2 || !isEvent(args[0]) || !isClosure(args[1]))
        throw std::invalid_argument("event.on expects event and closure argument");

    Value eventVal = args[0];
    Value closureVal = args[1];

    // record this handler on the current thread
    Value key = eventVal.weakRef();
    thread->eventHandlers[key].push_back(closureVal);

    // track the handler thread and subscribe the closure to the event
    ObjEvent* ev = asEvent(eventVal);
    ObjClosure* closure = asClosure(closureVal);
    closure->handlerThread = thread;
    ev->subscribers.push_back(closureVal.weakRef());

    return nilVal();
}

Value VM::weakref_builtin(int argCount, Value* args)
{
    if (argCount != 1)
        throw std::invalid_argument("weakref expects single argument");

    return args[0].weakRef();
}

Value VM::weak_alive_builtin(int argCount, Value* args)
{
    if (argCount != 1)
        throw std::invalid_argument("weak_alive expects single argument");

    return args[0].isAlive() ? trueVal() : falseVal();
}

Value VM::strongref_builtin(int argCount, Value* args)
{
    if (argCount != 1)
        throw std::invalid_argument("strongref expects single argument");

    return args[0].strongRef();
}

Value VM::vector_norm_builtin(int argCount, Value* args)
{
    if (argCount != 1 || !isVector(args[0]))
        throw std::invalid_argument("vector.norm expects no arguments");

    ObjVector* vec = asVector(args[0]);
    double n = vec->vec.norm();
    return realVal(n);
}

Value VM::vector_sum_builtin(int argCount, Value* args)
{
    if (argCount != 1 || !isVector(args[0]))
        throw std::invalid_argument("vector.sum expects no arguments");

    ObjVector* vec = asVector(args[0]);
    double s = vec->vec.sum();
    return realVal(s);
}

Value VM::vector_normalized_builtin(int argCount, Value* args)
{
    if (argCount != 1 || !isVector(args[0]))
        throw std::invalid_argument("vector.normalized expects no arguments");

    ObjVector* vec = asVector(args[0]);
    Eigen::VectorXd nvec = vec->vec.normalized();
    return objVal(vectorVal(nvec));
}

Value VM::vector_dot_builtin(int argCount, Value* args)
{
    if (argCount != 2 || !isVector(args[0]) || !isVector(args[1]))
        throw std::invalid_argument("vector.dot expects single vector argument");

    ObjVector* v1 = asVector(args[0]);
    ObjVector* v2 = asVector(args[1]);
    if (v1->length() != v2->length())
        throw std::invalid_argument("vector.dot requires vectors of same length");

    double d = v1->vec.dot(v2->vec);
    return realVal(d);
}

Value VM::matrix_rows_builtin(int argCount, Value* args)
{
    if (argCount != 1 || !isMatrix(args[0]))
        throw std::invalid_argument("matrix.rows expects no arguments");

    ObjMatrix* mat = asMatrix(args[0]);
    return intVal(mat->rows());
}

Value VM::matrix_cols_builtin(int argCount, Value* args)
{
    if (argCount != 1 || !isMatrix(args[0]))
        throw std::invalid_argument("matrix.cols expects no arguments");

    ObjMatrix* mat = asMatrix(args[0]);
    return intVal(mat->cols());
}

Value VM::matrix_transpose_builtin(int argCount, Value* args)
{
    if (argCount != 1 || !isMatrix(args[0]))
        throw std::invalid_argument("matrix.transpose expects no arguments");

    ObjMatrix* mat = asMatrix(args[0]);
    Eigen::MatrixXd tr = mat->mat.transpose();
    return objVal(matrixVal(tr));
}

Value VM::matrix_determinant_builtin(int argCount, Value* args)
{
    if (argCount != 1 || !isMatrix(args[0]))
        throw std::invalid_argument("matrix.determinant expects no arguments");

    ObjMatrix* mat = asMatrix(args[0]);
    if (mat->rows() != mat->cols())
        throw std::invalid_argument("matrix.determinant requires a square matrix");

    double det = mat->mat.determinant();
    return realVal(det);
}

Value VM::matrix_inverse_builtin(int argCount, Value* args)
{
    if (argCount != 1 || !isMatrix(args[0]))
        throw std::invalid_argument("matrix.inverse expects no arguments");

    ObjMatrix* mat = asMatrix(args[0]);
    if (mat->rows() != mat->cols())
        throw std::invalid_argument("matrix.inverse requires a square matrix");

    Eigen::MatrixXd inv = mat->mat.inverse();
    return objVal(matrixVal(inv));
}

Value VM::matrix_trace_builtin(int argCount, Value* args)
{
    if (argCount != 1 || !isMatrix(args[0]))
        throw std::invalid_argument("matrix.trace expects no arguments");

    ObjMatrix* mat = asMatrix(args[0]);
    double tr = mat->mat.trace();
    return realVal(tr);
}

Value VM::matrix_norm_builtin(int argCount, Value* args)
{
    if (argCount != 1 || !isMatrix(args[0]))
        throw std::invalid_argument("matrix.norm expects no arguments");

    ObjMatrix* mat = asMatrix(args[0]);
    double n = mat->mat.norm();
    return realVal(n);
}

Value VM::matrix_sum_builtin(int argCount, Value* args)
{
    if (argCount != 1 || !isMatrix(args[0]))
        throw std::invalid_argument("matrix.sum expects no arguments");

    ObjMatrix* mat = asMatrix(args[0]);
    double s = mat->mat.sum();
    return realVal(s);
}

Value VM::list_append_builtin(int argCount, Value* args)
{
    if (argCount != 2 || !isList(args[0]))
        throw std::invalid_argument("list.append expects single argument");

    // TODO: Signal values should be resolved when passed as function arguments
    // Currently signals may not be resolved immediately, requiring workarounds like arithmetic (0 + signal)
    ObjList* list = asList(args[0]);
    list->elts.push_back(args[1]);
    return nilVal();
}

Value VM::signal_run_builtin(int argCount, Value* args)
{
    if (argCount != 1 || !isSignal(args[0]))
        throw std::invalid_argument("signal.run expects no arguments");

    ObjSignal* objSignal = asSignal(args[0]);
    auto sig = objSignal->signal;
    if (!sig->isSourceSignal())
        throw std::runtime_error("signal.run not supported for non-source signal");

    sig->run();
    return nilVal();
}

Value VM::signal_stop_builtin(int argCount, Value* args)
{
    if (argCount != 1 || !isSignal(args[0]))
        throw std::invalid_argument("signal.stop expects no arguments");

    ObjSignal* objSignal = asSignal(args[0]);
    auto sig = objSignal->signal;
    if (!sig->isSourceSignal())
        throw std::runtime_error("signal.stop not supported for non-source signal");

    sig->stop();
    return nilVal();
}

Value VM::signal_tick_builtin(int argCount, Value* args)
{
    if (argCount != 1 || !isSignal(args[0]))
        throw std::invalid_argument("signal.tick expects no arguments");

    ObjSignal* objSignal = asSignal(args[0]);
    auto sig = objSignal->signal;
    if (!sig->isSourceSignal())
        throw std::runtime_error("signal.tick only supported for source signals");

    sig->tickOnce();
    return nilVal();
}

Value VM::math_identity_builtin(int argCount, Value* args)
{
    if (argCount != 1 || !args[0].isNumber())
        throw std::invalid_argument("math.identity expects single integer size");

    int n = toType(ValueType::Int, args[0], false).asInt();
    Eigen::MatrixXd m = Eigen::MatrixXd::Identity(n, n);
    return objVal(matrixVal(m));
}

Value VM::math_zeros_builtin(int argCount, Value* args)
{
    if (argCount != 2 || !args[0].isNumber() || !args[1].isNumber())
        throw std::invalid_argument("math.zeros expects two integer arguments");

    int r = toType(ValueType::Int, args[0], false).asInt();
    int c = toType(ValueType::Int, args[1], false).asInt();
    Eigen::MatrixXd m = Eigen::MatrixXd::Zero(r, c);
    return objVal(matrixVal(m));
}

Value VM::math_ones_builtin(int argCount, Value* args)
{
    if (argCount != 2 || !args[0].isNumber() || !args[1].isNumber())
        throw std::invalid_argument("math.ones expects two integer arguments");

    int r = toType(ValueType::Int, args[0], false).asInt();
    int c = toType(ValueType::Int, args[1], false).asInt();
    Eigen::MatrixXd m = Eigen::MatrixXd::Ones(r, c);
    return objVal(matrixVal(m));
}

Value VM::math_dot_builtin(int argCount, Value* args)
{
    if (argCount != 2 || !isVector(args[0]) || !isVector(args[1]))
        throw std::invalid_argument("math.dot expects two vector arguments");

    ObjVector* v1 = asVector(args[0]);
    ObjVector* v2 = asVector(args[1]);
    if (v1->length() != v2->length())
        throw std::invalid_argument("math.dot requires vectors of same length");

    double d = v1->vec.dot(v2->vec);
    return realVal(d);
}

Value VM::math_cross_builtin(int argCount, Value* args)
{
    if (argCount != 2 || !isVector(args[0]) || !isVector(args[1]))
        throw std::invalid_argument("math.cross expects two vector arguments");

    ObjVector* v1 = asVector(args[0]);
    ObjVector* v2 = asVector(args[1]);
    if (v1->length() != 3 || v2->length() != 3)
        throw std::invalid_argument("math.cross requires 3 element vectors");

    Eigen::Vector3d r = v1->vec.head<3>().cross(v2->vec.head<3>());
    Eigen::VectorXd res = r;
    return objVal(vectorVal(res));
}



//
// native

void VM::defineNativeFunctions()
{
    if (globals.existsGlobal(toUnicodeString("_clock")))
        return; // already defined

    auto addSys = [&](const std::string& name, NativeFn fn){
        defineNative(name, fn);
        sysModule->vars.store(toUnicodeString(name), objVal(nativeVal(fn)));
    };

    addSys("_clock", &VM::clock_native);
    addSys("_ussleep", &VM::usSleep_native);
    addSys("_mssleep", &VM::msSleep_native);
    addSys("clock", &VM::clock_signal_native);
    addSys("_engine_stop", &VM::engine_stop_native);
    addSys("typeof", &VM::typeof_native);
    addSys("loadlib", &VM::loadlib_native);
    //addSys("_sleep", &VM::sleep_native);

    auto addMath = [&](const std::string& name, void* fnPtr,
                       std::vector<ffi_type*> args){
        void* spec = createFFIWrapper(fnPtr, &ffi_type_double, args);
        mathModule->vars.store(toUnicodeString(name),
                               objVal(nativeVal(&VM::ffi_native, spec)));
    };

    auto addMathBuiltin = [&](const std::string& name, NativeFn fn){
        mathModule->vars.store(toUnicodeString(name), objVal(nativeVal(fn)));
    };

    addMath("sin",  (void*)(double (*)(double))sin,  {&ffi_type_double});
    addMath("cos",  (void*)(double (*)(double))cos,  {&ffi_type_double});
    addMath("tan",  (void*)(double (*)(double))tan,  {&ffi_type_double});
    addMath("asin", (void*)(double (*)(double))asin, {&ffi_type_double});
    addMath("acos", (void*)(double (*)(double))acos, {&ffi_type_double});
    addMath("atan", (void*)(double (*)(double))atan, {&ffi_type_double});
    addMath("atan2", (void*)(double (*)(double,double))atan2,
             {&ffi_type_double, &ffi_type_double});
    addMath("sinh", (void*)(double (*)(double))sinh, {&ffi_type_double});
    addMath("cosh", (void*)(double (*)(double))cosh, {&ffi_type_double});
    addMath("tanh", (void*)(double (*)(double))tanh, {&ffi_type_double});
    addMath("asinh", (void*)(double (*)(double))asinh, {&ffi_type_double});
    addMath("acosh", (void*)(double (*)(double))acosh, {&ffi_type_double});
    addMath("atanh", (void*)(double (*)(double))atanh, {&ffi_type_double});
    addMath("exp",  (void*)(double (*)(double))exp,  {&ffi_type_double});
    addMath("log",  (void*)(double (*)(double))log,  {&ffi_type_double});
    addMath("log10",(void*)(double (*)(double))log10,{&ffi_type_double});
    addMath("log2", (void*)(double (*)(double))log2, {&ffi_type_double});
    addMath("sqrt", (void*)(double (*)(double))sqrt, {&ffi_type_double});
    addMath("cbrt", (void*)(double (*)(double))cbrt, {&ffi_type_double});
    addMath("ceil", (void*)(double (*)(double))ceil, {&ffi_type_double});
    addMath("floor",(void*)(double (*)(double))floor,{&ffi_type_double});
    addMath("round",(void*)(double (*)(double))round,{&ffi_type_double});
    addMath("trunc",(void*)(double (*)(double))trunc,{&ffi_type_double});
    addMath("fabs", (void*)(double (*)(double))fabs, {&ffi_type_double});
    addMath("hypot",(void*)(double (*)(double,double))hypot,
             {&ffi_type_double, &ffi_type_double});
    addMath("fmod", (void*)(double (*)(double,double))fmod,
             {&ffi_type_double, &ffi_type_double});
    addMath("remainder", (void*)(double (*)(double,double))remainder,
             {&ffi_type_double, &ffi_type_double});
    addMath("fmax", (void*)(double (*)(double,double))fmax,
             {&ffi_type_double, &ffi_type_double});
    addMath("fmin", (void*)(double (*)(double,double))fmin,
             {&ffi_type_double, &ffi_type_double});
    addMath("pow",  (void*)(double (*)(double,double))pow,
             {&ffi_type_double, &ffi_type_double});
    addMath("fma",  (void*)(double (*)(double,double,double))fma,
             {&ffi_type_double, &ffi_type_double, &ffi_type_double});

    addMath("copysign", (void*)(double (*)(double,double))copysign,
            {&ffi_type_double, &ffi_type_double});
    addMath("erf",  (void*)(double (*)(double))erf,  {&ffi_type_double});
    addMath("erfc", (void*)(double (*)(double))erfc, {&ffi_type_double});
    addMath("exp2", (void*)(double (*)(double))exp2, {&ffi_type_double});
    addMath("expm1", (void*)(double (*)(double))expm1, {&ffi_type_double});
    addMath("fdim", (void*)(double (*)(double,double))fdim,
            {&ffi_type_double, &ffi_type_double});
    addMath("lgamma", (void*)(double (*)(double))lgamma, {&ffi_type_double});
    addMath("log1p", (void*)(double (*)(double))log1p, {&ffi_type_double});
    addMath("logb", (void*)(double (*)(double))logb, {&ffi_type_double});
    addMath("nearbyint", (void*)(double (*)(double))nearbyint,
            {&ffi_type_double});
    addMath("nextafter", (void*)(double (*)(double,double))nextafter,
            {&ffi_type_double, &ffi_type_double});
    addMath("rint", (void*)(double (*)(double))rint, {&ffi_type_double});
    addMath("tgamma", (void*)(double (*)(double))tgamma, {&ffi_type_double});

    addMathBuiltin("identity", &VM::math_identity_builtin);
    addMathBuiltin("zeros", &VM::math_zeros_builtin);
    addMathBuiltin("ones", &VM::math_ones_builtin);
    addMathBuiltin("dot", &VM::math_dot_builtin);
    addMathBuiltin("cross", &VM::math_cross_builtin);
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

Value VM::clock_signal_native(int argCount, Value* args)
{
    if ((argCount != 1) || !args[0].isNumber())
        throw std::invalid_argument("clock expects single numeric argument");

    double freq = args[0].asReal();
    auto sig = df::Signal::newClockSignal(freq);
    return objVal(signalVal(sig));
}

Value VM::engine_stop_native(int argCount, Value* args)
{
    df::DataflowEngine::instance()->stop();
    return nilVal();
}

Value VM::typeof_native(int argCount, Value* args)
{
    if (argCount != 1)
        throw std::invalid_argument("typeof expects single argument");

    Value val = args[0];
    ValueType valueType;

    // Determine the ValueType of the argument
    if (val.isNil()) {
        valueType = ValueType::Nil;
    } else if (val.isBool()) {
        valueType = ValueType::Bool;
    } else if (val.isByte()) {
        valueType = ValueType::Byte;
    } else if (val.isInt()) {
        valueType = ValueType::Int;
    } else if (val.isReal()) {
        valueType = ValueType::Real;
    } else if (val.isEnum()) {
        valueType = ValueType::Enum;
    } else if (val.isType()) {
        valueType = ValueType::Type;
    } else if (isSignal(val)) {
        valueType = ValueType::Signal;
    } else if (isEvent(val)) {
        valueType = ValueType::Event;
    } else if (val.isObj()) {
        // For object types, get the type from the object
        valueType = val.asObj()->valueType();
    } else {
        // Fallback
        valueType = ValueType::Nil;
    }

    // Create and return a type object
    ObjTypeSpec* typeObj = typeSpecVal(valueType);
    return objVal(typeObj);
}

Value VM::dataflow_tick_native(int argCount, Value* args)
{
    df::DataflowEngine::instance()->tick(false);
    return nilVal();
}

Value VM::dataflow_run_native(int argCount, Value* args)
{
    df::DataflowEngine::instance()->run();
    return nilVal();
}

Value VM::dataflow_run_for_native(int argCount, Value* args)
{
    if (argCount != 2 || !args[1].isNumber())
        throw std::invalid_argument("runFor expects single numeric argument");

    // TODO: _dataflow.runFor currently blocks the script thread instead of being asynchronous
    // This should be fixed so runFor sends a message to the dataflow actor thread and returns immediately
    auto duration = df::TimeDuration::microSecs(args[1].asInt());
    df::DataflowEngine::instance()->runFor(duration);
    return nilVal();
}

Value VM::loadlib_native(int argCount, Value* args)
{
    if (argCount != 1 || !isString(args[0]))
        throw std::invalid_argument("loadlib expects single string argument");

    std::string path = toUTF8StdString(asUString(args[0]));
    void* h = dlopen(path.c_str(), RTLD_LAZY);
    if (!h)
        throw std::runtime_error(std::string("dlopen failed: ") + dlerror());

    return objVal(libraryVal(h));
}

Value VM::sleep_native(int argCount, Value* args)
{
    if ((argCount != 1) || !args[0].isNumber())
        throw std::invalid_argument("sleep expects single numeric argument");

    std::this_thread::sleep_for(std::chrono::seconds(args[0].asInt()));

    return nilVal();
}

Value VM::ffi_native(int argCount, Value* args)
{
    ObjNative* native = asNative(*(args-1));
    FFIWrapper* spec = static_cast<FFIWrapper*>(native->data);
    if (!spec)
        throw std::runtime_error("ffi_native called without spec");
    if (argCount != (int)spec->argTypes.size())
        throw std::invalid_argument("invalid argument count for ffi function");

    std::vector<void*> argValues(argCount);
    std::vector<double> realVals(argCount);
    std::vector<float> floatVals(argCount);
    std::vector<int> intVals(argCount);
    std::vector<uint8_t> boolVals(argCount);

    for(int i=0;i<argCount;i++) {
        if (spec->argTypes[i] == &ffi_type_double || spec->argTypes[i] == &ffi_type_float) {
            if (!args[i].isNumber())
                throw std::invalid_argument("ffi argument not number");
            if (spec->argTypes[i] == &ffi_type_double) {
                realVals[i] = args[i].asReal();
                argValues[i] = &realVals[i];
            } else {
                floatVals[i] = args[i].asReal();
                argValues[i] = &floatVals[i];
            }
        } else if (spec->argTypes[i] == &ffi_type_sint32) {
            if (!args[i].isNumber())
                throw std::invalid_argument("ffi argument not int");
            intVals[i] = args[i].asInt();
            argValues[i] = &intVals[i];
        } else if (spec->argTypes[i] == &ffi_type_uint8) {
            if (!args[i].isBool())
                throw std::invalid_argument("ffi argument not bool");
            boolVals[i] = args[i].asBool() ? 1 : 0;
            argValues[i] = &boolVals[i];
        } else {
            throw std::runtime_error("unsupported ffi arg type");
        }
    }

    union {
        double d;
        float f;
        int i;
        uint8_t b;
    } ret;

    ffi_call(&spec->cif, FFI_FN(spec->fn), &ret, argValues.data());

    if (spec->retType == &ffi_type_double)
        return Value(ret.d);
    else if (spec->retType == &ffi_type_sint32)
        return Value(intVal(ret.i));
    else if (spec->retType == &ffi_type_uint8)
        return Value(boolVal(ret.b != 0));
    else
        throw std::runtime_error("unsupported ffi return type");
}

Value VM::callCFunc(ObjClosure* closure, const CallSpec& callSpec)
{
    ObjFunction* function = closure->function;
    FFIWrapper* spec = static_cast<FFIWrapper*>(function->nativeSpec);

    if (!spec) {
        // find annotation args
        ptr<ast::Annotation> annot;
        for(const auto& a : function->annotations)
            if (a->name == "cfunc") { annot = a; break; }
        if (!annot)
            throw std::runtime_error("cfunc annotation missing");

        ObjModuleType* mod = asModuleType(function->moduleType);

        auto getArg = [&](const std::string& n) -> ptr<ast::Expression> {
            for(const auto& an : annot->args)
                if (toUTF8StdString(an.first) == n) return an.second;
            return nullptr;
        };

        auto libExpr = getArg("lib");
        auto cnameExpr = getArg("cname");
        auto argsExpr = getArg("args");
        auto retExpr = getArg("ret");
        if (!libExpr || !cnameExpr)
            throw std::runtime_error("cfunc annotation requires lib and cname");

        auto evalExpr = [&](ptr<ast::Expression> expr) -> Value {
            if (auto s = std::dynamic_pointer_cast<ast::Str>(expr)) {
                return objVal(stringVal(s->str));
            } else if (auto n = std::dynamic_pointer_cast<ast::Num>(expr)) {
                if (std::holds_alternative<int32_t>(n->num))
                    return Value(std::get<int32_t>(n->num));
                else
                    return Value(std::get<double>(n->num));
            } else if (auto b = std::dynamic_pointer_cast<ast::Bool>(expr)) {
                return boolVal(b->value);
            } else if (auto v = std::dynamic_pointer_cast<ast::Variable>(expr)) {
                auto name = v->name;
                auto opt = mod->vars.load(name);
                if (!opt.has_value())
                    throw std::runtime_error("unknown variable in cfunc annotation: "+toUTF8StdString(name));
                return opt.value();
            } else {
                throw std::runtime_error("unsupported expression in cfunc annotation");
            }
        };

        Value libVal = evalExpr(libExpr);
        if (!isLibrary(libVal))
            throw std::runtime_error("lib argument not library handle");
        void* handle = asLibrary(libVal)->handle;

        Value cnameVal = evalExpr(cnameExpr);
        std::string cname = toUTF8StdString(asUString(cnameVal));

        std::vector<ffi_type*> argTypes;
        if (argsExpr) {
            std::string argsStr = toUTF8StdString(asUString(evalExpr(argsExpr)));
            std::stringstream ss(argsStr);
            std::string token;
            while(std::getline(ss, token, ',')) {
                token.erase(0, token.find_first_not_of(" \t"));
                size_t space = token.find(' ');
                std::string type = (space==std::string::npos)?token:token.substr(0,space);
                if (type=="float")
                    argTypes.push_back(&ffi_type_float);
                else if (type=="double" || type=="real")
                    argTypes.push_back(&ffi_type_double);
                else if (type=="int")
                    argTypes.push_back(&ffi_type_sint32);
                else if (type=="bool")
                    argTypes.push_back(&ffi_type_uint8);
                else if (!type.empty() && type.back()=='*')
                    argTypes.push_back(&ffi_type_pointer);
                else
                    throw std::runtime_error("unsupported arg type: "+type);
            }
        }

        ffi_type* retType = &ffi_type_void;
        ObjObjectType* retObjType = nullptr;
        std::vector<ffi_type*> retElems;
        ffi_type retStruct;
        if (retExpr) {
            std::string r = toUTF8StdString(asUString(evalExpr(retExpr)));
            if (r=="float")
                retType = &ffi_type_float;
            else if (r=="double" || r=="real")
                retType = &ffi_type_double;
            else if (r=="int")
                retType = &ffi_type_sint32;
            else if (r=="bool")
                retType = &ffi_type_uint8;
            else if (r=="void")
                retType = &ffi_type_void;
            else {
                auto opt = mod->vars.load(toUnicodeString(r));
                if (opt.has_value() && isObjectType(opt.value())) {
                    ObjObjectType* t = asObjectType(opt.value());
                    if (!t->isCStruct)
                        throw std::runtime_error("return type not cstruct: "+r);
                    retObjType = t;
                    for (int32_t h : t->propertyOrder) {
                        const auto& prop = t->properties.at(h);
                        std::string ct;
                        if (prop.ctype.has_value())
                            ct = toUTF8StdString(prop.ctype.value());
                        auto byName = [&](const std::string& n) -> ffi_type* {
                            if (n=="float") return &ffi_type_float;
                            if (n=="double" || n=="real") return &ffi_type_double;
                            if (n=="int") return &ffi_type_sint32;
                            if (n=="bool") return &ffi_type_uint8;
                            return nullptr;
                        };
                        ffi_type* et = nullptr;
                        if (!ct.empty()) {
                            et = byName(ct);
                        } else if (isTypeSpec(prop.type)) {
                            ObjTypeSpec* ts = asTypeSpec(prop.type);
                            switch(ts->typeValue) {
                                case ValueType::Bool: et=&ffi_type_uint8; break;
                                case ValueType::Byte: et=&ffi_type_uint8; break;
                                case ValueType::Int: et=&ffi_type_sint32; break;
                                case ValueType::Real: et=&ffi_type_double; break;
                                case ValueType::Enum: et=&ffi_type_sint32; break;
                                default: break;
                            }
                        }
                        if (!et)
                            throw std::runtime_error("unsupported struct field type");
                        retElems.push_back(et);
                    }
                    retElems.push_back(nullptr);
                    retStruct.size = 0; retStruct.alignment = 0; retStruct.type = FFI_TYPE_STRUCT;
                    retStruct.elements = retElems.data();
                    retType = &retStruct;
                } else {
                    throw std::runtime_error("unsupported return type: "+r);
                }
            }
        }

        void* fnPtr = dlsym(handle, cname.c_str());
        if (!fnPtr)
            throw std::runtime_error(std::string("dlsym failed: ")+dlerror());

        spec = new FFIWrapper;
        spec->fn = fnPtr;
        spec->argTypes = argTypes;
        spec->retType = retType;
        spec->retObjType = retObjType;
        if (retObjType) {
            spec->retStructElems = retElems;
            spec->retStructType = retStruct;
            spec->retStructType.elements = spec->retStructElems.data();
            spec->retType = &spec->retStructType;
        }
        if (ffi_prep_cif(&spec->cif, FFI_DEFAULT_ABI, argTypes.size(), spec->retType,
                         spec->argTypes.data()) != FFI_OK)
            throw std::runtime_error("ffi_prep_cif failed");
        function->nativeSpec = spec;
    }

    if (callSpec.argCount != (int)spec->argTypes.size())
        throw std::invalid_argument("invalid argument count for cfunc call");

    std::vector<Value> argVector(callSpec.argCount);
    for(int i=0;i<callSpec.argCount;i++)
        argVector[i] = *(thread->stackTop - callSpec.argCount + i);

    std::vector<void*> argValues(callSpec.argCount);
    std::vector<double> realVals(callSpec.argCount);
    std::vector<float> floatVals(callSpec.argCount);
    std::vector<int> intVals(callSpec.argCount);
    std::vector<uint8_t> boolVals(callSpec.argCount);
    std::vector<std::vector<uint8_t>> structBuffers(callSpec.argCount);
    std::vector<void*> structPtrs(callSpec.argCount);

    for(int i=0;i<callSpec.argCount;i++) {
        if (spec->argTypes[i] == &ffi_type_double || spec->argTypes[i] == &ffi_type_float) {
            if (!argVector[i].isNumber())
                throw std::invalid_argument("ffi arg not number");
            if (spec->argTypes[i] == &ffi_type_double) {
                realVals[i] = argVector[i].asReal();
                argValues[i] = &realVals[i];
            } else {
                floatVals[i] = argVector[i].asReal();
                argValues[i] = &floatVals[i];
            }
        } else if (spec->argTypes[i] == &ffi_type_sint32) {
            if (!argVector[i].isNumber())
                throw std::invalid_argument("ffi arg not int");
            intVals[i] = argVector[i].asInt();
            argValues[i] = &intVals[i];
        } else if (spec->argTypes[i] == &ffi_type_uint8) {
            if (!argVector[i].isBool())
                throw std::invalid_argument("ffi arg not bool");
            boolVals[i] = argVector[i].asBool() ? 1 : 0;
            argValues[i] = &boolVals[i];
        } else if (spec->argTypes[i] == &ffi_type_pointer) {
            if (!isObjectInstance(argVector[i]))
                throw std::invalid_argument("ffi arg not object instance for pointer");
            ObjectInstance* inst = asObjectInstance(argVector[i]);
            structBuffers[i] = objectToCStruct(inst);
            structPtrs[i] = structBuffers[i].data();
            argValues[i] = &structPtrs[i];
        } else {
            throw std::runtime_error("unsupported ffi arg type");
        }
    }

    union { double d; float f; int i; uint8_t b; void* p; } ret;
    void* retPtr = &ret;
    std::vector<uint8_t> retBuf;
    if (spec->retObjType) {
        retBuf.resize(spec->cif.rtype->size);
        retPtr = retBuf.data();
    }
    ffi_call(&spec->cif, FFI_FN(spec->fn), retPtr, argValues.data());

    if (spec->retObjType) {
        ObjectInstance* inst = objectFromCStruct(spec->retObjType, retBuf.data(), retBuf.size());
        return Value(inst);
    } else if (spec->retType == &ffi_type_double)
        return Value(ret.d);
    else if (spec->retType == &ffi_type_float)
        return Value(double(ret.f));
    else if (spec->retType == &ffi_type_sint32)
        return Value(intVal(ret.i));
    else if (spec->retType == &ffi_type_uint8)
        return Value(boolVal(ret.b != 0));
    else
        return nilVal();
}

ObjModuleType* VM::getBuiltinModule(const icu::UnicodeString& name)
{
    if (name == UnicodeString("sys"))
        return sysModule;
    if (name == UnicodeString("math"))
        return mathModule;
    return nullptr;
}

std::vector<uint8_t> VM::objectToCStruct(ObjectInstance* instance)
{
    if (!instance)
        throw std::invalid_argument("objectToCStruct null instance");

    ObjObjectType* type = instance->instanceType;
    if (!type->isCStruct)
        throw std::runtime_error("objectToCStruct called on non-cstruct type");

    std::vector<uint8_t> buffer;
    size_t offset = 0;
    size_t structAlign = 1;

    auto appendPadded = [&](const void* data, size_t size, size_t align) {
        size_t padding = (align - (offset % align)) % align;
        buffer.insert(buffer.end(), padding, 0);
        const uint8_t* p = reinterpret_cast<const uint8_t*>(data);
        buffer.insert(buffer.end(), p, p + size);
        offset += padding + size;
        structAlign = std::max(structAlign, align);
    };

    for (int32_t hash : type->propertyOrder) {
        const auto& prop = type->properties.at(hash);
        auto it = instance->properties.find(prop.name.hashCode());
        if (it == instance->properties.end())
            throw std::runtime_error("instance missing property in objectToCStruct");

        Value val = it->second;

        std::string ctypeStr;
        if (prop.ctype.has_value())
            ctypeStr = toUTF8StdString(prop.ctype.value());

        auto writeByName = [&](const std::string& ctype) -> bool {
            if (ctype == "float") { float f = float(val.asReal()); appendPadded(&f, sizeof(f), 4); return true; }
            if (ctype == "double" || ctype == "real") { double d = val.asReal(); appendPadded(&d, sizeof(d), 8); return true; }
            if (ctype == "int") { int32_t i = val.asInt(); appendPadded(&i, sizeof(i), 4); return true; }
            if (ctype == "bool") { uint8_t b = val.asBool()?1:0; appendPadded(&b, sizeof(b),1); return true; }
            return false;
        };

        if (!ctypeStr.empty()) {
            if (writeByName(ctypeStr))
                continue;
            throw std::runtime_error("unsupported ctype annotation: " + ctypeStr);
        }

        if (isTypeSpec(prop.type)) {
            ObjTypeSpec* ts = asTypeSpec(prop.type);
            switch (ts->typeValue) {
                case ValueType::Bool: {
                    uint8_t b = val.asBool();
                    appendPadded(&b, sizeof(b), 1);
                    break; }
                case ValueType::Byte: {
                    uint8_t v = val.asByte();
                    appendPadded(&v, sizeof(v), 1);
                    break; }
                case ValueType::Int: {
                    int32_t i = val.asInt();
                    appendPadded(&i, sizeof(i), 4);
                    break; }
                case ValueType::Real: {
                    double d = val.asReal();
                    appendPadded(&d, sizeof(d), 8);
                    break; }
                case ValueType::Enum: {
                    int32_t e = val.asEnum();
                    appendPadded(&e, sizeof(e), 4);
                    break; }
                default:
                    throw std::runtime_error("unsupported struct property type");
            }
        } else {
            throw std::runtime_error("struct property has no builtin type");
        }
    }

    size_t finalPad = (structAlign - (buffer.size() % structAlign)) % structAlign;
    buffer.insert(buffer.end(), finalPad, 0);

    return buffer;
}

ObjectInstance* VM::objectFromCStruct(ObjObjectType* type, const void* data, size_t len)
{
    if (!type || !data)
        throw std::invalid_argument("objectFromCStruct null type or data");

    if (!type->isCStruct)
        throw std::runtime_error("objectFromCStruct called on non-cstruct type");

    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);
    size_t offset = 0;
    size_t structAlign = 1;

    auto readPadded = [&](void* out, size_t size, size_t align) {
        size_t padding = (align - (offset % align)) % align;
        if (offset + padding + size > len)
            throw std::runtime_error("buffer too small for objectFromCStruct");
        offset += padding;
        memcpy(out, bytes + offset, size);
        offset += size;
        structAlign = std::max(structAlign, align);
    };

    ObjectInstance* inst = objectInstanceVal(type);

    for (int32_t hash : type->propertyOrder) {
        const auto& prop = type->properties.at(hash);

        Value val;

        std::string ctypeStr;
        if (prop.ctype.has_value())
            ctypeStr = toUTF8StdString(prop.ctype.value());

        auto readByName = [&](const std::string& ctype) -> bool {
            if (ctype == "float") { float f; readPadded(&f, sizeof(f), 4); val = Value(double(f)); return true; }
            if (ctype == "double" || ctype == "real") { double d; readPadded(&d, sizeof(d), 8); val = Value(d); return true; }
            if (ctype == "int") { int32_t i; readPadded(&i, sizeof(i), 4); val = intVal(i); return true; }
            if (ctype == "bool") { uint8_t b; readPadded(&b, sizeof(b), 1); val = boolVal(b != 0); return true; }
            return false;
        };

        if (!ctypeStr.empty()) {
            if (!readByName(ctypeStr))
                throw std::runtime_error("unsupported ctype annotation: " + ctypeStr);
        } else if (isTypeSpec(prop.type)) {
            ObjTypeSpec* ts = asTypeSpec(prop.type);
            switch (ts->typeValue) {
                case ValueType::Bool: { uint8_t b; readPadded(&b, sizeof(b), 1); val = boolVal(b != 0); break; }
                case ValueType::Byte: { uint8_t v; readPadded(&v, sizeof(v), 1); val = byteVal(v); break; }
                case ValueType::Int: { int32_t i; readPadded(&i, sizeof(i), 4); val = intVal(i); break; }
                case ValueType::Real: { double d; readPadded(&d, sizeof(d), 8); val = Value(d); break; }
                case ValueType::Enum: { int32_t e; readPadded(&e, sizeof(e), 4); val = intVal(e); break; }
                default:
                    throw std::runtime_error("unsupported struct property type");
            }
        } else {
            throw std::runtime_error("struct property has no builtin type");
        }

        inst->properties[prop.name.hashCode()] = val;
    }

    size_t finalPad = (structAlign - (offset % structAlign)) % structAlign;
    if (offset + finalPad > len)
        throw std::runtime_error("buffer too small for objectFromCStruct");

    return inst;
}
