#include <functional>
#include <time.h>
#include <math.h>
#include <chrono>
#include <thread>
#include <utility>
#include <memory>
#include <iomanip>
#include <sstream>
#include <filesystem>
#include <map>
#include <unordered_set>
#include <ffi.h>
#include <dlfcn.h>

#include <core/json11.h>


#include "ASTGenerator.h"
#include "ASTGraphviz.h"
#include "RoxalCompiler.h"
#include "dataflow/Signal.h"
#include "dataflow/DataflowEngine.h"
#include "dataflow/FuncNode.h"

#include <core/TimePoint.h>
#include "VM.h"
#include "Object.h"
#include "FFI.h"
#include "ModuleMath.h"
#include "ModuleSys.h"
#include "SimpleMarkSweepGC.h"
#include <Eigen/Dense>
#include <core/types.h>
#include <core/common.h>
#include <core/AST.h>
#include <fstream>
#include <cstdio>
#include <iostream>

using namespace roxal;

namespace {

struct BoundCallGuard {
    explicit BoundCallGuard(Thread* thread) : thread_(thread) {}
    BoundCallGuard(const BoundCallGuard&) = delete;
    BoundCallGuard& operator=(const BoundCallGuard&) = delete;
    ~BoundCallGuard() {
        if (thread_) {
            thread_->currentBoundCall = Value::nilVal();
        }
    }

private:
    Thread* thread_;
};

} // namespace

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

static ptr<type::Type> builtinConstructorType(ValueType t)
{
    using PT = type::Type::FuncType::ParamType;
    switch (t) {
        case ValueType::Signal: {
            static ptr<type::Type> sigType;
            if (!sigType) {
                sigType = make_ptr<type::Type>(type::BuiltinType::Func);
                sigType->func = type::Type::FuncType();
                PT pFreq(toUnicodeString("freq"));
                pFreq.type = make_ptr<type::Type>(type::BuiltinType::Real);
                pFreq.hasDefault = false;
                PT pInit(toUnicodeString("initial"));
                pInit.hasDefault = true;
                sigType->func->params = {pFreq, pInit};
            }
            return sigType;
        }
        default:
            return nullptr;
    }
}


static bool isExceptionType(ObjObjectType* type)
{
    while (type) {
        if (toUTF8StdString(type->name) == "exception")
            return true;
        if (type->superType.isNil())
            break;
        type = asObjectType(type->superType);
    }
    return false;
}


size_t VM::marshalArgs(ptr<type::Type> funcType,
                       const std::vector<Value>& defaults,
                       const CallSpec& callSpec,
                       Value* out,
                       bool includeReceiver,
                       const Value& receiver,
                       const std::map<int32_t, Value>& paramDefaultFuncs)
{
    const auto& params = funcType->func.value().params;
    auto paramPositions = callSpec.paramPositions(funcType, true);

    size_t idx = 0;
    if (includeReceiver)
        out[idx++] = receiver;

    for(size_t pi = 0; pi < params.size(); ++pi) {
        Value arg;
        int pos = paramPositions[pi];
        if (pos >= 0)
            arg = *(&(*thread->stackTop) - callSpec.argCount + pos);
        else if (pi < defaults.size())
            arg = defaults[pi];
        else {
            auto it = paramDefaultFuncs.find(params[pi]->nameHashCode);
            if (it != paramDefaultFuncs.end()) {
                Value defFunc = it->second;
                Value defClosure = Value::closureVal(defFunc);
                auto res = callAndExec(asClosure(defClosure), {});
                if (res.first != InterpretResult::OK)
                    throw std::runtime_error("failed to evaluate default parameter");
                arg = res.second;
            } else {
                arg = Value::nilVal();
            }
        }

        if (params[pi].has_value() && params[pi]->type.has_value()) {
            ValueType vt = builtinToValueType(params[pi]->type.value()->builtin);
            bool strictConv = false;
            if (thread->frames.size() >= 1)
                strictConv = (thread->frames.end()-1)->strict;
            arg = toType(vt, arg, strictConv);
        }
        out[idx++] = arg;
    }
    return idx;
}

bool VM::callNativeFn(NativeFn fn, ptr<type::Type> funcType,
                      const std::vector<Value>& defaults,
                      const CallSpec& callSpec,
                      bool includeReceiver,
                      const Value& receiver,
                      const Value& declFunction)
{
    try {
        if (funcType) {
            size_t paramCount = funcType->func.value().params.size() + (includeReceiver ? 1 : 0);
            constexpr size_t Small = 8;
            Value stackArgs[Small];
            std::vector<Value> heapArgs;
            Value* buf = stackArgs;
            if (paramCount > Small) {
                heapArgs.resize(paramCount);
                buf = heapArgs.data();
            }
            static const std::map<int32_t, Value> emptyDefaults;
            const auto& paramDefaults = declFunction.isNonNil() ? asFunction(declFunction)->paramDefaultFunc : emptyDefaults;
            size_t actual = marshalArgs(funcType, defaults, callSpec, buf, includeReceiver, receiver, paramDefaults);
            ArgsView view{buf, actual};
            Value result { fn(*this, view) };
            *(thread->stackTop - callSpec.argCount - 1) = result;
            popN(callSpec.argCount);
            return true;
        } else {
            Value* base = &(*thread->stackTop) - callSpec.argCount - (includeReceiver ? 1 : 0);
            ArgsView view{base, static_cast<size_t>(callSpec.argCount + (includeReceiver ? 1 : 0))};
            Value result { fn(*this, view) };
            *(thread->stackTop - callSpec.argCount - 1) = result;
            popN(callSpec.argCount);
            return true;
        }
    } catch (std::exception& e) {
        runtimeError(e.what());
        return false;
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




VM::VM()
    : lineMode(false)
    , cacheModeSetting(CacheMode::Normal)
{
    SimpleMarkSweepGC::instance().setVM(this);

    assert(sizeof(Value) == sizeof(uint64_t)); // ensure Value is 64bit

    runtimeErrorFlag = false;
    exitRequested = false;
    exitCodeValue = 0;

    thread = nullptr;
    initString = Value::stringVal(UnicodeString("init"));

    registerBuiltinModule(make_ptr<ModuleMath>());
    registerBuiltinModule(make_ptr<ModuleSys>());
#ifdef ROXAL_ENABLE_FILEIO
    registerBuiltinModule(make_ptr<ModuleFileIO>());
#endif

    // Execute builtin module scripts to attach declarations and docs
    ptr<Thread> initThread = make_ptr<Thread>();
    thread = initThread;
#ifdef ROXAL_ENABLE_FILEIO
    executeBuiltinModuleScript("compiler/fileio.rox", getBuiltinModule(toUnicodeString("fileio")));
#endif
    executeBuiltinModuleScript("compiler/sys.rox", getBuiltinModule(toUnicodeString("sys")));
    executeBuiltinModuleScript("compiler/math.rox", getBuiltinModule(toUnicodeString("math")));

    thread = nullptr;

    // Reset thread ids so the first real VM thread starts at 2
    Thread::resetIdCounter(1);

    // Initialize dataflow engine as builtin actor
    dataflowEngine = df::DataflowEngine::instance();
    auto dataflowType = newObjectTypeObj(toUnicodeString("_DataflowEngine"), true);
    Value dataflowTypeVal { Value::objVal(std::move(dataflowType)) };
    dataflowEngineActor = Value::actorInstanceVal(dataflowTypeVal);
    dataflowEngineThread = make_ptr<Thread>();
    dataflowEngineThread->act(dataflowEngineActor);

    // Start the dataflow engine run loop on its actor thread
    {
        ActorInstance* inst = asActorInstance(dataflowEngineActor);
        CallSpec cs{}; cs.argCount = 0; cs.allPositional = true;
        Value callee { Value::boundNativeVal(dataflowEngineActor, std::mem_fn(&VM::dataflow_run_native), true, nullptr, {}) };
        inst->queueCall(callee, cs, nullptr);
    }

    // Make dataflow engine available as global variable
    globals.storeGlobal(toUnicodeString("_dataflow"), dataflowEngineActor);

    // built-in exception hierarchy
    Value exType = Value::objVal(newObjectTypeObj(toUnicodeString("exception"), false));
    Value runtimeExType = Value::objVal(newObjectTypeObj(toUnicodeString("RuntimeException"), false));
    asObjectType(runtimeExType)->superType = exType;
    Value programExType = Value::objVal(newObjectTypeObj(toUnicodeString("ProgramException"), false));
    asObjectType(programExType)->superType = exType;
    Value condIntType = Value::objVal(newObjectTypeObj(toUnicodeString("ConditionalInterrupt"), false));
    asObjectType(condIntType)->superType = exType;
#ifdef ROXAL_ENABLE_FILEIO
    Value fileIOExceptionTypeVal = Value::objectTypeVal(toUnicodeString("FileIOException"), false);
    asObjectType(fileIOExceptionTypeVal)->superType = runtimeExType;
#endif

    globals.storeGlobal(toUnicodeString("exception"), exType);
    globals.storeGlobal(toUnicodeString("RuntimeException"), runtimeExType);
    globals.storeGlobal(toUnicodeString("ProgramException"), programExType);
    globals.storeGlobal(toUnicodeString("ConditionalInterrupt"), condIntType);
#ifdef ROXAL_ENABLE_FILEIO
    globals.storeGlobal(toUnicodeString("FileIOException"), fileIOExceptionTypeVal);
#endif

    defineBuiltinFunctions();
    defineBuiltinMethods();
    defineBuiltinProperties();
    defineNativeFunctions();

    // Create builtin __conditional_interrupt closure
    {
        Value fn { Value::functionVal(toUnicodeString("__conditional_interrupt"),
                                      toUnicodeString("sys"), toUnicodeString("__internal"), toUnicodeString("internal")) };
        ObjFunction* fnObj = asFunction(fn);
        fnObj->arity = 0;
        fnObj->upvalueCount = 0;

        Value condIntType = globals.load(toUnicodeString("ConditionalInterrupt")).value();
        fnObj->chunk->writeConsant(condIntType, 0, 0);
        fnObj->chunk->write(OpCode::ConstNil, 0, 0);
        CallSpec cs{1};
        auto bytes = cs.toBytes();
        fnObj->chunk->write(OpCode::Call, 0, 0);
        fnObj->chunk->write(bytes[0], 0, 0);
        fnObj->chunk->write(OpCode::Throw, 0, 0);
        fnObj->chunk->write(OpCode::ConstNil, 0, 0);
        fnObj->chunk->write(OpCode::Return, 0, 0);

        conditionalInterruptClosure = Value::closureVal(fn);
        globals.storeGlobal(toUnicodeString("__conditional_interrupt"), conditionalInterruptClosure);
    }

    //CallSpec::testParamPositions();
    //Value::testPrimitiveValues();
    //testObjectValues();
}



void VM::ensureDataflowEngineStopped()
{
    if (dataflowEngine) {
        dataflowEngine->stop();
    } else {
        if (auto engine = df::DataflowEngine::instance(false)) {
            engine->stop();
        }
    }

    if (dataflowEngineThread) {
        dataflowEngineThread->wake();
    }
}



VM::~VM()
{
    SimpleMarkSweepGC::instance().setVM(nullptr);

    for (auto moduleTypeVal : ObjModuleType::allModules.get()) {
        ObjModuleType* moduleType = asModuleType(moduleTypeVal);
        if (moduleType) {
            moduleType->dropReferences();
        }
    }
    ObjModuleType::allModules.clear();

    // Clean up dataflow engine resources before globals cleanup. Signal the
    // engine to exit cooperatively before waiting on the actor thread so the
    // join cannot block inside the run loop.
    ensureDataflowEngineStopped();
    if (dataflowEngineThread) {
        dataflowEngineThread->join();
        dataflowEngineThread.reset();
    }
    dataflowEngineActor = Value::nilVal();  // This will call decRef() via Value destructor

    // join any remaining threads to prevent leak reports
    joinAllThreads();


    globals.forEach([](const VariablesMap::NameValue& nv) {
        auto value = nv.second;
        if (isClosure(value)) {
            auto closure = asClosure(value);
            closure->upvalues.clear();
            asFunction(closure->function)->moduleType = Value::nilVal();
            asFunction(closure->function)->paramDefaultFunc.clear();
        }
        if (isFunction(value)) {
            asFunction(value)->clear();
        }
    });

    globals.clearGlobals();

    initString = Value::nilVal();

    builtinModules.clear();

    conditionalInterruptClosure = Value::nilVal();

    if (dataflowEngine)
        dataflowEngine->clear();


    // Release REPL thread resources before reporting potential leaks
    replThread.reset();

    // Release the main thread before final garbage collection so any
    // objects referenced through its stacks and handlers can be reclaimed
    thread.reset();

    // Flush any reference-counted objects before performing a final tracing
    // collection so we do not enqueue the same object twice.
    freeObjects();

    // With no mutator threads remaining, force a final GC cycle so any
    // objects kept alive only by cycles are discovered before we report
    // leaks under DEBUG_TRACE_MEMORY.
    SimpleMarkSweepGC& shutdownCollector = SimpleMarkSweepGC::instance();
    while (shutdownCollector.collectNowForShutdown() > 0) {
        freeObjects();
    }

    // Final cleanup pass for any objects that became unreferenced during destructor
    freeObjects();

    // ensure all threads are gone before reporting
    joinAllThreads();

    #ifdef DEBUG_TRACE_MEMORY
    // Final attempt to release any objects that might still be pending
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

void VM::setCacheMode(CacheMode mode)
{
    cacheModeSetting = mode;
}

bool VM::cacheReadsEnabled() const
{
    return cacheModeSetting == CacheMode::Normal;
}

bool VM::cacheWritesEnabled() const
{
    return cacheModeSetting != CacheMode::NoCache;
}




InterpretResult VM::interpret(std::istream& source, const std::string& name)
{
    Value function { Value::nilVal() }; // ObjFunction

    runtimeErrorFlag = false;

    try {
        RoxalCompiler compiler {};
        compiler.setOutputBytecodeDisassembly(outputBytecodeDisassembly);
        compiler.setCacheReadEnabled(cacheReadsEnabled());
        compiler.setCacheWriteEnabled(cacheWritesEnabled());
        compiler.setModulePaths(modulePaths);

        std::filesystem::path cacheSourcePath;
        if (!name.empty()) {
            try {
                std::filesystem::path namePath(name);
                if (namePath.has_extension() && namePath.extension() == ".rox")
                    cacheSourcePath = std::filesystem::canonical(std::filesystem::absolute(namePath));
            } catch (...) {
                cacheSourcePath.clear();
            }
        }

        bool loadedFromCache = false;
        if (!cacheSourcePath.empty()) {
            Value cached = compiler.loadFileCache(cacheSourcePath);
            if (cached.isNonNil()) {
                function = cached;
                loadedFromCache = true;
            }
        }

        if (!loadedFromCache) {
            function = compiler.compile(source, name);
            if (!function.isNil() && !cacheSourcePath.empty())
                compiler.storeFileCache(cacheSourcePath, function);
        }

    } catch (std::exception& e) {
        return InterpretResult::CompileError;
    }

    if (function.isNil())
        return InterpretResult::CompileError;

    Value closureValue { Value::closureVal(function) };

    ptr<Thread> firstThread = make_ptr<Thread>();
    threads.store(firstThread->id(), firstThread);

    // go
    firstThread->spawn(closureValue);

    // Transfer execution ownership to the spawned thread. Drop our local strong
    // references so they cannot outlive the running closure and get collected
    // out from underneath us.
    closureValue = Value::nilVal();
    function = Value::nilVal();

    // join all threads (they may spawn additional threads while running)
    InterpretResult joinResult = joinAllThreads();
    InterpretResult result = firstThread->result;

    if (joinResult != InterpretResult::OK || runtimeErrorFlag.load())
        result = InterpretResult::RuntimeError;

    if (exitRequested.load())
        result = InterpretResult::OK;

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


InterpretResult VM::interpretLine(std::istream& linestream, bool replMode)
{
    Value function { Value::nilVal() }; // ObjFunction

    runtimeErrorFlag = false;

    static RoxalCompiler compiler {};
    static Value replModule { Value::nilVal() };
    compiler.setOutputBytecodeDisassembly(outputBytecodeDisassembly);
    compiler.setCacheReadEnabled(cacheReadsEnabled());
    compiler.setCacheWriteEnabled(cacheWritesEnabled());
    compiler.setModulePaths(modulePaths);
    compiler.setReplMode(replMode);

    try {
        function = compiler.compile(linestream, "cli", replModule);

    } catch (std::exception& e) {
        return InterpretResult::CompileError;
    }

    if (function.isNil())
        return InterpretResult::CompileError;

    if (replModule.isNil())
        replModule = asFunction(function)->moduleType.strongRef();

    lineMode = true;
    lineStream = &linestream;
    compiler.setReplMode(false);

    Value closure = Value::closureVal(function); // ObjClosure

    if (!replThread) {
        replThread = make_ptr<Thread>();
    }

    thread = replThread;
    resetStack();

    auto resultPair = callAndExec(asClosure(closure), {});
    InterpretResult result = resultPair.first;
    if (runtimeErrorFlag.load())
        result = InterpretResult::RuntimeError;

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
    bool paramDefaultAndArgsReorderNeeded = !(callSpec.allPositional && callSpec.argCount == asFunction(closure->function)->arity);

    CallFrame callframe {};
    auto argCount = callSpec.argCount;

    if (paramDefaultAndArgsReorderNeeded) {

        assert(asFunction(closure->function)->funcType.has_value());
        ptr<type::Type> calleeType { asFunction(closure->function)->funcType.value() };

        callframe.reorderArgs = callSpec.paramPositions(calleeType, true);

        // handle execution of default param expression 'func' for params not supplied
        if (argCount < asFunction(closure->function)->arity) {
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

                    auto funcIt = asFunction(closure->function)->paramDefaultFunc.find(param.value().nameHashCode);
                    #ifdef DEBUG_BUILD
                    if (funcIt == asFunction(closure->function)->paramDefaultFunc.cend())
                        runtimeError("No default value function found for parameter '"+toUTF8StdString(param.value().name)+"' in function '"+toUTF8StdString(asFunction(closure->function)->name)+"'.");
                    assert(funcIt != asFunction(closure->function)->paramDefaultFunc.cend());
                    #endif

                    Value defValFunc = funcIt->second; // ObjFunction

                    // call it, which will leave the returned default val on the stack as an arg for this call
                    Value defValClosure = Value::closureVal(defValFunc); //  ObjClosure

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

                    if (asClosure(defValClosure)->upvalues.size() > 0) {
                        auto paramName = param.value().name;
                        runtimeError("Captured variables in default parameter '"+toUTF8StdString(paramName)+"' value expressions are not allowed"
                                    +" in declaration of function '"+toUTF8StdString(asFunction(closure->function)->name)+"'.");
                        return false;
                    }

                    call(asClosure(defValClosure),CallSpec(0));
                    defValFrames.push_back(std::make_pair(defValClosure ,*(thread->frames.end()-1)) );
                    thread->popFrame();

                    // push a place-holder (nil) value onto the stack for the value
                    //  (since caller didn't push it before the call)
                    push(Value::nilVal());

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
        else if (argCount > asFunction(closure->function)->arity) {
            runtimeError("Passed "+std::to_string(argCount)+" arguments for function "
                        +toUTF8StdString(asFunction(closure->function)->name)+" which has "
                        +std::to_string(asFunction(closure->function)->arity)+" parameters.");
            return false;
        }
        assert(argCount == asFunction(closure->function)->arity);
    }

    if (thread->frames.size() > 128) {
        runtimeError("Stack overflow.");
        return false;
    }


    callframe.closure = Value::objRef(closure);
    callframe.startIp = callframe.ip = asFunction(closure->function)->chunk->code.begin();
    callframe.slots = &(*(thread->stackTop - argCount - 1));
    callframe.strict = asFunction(closure->function)->strict;
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
    auto argBegin = thread->stackTop - callSpec.argCount;
    auto argEnd = thread->stackTop;
    try {
        if (!callSpec.allPositional) {
            auto ctorType = builtinConstructorType(builtinType);
            if (!ctorType)
                throw std::runtime_error("Named parameters unsupported in constructor for " + to_string(builtinType));
            auto paramPositions = callSpec.paramPositions(ctorType, true);
            std::vector<Value> ordered;
            ordered.reserve(paramPositions.size());
            for (size_t pi = 0; pi < paramPositions.size(); ++pi) {
                int pos = paramPositions[pi];
                if (pos >= 0)
                    ordered.push_back(*(argBegin + pos));
                else
                    ordered.push_back(Value::nilVal());
            }
            *(thread->stackTop - callSpec.argCount - 1) = construct(builtinType, ordered.begin(), ordered.end());
            popN(callSpec.argCount);
            return true;
        }

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

        auto functionObj = asFunction(asClosure(closureVal)->function);
        if (functionObj->funcType.has_value()) {
            auto calleeType = functionObj->funcType.value();
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

        auto baseName = toUTF8StdString(functionObj->name);
        auto name = df::DataflowEngine::uniqueFuncName(baseName);
        ptr<df::FuncNode> node = roxal::make_ptr<df::FuncNode>(name, closureVal, constArgs, sigArgs);
        node->addToEngine();
        auto outputs = node->outputs(); // creates output signals if they don't exist
        dataflowEngine->evaluate(); // Initialize signal values for new node
        popN(callSpec.argCount + 1);
        if (outputs.size() == 1) {
            push(Value::signalVal(outputs[0]));
        } else if (outputs.empty()) {
            push(Value::nilVal());
        } else {
            std::vector<Value> outVals;
            outVals.reserve(outputs.size());
            for(const auto& s : outputs)
                outVals.push_back(Value::signalVal(s));
            push(Value::listVal(outVals));
        }
        return true;
    }

    if (callee.isObj()) {
        switch (objType(callee)) {
            case ObjType::BoundMethod: {
                Value boundValue = callee;
                ObjBoundMethod* boundMethod { asBoundMethod(boundValue) };

                if (!isActorInstance(boundMethod->receiver)) {
                    thread->currentBoundCall = boundValue;
                    BoundCallGuard guard(thread.get());
                    *(thread->stackTop - callSpec.argCount - 1) = boundMethod->receiver;
                    return call(asClosure(boundMethod->method), callSpec);
                }
                else {
                    // call to actor method.
                    //  If the caller is the same actor, treat like regular method call
                    //  otherwise, instead of calling on this thread,
                    //  queue the call for the actor thread to handle

                    ActorInstance* inst = asActorInstance(boundMethod->receiver);

                    if (std::this_thread::get_id() == inst->thread_id) {
                        // actor to this/self method call
                        thread->currentBoundCall = boundValue;
                        BoundCallGuard guard(thread.get());
                        *(thread->stackTop - callSpec.argCount - 1) = boundMethod->receiver; // FIXME: or inst??
                        return call(asClosure(boundMethod->method), callSpec);
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
                    ObjObjectType* tInit = type;
                    const ObjObjectType::Method* initMethod = nullptr;
                    while (tInit != nullptr && initMethod == nullptr) {
                        auto it = tInit->methods.find(asStringObj(initString)->hash);
                        if (it != tInit->methods.end())
                            initMethod = &it->second;
                        else
                            tInit = tInit->superType.isNil() ? nullptr : asObjectType(tInit->superType);
                    }

                    if (initMethod == nullptr && isExceptionType(type) && callSpec.argCount == 1) {
                        Value msg = peek(0);
                        *(thread->stackTop - callSpec.argCount - 1) = Value::exceptionVal(msg, Value::objRef(type));
                        pop();
                        return true;
                    }

                    Value inst {};
                    if (!type->isActor) {
                        inst = Value::objectInstanceVal(callee);
                        *(thread->stackTop - callSpec.argCount - 1) = inst;
                    }
                    else {
                        inst = Value::actorInstanceVal(callee);

                        // spawn Thread to handle actor method calls
                        ptr<Thread> newThread = make_ptr<Thread>();
                        threads.store(newThread->id(), newThread);
                        newThread->act(inst);

                        *(thread->stackTop - callSpec.argCount - 1) = inst;
                    }
                    bool dictArg = (!type->isActor && callSpec.argCount == 1 && isDict(peek(0)));
                    bool initAcceptsDict = false;

                    auto initClosureObj {initMethod != nullptr ? asClosure(initMethod->closure) : nullptr };
                    auto initFuncObj { initClosureObj != nullptr ? asFunction(initClosureObj->function) : nullptr };

                    if (initFuncObj != nullptr && initFuncObj->funcType.has_value()) {
                        auto ftype = initFuncObj->funcType.value();
                        if (ftype->builtin == type::BuiltinType::Func) {
                            const auto& params = ftype->func.value().params;
                            if (params.size() == 1) {
                                if (!params[0].has_value() || !params[0]->type.has_value())
                                    initAcceptsDict = true;
                                else if (builtinToValueType(params[0]->type.value()->builtin) == ValueType::Dict)
                                    initAcceptsDict = true;
                            }
                        }
                    }

                    if (initClosureObj != nullptr && !(dictArg && !initAcceptsDict)) {
                        if (!type->isActor) {
                            bool isNative = initFuncObj != nullptr && initFuncObj->nativeImpl;
                            Value calleeVal;
                            if (isNative) {
                                NativeFn fn = initFuncObj->nativeImpl;
                                calleeVal = Value::boundNativeVal(inst, fn,
                                                                  initFuncObj->funcType.has_value() &&
                                                                     initFuncObj->funcType.value()->func.has_value() ?
                                                                     initFuncObj->funcType.value()->func->isProc : false,
                                                                  initFuncObj->funcType.has_value() ?
                                                                     initFuncObj->funcType.value() : nullptr,
                                                                  initFuncObj->nativeDefaults,
                                                                  Value::objRef(initFuncObj));
                            } else {
                                calleeVal = Value::boundMethodVal(inst, initMethod->closure);
                            }
                            *(thread->stackTop - callSpec.argCount - 1) = calleeVal;
                            bool ok = callValue(calleeVal, callSpec);
                            if (isNative)
                                *(thread->stackTop - 1) = inst; // native init returns instance
                            return ok;
                        } else {
                            auto boundInit = newBoundMethodObj(inst, initMethod->closure);
                            Value calleeVal = Value::objVal(std::move(boundInit));
                            ActorInstance* actorInst = asActorInstance(inst);
                            actorInst->queueCall(calleeVal, callSpec, &(*thread->stackTop));
                            popN(callSpec.argCount); // remove init args
                        }
                    } else {
                        if (dictArg) {
                            ObjDict* argDict = asDict(peek(0));
                            ObjectInstance* objInst = asObjectInstance(inst);
                            bool strictConv = false;
                            if (thread->frames.size() >= 1)
                                strictConv = (thread->frames.end()-1)->strict;
                            for(const auto& kv : argDict->items()) {
                                if (!isString(kv.first))
                                    continue;
                                int32_t hash = asStringObj(kv.first)->hash;
                                auto pit = type->properties.find(hash);
                                if (pit == type->properties.end())
                                    continue;
                                const auto& prop { pit->second };
                                if (prop.access != ast::Access::Public)
                                    continue;
                                Value val { kv.second };
                                if (!prop.type.isNil() && isTypeSpec(prop.type)) {
                                    ObjTypeSpec* ts = asTypeSpec(prop.type);
                                    if (ts->typeValue != ValueType::Nil) {
                                        try {
                                            val = toType(ts->typeValue, val, strictConv);
                                        } catch (std::exception& e) {
                                            runtimeError(e.what());
                                            return false;
                                        }
                                    }
                                }
                                objInst->properties[hash] = val;
                            }
                            pop();
                        } else if (callSpec.argCount != 0) {
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

                    Value value { Value::nilVal() };

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
                            auto hash = asStringObj(arg)->hash;
                            auto it = type->enumLabelValues.find(hash);
                            if (it == type->enumLabelValues.end() || it->second.first != asStringObj(arg)->s) {
                                runtimeError("enum type '"+toUTF8StdString(type->name)+"' has no label '"+toUTF8StdString(asStringObj(arg)->s)+"'");
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
                else if (ts->typeValue == ValueType::Signal) {
                    return call(ValueType::Signal, callSpec);
                }
                else {
                    throw std::runtime_error("unimplemented construction for type '"+to_string(ts->typeValue)+"'");
                }
            }
            case ObjType::Closure: {
                ObjClosure* closure = asClosure(callee);
                ObjFunction* function = asFunction(closure->function);
                if (function->nativeImpl) {
                    NativeFn native = function->nativeImpl;
                    ptr<type::Type> funcType = function->funcType.has_value()
                        ? function->funcType.value() : nullptr;
                    return callNativeFn(native, funcType,
                                        function->nativeDefaults, callSpec,
                                        false, Value::nilVal(), closure->function);
                } else {
                    bool cfunc = false;
                    for(const auto& annot : function->annotations) {
                        if (annot->name == "cfunc") { cfunc = true; break; }
                    }
                    if (cfunc) {
                        try {
                            Value result { roxal::callCFunc(closure, callSpec, &*(thread->stackTop - callSpec.argCount)) };
                            *(thread->stackTop - callSpec.argCount - 1) = result;
                            popN(callSpec.argCount);
                            return true;
                        } catch (std::exception& e) {
                            runtimeError(e.what());
                            return false;
                        }
                    }
                    return call(closure, callSpec);
                }
            }
            case ObjType::Native: {
                ObjNative* nativeObj = asNative(callee);
                NativeFn native = nativeObj->function;
                return callNativeFn(native, nativeObj->funcType,
                                    nativeObj->defaultValues, callSpec,
                                    false, Value::nilVal(), Value::nilVal());
            }
            case ObjType::BoundNative: {
                Value boundValue = callee;
                ObjBoundNative* bound { asBoundNative(boundValue) };

                if (!isActorInstance(bound->receiver)) {
                    thread->currentBoundCall = boundValue;
                    BoundCallGuard guard(thread.get());
                    *(thread->stackTop - callSpec.argCount - 1) = bound->receiver;
                    NativeFn native = bound->function;
                    return callNativeFn(native, bound->funcType,
                                        bound->defaultValues, callSpec,
                                        true, bound->receiver,
                                        bound->declFunction);
                }
                else {
                    // call to actor native method.
                    //  If the caller is the same actor, treat like regular method call
                    //  otherwise, instead of calling on this thread,
                    //  queue the call for the actor thread to handle

                    ActorInstance* inst = asActorInstance(bound->receiver);

                    if (std::this_thread::get_id() == inst->thread_id) {
                        // actor to this/self native method call
                        thread->currentBoundCall = boundValue;
                        BoundCallGuard guard(thread.get());
                        *(thread->stackTop - callSpec.argCount - 1) = bound->receiver;
                        NativeFn native = bound->function;
                        return callNativeFn(native, bound->funcType,
                                            bound->defaultValues, callSpec,
                                            true, bound->receiver,
                                            bound->declFunction);
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
    push(Value::objRef(closure));
    for(const auto& a : args)
        push(a);
    CallSpec spec(args.size());
    if(!call(closure, spec))
        return { InterpretResult::RuntimeError, Value::nilVal() };

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

        return invokeFromType(asObjectType(instance->instanceType), name, callSpec);
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
        ObjObjectType* type = asObjectType(instance->instanceType);
        auto methodIt = type->methods.find(name->hash);
        if (methodIt != type->methods.end()) {
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
                    if (methodInfo.funcType) {
                        return callNativeFn(fn, methodInfo.funcType,
                                            methodInfo.defaultValues, callSpec,
                                            true, receiver, Value::nilVal());
                    } else {
                        return callNativeFn(fn, nullptr, {}, callSpec,
                                            true, receiver, Value::nilVal());
                    }
                } else {
                    // Different thread - queue the call
                    Value callee = Value::boundNativeVal(receiver, fn, methodInfo.isProc,
                                                         methodInfo.funcType, methodInfo.defaultValues);
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
                    if (methodInfo.funcType) {
                        return callNativeFn(fn, methodInfo.funcType,
                                            methodInfo.defaultValues, callSpec,
                                            true, receiver, Value::nilVal());
                    } else {
                        return callNativeFn(fn, nullptr, {}, callSpec,
                                            true, receiver, Value::nilVal());
                    }
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
                ObjString* str = asStringObj(indexable);
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
                        push(Value::signalVal(newSig));
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

    if (isClosure(method) && asFunction(asClosure(method)->function)->nativeImpl) {
        ObjClosure* cl = asClosure(method);
        ObjFunction* func = asFunction(cl->function);
        NativeFn fn = func->nativeImpl;
        Value boundNative { Value::boundNativeVal(peek(0), fn,
                                                  func->funcType.has_value() &&
                                                      func->funcType.value()->func.has_value() ?
                                                      func->funcType.value()->func->isProc : false,
                                                  func->funcType.has_value() ?
                                                      func->funcType.value() : nullptr,
                                                  func->nativeDefaults,
                                                  cl->function) };
        pop();
        push(boundNative);
    } else {
        Value boundMethod { Value::boundMethodVal(peek(0), method) };
        pop();
        push(boundMethod);
    }

    return BindResult::Bound;
}



Value VM::captureUpvalue(Value& local)
{
    auto& openUpvalues = thread->openUpvalues;
    auto begin = openUpvalues.begin();
    auto end = openUpvalues.end();
    auto it { begin };
    while ((it != end) && (asUpvalue(*it)->location > &local)) {
        ++it;
    }

    if (it != end && asUpvalue(*it)->location == &local)
        return *it;

    Value createdUpvalue { Value::upvalueVal(&local) };

    openUpvalues.insert(it, createdUpvalue);

    // TODO: add debug/test code to ensure openUpvalues are decreasing stack order

    return createdUpvalue;
}


void VM::closeUpvalues(Value* last)
{
    auto& openUpvalues = thread->openUpvalues;
    while (!openUpvalues.empty() && (asUpvalue(openUpvalues.front())->location >= last)) {
        Value upvalue = openUpvalues.front();
        ObjUpvalue* upvalueObj = asUpvalue(upvalue);
        upvalueObj->closed = *upvalueObj->location;
        upvalueObj->location = &upvalueObj->closed;
        openUpvalues.pop_front();
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
        ObjFunction* fn = asFunction(asClosure(it->closure)->function);
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
                propertyInitial = toType(propertyType, propertyInitial, /*strict=*/false);
        }
    }

    ast::Access access = (!accessVal.isNil() && accessVal.isBool() && accessVal.asBool()) ? ast::Access::Private : ast::Access::Public;
    objType->properties[name->hash] = {name->s, propertyType, propertyInitial,
                                      access, Value::objRef(objType).weakRef()};
    objType->propertyOrder.push_back(name->hash);

    // check module annotations for ctype
    if (!thread->frames.empty()) {
        auto frame = thread->frames.end()-1;
        ObjModuleType* mod = asModuleType(asFunction(asClosure(frame->closure)->function)->moduleType);
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
    ObjFunction* function = asFunction(closure->function);
    function->ownerType = Value::objRef(type).weakRef();

    type->methods[name->hash] = {name->s, method, function->access,
                                 Value::objRef(type).weakRef()};
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




void VM::defineNative(const std::string& name, NativeFn function,
                      ptr<type::Type> funcType,
                      std::vector<Value> defaults)
{
    UnicodeString uname { toUnicodeString(name) };
    Value funcVal { Value::nativeVal(function, nullptr, funcType, defaults) };
    globals.storeGlobal(uname,funcVal);
}

void VM::wakeAllThreadsForGC()
{
    threads.unsafeApply([](const auto& registered) {
        for (const auto& entry : registered) {
            if (entry.second) {
                entry.second->wake();
            }
        }
    });

    if (replThread) {
        replThread->wake();
    }

    if (dataflowEngineThread) {
        dataflowEngineThread->wake();
    }

    if (thread) {
        thread->wake();
    }
}


void VM::requestObjectCleanup()
{
    objectCleanupPending.store(true, std::memory_order_release);
}

bool VM::consumePendingObjectCleanup()
{
    return objectCleanupPending.exchange(false, std::memory_order_acq_rel);
}

bool VM::isObjectCleanupPending() const
{
    return objectCleanupPending.load(std::memory_order_acquire);
}


std::pair<InterpretResult,Value> VM::execute()
{
    if (thread->frames.empty() ||
        asFunction(asClosure(thread->frames.back().closure)->function)->chunk->code.size() == 0)
        return std::make_pair(InterpretResult::OK, Value::nilVal()); // nothing to execute

    SimpleMarkSweepGC& valueGC = SimpleMarkSweepGC::instance();
    valueGC.onThreadEnter();
    struct ThreadExecutionGuard {
        SimpleMarkSweepGC& gc;
        ~ThreadExecutionGuard() { gc.onThreadExit(); }
    } executionGuard{valueGC};

    if (valueGC.isCollectionRequested()) {
        valueGC.safepoint(*thread);
    }

    // Track execution depth for nested calls
    thread->execute_depth++;
    size_t frame_depth_on_entry = thread->frames.size();

    auto frame { thread->frames.end()-1 };

    uint8_t instructionByte {};
    OpCode instruction {};

    // does the next instruction OpCode expect 2 bytes or 1 byte for it's argument in the Chunk?
    //  (used by read* lambdas below)
    bool singleByteArg = true;

    auto readByte = [&]() -> uint8_t {
        #ifdef DEBUG_BUILD
            if (frame->ip == asFunction(asClosure(frame->closure)->function)->chunk->code.end())
                throw std::runtime_error("Invalid IP");
        #endif
        return *frame->ip++;
    };

    auto readShort = [&]() -> uint16_t {
        #ifdef DEBUG_BUILD
            if (frame->ip == asFunction(asClosure(frame->closure)->function)->chunk->code.end())
                throw std::runtime_error("Invalid IP");
        #endif
        frame->ip += 2;
        return (frame->ip[-2] << 8) | frame->ip[-1];
    };

    auto readConstant = [&]() -> Value {
        #ifdef DEBUG_BUILD
            auto index { Chunk::size_type(singleByteArg ? readByte() : (readByte() << 8) + readByte()) };
            auto constantsSize = asFunction(asClosure(frame->closure)->function)->chunk->constants.size();
            if (index >= constantsSize)
                throw std::runtime_error("Chunk instruction read constant invalid index ("+std::to_string(index)+") into constants table (size "+std::to_string(constantsSize)+") for instruction "+std::to_string(instructionByte)+(singleByteArg?"":" (2 byte arg)"));
            return asFunction(asClosure(frame->closure)->function)->chunk->constants.at(index);
        #else
            return asFunction(asClosure(frame->closure)->function)->chunk->constants[Chunk::size_type(singleByteArg ? readByte() : (readByte() << 8) + readByte())];
        #endif
    };

    auto readString = [&]() -> ObjString* {
        #ifdef DEBUG_BUILD
        auto constant { readConstant() };
        debug_assert_msg(isString(constant), (std::string("Chunk instruction read string expected a string constant, got ")+constant.typeName()).c_str());
        return asStringObj(constant);
        #else
          return asStringObj(readConstant());
        #endif
    };


    auto binaryOp = [&](std::function<Value(Value, Value)> op) {
        Value rhs = pop();
        Value lhs = pop();
        push( op(lhs,rhs) );
    };

    auto unwindFrame = [&]() {
        auto f = thread->frames.back();
        closeUpvalues(f.slots);
        size_t popCount = &(*thread->stackTop) - f.slots;
        for(size_t i=0;i<popCount;i++) pop();
        thread->popFrame();
    };


    #if defined(DEBUG_TRACE_EXECUTION)
    std::cout << std::endl << "== executing ==" << std::endl;
    #endif

    auto errorReturn = std::make_pair(InterpretResult::RuntimeError,Value::nilVal());


    //
    //  main dispatch loop

    for(;;) {

        if (runtimeErrorFlag.load())
            return errorReturn;
        if (exitRequested.load())
            return std::make_pair(InterpretResult::OK,Value::nilVal());

        // if we're 'sleeping' don't execute any instructions
        //  (we may have been woken up by an event or a spurious wakeup, in which case we'll re-block below)
        if (thread->threadSleep)
           goto postInstructionDispatch;


        #if defined(DEBUG_TRACE_EXECUTION)
            // output stack
            thread->outputStack();
            if (frame->ip != asFunction(asClosure(frame->closure)->function)->chunk->code.end()) {
                // and instruction
                asFunction(asClosure(frame->closure)->function)->chunk->disassembleInstruction(
                    frame->ip - asFunction(asClosure(frame->closure)->function)->chunk->code.begin());
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
                int16_t argIndex = asFunction(asClosure(frame->closure)->function)->arity - frame->tailArgValues.size();
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
            if (asFunction(asClosure(frame->closure)->function)->funcType.has_value()) {
                const auto& params = asFunction(asClosure(frame->closure)->function)->funcType.value()->func.value().params;
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
                            return std::make_pair(InterpretResult::RuntimeError,Value::nilVal());
                        }
                    }
                }
            }


        }


        // Fetch the next instruction OpCode from the Chunk
        //  If it has the DoubleByteArg flag set, clear it and note the OpCode
        //  expects two bytes for its 'argument'
        singleByteArg = true; // common case
        instructionByte = readByte();
        if ((instructionByte & DoubleByteArg) == 0)
            instruction = OpCode(instructionByte);
        else {
            instruction = OpCode(instructionByte & ~DoubleByteArg);
            singleByteArg = false; // expects 2 bytes of argument
        }

        thread->frameStart = false;

        // TODO: consider if using gcc/clang extension will help performance:
        //   https://stackoverflow.com/questions/8019849/labels-as-values-vs-switch-statement
        switch(instruction) {
            case OpCode::Constant: {
                Value constant = readConstant();
                push(constant);
                break;
            }
            case OpCode::ConstTrue: {
                push(Value::trueVal());
                break;
            }
            case OpCode::ConstFalse: {
                push(Value::falseVal());
                break;
            }
            case OpCode::ConstInt0: {
                push(Value::intVal(0));
                break;
            }
            case OpCode::ConstInt1: {
                push(Value::intVal(1));
                break;
            }
            case OpCode::GetProp: {
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
                            Value bm { Value::boundNativeVal(inst, methodInfo.function, methodInfo.isProc,
                                                             methodInfo.funcType, methodInfo.defaultValues) };
                            pop();
                            push(bm);
                            break;
                        }
                    }

                    runtimeError("Undefined property '"+toUTF8StdString(name->s)+"' for signal.");
                    return errorReturn;
                }

                inst.resolve();
                if (isDict(inst)) {
                    ObjDict* dict = asDict(inst);
                    Value key { Value::objRef(name) };
                    bool hasKey = false;
                    try {
                        hasKey = dict->contains(key);
                    } catch (std::exception&) {
                        hasKey = false;
                    }
                    if (hasKey) {
                        Value result {};
                        try {
                            result = dict->at(key);
                        } catch (std::exception&) {
                            runtimeError("KeyError: key '" + toString(key) + "' not found in dict.");
                            return errorReturn;
                        }
                        pop();
                        push(result);
                        break;
                    } else {
                        runtimeError("KeyError: key '" + toString(key) + "' not found in dict.");
                        return errorReturn;
                    }
                } else if (isObjectInstance(inst)) {
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
                        auto br = bindMethod(asObjectType(objInst->instanceType), name);
                        if (br == BindResult::Bound)
                            break;
                        if (br == BindResult::Private)
                            return errorReturn;

                        runtimeError("Undefined method or property '"+toUTF8StdString(name->s)+"' for instance type '"+toUTF8StdString(asObjectType(objInst->instanceType)->name)+"'.");
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
                        auto br = bindMethod(asObjectType(actorInst->instanceType), name);
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
                                Value boundNative { Value::boundNativeVal(inst, fn, methodInfo.isProc,
                                                                          methodInfo.funcType, methodInfo.defaultValues) };
                                pop();
                                push(boundNative);
                                break;
                            }
                        }

                        runtimeError("Undefined method or property '"+toUTF8StdString(name->s)+"' for instance type '"+toUTF8StdString(asObjectType(actorInst->instanceType)->name)+"'.");
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
                        runtimeError("Undefined variable '"+name->toStdString()+"'");
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
                            Value bm { Value::boundNativeVal(inst, methodInfo.function, methodInfo.isProc,
                                                             methodInfo.funcType, methodInfo.defaultValues) };
                            pop();
                            push(bm);
                            break;
                        }
                    }
                    // Check builtin properties
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
                }

                runtimeError("Only object and actor instances have methods and only object, actor, and dictionary instances have properties (string keys only).");
                return errorReturn;
                break;
            }
            case OpCode::GetPropCheck: {
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
                            Value bm { Value::boundNativeVal(inst, methodInfo.function, methodInfo.isProc,
                                                             methodInfo.funcType, methodInfo.defaultValues) };
                            pop();
                            push(bm);
                            break;
                        }
                    }

                    runtimeError("Undefined property '"+toUTF8StdString(name->s)+"' for signal.");
                    return errorReturn;
                }

                inst.resolve();
                if (isDict(inst)) {
                    ObjDict* dict = asDict(inst);
                    Value key { Value::objRef(name) };
                    bool hasKey = false;
                    try {
                        hasKey = dict->contains(key);
                    } catch (std::exception&) {
                        hasKey = false;
                    }
                    if (hasKey) {
                        Value result {};
                        try {
                            result = dict->at(key);
                        } catch (std::exception&) {
                            runtimeError("KeyError: key '" + toString(key) + "' not found in dict.");
                            return errorReturn;
                        }
                        pop();
                        push(result);
                        break;
                    } else {
                        runtimeError("KeyError: key '" + toString(key) + "' not found in dict.");
                        return errorReturn;
                    }
                } else if (isObjectInstance(inst)) {
                    ObjectInstance* objInst = asObjectInstance(inst);
                    ObjObjectType* t = asObjectType(objInst->instanceType);
                    auto it = objInst->properties.find(name->hash);
                    if (it != objInst->properties.end()) {
                        auto pit = t->properties.find(name->hash);
                        ast::Access propAccess = ast::Access::Public;
                        Value ownerT = objInst->instanceType.weakRef();
                        if (pit != t->properties.end()) {
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
                        auto br = bindMethod(t, name);
                        if (br == BindResult::Bound)
                            break;
                        if (br == BindResult::Private)
                            return errorReturn;

                        runtimeError("Undefined method or property '"+toUTF8StdString(name->s)+"' for instance type '"+toUTF8StdString(t->name)+"'.");
                        return errorReturn;
                    }
                } else if (isActorInstance(inst)) {
                    ActorInstance* actorInst = asActorInstance(inst);
                    ObjObjectType* t = asObjectType(actorInst->instanceType);
                    auto pit = t->properties.find(name->hash);
                    auto it = actorInst->properties.find(name->hash);
                    if (it != actorInst->properties.end()) {
                        ast::Access propAccess = ast::Access::Public;
                        Value ownerT = actorInst->instanceType.weakRef();
                        if (pit != t->properties.end()) {
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
                        auto br = bindMethod(t, name);
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
                                Value boundNative { Value::boundNativeVal(inst, fn, methodInfo.isProc,
                                                                          methodInfo.funcType, methodInfo.defaultValues) };
                                pop();
                                push(boundNative);
                                break;
                            }
                        }

                        runtimeError("Undefined method or property '"+toUTF8StdString(name->s)+"' for instance type '"+toUTF8StdString(t->name)+"'.");
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
                        runtimeError("Undefined variable '"+name->toStdString()+"'");
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
                            Value bm { Value::boundNativeVal(inst, methodInfo.function, methodInfo.isProc,
                                                             methodInfo.funcType, methodInfo.defaultValues) };
                            pop();
                            push(bm);
                            break;
                        }
                    }
                    // Check builtin properties
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
                }

                runtimeError("Only object and actor instances have methods and only object, actor, and dictionary instances have properties (string keys only).");
                return errorReturn;
                break;
            }
            case OpCode::SetProp: {
                Value& inst { peek(1) };
                ObjString* name = readString();

                if (isSignal(inst)) {
                    auto vt = inst.type();
                    auto pit = builtinProperties.find(vt);
                    if (pit != builtinProperties.end()) {
                        auto propIt = pit->second.find(name->hash);
                        if (propIt != pit->second.end()) {
                            const BuiltinPropertyInfo& propInfo = propIt->second;
                            if (!propInfo.setter) {
                                runtimeError("Cannot assign to read-only property '" + toUTF8StdString(name->s) + "'");
                                return errorReturn;
                            }
                            Value value { peek(0) };
                            (this->*(propInfo.setter))(inst, value);
                            popN(2);
                            push(value);
                            break;
                        }
                    }

                    runtimeError("Undefined property '"+toUTF8StdString(name->s)+"' for signal.");
                    return errorReturn;
                }

                inst.resolve();
                if (isDict(inst)) {
                    ObjDict* dict = asDict(inst);
                    Value value { peek(0) };
                    Value key { Value::objRef(name) };
                    dict->store(key, value);
                    popN(2);
                    push(value);
                    break;
                } else if (isObjectInstance(inst)) {
                    ObjectInstance* objInst = asObjectInstance(inst);
                    //std::cout << "setting prop " << toUTF8StdString(name->s) << " of " << toString(inst) << " to " << toString(peek(0)) << std::endl;

                    Value value { peek(0) };

                    if (!value.isNil()) {
                        bool strictConv = asFunction(asClosure(frame->closure)->function)->strict;
                        // if type object specified the property type in the declaration,
                        //  convert the value to that type (if possible)
                        ObjObjectType* t = asObjectType(objInst->instanceType);
                        const auto& properties { t->properties };
                        const auto& property = properties.find(name->hash);
                        if (property != properties.end()) {
                            const auto& prop { property->second };
                            if (!prop.type.isNil() && isTypeSpec(prop.type)) {
                                ObjTypeSpec* typeSpec = asTypeSpec(prop.type);
                                if (typeSpec->typeValue != ValueType::Nil)
                                    try {
                                        // TODO: implement & use a canConvertToType()
                                        value = toType(prop.type, value, strictConv);
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

                    Value value { peek(0) };

                    if (!value.isNil()) {
                        bool strictConv = asFunction(asClosure(frame->closure)->function)->strict;
                        ObjObjectType* tA = asObjectType(actorInst->instanceType);
                        const auto& properties { tA->properties };
                        const auto& property = properties.find(name->hash);
                        if (property != properties.end()) {
                            const auto& prop { property->second };
                            if (!prop.type.isNil() && isTypeSpec(prop.type)) {
                                ObjTypeSpec* typeSpec = asTypeSpec(prop.type);
                                if (typeSpec->typeValue != ValueType::Nil)
                                    try {
                                        value = toType(prop.type, value, strictConv);
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
                runtimeError("Only object, actor, and dictionary instances have properties (string keys only).");
                return errorReturn;
                break;
            }
            case OpCode::SetPropCheck: {
                Value& inst { peek(1) };
                ObjString* name = readString();
                if (isSignal(inst)) {
                    auto vt = inst.type();
                    auto pit = builtinProperties.find(vt);
                    if (pit != builtinProperties.end()) {
                        auto propIt = pit->second.find(name->hash);
                        if (propIt != pit->second.end()) {
                            const BuiltinPropertyInfo& propInfo = propIt->second;
                            if (!propInfo.setter) {
                                runtimeError("Cannot assign to read-only property '" + toUTF8StdString(name->s) + "'");
                                return errorReturn;
                            }
                            Value value { peek(0) };
                            (this->*(propInfo.setter))(inst, value);
                            popN(2);
                            push(value);
                            break;
                        }
                    }

                    runtimeError("Undefined property '"+toUTF8StdString(name->s)+"' for signal.");
                    return errorReturn;
                }

                inst.resolve();
                if (isDict(inst)) {
                    ObjDict* dict = asDict(inst);

                    Value value { peek(0) };

                    dict->store(Value::objRef(name), value);
                    popN(2);
                    push(value);
                    break;
                } else if (isObjectInstance(inst)) {
                    ObjectInstance* objInst = asObjectInstance(inst);
                    ObjObjectType* t = asObjectType(objInst->instanceType);

                    Value value { peek(0) };

                    if (!value.isNil()) {
                        bool strictConv = asFunction(asClosure(frame->closure)->function)->strict;
                        const auto& properties { t->properties };
                        const auto& property = properties.find(name->hash);
                        if (property != properties.end()) {
                            const auto& prop { property->second };
                            if (!prop.type.isNil() && isTypeSpec(prop.type)) {
                                ObjTypeSpec* typeSpec = asTypeSpec(prop.type);
                                if (typeSpec->typeValue != ValueType::Nil)
                                    try {
                                        value = toType(prop.type, value, strictConv);
                                    } catch(std::exception& e) {
                                        runtimeError(e.what());
                                        return errorReturn;
                                    }
                            }
                        }
                    }

                    auto pit = t->properties.find(name->hash);
                    ast::Access propAccess = ast::Access::Public;
                    Value ownerT = objInst->instanceType.weakRef();
                    if (pit != t->properties.end()) {
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
                    ObjObjectType* tA = asObjectType(actorInst->instanceType);

                    Value value { peek(0) };

                    if (!value.isNil()) {
                        bool strictConv = asFunction(asClosure(frame->closure)->function)->strict;
                        const auto& properties { tA->properties };
                        const auto& property = properties.find(name->hash);
                        if (property != properties.end()) {
                            const auto& prop { property->second };
                            if (!prop.type.isNil() && isTypeSpec(prop.type)) {
                                ObjTypeSpec* typeSpec = asTypeSpec(prop.type);
                                if (typeSpec->typeValue != ValueType::Nil)
                                    try {
                                        value = toType(prop.type, value, strictConv);
                                    } catch(std::exception& e) {
                                        runtimeError(e.what());
                                        return errorReturn;
                                    }
                            }
                        }
                    }

                    auto pit = tA->properties.find(name->hash);
                    ast::Access propAccess = ast::Access::Public;
                    Value ownerT = actorInst->instanceType.weakRef();
                    if (pit != tA->properties.end()) {
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
                runtimeError("Only object, actor, and dictionary instances have properties (string keys only).");
                return errorReturn;
                break;
            }
            case OpCode::GetSuper: {
                ObjString* name = readString();
                #ifdef DEBUG_BUILD
                if (!isTypeSpec(peek(0)) && !isObjectType(peek(0)))
                    throw std::runtime_error("super doesn't reference an object or actor type.");
                #endif

                ObjObjectType* superType = asObjectType(pop());
                auto br = bindMethod(superType,name);
                if (br != BindResult::Bound)
                    return std::make_pair(InterpretResult::RuntimeError,Value::nilVal());

                break;
            }
            case OpCode::Equal: {
                peek(0).resolveFuture();
                peek(1).resolveFuture();

                try {
                    binaryOp([&](Value a, Value b) -> Value { return equal(a, b, frame->strict); });
                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return errorReturn;
                }
                break;
            }
            case OpCode::Is: {
                Value b = pop();
                Value a = pop();
                a.resolve();
                b.resolve();
                push(Value::boolVal(a.is(b, frame->strict)));
                break;
            }
            case OpCode::Greater: {
                peek(0).resolveFuture();
                peek(1).resolveFuture();

                try {
                    binaryOp([](Value a, Value b) -> Value { return greater(a,b); });
                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return errorReturn;
                }
                break;
            }
            case OpCode::Less: {
                peek(0).resolveFuture();
                peek(1).resolveFuture();

                try {
                    binaryOp([](Value a, Value b) -> Value { return less(a,b); });
                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return errorReturn;
                }
                break;
            }
            case OpCode::Add: {
                peek(0).resolveFuture();
                peek(1).resolveFuture();

                if (isString(peek(1))) {
                    concatenate();
                } else {
                    try {
                        binaryOp([](Value l, Value r) -> Value { return add(l, r); });
                    } catch (std::exception& e) {
                        runtimeError(e.what());
                        return errorReturn;
                    }
                }
                break;
            }
            case OpCode::Subtract: {
                peek(0).resolveFuture();
                peek(1).resolveFuture();

                try {
                    binaryOp([](Value l, Value r) -> Value { return subtract(l, r); });
                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return errorReturn;
                }
                break;
            }
            case OpCode::Multiply: {
                peek(0).resolveFuture();
                peek(1).resolveFuture();

                try {
                    binaryOp([](Value l, Value r) -> Value { return multiply(l, r); });
                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return errorReturn;
                }
                break;
            }
            case OpCode::Divide: {
                peek(0).resolveFuture();
                peek(1).resolveFuture();

                try {
                    binaryOp([](Value l, Value r) -> Value { return divide(l, r); });
                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return errorReturn;
                }
                break;
            }
            case OpCode::Negate: {
                Value& operand { peek(0) };
                operand.resolveFuture();

                try {
                    push(negate(pop()));
                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return errorReturn;
                }
                break;
            }
            case OpCode::Modulo: {
                // TODO: support decimal
                peek(0).resolveFuture();
                peek(1).resolveFuture();

                try {
                    binaryOp([](Value a, Value b) -> Value { return mod(a,b); });
                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return errorReturn;
                }
                break;
            }
            case OpCode::And: {
                peek(0).resolveFuture();
                peek(1).resolveFuture();
                if (!peek(0).isBool() && !isSignal(peek(0))) {
                    runtimeError("Operand of 'and' must be a bool");
                    return errorReturn;
                }
                if (!peek(1).isBool() && !isSignal(peek(1))) {
                    runtimeError("Operand of 'and' must be a bool");
                    return errorReturn;
                }
                try {
                    binaryOp([](Value a, Value b) -> Value { return land(a,b); });
                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return errorReturn;
                }
                break;
            }
            case OpCode::Or: {
                peek(0).resolveFuture();
                peek(1).resolveFuture();
                if (!peek(0).isBool() && !isSignal(peek(0))) {
                    runtimeError("Operand of 'or' must be a bool");
                    return errorReturn;
                }
                if (!peek(1).isBool() && !isSignal(peek(1))) {
                    runtimeError("Operand of 'or' must be a bool");
                    return errorReturn;
                }
                try {
                    binaryOp([](Value a, Value b) -> Value { return lor(a,b); });
                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return errorReturn;
                }
                break;
            }
            case OpCode::BitAnd: {
                peek(0).resolveFuture();
                peek(1).resolveFuture();
                try {
                    binaryOp([](Value a, Value b) -> Value { return band(a,b); });
                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return errorReturn;
                }
                break;
            }
            case OpCode::BitOr: {
                peek(0).resolveFuture();
                peek(1).resolveFuture();
                try {
                    binaryOp([](Value a, Value b) -> Value { return bor(a,b); });
                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return errorReturn;
                }
                break;
            }
            case OpCode::BitXor: {
                peek(0).resolveFuture();
                peek(1).resolveFuture();
                try {
                    binaryOp([](Value a, Value b) -> Value { return bxor(a,b); });
                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return errorReturn;
                }
                break;
            }
            case OpCode::BitNot: {
                Value& operand { peek(0) };
                operand.resolveFuture();
                try {
                    push(bnot(pop()));
                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return errorReturn;
                }
                break;
            }
            case OpCode::Pop: {
                pop();
                break;
            }
            case OpCode::PopN: {
                uint8_t count = readByte();
                for(auto i=0; i<count; i++)
                    pop();
                break;
            }
            case OpCode::Dup: {
                auto value = peek(0);
                push(value);
                break;
            }
            case OpCode::DupBelow: {
                auto value = peek(1);
                push(value);
                break;
            }
            case OpCode::Swap: {
                std::swap(peek(0), peek(1));
                break;
            }
            case OpCode::CopyInto: {
                Value rhs = pop();
                Value lhs = pop();
                try {
                    copyInto(lhs, rhs);
                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return errorReturn;
                }
                push(lhs);
                break;
            }
            case OpCode::JumpIfFalse: {
                uint16_t jumpDist = readShort();
                peek(0).resolve();
                if (isFalsey(peek(0)))
                    frame->ip += jumpDist;
                break;
            }
            case OpCode::JumpIfTrue: {
                uint16_t jumpDist = readShort();
                peek(0).resolve();
                if (isTruthy(peek(0)))
                    frame->ip += jumpDist;
                break;
            }
            case OpCode::Jump: {
                uint16_t jumpDist = readShort();
                frame->ip += jumpDist;
                break;
            }
            case OpCode::Loop: {
                uint16_t jumpDist = readShort();
                frame->ip -= jumpDist;
                break;
            }
            case OpCode::Call: {
                CallSpec callSpec{frame->ip};
                Value& callee { peek(callSpec.argCount) };
                callee.resolve();
                if (!callValue(callee, callSpec))
                    return errorReturn;
                frame = thread->frames.end()-1;
                break;
            }
            case OpCode::Index: {
                uint8_t argCount = readByte();
                peek(argCount).resolveFuture(); // don't resolve signals here
                if (!indexValue(peek(argCount), argCount))
                    return errorReturn;
                break;
            }
            // TODO: reimplement optimization to use Invoke as single step for object.method()
            //  instead of current two step push & call (see original Antlr visitor compiler impl)
            case OpCode::Invoke: {
                ObjString* method = readString();
                CallSpec callSpec{frame->ip};
                if (!invoke(method, callSpec))
                    return errorReturn;
                frame = thread->frames.end()-1;
                break;
            }
            case OpCode::Closure: {
                Value function = readConstant();
                debug_assert_msg(isFunction(function), "Expected a function value for OpCode::Closure");
                Value closure { Value::closureVal(function) };
                ObjFunction* funcObj = asFunction(function);
                if (funcObj->ownerType.isNil() && !asFunction(asClosure(frame->closure)->function)->ownerType.isNil())
                    funcObj->ownerType = asFunction(asClosure(frame->closure)->function)->ownerType;
                push(closure);
                for (int i = 0; i < asClosure(closure)->upvalues.size(); i++) {
                    uint8_t isLocal = readByte();
                    uint8_t index = readByte();
                    Value upvalue; // ObjUpvalue
                    if (isLocal)
                        upvalue = captureUpvalue(*(frame->slots + index));
                    else
                        upvalue = asClosure(frame->closure)->upvalues[index];

                    asClosure(closure)->upvalues[i] = upvalue;
                }
                break;
            }
            case OpCode::CloseUpvalue: {
                closeUpvalues(&(*(thread->stackTop -1)));
                pop();
                break;
            }
            case OpCode::Return: {

                try {
                    Value result = opReturn();

                    push(result);

                    // For nested execute() calls, only terminate when we return to entry depth
                    if (thread->execute_depth > 1 && thread->frames.size() <= frame_depth_on_entry) {
                        Value returnVal = pop();

                        if (consumePendingObjectCleanup()) {
                            freeObjects(); // Drain pending objects on scope exit for deterministic destruction
                        }

                        if (thread->execute_depth > 0) thread->execute_depth--;
                        return std::make_pair(InterpretResult::OK,returnVal);
                    }

                    // For top-level execute(), use original termination logic
                    if (thread->execute_depth == 1 && thread->frames.empty()) {
                        Value returnVal = pop();

                        if (consumePendingObjectCleanup()) {
                            freeObjects();
                        }

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
            case OpCode::ReturnStore: {

                try {
                    Value result = opReturn();

                    // For nested execute() calls, only terminate when we return to entry depth
                    if (thread->execute_depth > 1 && thread->frames.size() <= frame_depth_on_entry) {
                        if (consumePendingObjectCleanup()) {
                            freeObjects(); // Drain pending objects on scope exit for deterministic destruction
                        }

                        if (thread->execute_depth > 0) thread->execute_depth--;
                        return std::make_pair(InterpretResult::OK,result);
                    }

                    // For top-level execute(), use original termination logic
                    if (thread->execute_depth == 1 && thread->frames.empty()) {
                        if (consumePendingObjectCleanup()) {
                            freeObjects();
                        }

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
            case OpCode::ConstNil: {
                push(Value::nilVal());
                break;
            }
            case OpCode::GetLocal: {
                uint16_t slot = singleByteArg ? readByte() : readShort();
                #ifdef DEBUG_BUILD
                auto stackIndex = (frame->slots - &thread->stack[0]) + slot;
                if (stackIndex >= thread->stack.size())
                    throw std::runtime_error("Stack overflow access");
                #endif
                push(frame->slots[slot]);
                break;
            }
            case OpCode::SetLocal: {
                uint16_t slot = singleByteArg ? readByte() : readShort();
                #ifdef DEBUG_BUILD
                auto stackIndex = (frame->slots - &thread->stack[0]) + slot;
                if (stackIndex >= thread->stack.size())
                    throw std::runtime_error("Stack overflow access");
                #endif
                frame->slots[slot] = peek(0);
                break;
            }
            case OpCode::SetIndex: {
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
            case OpCode::DefineModuleVar: {
                ObjString* name = readString();
                moduleVars().store(name->hash, name->s,pop());
                break;
            }
            case OpCode::GetModuleVar: {
                ObjString* name = readString();
                auto& vars { moduleVars() };
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
            case OpCode::SetModuleVar: {
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
            case OpCode::SetNewModuleVar: {
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
            case OpCode::GetUpvalue: {
                uint16_t slot = singleByteArg ? readByte() : readShort();
                push(*asUpvalue(asClosure(frame->closure)->upvalues[slot])->location);
                break;
            }
            case OpCode::SetUpvalue: {
                uint16_t slot = singleByteArg ? readByte() : readShort();
                *(asUpvalue(asClosure(frame->closure)->upvalues[slot])->location) = peek(0);
                break;
            }
            case OpCode::NewRange: {
                bool closed = ((readByte() & 1) > 0);
                if (!peek(2).isNil() && !peek(2).isNumber())
                    runtimeError("The start bound of a range must be a number");
                if (!peek(1).isNil() && !peek(1).isNumber())
                    runtimeError("The stop bound of a range must be a number");
                if (!peek(0).isNil() && !peek(0).isNumber())
                    runtimeError("The step of a range must be a number");
                auto rangeVal = Value::rangeVal(peek(2),peek(1),peek(0),closed);
                popN(3);
                push(rangeVal);
                break;
            }
            case OpCode::NewList: {
                int eltCount = readByte();
                std::vector<Value> elts {};
                elts.reserve(eltCount);
                // top of stack is last list elt by index
                for(int i=0; i<eltCount;i++)
                    elts.push_back(peek(eltCount-i-1));
                for(int i=0; i<eltCount;i++) pop();
                push(Value::listVal(elts));
                break;
            }
            case OpCode::NewDict: {
                int entryCount = readByte();
                std::vector<std::pair<Value,Value>> entries {};
                entries.reserve(entryCount);
                // top of stack is last dict entry (the value)
                for(int i=0; i<entryCount;i++) {
                    entries.push_back(std::make_pair(peek(2*(entryCount-1-i)+1),
                                                     peek(2*(entryCount-1-i))));
                }
                for(int i=0; i<entryCount*2;i++) pop();
                push(Value::dictVal(entries));
                break;
            }
            case OpCode::NewVector: {
                int eltCount = readByte();
                Eigen::VectorXd vals(eltCount);
                for(int i=0; i<eltCount; i++)
                    vals[i] = toType(ValueType::Real, peek(eltCount-i-1), false).asReal();
                for(int i=0; i<eltCount; i++) pop();
                push(Value::vectorVal(vals));
                break;
            }
            case OpCode::NewMatrix: {
                int rowCount = readByte();
                if (rowCount == 0) {
                    push(Value::matrixVal());
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
                push(Value::matrixVal(mat));
                break;
            }
            case OpCode::IfDictToKeys: {
                Value& maybeDict = peek(0);
                if (!isDict(maybeDict))
                    maybeDict.resolve();
                if (isDict(maybeDict)) {
                    Value d { maybeDict };
                    pop();
                    auto keys { asDict(d)->keys() };
                    push(Value::listVal(keys));
                }
                break;
            }
            case OpCode::IfDictToItems: {
                Value& maybeDict = peek(0);
                if (!isDict(maybeDict))
                    maybeDict.resolve();
                if (isDict(maybeDict)) {
                    Value d { maybeDict };
                    pop();
                    auto vecItemPairs { asDict(d)->items() };
                    Value listItems { Value::listVal() };
                    for(const auto& item : vecItemPairs) {
                        Value itemList { Value::listVal() };
                        asList(itemList)->elts.push_back(item.first);
                        asList(itemList)->elts.push_back(item.second);
                        asList(listItems)->elts.push_back(itemList);
                    }
                    push(listItems);
                }
                break;
            }
            case OpCode::ToType: {
                uint8_t typeByte = readByte();
                try {
                    peek(0) = toType(ValueType(typeByte), peek(0), /*strict=*/false);
                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return errorReturn;
                }
                break;
            }
            case OpCode::ToTypeStrict: {
                uint8_t typeByte = readByte();
                try {
                    peek(0) = toType(ValueType(typeByte), peek(0), /*strict=*/true);
                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return errorReturn;
                }
                break;
            }
            case OpCode::ToTypeSpec: {
                Value typeSpec = pop();
                try {
                    peek(0) = toType(typeSpec, peek(0), /*strict=*/false);
                } catch(std::exception& e) {
                    runtimeError(e.what());
                    return errorReturn;
                }
                break;
            }
            case OpCode::ToTypeSpecStrict: {
                Value typeSpec = pop();
                try {
                    peek(0) = toType(typeSpec, peek(0), /*strict=*/true);
                } catch(std::exception& e) {
                    runtimeError(e.what());
                    return errorReturn;
                }
                break;
            }
            case OpCode::EventOn: {
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
                    thread->eventToSignal[eventVal.weakRef()] = Value::objRef(sigObj);
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
            case OpCode::EventOff: {
                Value closureVal = pop();
                Value eventVal = pop();
                if (!isClosure(closureVal) || !(isEvent(eventVal) || isSignal(eventVal))) {
                    runtimeError("EVENT_OFF expects event/signal and closure");
                    return errorReturn;
                }

                ObjEvent* ev = nullptr;
                if (isEvent(eventVal)) {
                    ev = asEvent(eventVal);
                } else {
                    ObjSignal* sigObj = asSignal(eventVal);
                    ev = sigObj->ensureChangeEvent();
                    eventVal = sigObj->changeEvent;
                    thread->eventToSignal.erase(eventVal.weakRef());
                }

                Value key = eventVal.weakRef();
                auto it = thread->eventHandlers.find(key);
                if (it != thread->eventHandlers.end()) {
                    auto& handlers = it->second;
                    for(auto hit = handlers.begin(); hit != handlers.end(); ) {
                        if (hit->isAlive() && asClosure(*hit) == asClosure(closureVal))
                            hit = handlers.erase(hit);
                        else
                            ++hit;
                    }
                    if (handlers.empty())
                        thread->eventHandlers.erase(it);
                }

                for(auto it = ev->subscribers.begin(); it != ev->subscribers.end(); ) {
                    if (it->isAlive() && asClosure(*it) == asClosure(closureVal))
                        it = ev->subscribers.erase(it);
                    else
                        ++it;
                }

                break;
            }
            case OpCode::SetupExcept: {
                uint16_t off = readShort();
                CallFrame::ExceptionHandler h;
                h.handlerIp = frame->ip + off;
                h.stackDepth = thread->stackTop - thread->stack.begin();
                h.frameDepth = thread->frames.size();
                frame->exceptionHandlers.push_back(h);
                break;
            }
            case OpCode::EndExcept: {
                if (!frame->exceptionHandlers.empty())
                    frame->exceptionHandlers.pop_back();
                break;
            }
            case OpCode::Throw: {
                Value exc = pop();
                if (!isException(exc))
                    exc = Value::exceptionVal(exc);
                ObjException* exObj = asException(exc);
                if (exObj->stackTrace.isNil())
                    exObj->stackTrace = captureStacktrace();
                while (true) {
                    if (thread->frames.empty()) {
                        runtimeError("Uncaught exception: " + objExceptionToString(asException(exc)));
                        return errorReturn;
                    }
                    auto &cf = thread->frames.back();
                    if (!cf.exceptionHandlers.empty()) {
                        auto h = cf.exceptionHandlers.back();
                        cf.exceptionHandlers.pop_back();
                        while (thread->frames.size() > h.frameDepth)
                            unwindFrame();
                        frame = thread->frames.end()-1;
                        frame->ip = h.handlerIp;
                        while (thread->stackTop - thread->stack.begin() > h.stackDepth)
                            pop();
                        push(exc);
                        break;
                    } else {
                        unwindFrame();
                    }
                }
                frame = thread->frames.end()-1;
                break;
            }
            case OpCode::ObjectType: {
                ObjString* name = readString();
                Value tv { Value::objectTypeVal(name->s, false) }; // ObjObjectType
                if (!thread->frames.empty()) {
                    auto frame = thread->frames.end()-1;
                    ObjModuleType* mod = asModuleType(asFunction(asClosure(frame->closure)->function)->moduleType);
                    auto it = mod->cstructArch.find(name->hash);
                    if (it != mod->cstructArch.end()) {
                        auto t { asObjectType(tv) };
                        t->isCStruct = true;
                        t->cstructArch = it->second;
                    }
                }
                push(tv);
                break;
            }
            case OpCode::ActorType: {
                ObjString* name = readString();
                Value tv { Value::objectTypeVal(name->s, true) }; // ObjObjectType
                if (!thread->frames.empty()) {
                    auto frame = thread->frames.end()-1;
                    ObjModuleType* mod = asModuleType(asFunction(asClosure(frame->closure)->function)->moduleType);
                    auto it = mod->cstructArch.find(name->hash);
                    if (it != mod->cstructArch.end()) {
                        auto t { asObjectType(tv) };
                        t->isCStruct = true;
                        t->cstructArch = it->second;
                    }
                }
                push(tv);
                break;
            }
            case OpCode::InterfaceType: {
                // interface types are represented as object types (but are abstract - all abstract methods)
                ObjString* name = readString();
                Value tv { Value::objectTypeVal(name->s, false, true) };
                if (!thread->frames.empty()) {
                    auto frame = thread->frames.end()-1;
                    ObjModuleType* mod = asModuleType(asFunction(asClosure(frame->closure)->function)->moduleType);
                    auto it = mod->cstructArch.find(name->hash);
                    if (it != mod->cstructArch.end()) {
                        auto t { asObjectType(tv) };
                        t->isCStruct = true;
                        t->cstructArch = it->second;
                    }
                }
                push(tv);
                break;
            }
            case OpCode::EnumerationType: {
                ObjString* name = readString();
                Value tv { Value::objectTypeVal(name->s, false, false, true) };
                if (!thread->frames.empty()) {
                    auto frame = thread->frames.end()-1;
                    ObjModuleType* mod = asModuleType(asFunction(asClosure(frame->closure)->function)->moduleType);
                    auto it = mod->cstructArch.find(name->hash);
                    if (it != mod->cstructArch.end()) {
                        auto t { asObjectType(tv) };
                        t->isCStruct = true;
                        t->cstructArch = it->second;
                    }
                }
                push(tv);
                break;
            }
            case OpCode::Property: {
                try {
                    defineProperty(readString());
                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return errorReturn;
                }
                break;
            }
            case OpCode::Method: {
                try {
                    defineMethod(readString());
                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return errorReturn;
                }
                break;
            }
            case OpCode::EnumLabel: {
                try {
                    defineEnumLabel(readString());
                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return errorReturn;
                }
                break;
            }
            case OpCode::Extend: {
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
                subType->superType = Value::objRef(superType);
                subType->properties.insert(superType->properties.cbegin(), superType->properties.cend());
                subType->propertyOrder.insert(subType->propertyOrder.end(),
                                             superType->propertyOrder.begin(),
                                             superType->propertyOrder.end());
                pop();
                break;
            }
            case OpCode::ImportModuleVars: {
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
                    if (isString(firstElement) && asStringObj(firstElement)->s == "*") {
                        fromModuleType->vars.forEach(
                            [&](const VariablesMap::NameValue& nameValue) {
                                //const icu::UnicodeString& name { nameValue.first };
                                toModuleType->vars.store(nameValue);
                                //std::cout << "declaring " << toUTF8StdString(nameValue.first) << " into " << toUTF8StdString(toModuleType->name) << std::endl;
                            });
                    }
                    else { // import the symbols explicitly listed
                        for(const auto& symbol : symbolsListObj->elts.get()) {
                            const auto& symbolString { asStringObj(symbol) };
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
            case OpCode::Nop: {
                break;
            }
            default:
                #ifdef DEBUG_BUILD
                runtimeError("Invalid instruction "+std::to_string(int(instruction)));
                #endif
                return std::make_pair(InterpretResult::RuntimeError,Value::nilVal());
                break;
        }

        postInstructionDispatch:

        if (valueGC.isCollectionRequested()) {
            valueGC.safepoint(*thread);
        }

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

        if (consumePendingObjectCleanup()) {
            freeObjects();
        }

    } // for

    if (thread->execute_depth > 0) thread->execute_depth--;
    return std::make_pair(InterpretResult::OK, Value::nilVal());

}


bool VM::processPendingEvents()
{

    if (exitRequested.load()) return false;

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
                auto closureObj = asClosure(handler);

                auto prevThreadSleep = thread->threadSleep.load();
                auto prevThreadSleepUntil = thread->threadSleepUntil.load();

                thread->threadSleep = false;

                if (handler == conditionalInterruptClosure) {
                    bool raise = true;
                    auto sigIt = thread->eventToSignal.find(tev.event);
                    if (sigIt != thread->eventToSignal.end()) {
                        Value sigVal = sigIt->second;
                        if (isSignal(sigVal)) {
                            ObjSignal* sigObj = asSignal(sigVal);
                            Value cur = sigObj->signal->lastValue();
                            if (cur.isBool() && cur.asBool()) {
                                raise = true;
                            } else {
                                raise = false;
                            }
                        }
                    }
                    if (raise) {
                        Value excType = globals.load(toUnicodeString("ConditionalInterrupt")).value();
                        Value exc = Value::exceptionVal(Value::nilVal(), excType);
                        raiseException(exc);
                    }
                } else {
                    auto result = callAndExec(closureObj, {});
                    assert(!thread->threadSleep);

                    if (result.first != InterpretResult::OK)
                        return false;

                    thread->threadSleep = prevThreadSleep;
                    thread->threadSleepUntil = prevThreadSleepUntil;
                }
            }
        }
    }
    return true;
}

void VM::unwindFrame()
{
    auto f = thread->frames.back();
    closeUpvalues(f.slots);
    size_t popCount = &(*thread->stackTop) - f.slots;
    for(size_t i = 0; i < popCount; i++) pop();
    thread->popFrame();
}

void VM::raiseException(Value exc)
{
    if (!isException(exc))
        exc = Value::exceptionVal(exc);

    ObjException* exObj = asException(exc);
    if (exObj->stackTrace.isNil())
        exObj->stackTrace = captureStacktrace();

    while (true) {
        if (thread->frames.empty()) {
            runtimeError("Uncaught exception: " + objExceptionToString(asException(exc)));
            return;
        }

        auto& cf = thread->frames.back();
        if (!cf.exceptionHandlers.empty()) {
            auto h = cf.exceptionHandlers.back();
            cf.exceptionHandlers.pop_back();
            while (thread->frames.size() > h.frameDepth)
                unwindFrame();
            auto frame = thread->frames.end()-1;
            frame->ip = h.handlerIp;
            while (thread->stackTop - thread->stack.begin() > h.stackDepth)
                pop();
            push(exc);
            break;
        } else {
            unwindFrame();
        }
    }
}


void VM::resetStack()
{
    if (!thread) return;
    thread->stack.clear();
    thread->stack.resize(MaxStack);
    thread->stackTop = thread->stack.begin();

    thread->frames.clear();
    thread->frames.reserve(128);
    thread->frameStart = false;
    thread->openUpvalues.clear();
}


bool VM::isCurrentThreadActorWorker() const
{
    return thread && thread->isActorThread();
}

void VM::enqueueActorFinalizer(ActorInstance* actorInst)
{
    // Actor workers call into freeObjects() from their own GC safepoints just
    // like any other thread.  When they encounter another actor instance we do
    // not let them perform the blocking join immediately—doing so risks
    // deadlocking if two actors try to finalize each other.  Instead they push
    // the instance onto this queue for a later, safe thread to handle.
    if (!actorInst) {
        return;
    }

    std::lock_guard<std::mutex> lock(actorFinalizerMutex);
    pendingActorFinalizers.push_back(actorInst);
    requestObjectCleanup();
}

void VM::drainActorFinalizerQueue(std::vector<ActorInstance*>& out)
{
    // Non-actor threads call this helper before running the regular GC pass so
    // they can take ownership of any actor instances enqueued by worker
    // threads.  Once an instance is moved out it will be joined and deleted in
    // finalizeActorInstances().
    std::lock_guard<std::mutex> lock(actorFinalizerMutex);
    while (!pendingActorFinalizers.empty()) {
        ActorInstance* inst = pendingActorFinalizers.front();
        pendingActorFinalizers.pop_front();
        if (!inst) {
            continue;
        }
        out.push_back(inst);
    }
}

void VM::finalizeActorInstances(std::vector<ActorInstance*>& actors)
{
    // Every ActorInstance handed to this function has already been detached
    // from the ref-counted object graph.  The only remaining teardown step is
    // to request the worker thread to exit and wait for it to finish.  The
    // join() call handles both, so once it returns we can safely destroy the
    // ActorInstance itself.
    for (ActorInstance* inst : actors) {
        if (!inst) {
            continue;
        }
        if (auto t = inst->thread.lock()) {
            t->join(inst);
        }
        delObj(inst);
    }

    actors.clear();
}

void VM::freeObjects()
{
    std::vector<Obj*> pending;
    pending.reserve(64);

    const bool actorWorker = isCurrentThreadActorWorker();
    std::vector<ActorInstance*> actorsToFinalize;
    actorsToFinalize.reserve(16);

    if (!actorWorker) {
        // Only non-actor threads are allowed to drain the finalizer queue so
        // that all joins are performed from a context that cannot be the
        // target of the join itself.
        drainActorFinalizerQueue(actorsToFinalize);
    }

    // Objects that reach zero strong references are appended to
    // Obj::unrefedObjs by the ref-counting slow path.  We drain the queue in
    // batches so we can dropReferences() on every pending object first,
    // severing outgoing edges before any destructors run.  That way, if a
    // destructor looks at another object from the same batch, it observes a
    // consistent, fully dropped state instead of an object halfway through
    // teardown, which prevents cross-object use-after-free hazards.
    while (true) {
        while (!Obj::unrefedObjs.empty()) {
            Obj::unrefedObjs.pop_back_and_apply([&pending](Obj* obj) {
                if (obj) {
                    pending.push_back(obj);
                }
            });
        }

        if (pending.empty()) {
            break;
        }

        for (Obj* obj : pending) {
            if (obj) {
                obj->dropReferences();
            }
        }

        for (Obj* obj : pending) {
            if (!obj) {
                continue;
            }

            if (obj->type == ObjType::Actor) {
                auto actorInst = static_cast<ActorInstance*>(obj);
                if (actorWorker) {
                    // Actor workers enqueue actor instances for later
                    // processing.  The actual join will happen when a
                    // non-actor thread next calls freeObjects().
                    enqueueActorFinalizer(actorInst);
                } else {
                    // Non-actor threads can finalize the actor in-line once
                    // the current batch is done dropping references.
                    actorsToFinalize.push_back(actorInst);
                }
                continue;
            }

            delObj(obj);
        }

        pending.clear();
    }

    if (!actorWorker) {
        // When freeObjects() runs on a non-actor thread we now own the right to
        // perform the actual joins for any actors we collected above.
        finalizeActorInstances(actorsToFinalize);
    } else {
        actorsToFinalize.clear();
    }

    if (thread) {
        thread->pruneEventRegistrations();
    }
}

void VM::cleanupWeakRegistries()
{
    purgeDeadInternedStrings();

    std::vector<ptr<Thread>> threadsToClean;
    threads.unsafeApply([&threadsToClean](const auto& registered) {
        threadsToClean.reserve(registered.size());
        for (const auto& entry : registered) {
            if (entry.second) {
                threadsToClean.push_back(entry.second);
            }
        }
    });

    threadsToClean.reserve(threadsToClean.size() + 3);
    auto appendThread = [&threadsToClean](const ptr<Thread>& candidate) {
        if (candidate) {
            threadsToClean.push_back(candidate);
        }
    };

    appendThread(replThread);
    appendThread(dataflowEngineThread);
    appendThread(VM::thread);

    std::unordered_set<Thread*> seen;
    for (const auto& threadPtr : threadsToClean) {
        if (!threadPtr) {
            continue;
        }
        Thread* raw = threadPtr.get();
        if (!seen.insert(raw).second) {
            continue;
        }
        raw->pruneEventRegistrations();
    }
}


void VM::outputAllocatedObjs()
{
    #ifdef DEBUG_TRACE_MEMORY
    if (Obj::allocatedObjs.size()>0) {
        std::cout << "== allocated Objs (" << Obj::allocatedObjs.size() << ") ==" << std::endl;
        std::cout << std::hex;
        for(const auto& p : Obj::allocatedObjs.get()) {
            std::cout << "  " << uint64_t(p.first);
            if (!p.second.empty()) std::cout << " " << p.second;
            std::cout << " " << objTypeName(p.first) << std::endl;
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
    push( Value::stringVal(combined) );
}


void VM::runtimeError(const std::string& format, ...)
{
    runtimeErrorFlag = true;

    // Wake all threads so they can notice the error flag and terminate
    threads.apply([](const std::pair<const uint64_t, ptr<Thread>>& entry){
        if (entry.second)
            entry.second->wake();
    });

    if (!thread || thread->frames.empty()) {
        fprintf(stderr, "error: ");
        va_list args;
        va_start(args, format);
        vfprintf(stderr, format.c_str(), args);
        va_end(args);
        fputs("\n", stderr);
        resetStack();
        return;
    }

    auto frame { thread->frames.end()-1 };

    size_t instruction = frame->ip -
        asFunction(asClosure(frame->closure)->function)->chunk->code.begin() - 1;
    auto chunk = asFunction(asClosure(frame->closure)->function)->chunk;
    int line = chunk->getLine(instruction);
    int col  = chunk->getColumn(instruction);
    std::string fname = toUTF8StdString(chunk->sourceName);

    // output stacktrace
    for(auto it = thread->frames.begin(); it != thread->frames.end(); ++it) {
        const CallFrame& f { *it };
        auto c = asFunction(asClosure(f.closure)->function)->chunk;
        size_t instr = 0;
        if (f.ip > c->code.begin())
            instr = f.ip - c->code.begin() - 1;
        int ln = c->getLine(instr);
        int cl = c->getColumn(instr);
        std::string fn = toUTF8StdString(c->sourceName);
        UnicodeString funcName = asFunction(asClosure(f.closure)->function)->name;
        if (funcName.isEmpty())
            funcName = UnicodeString("<script>");
        if (!fn.empty())
            fprintf(stderr, "%s:%d:%d: in %s\n", fn.c_str(), ln, cl,
                    toUTF8StdString(funcName).c_str());
        else
            fprintf(stderr, "[line %d:%d]: in %s\n", ln, cl,
                    toUTF8StdString(funcName).c_str());
    }

    if (!fname.empty())
        fprintf(stderr, "%s:%d:%d: error: ", fname.c_str(), line, col);
    else
        fprintf(stderr, "[line %d:%d]: error: ", line, col);

    va_list args;
    va_start(args, format);
    vfprintf(stderr, format.c_str(), args);
    va_end(args);
    fputs("\n", stderr);

    if (!fname.empty()) {
        std::ifstream src(fname);
        if (src.good()) {
            std::string srcLine;
            for (int i = 1; i <= line && std::getline(src, srcLine); ++i) {
                if (i == line) {
                    fprintf(stderr, "    %d | %s\n", line, srcLine.c_str());
                    std::string lstr = std::to_string(line);
                    size_t indent = 4 + lstr.length() + 1; // spaces before '|'
                    fprintf(stderr, "%s| %s^\n", spaces(indent).c_str(), spaces(col).c_str());
                }
            }
        }
    }

    resetStack();
}



//
// builtins

void VM::defineBuiltinFunctions()
{
    for (auto& mod : builtinModules) {
        try {
           mod->registerBuiltins(*this);
        } catch (std::exception& e) {
            runtimeError("Error registering builtins for module '%s': %s",
                         toUTF8StdString(asModuleType(mod->moduleType())->name).c_str(), e.what());
            return;
        }
    }
}

void VM::defineBuiltinMethods()
{
    defineBuiltinMethod(ValueType::Vector, "norm", std::mem_fn(&VM::vector_norm_builtin));
    defineBuiltinMethod(ValueType::Vector, "sum", std::mem_fn(&VM::vector_sum_builtin));
    defineBuiltinMethod(ValueType::Vector, "normalized", std::mem_fn(&VM::vector_normalized_builtin));
    defineBuiltinMethod(ValueType::Vector, "dot", std::mem_fn(&VM::vector_dot_builtin));

    defineBuiltinMethod(ValueType::Matrix, "rows", std::mem_fn(&VM::matrix_rows_builtin));
    defineBuiltinMethod(ValueType::Matrix, "cols", std::mem_fn(&VM::matrix_cols_builtin));
    defineBuiltinMethod(ValueType::Matrix, "transpose", std::mem_fn(&VM::matrix_transpose_builtin));
    defineBuiltinMethod(ValueType::Matrix, "determinant", std::mem_fn(&VM::matrix_determinant_builtin));
    defineBuiltinMethod(ValueType::Matrix, "inverse", std::mem_fn(&VM::matrix_inverse_builtin));
    defineBuiltinMethod(ValueType::Matrix, "trace", std::mem_fn(&VM::matrix_trace_builtin));
    defineBuiltinMethod(ValueType::Matrix, "norm", std::mem_fn(&VM::matrix_norm_builtin));
    defineBuiltinMethod(ValueType::Matrix, "sum", std::mem_fn(&VM::matrix_sum_builtin));

    defineBuiltinMethod(ValueType::List, "append", std::mem_fn(&VM::list_append_builtin));

    defineBuiltinMethod(ValueType::Signal, "run", std::mem_fn(&VM::signal_run_builtin));
    defineBuiltinMethod(ValueType::Signal, "stop", std::mem_fn(&VM::signal_stop_builtin));
    defineBuiltinMethod(ValueType::Signal, "tick", std::mem_fn(&VM::signal_tick_builtin));
    defineBuiltinMethod(ValueType::Signal, "freq", std::mem_fn(&VM::signal_freq_builtin));
    defineBuiltinMethod(ValueType::Signal, "set", std::mem_fn(&VM::signal_set_builtin));

    defineBuiltinMethod(ValueType::Event, "emit", std::mem_fn(&VM::event_emit_builtin), true);
    defineBuiltinMethod(ValueType::Event, "on", std::mem_fn(&VM::event_on_builtin), true);
    defineBuiltinMethod(ValueType::Event, "off", std::mem_fn(&VM::event_off_builtin), true);

    defineBuiltinMethod(ValueType::Actor, "tick", std::mem_fn(&VM::dataflow_tick_native), true);  // proc
    defineBuiltinMethod(ValueType::Actor, "run", std::mem_fn(&VM::dataflow_run_native), true);   // proc
    defineBuiltinMethod(ValueType::Actor, "runFor", std::mem_fn(&VM::dataflow_run_for_native), true);  // proc
}

void VM::defineBuiltinMethod(ValueType type, const std::string& name, NativeFn fn,
                             bool isProc,
                             ptr<type::Type> funcType,
                             std::vector<Value> defaults)
{
    auto us = toUnicodeString(name);
    builtinMethods[type][us.hashCode()] = BuiltinMethodInfo(fn, isProc, funcType, std::move(defaults));
}

void VM::defineBuiltinProperties()
{
    if (!builtinProperties.empty())
        return;

    // Signal properties
    defineBuiltinProperty(ValueType::Signal, "value", &VM::signal_value_getter);
    defineBuiltinProperty(ValueType::Signal, "name", &VM::signal_name_getter,
                         &VM::signal_name_setter);
    defineBuiltinProperty(ValueType::Object, "stackTrace", &VM::exception_stacktrace_getter);
    defineBuiltinProperty(ValueType::Object, "stackTraceString", &VM::exception_stacktrace_string_getter);
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

Value VM::signal_name_getter(Value& receiver)
{
#ifdef DEBUG_BUILD
    if (!isSignal(receiver))
        throw std::invalid_argument("signal.name property called on non-signal value");
#endif

    ObjSignal* objSignal = asSignal(receiver);
    return Value::stringVal(toUnicodeString(objSignal->signal->name()));
}

void VM::signal_name_setter(Value& receiver, Value value)
{
#ifdef DEBUG_BUILD
    if (!isSignal(receiver))
        throw std::invalid_argument("signal.name property called on non-signal value");
#endif

    ObjSignal* objSignal = asSignal(receiver);
    std::string newName;
    if (isString(value))
        newName = toUTF8StdString(asStringObj(value)->s);
    else
        newName = toString(value);

    objSignal->signal->rename(newName);
}

Value VM::exception_stacktrace_getter(Value& receiver)
{
#ifdef DEBUG_BUILD
    if (!isException(receiver))
        throw std::invalid_argument("exception.stackTrace property on non-exception");
#endif
    if (!isException(receiver)) {
        runtimeError("Undefined property 'stackTrace'");
        return Value::nilVal();
    }
    ObjException* ex = asException(receiver);
    return ex->stackTrace;
}

Value VM::exception_stacktrace_string_getter(Value& receiver)
{
#ifdef DEBUG_BUILD
    if (!isException(receiver))
        throw std::invalid_argument("exception.stackTraceString property on non-exception");
#endif
    if (!isException(receiver)) {
        runtimeError("Undefined property 'stackTraceString'");
        return Value::nilVal();
    }
    ObjException* ex = asException(receiver);
    std::string out = stackTraceToString(ex->stackTrace);
    return Value::stringVal(toUnicodeString(out));
}

Value VM::captureStacktrace()
{
    Value framesList { Value::listVal() };

    for(auto it = thread->frames.begin(); it != thread->frames.end(); ++it) {
        const CallFrame& frame { *it };
        Value frameDict { Value::dictVal() };

        UnicodeString funcName = asFunction(asClosure(frame.closure)->function)->name;
        if (funcName.isEmpty())
            funcName = UnicodeString("<script>");

        asDict(frameDict)->store(Value::stringVal(UnicodeString("function")),
                                 Value::stringVal(funcName));

        auto chunk = asFunction(asClosure(frame.closure)->function)->chunk;
        size_t instruction = 0;
        if (frame.ip > chunk->code.begin())
            instruction = frame.ip - chunk->code.begin() - 1;
        int line = chunk->getLine(instruction);
        int col  = chunk->getColumn(instruction);

        asDict(frameDict)->store(Value::stringVal(UnicodeString("line")), Value::intVal(line));
        asDict(frameDict)->store(Value::stringVal(UnicodeString("col")), Value::intVal(col));

        asDict(frameDict)->store(Value::stringVal(UnicodeString("filename")),
                                 Value::stringVal(chunk->sourceName));

        asList(framesList)->append(frameDict);
    }

    return framesList;
}

Value VM::event_emit_builtin(ArgsView args)
{
    if (args.size() > 2 || !isEvent(args[0]))
        throw std::invalid_argument("event.emit expects optional time argument in microseconds");

    TimePoint when = TimePoint::currentTime();
    if (args.size() == 2) {
        if (!args[1].isNumber())
            throw std::invalid_argument("event.emit time argument must be numeric microseconds");
        when = TimePoint::microSecs(args[1].asInt());
    }

    Value eventWeak = args[0].weakRef();
    ObjEvent* ev = asEvent(args[0]);
    if (ev->subscribers.empty())
        return Value::nilVal();

    scheduleEventHandlers(eventWeak, ev, when);

    return Value::nilVal();
}

Value VM::event_on_builtin(ArgsView args)
{
    if (args.size() != 2 || !isEvent(args[0]) || !isClosure(args[1]))
        throw std::invalid_argument("event.on expects event and closure argument");

    Value eventVal = args[0];
    Value closureVal = args[1];

    Value key = eventVal.weakRef();
    thread->eventHandlers[key].push_back(closureVal);

    ObjEvent* ev = asEvent(eventVal);
    ObjClosure* closure = asClosure(closureVal);
    closure->handlerThread = thread;
    ev->subscribers.push_back(closureVal.weakRef());

    return Value::nilVal();
}

Value VM::event_off_builtin(ArgsView args)
{
    if (args.size() != 2 || !(isEvent(args[0]) || isSignal(args[0])) || !isClosure(args[1]))
        throw std::invalid_argument("event.off expects event/signal and closure argument");

    Value eventVal = args[0];
    Value closureVal = args[1];

    ObjEvent* ev = nullptr;
    if (isEvent(eventVal)) {
        ev = asEvent(eventVal);
    } else {
        ObjSignal* sigObj = asSignal(eventVal);
        ev = sigObj->ensureChangeEvent();
        eventVal = sigObj->changeEvent;
        thread->eventToSignal.erase(eventVal.weakRef());
    }

    Value key = eventVal.weakRef();
    auto it = thread->eventHandlers.find(key);
    if (it != thread->eventHandlers.end()) {
        auto& handlers = it->second;
        for(auto hit = handlers.begin(); hit != handlers.end(); ) {
            if (hit->isAlive() && asClosure(*hit) == asClosure(closureVal))
                hit = handlers.erase(hit);
            else
                ++hit;
        }
        if (handlers.empty())
            thread->eventHandlers.erase(it);
    }

    for(auto it = ev->subscribers.begin(); it != ev->subscribers.end(); ) {
        if (it->isAlive() && asClosure(*it) == asClosure(closureVal))
            it = ev->subscribers.erase(it);
        else
            ++it;
    }

    return Value::nilVal();
}



Value VM::vector_norm_builtin(ArgsView args)
{
    if (args.size() != 1 || !isVector(args[0]))
        throw std::invalid_argument("vector.norm expects no arguments");

    ObjVector* vec = asVector(args[0]);
    double n = vec->vec.norm();
    return Value::realVal(n);
}

Value VM::vector_sum_builtin(ArgsView args)
{
    if (args.size() != 1 || !isVector(args[0]))
        throw std::invalid_argument("vector.sum expects no arguments");

    ObjVector* vec = asVector(args[0]);
    double s = vec->vec.sum();
    return Value::realVal(s);
}

Value VM::vector_normalized_builtin(ArgsView args)
{
    if (args.size() != 1 || !isVector(args[0]))
        throw std::invalid_argument("vector.normalized expects no arguments");

    ObjVector* vec = asVector(args[0]);
    Eigen::VectorXd nvec = vec->vec.normalized();
    return Value::vectorVal(nvec);
}

Value VM::vector_dot_builtin(ArgsView args)
{
    if (args.size() != 2 || !isVector(args[0]) || !isVector(args[1]))
        throw std::invalid_argument("vector.dot expects single vector argument");

    ObjVector* v1 = asVector(args[0]);
    ObjVector* v2 = asVector(args[1]);
    if (v1->length() != v2->length())
        throw std::invalid_argument("vector.dot requires vectors of same length");

    double d = v1->vec.dot(v2->vec);
    return Value::realVal(d);
}

Value VM::matrix_rows_builtin(ArgsView args)
{
    if (args.size() != 1 || !isMatrix(args[0]))
        throw std::invalid_argument("matrix.rows expects no arguments");

    ObjMatrix* mat = asMatrix(args[0]);
    return Value::intVal(mat->rows());
}

Value VM::matrix_cols_builtin(ArgsView args)
{
    if (args.size() != 1 || !isMatrix(args[0]))
        throw std::invalid_argument("matrix.cols expects no arguments");

    ObjMatrix* mat = asMatrix(args[0]);
    return Value::intVal(mat->cols());
}

Value VM::matrix_transpose_builtin(ArgsView args)
{
    if (args.size() != 1 || !isMatrix(args[0]))
        throw std::invalid_argument("matrix.transpose expects no arguments");

    ObjMatrix* mat = asMatrix(args[0]);
    Eigen::MatrixXd tr = mat->mat.transpose();
    return Value::matrixVal(tr);
}

Value VM::matrix_determinant_builtin(ArgsView args)
{
    if (args.size() != 1 || !isMatrix(args[0]))
        throw std::invalid_argument("matrix.determinant expects no arguments");

    ObjMatrix* mat = asMatrix(args[0]);
    if (mat->rows() != mat->cols())
        throw std::invalid_argument("matrix.determinant requires a square matrix");

    double det = mat->mat.determinant();
    return Value::realVal(det);
}

Value VM::matrix_inverse_builtin(ArgsView args)
{
    if (args.size() != 1 || !isMatrix(args[0]))
        throw std::invalid_argument("matrix.inverse expects no arguments");

    ObjMatrix* mat = asMatrix(args[0]);
    if (mat->rows() != mat->cols())
        throw std::invalid_argument("matrix.inverse requires a square matrix");

    Eigen::MatrixXd inv = mat->mat.inverse();
    return Value::matrixVal(inv);
}

Value VM::matrix_trace_builtin(ArgsView args)
{
    if (args.size() != 1 || !isMatrix(args[0]))
        throw std::invalid_argument("matrix.trace expects no arguments");

    ObjMatrix* mat = asMatrix(args[0]);
    double tr = mat->mat.trace();
    return Value::realVal(tr);
}

Value VM::matrix_norm_builtin(ArgsView args)
{
    if (args.size() != 1 || !isMatrix(args[0]))
        throw std::invalid_argument("matrix.norm expects no arguments");

    ObjMatrix* mat = asMatrix(args[0]);
    double n = mat->mat.norm();
    return Value::realVal(n);
}

Value VM::matrix_sum_builtin(ArgsView args)
{
    if (args.size() != 1 || !isMatrix(args[0]))
        throw std::invalid_argument("matrix.sum expects no arguments");

    ObjMatrix* mat = asMatrix(args[0]);
    double s = mat->mat.sum();
    return Value::realVal(s);
}

Value VM::list_append_builtin(ArgsView args)
{
    if (args.size() != 2 || !isList(args[0]))
        throw std::invalid_argument("list.append expects single argument");

    // TODO: Signal values should be resolved when passed as function arguments
    // Currently signals may not be resolved immediately, requiring workarounds like arithmetic (0 + signal)
    ObjList* list = asList(args[0]);
    list->elts.push_back(args[1]);
    return Value::nilVal();
}

Value VM::signal_run_builtin(ArgsView args)
{
    if (args.size() != 1 || !isSignal(args[0]))
        throw std::invalid_argument("signal.run expects no arguments");

    ObjSignal* objSignal = asSignal(args[0]);
    auto sig = objSignal->signal;
    if (!sig->isSourceSignal())
        throw std::runtime_error("signal.run not supported for non-source signal");

    sig->run();
    return Value::nilVal();
}

Value VM::signal_stop_builtin(ArgsView args)
{
    if (args.size() != 1 || !isSignal(args[0]))
        throw std::invalid_argument("signal.stop expects no arguments");

    ObjSignal* objSignal = asSignal(args[0]);
    auto sig = objSignal->signal;
    if (!sig->isSourceSignal())
        throw std::runtime_error("signal.stop not supported for non-source signal");

    sig->stop();
    return Value::nilVal();
}

Value VM::signal_tick_builtin(ArgsView args)
{
    if (args.size() != 1 || !isSignal(args[0]))
        throw std::invalid_argument("signal.tick expects no arguments");

    ObjSignal* objSignal = asSignal(args[0]);
    auto sig = objSignal->signal;
    if (!sig->isSourceSignal())
        throw std::runtime_error("signal.tick only supported for source signals");

    sig->tickOnce();
    return Value::nilVal();
}

Value VM::signal_freq_builtin(ArgsView args)
{
    if (args.size() != 1 || !isSignal(args[0]))
        throw std::invalid_argument("signal.freq expects no arguments");

    ObjSignal* objSignal = asSignal(args[0]);
    auto sig = objSignal->signal;
    return Value::intVal(sig->frequency());
}

Value VM::signal_set_builtin(ArgsView args)
{
    if (args.size() != 2 || !isSignal(args[0]))
        throw std::invalid_argument("signal.set expects single value argument");

    ObjSignal* objSignal = asSignal(args[0]);
    auto sig = objSignal->signal;
    if (!sig->isSourceSignal())
        throw std::runtime_error("signal.set not supported for non-source signal");

    sig->set(args[1]);
    return Value::nilVal();
}




//
// native

void VM::defineNativeFunctions()
{
    // native sys functions are now registered via ModuleSys
}


Value VM::dataflow_tick_native(ArgsView args)
{
    df::DataflowEngine::instance()->tick(false);
    return Value::nilVal();
}

Value VM::dataflow_run_native(ArgsView args)
{
    df::DataflowEngine::instance()->run();
    return Value::nilVal();
}

Value VM::dataflow_run_for_native(ArgsView args)
{
    if (args.size() != 2 || !args[1].isNumber())
        throw std::invalid_argument("runFor expects single numeric argument");

    // TODO: _dataflow.runFor currently blocks the script thread instead of being asynchronous
    // This should be fixed so runFor sends a message to the dataflow actor thread and returns immediately
    auto duration = df::TimeDuration::microSecs(args[1].asInt());
    df::DataflowEngine::instance()->runFor(duration);
    return Value::nilVal();
}


Value VM::loadlib_native(ArgsView args)
{
    return roxal::loadlib_native(args);
}


Value VM::ffi_native(ArgsView args)
{
    return roxal::ffi_native(args);
}



Value VM::getBuiltinModule(const icu::UnicodeString& name)
{
    for (auto& m : builtinModules) {
        if (asModuleType(m->moduleType())->name == name)
            return m->moduleType();
    }
    return Value::nilVal();
}

void VM::executeBuiltinModuleScript(const std::string& path, Value moduleType)
{
    debug_assert_msg(isModuleType(moduleType),"is ObjModuleType");
    std::ifstream in(path);
    if (!in.is_open()) {
        std::string alt = "../" + path;
        in.open(alt);
        if (!in.is_open()) {
            runtimeError("Cannot open builtin module script: " + path + " or " + alt);
            return;
        }
    }

    RoxalCompiler compiler;
    compiler.setOutputBytecodeDisassembly(false);
    compiler.setCacheReadEnabled(cacheReadsEnabled());
    compiler.setCacheWriteEnabled(cacheWritesEnabled());
    compiler.setModulePaths(modulePaths);

    Value fn = compiler.compile(in, path, moduleType);
    if (fn.isNil())
        return;

    Value closure { Value::closureVal(fn) };
    ptr<Thread> t = make_ptr<Thread>();
    thread = t;
    resetStack();
    callAndExec(asClosure(closure), {});
    thread = nullptr;
}

void VM::registerBuiltinModule(ptr<BuiltinModule> module)
{
    builtinModules.push_back(module);
}

void VM::dumpStackTraces()
{
    fprintf(stderr, "\n=== Stack traces ===\n");
    threads.apply([this](const std::pair<const uint64_t, ptr<Thread>>& entry){
        if (!entry.second)
            return;

        ptr<Thread> t = entry.second;

        fprintf(stderr, "-- Thread %llu --\n", (unsigned long long)entry.first);

        if (t->frames.empty()) {
            fprintf(stderr, "<no frames>\n");
            return;
        }

        auto current = thread;
        thread = t;
        Value framesVal = captureStacktrace();
        thread = current;

        std::string traceStr = stackTraceToString(framesVal);
        fprintf(stderr, "%s", traceStr.c_str());
    });
    fflush(stderr);
}

InterpretResult VM::joinAllThreads(uint64_t skipId)
{
    InterpretResult combined = InterpretResult::OK;
    for (;;) {
        auto ids = threads.keys();
        bool joinedAny = false;
        for(uint64_t id : ids) {
            if (skipId != 0 && id == skipId)
                continue;
            joinedAny = true;
            ptr<Thread> t;
            {
                auto opt = threads.lookup(id);
                if (opt)
                    t = *opt;
            }

            if (t) {
                t->join();
                if (t->result != InterpretResult::OK)
                    combined = InterpretResult::RuntimeError;
            }

            threads.erase(id);
        }
        if (!joinedAny)
            break;
    }
    return combined;
}

void VM::requestExit(int code)
{
    exitCodeValue = code;
    exitRequested = true;

    // wake all threads so they can terminate promptly
    threads.apply([](const std::pair<const uint64_t, ptr<Thread>>& entry){
        if (entry.second)
            entry.second->wake();
    });

    ensureDataflowEngineStopped();

    uint64_t currentId = thread ? thread->id() : 0;
    joinAllThreads(currentId);
}
