#include <functional>
#include <time.h>
#include <math.h>
#include <chrono>
#include <thread>
#include <utility>
#include <memory>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <mutex>
#include <vector>
#include <map>
#include <unordered_set>
#include <unordered_map>
#include <system_error>
#if defined(_WIN32)
#include <windows.h>
#endif
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
#ifdef ROXAL_ENABLE_GRPC
#include "ModuleGrpc.h"
#endif
#ifdef ROXAL_ENABLE_DDS
#include "dds/ModuleDDS.h"
#endif
#ifdef ROXAL_ENABLE_REGEX
#include "RegexWrapper.h"
#endif
#include "SimpleMarkSweepGC.h"
#include "RTCallbackManager.h"
#include <Eigen/Dense>
#include <core/types.h>
#include <core/common.h>
#include <core/AST.h>
#include <fstream>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <atomic>

using namespace roxal;

// Static thread_local definitions for native call timing instrumentation
thread_local TimePoint VM::nativeCallDeadline_ { TimePoint::max() };
thread_local UnicodeString VM::nativeCallContext_;
thread_local std::string VM::nativeCallOverrun_;
thread_local bool VM::onDataflowThread_ { false };

std::string VM::consumeNativeCallOverrun()
{
    std::string result;
    result.swap(nativeCallOverrun_);
    return result;
}

namespace {

// Staging slots for CLI-provided limits. These are written before the VM
// singleton exists and copied into the instance once it is constructed.
std::atomic<size_t> configuredStackLimit{VM::DefaultMaxStack};
std::atomic<size_t> configuredCallFrameLimit{VM::DefaultMaxCallFrames};
// Tracks whether VM::instance() has already materialized the singleton so
// later configureStackLimits() calls can update it in-place.
std::atomic<bool> vmConstructed{false};
std::atomic<VM::CacheMode> configuredCacheMode{VM::CacheMode::Normal};
std::mutex configuredModulePathsMutex;
std::vector<std::string> configuredModulePaths;

void appendUnique(std::vector<std::string>& target, const std::vector<std::string>& additions)
{
    for (const auto& path : additions) {
        if (std::find(target.begin(), target.end(), path) == target.end())
            target.push_back(path);
    }
}

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

std::filesystem::path resolveExecutablePath()
{
#if defined(_WIN32)
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (len == 0)
            return {};
        if (len < buffer.size()) {
            buffer.resize(len);
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(std::filesystem::path(buffer), ec);
    if (!ec)
        return canonical;
    return std::filesystem::path(buffer);
#else
    std::error_code ec;
    auto link = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec)
        return {};
    std::error_code canonEc;
    auto canonical = std::filesystem::weakly_canonical(link, canonEc);
    if (!canonEc)
        return canonical;
    return link;
#endif
}

} // namespace

std::filesystem::path VM::executablePath()
{
    return resolveExecutablePath();
}

std::vector<std::string> VM::defaultModuleSearchPaths()
{
    std::vector<std::string> defaults;
    const auto exePath = resolveExecutablePath();
    if (exePath.empty())
        return defaults;

    const auto exeDir = exePath.parent_path();

    auto addIfDir = [&](const std::filesystem::path& candidate) {
        if (candidate.empty())
            return;
        std::error_code ec;
        auto normalized = std::filesystem::weakly_canonical(candidate, ec);
        const auto& resolved = ec ? candidate : normalized;
        ec.clear();
        if (!std::filesystem::is_directory(resolved, ec) || ec)
            return;
        auto pathStr = resolved.string();
        if (std::find(defaults.begin(), defaults.end(), pathStr) == defaults.end())
            defaults.push_back(pathStr);
    };

    addIfDir(exeDir / ".." / "share" / "roxal");
    addIfDir(exeDir / ".." / "modules");
    addIfDir(exeDir / "modules");

    return defaults;
}

std::string VM::versionString()
{
    std::string version =
    #ifdef ROXAL_VERSION
        ROXAL_VERSION;
    #else
        "unknown";
    #endif

    if (version.empty())
        version = "unknown";

    const std::string prerelease =
#ifdef ROXAL_PRERELEASE
        ROXAL_PRERELEASE;
#else
        "";
#endif

    std::string gitHash =
#ifdef ROXAL_GIT_HASH
        ROXAL_GIT_HASH;
#else
        "unknown";
    #endif
    if (gitHash.empty())
        gitHash = "unknown";

    std::string fullVersion = version;
    if (!prerelease.empty())
        fullVersion += "-" + prerelease;
    fullVersion += "+" + gitHash;

    return fullVersion;
}

// Map BuiltinType to ValueType for automatic type conversion at call sites.
// Returns nullopt for types that don't support automatic conversion (e.g.
// Object, Actor — these are user-defined types where toType() doesn't apply).
static std::optional<ValueType> builtinToValueType(type::BuiltinType bt)
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
        case type::BuiltinType::Signal:  return ValueType::Signal;
        case type::BuiltinType::Enum:    return ValueType::Enum;
        default:
            return std::nullopt; // Object, Actor, Func, Type — no auto-conversion
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
        case ValueType::Tensor: {
            static ptr<type::Type> tensorType;
            if (!tensorType) {
                tensorType = make_ptr<type::Type>(type::BuiltinType::Func);
                tensorType->func = type::Type::FuncType();
                PT pShape(toUnicodeString("shape"));
                pShape.type = make_ptr<type::Type>(type::BuiltinType::List);
                pShape.hasDefault = false;
                PT pData(toUnicodeString("data"));
                pData.type = make_ptr<type::Type>(type::BuiltinType::List);
                pData.hasDefault = true;
                PT pDtype(toUnicodeString("dtype"));
                pDtype.type = make_ptr<type::Type>(type::BuiltinType::String);
                pDtype.hasDefault = true;
                tensorType->func->params = {pShape, pData, pDtype};
            }
            return tensorType;
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


void VM::configureStackLimits(size_t stackSize, size_t callFrameLimit)
{
    if (stackSize == 0 || callFrameLimit == 0)
        throw std::invalid_argument("Stack size and call frame limit must be greater than zero.");

    configuredStackLimit.store(stackSize, std::memory_order_relaxed);
    configuredCallFrameLimit.store(callFrameLimit, std::memory_order_relaxed);

    if (vmConstructed.load(std::memory_order_acquire)) {
        VM::instance().setStackLimits(stackSize, callFrameLimit);
    }
}


void VM::configureCacheMode(CacheMode mode)
{
    configuredCacheMode.store(mode, std::memory_order_relaxed);

    if (vmConstructed.load(std::memory_order_acquire)) {
        VM::instance().setCacheMode(mode);
    }
}

void VM::configureModulePaths(const std::vector<std::string>& modulePaths)
{
    std::lock_guard<std::mutex> lock(configuredModulePathsMutex);
    appendUnique(configuredModulePaths, modulePaths);

    if (vmConstructed.load(std::memory_order_acquire)) {
        VM::instance().appendModulePaths(modulePaths);
    }
}


void VM::setStackLimits(size_t stackSize, size_t callFrameLimitValue)
{
    if (stackSize == 0 || callFrameLimitValue == 0)
        throw std::invalid_argument("Stack size and call frame limit must be greater than zero.");

    stackLimit = stackSize;
    callFrameLimit = callFrameLimitValue;
}


// Identifies which param indices require closure default evaluation.
// Returns vector of param indices that need closure evaluation (empty if none).
std::vector<size_t> VM::getClosureDefaultParamIndices(
    ptr<type::Type> funcType,
    const std::vector<Value>& defaults,
    const CallSpec& callSpec,
    const std::map<int32_t, Value>& paramDefaultFuncs)
{
    std::vector<size_t> indices;
    const auto& params = funcType->func.value().params;
    auto paramPositions = callSpec.paramPositions(funcType, true);

    for (size_t pi = 0; pi < params.size(); ++pi) {
        int pos = paramPositions[pi];
        // If arg is supplied explicitly (pos >= 0) or has static default (pi < defaults.size()),
        // no closure evaluation needed
        if (pos >= 0 || pi < defaults.size())
            continue;
        // Check if this param has a closure default
        auto it = paramDefaultFuncs.find(params[pi]->nameHashCode);
        if (it != paramDefaultFuncs.end())
            indices.push_back(pi);
    }
    return indices;
}

// Marshal args without evaluating closure defaults.
// For params that need closure evaluation, stores nilVal() placeholder.
size_t VM::marshalArgsPartial(ptr<type::Type> funcType,
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

    for (size_t pi = 0; pi < params.size(); ++pi) {
        Value arg;
        bool needsClosureDefault = false;
        int pos = paramPositions[pi];
        if (pos >= 0)
            arg = *(&(*thread->stackTop) - callSpec.argCount + pos);
        else if (pi < defaults.size())
            arg = defaults[pi];
        else {
            // Check if this param has a closure default
            auto it = paramDefaultFuncs.find(params[pi]->nameHashCode);
            if (it != paramDefaultFuncs.end()) {
                // Closure default - store placeholder (will be filled by continuation)
                arg = Value::nilVal();
                needsClosureDefault = true;
            } else {
                arg = Value::nilVal();
            }
        }

        // Skip type conversion for params that will be filled by closure evaluation
        if (!needsClosureDefault && params[pi].has_value() && params[pi]->type.has_value()) {
            auto vt = builtinToValueType(params[pi]->type.value()->builtin);
            if (vt.has_value()) {
                bool strictConv = false;
                if (thread->frames.size() >= 1)
                    strictConv = (thread->frames.end()-1)->strict;
                arg = toType(vt.value(), arg, strictConv);
            }
        }
        out[idx++] = arg;
    }
    return idx;
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
            // Closure defaults are handled via continuation mechanism in callNativeFn()
            // before marshalArgs() is called. If we reach here with a closure default,
            // something is wrong.
            auto it = paramDefaultFuncs.find(params[pi]->nameHashCode);
            assert(it == paramDefaultFuncs.end() &&
                   "Closure default params should be handled via continuation, not marshalArgs");
            arg = Value::nilVal();
        }

        if (params[pi].has_value() && params[pi]->type.has_value()) {
            auto vt = builtinToValueType(params[pi]->type.value()->builtin);
            if (vt.has_value()) {
                bool strictConv = false;
                if (thread->frames.size() >= 1)
                    strictConv = (thread->frames.end()-1)->strict;
                arg = toType(vt.value(), arg, strictConv);
            }
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
                      const Value& declFunction,
                      uint32_t resolveArgMask)
{
    Thread* currentThread = thread.get();
    auto stackDepthBefore = thread ? static_cast<size_t>(thread->stackTop - thread->stack.begin()) : 0;
    auto frameDepthBefore = thread ? thread->frames.size() : 0;
    struct NativeCallGuard {
        Thread* t;
        explicit NativeCallGuard(Thread* thread) : t(thread) {
            if (t)
                t->nativeCallDepth++;
        }
        ~NativeCallGuard() {
            if (t)
                t->nativeCallDepth--;
        }
    } nativeCallGuard(currentThread);

    try {
        if (funcType) {
            size_t paramCount = funcType->func.value().params.size() + (includeReceiver ? 1 : 0);
            static const std::map<int32_t, Value> emptyDefaults;
            const auto& paramDefaults = declFunction.isNonNil() ? asFunction(declFunction)->paramDefaultFunc : emptyDefaults;

            // Check if any params need closure default evaluation
            auto closureIndices = getClosureDefaultParamIndices(funcType, defaults, callSpec, paramDefaults);

            if (!closureIndices.empty()) {
                // Defer native call: set up state and push first closure default frame
                auto& state = thread->nativeDefaultParamState;
                state.active = true;
                state.nativeFunc = fn;
                state.funcType = funcType;
                state.staticDefaults = defaults;
                state.callSpec = callSpec;
                state.includeReceiver = includeReceiver;
                state.receiver = receiver;
                state.declFunction = declFunction;
                state.resolveArgMask = resolveArgMask;
                state.closureParamIndices = std::move(closureIndices);
                state.nextClosureIndex = 0;
                state.paramDefaultFuncs = paramDefaults;
                state.originalArgCount = callSpec.argCount;

                // Partially marshal args (without evaluating closure defaults)
                state.argsBuffer.resize(paramCount);
                marshalArgsPartial(funcType, defaults, callSpec, state.argsBuffer.data(),
                                   includeReceiver, receiver, paramDefaults);

                // Push first closure default frame
                size_t paramIdx = state.closureParamIndices[0];
                const auto& params = funcType->func.value().params;
                auto it = paramDefaults.find(params[paramIdx]->nameHashCode);
                Value defFunc = it->second;
                Value defClosure = Value::closureVal(defFunc);

                // Check for captured variables (not allowed in default params)
                if (asClosure(defClosure)->upvalues.size() > 0) {
                    state.clear();
                    auto paramName = params[paramIdx]->name;
                    runtimeError("Captured variables in default parameter '" + toUTF8StdString(paramName) +
                                "' value expressions are not allowed.");
                    return false;
                }

                // Set up continuation callback that will process each default value
                auto& cont = thread->nativeContinuation;
                cont.active = true;
                cont.state = Value::nilVal();  // State is in nativeDefaultParamState
                cont.resultSlot = nullptr;     // We handle stack cleanup ourselves
                cont.onComplete = [](VM& vm, Value defaultValue) -> bool {
                    return vm.processNativeDefaultParamDispatch(defaultValue);
                };

                // Push closure and call it using continuation mechanism
                push(defClosure);
                if (!call(asClosure(defClosure), CallSpec(0))) {
                    state.clear();
                    cont.clear();
                    return false;
                }
                thread->frames.back().isContinuationCallback = true;
                return true;  // Deferred - execute() will continue with closure frame
            }

            // Check if any params need async user-defined conversion (operator->T or constructor)
            {
                const auto& params = funcType->func.value().params;
                auto paramPositions = callSpec.paramPositions(funcType, true);
                std::vector<size_t> asyncConvIndices;
                for (size_t pi = 0; pi < params.size(); ++pi) {
                    if (!params[pi].has_value() || !params[pi]->type.has_value())
                        continue;
                    int pos = paramPositions[pi];
                    if (pos < 0) continue; // default or missing — sync path handles it
                    Value arg = *(&(*thread->stackTop) - callSpec.argCount + pos);
                    if (needsAsyncConversion(arg, params[pi]->type.value()))
                        asyncConvIndices.push_back(pi);
                }

                if (!asyncConvIndices.empty()) {
                    // Defer: marshal args without converting async params, then push conversion frames
                    auto& state = thread->nativeParamConversionState;
                    state.active = true;
                    state.nativeFunc = fn;
                    state.funcType = funcType;
                    state.callSpec = callSpec;
                    state.includeReceiver = includeReceiver;
                    state.receiver = receiver;
                    state.declFunction = declFunction;
                    state.resolveArgMask = resolveArgMask;
                    state.originalArgCount = callSpec.argCount;

                    // Marshal args — sync conversions happen here; async params get default
                    // toType result (e.g. "<object Foo>" for string) which we'll overwrite
                    state.argsBuffer.resize(paramCount);
                    // For async params, store original value (skip toType which may throw)
                    // Use marshalArgsPartial which handles defaults but still does toType;
                    // override async param slots with original values afterward
                    marshalArgsPartial(funcType, defaults, callSpec, state.argsBuffer.data(),
                                       includeReceiver, receiver, paramDefaults);
                    // Overwrite async param slots with original (unconverted) values
                    for (size_t pi : asyncConvIndices) {
                        int pos = paramPositions[pi];
                        if (pos >= 0) {
                            Value arg = *(&(*thread->stackTop) - callSpec.argCount + pos);
                            state.argsBuffer[pi + (includeReceiver ? 1 : 0)] = arg;
                        }
                    }

                    state.conversionParamIndices = std::move(asyncConvIndices);
                    state.nextConversionIndex = 0;

                    // Set up continuation
                    auto& cont = thread->nativeContinuation;
                    cont.active = true;
                    cont.state = Value::nilVal();
                    cont.resultSlot = nullptr;
                    cont.onComplete = [](VM& vm, Value convertedValue) -> bool {
                        return vm.processNativeParamConversion(convertedValue);
                    };

                    // Push first conversion frame
                    size_t firstParamIdx = state.conversionParamIndices[0];
                    size_t firstBufIdx = firstParamIdx + (includeReceiver ? 1 : 0);
                    const auto& firstParamType = params[firstParamIdx]->type.value();
                    if (!pushParamConversionFrame(state.argsBuffer[firstBufIdx], firstParamType)) {
                        state.clear();
                        cont.clear();
                        runtimeError("Failed to set up parameter conversion");
                        return false;
                    }
                    return true;  // Deferred — execute() continues with conversion frame
                }
            }

            // No async conversions needed - proceed with immediate call (original code path)
            constexpr size_t Small = 8;
            Value stackArgs[Small];
            std::vector<Value> heapArgs;
            Value* buf = stackArgs;
            if (paramCount > Small) {
                heapArgs.resize(paramCount);
                buf = heapArgs.data();
            }
            size_t actual = marshalArgs(funcType, defaults, callSpec, buf, includeReceiver, receiver, paramDefaults);

            // Non-blocking resolution of future args indicated by mask
            if (resolveArgMask) {
                for (size_t i = 0; i < actual && resolveArgMask >> i; ++i) {
                    if ((resolveArgMask & (1u << i)) && isFuture(buf[i])) {
                        auto s = buf[i].tryResolveFuture();
                        if (s == FutureStatus::Pending) {
                            thread->awaitedFuture = buf[i];
                            return true;
                        }
                        if (s == FutureStatus::Error) return false;
                    }
                }
            }

            ArgsView view{buf, actual};
            Value result;
            if (nativeCallTimingEnabled_ && nativeCallDeadline_ != TimePoint::max()) {
                auto before = TimePoint::currentTime();
                result = fn(*this, view);
                auto elapsed = TimePoint::currentTime() - before;
                auto remaining = nativeCallDeadline_ - before;
                if (elapsed > remaining) {
                    auto name = toUTF8StdString(nativeCallContext_);
                    nativeCallOverrun_ = "'" + name + "' took "
                        + std::to_string((long)elapsed.microSecs()) + "us (budget "
                        + std::to_string((long)remaining.microSecs()) + "us)";
                }
            } else {
                result = fn(*this, view);
            }
            bool unwound = false;
            if (thread) {
                auto stackDepthAfter = static_cast<size_t>(thread->stackTop - thread->stack.begin());
                auto frameDepthAfter = thread->frames.size();
                unwound = stackDepthAfter < stackDepthBefore || frameDepthAfter < frameDepthBefore;
            }
            // Skip stack cleanup only when THIS native call pushed frames (set up a
            // continuation or deferred call). Frame depth increase distinguishes this from
            // nested native calls during an outer continuation (e.g., len() inside
            // operator->string called via print()'s continuation — len should clean up normally).
            bool thisCallPushedFrames = thread && thread->frames.size() > frameDepthBefore;
            if (currentThread && (currentThread->exceptionJumpPending.load(std::memory_order_relaxed) || unwound)) {
                currentThread->exceptionJumpPending.store(false, std::memory_order_relaxed);
                return true;
            }
            if (thisCallPushedFrames) {
                // Native set up a continuation — ensure resultSlot/stackBase are set
                // so processContinuationDispatch (or unwindFrame on exception) can
                // clean up the original callee+args area.
                auto& cont = thread->nativeContinuation;
                if (cont.active && !cont.resultSlot) {
                    size_t calleePos = stackDepthBefore - callSpec.argCount - 1;
                    cont.resultSlot = &*(thread->stack.begin() + calleePos);
                    cont.stackBase = thread->stack.begin() + calleePos + 1;
                }
                return true;
            }
            *(thread->stackTop - callSpec.argCount - 1) = result;
            popN(callSpec.argCount);
            return true;
        } else {
            Value* base = &(*thread->stackTop) - callSpec.argCount - (includeReceiver ? 1 : 0);
            size_t actual = static_cast<size_t>(callSpec.argCount + (includeReceiver ? 1 : 0));

            // Non-blocking resolution of future args indicated by mask
            if (resolveArgMask) {
                for (size_t i = 0; i < actual && resolveArgMask >> i; ++i) {
                    if ((resolveArgMask & (1u << i)) && isFuture(base[i])) {
                        auto s = base[i].tryResolveFuture();
                        if (s == FutureStatus::Pending) {
                            thread->awaitedFuture = base[i];
                            return true;
                        }
                        if (s == FutureStatus::Error) return false;
                    }
                }
            }

            ArgsView view{base, actual};
            Value result;
            if (nativeCallTimingEnabled_ && nativeCallDeadline_ != TimePoint::max()) {
                auto before = TimePoint::currentTime();
                result = fn(*this, view);
                auto elapsed = TimePoint::currentTime() - before;
                auto remaining = nativeCallDeadline_ - before;
                if (elapsed > remaining) {
                    auto name = toUTF8StdString(nativeCallContext_);
                    nativeCallOverrun_ = "'" + name + "' took "
                        + std::to_string((long)elapsed.microSecs()) + "us (budget "
                        + std::to_string((long)remaining.microSecs()) + "us)";
                }
            } else {
                result = fn(*this, view);
            }
            bool unwound = false;
            if (thread) {
                auto stackDepthAfter = static_cast<size_t>(thread->stackTop - thread->stack.begin());
                auto frameDepthAfter = thread->frames.size();
                unwound = stackDepthAfter < stackDepthBefore || frameDepthAfter < frameDepthBefore;
            }
            bool thisCallPushedFrames2 = thread && thread->frames.size() > frameDepthBefore;
            if (currentThread && (currentThread->exceptionJumpPending.load(std::memory_order_relaxed) || unwound)) {
                currentThread->exceptionJumpPending.store(false, std::memory_order_relaxed);
                return true;
            }
            if (thisCallPushedFrames2) {
                auto& cont = thread->nativeContinuation;
                if (cont.active && !cont.resultSlot) {
                    size_t calleePos = stackDepthBefore - callSpec.argCount - 1;
                    cont.resultSlot = &*(thread->stack.begin() + calleePos);
                    cont.stackBase = thread->stack.begin() + calleePos + 1;
                }
                return true;
            }
            *(thread->stackTop - callSpec.argCount - 1) = result;
            popN(callSpec.argCount);
            return true;
        }
    } catch (std::exception& e) {
        runtimeError(e.what());
        return false;
    }
}

void roxal::scheduleEventHandlers(Value eventWeak, ObjEventType* ev, Value eventInstance, TimePoint when)
{
    Thread::PendingEvent baseEvent;
    baseEvent.when = when;
    baseEvent.eventType = eventWeak;
    baseEvent.instance = eventInstance;

    // Track which threads have already been scheduled for this event.
    // processPendingEvents() calls ALL handlers for an event type when processing
    // a single pending event, so we only need to schedule once per thread.
    std::unordered_set<Thread*> scheduledThreads;

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

        // Skip if we've already scheduled an event to this thread
        if (scheduledThreads.count(handlerThread.get()) > 0) {
            ++it;
            continue;
        }

        Value key = eventWeak;
        auto regIt = handlerThread->eventHandlers.find(key);
        if (regIt == handlerThread->eventHandlers.end()) {
            ++it;
            continue;
        }

        // Check if any handler on this thread should receive the event
        // (considering matchValue and targetFilter filters)
        bool shouldSchedule = false;
        for (const auto& reg : regIt->second) {
            if (!reg.closure.isAlive())
                continue;
            if (reg.matchValue.has_value()) {
                if (!isEventInstance(eventInstance))
                    continue;
                auto* inst = asEventInstance(eventInstance);
                // Look up the "value" property for signal change events
                static const int32_t valueHash = toUnicodeString("value").hashCode();
                auto it = inst->payload.find(valueHash);
                if (it == inst->payload.end())
                    continue;
                const Value& sample = it->second;
                if (!sample.equals(reg.matchValue.value(), /*strict=*/false))
                    continue;
            }
            // Check target filter (for 'where evt.target == <value>')
            if (reg.targetFilter.has_value()) {
                if (!isEventInstance(eventInstance))
                    continue;
                auto* inst = asEventInstance(eventInstance);
                // Look up the "target" property
                static const int32_t targetHash = toUnicodeString("target").hashCode();
                auto it = inst->payload.find(targetHash);
                if (it == inst->payload.end())
                    continue;
                const Value& eventTarget = it->second;
                if (!eventTarget.equals(reg.targetFilter.value(), /*strict=*/false))
                    continue;
            }
            shouldSchedule = true;
            break;
        }

        if (shouldSchedule) {
            scheduledThreads.insert(handlerThread.get());
            Thread::PendingEvent pending = baseEvent;
            pending.sequence = handlerThread->nextPendingEventId.fetch_add(1, std::memory_order_relaxed);
            handlerThread->pendingEvents.push(pending);
            handlerThread->pendingEventCount.fetch_add(1, std::memory_order_release);
            handlerThread->wake();
        }

        ++it;
    }
}




VM::VM()
    : lineMode(false)
    , cacheModeSetting(CacheMode::Normal)
{
    stackLimit = configuredStackLimit.load(std::memory_order_relaxed);
    callFrameLimit = configuredCallFrameLimit.load(std::memory_order_relaxed);
    cacheModeSetting = configuredCacheMode.load(std::memory_order_relaxed);

    SimpleMarkSweepGC::instance().setVM(this);

    assert(sizeof(Value) == sizeof(uint64_t)); // ensure Value is 64bit

    runtimeErrorFlag = false;
    exitRequested = false;
    exitCodeValue = 0;

    for (auto& counter : opcodeProfileCounts)
        counter.store(0, std::memory_order_relaxed);

    thread = nullptr;
    initString = Value::stringVal(UnicodeString("init"));

    // Pre-hash operator method names for fast dispatch
    auto makeOpHashes = [](const char* sym) -> OperatorHashes {
        return {
            (UnicodeString("operator") + sym).hashCode(),
            (UnicodeString("loperator") + sym).hashCode(),
            (UnicodeString("roperator") + sym).hashCode()
        };
    };
    opHashAdd = makeOpHashes("+");
    opHashSub = makeOpHashes("-");
    opHashMul = makeOpHashes("*");
    opHashDiv = makeOpHashes("/");
    opHashMod = makeOpHashes("%");
    opHashEq  = makeOpHashes("==");
    opHashNe  = makeOpHashes("!=");
    opHashLt  = makeOpHashes("<");
    opHashGt  = makeOpHashes(">");
    opHashLe  = makeOpHashes("<=");
    opHashGe  = makeOpHashes(">=");
    opHashNeg = UnicodeString("uoperator-").hashCode();
    opHashConvString = UnicodeString("operator->string").hashCode();

    // Eagerly load sys & math modules
    registerBuiltinModule(make_ptr<ModuleSys>());
    registerBuiltinModule(make_ptr<ModuleMath>());

    // Register factories for lazy-loaded modules (loaded on first import)
    #ifdef ROXAL_ENABLE_FILEIO
    lazyModuleRegistry.registerFactory("fileio", []{ return make_ptr<ModuleFileIO>(); });
    #endif
    #ifdef ROXAL_ENABLE_GRPC
    lazyModuleRegistry.registerFactory("grpc", []{ return make_ptr<ModuleGrpc>(); });
    #endif
    #ifdef ROXAL_ENABLE_DDS
    lazyModuleRegistry.registerFactory("dds", []{ return make_ptr<ModuleDDS>(); });
    #endif
    #ifdef ROXAL_ENABLE_REGEX
    lazyModuleRegistry.registerFactory("regex", []{ return make_ptr<ModuleRegex>(); });
    #endif
    #ifdef ROXAL_ENABLE_SOCKET
    lazyModuleRegistry.registerFactory("socket", []{ return make_ptr<ModuleSocket>(); });
    #endif
    #ifdef ROXAL_ENABLE_AI_NN
    lazyModuleRegistry.registerFactory("ai.nn", []{ return make_ptr<ModuleNN>(); });
    #endif
    #ifdef ROXAL_ENABLE_MEDIA
    lazyModuleRegistry.registerFactory("media", []{ return make_ptr<ModuleMedia>(); });
    #endif

    std::vector<std::string> stagedModulePaths;
    {
        std::lock_guard<std::mutex> lock(configuredModulePathsMutex);
        stagedModulePaths = configuredModulePaths;
    }
    appendModulePaths(stagedModulePaths);
    appendModulePaths(VM::defaultModuleSearchPaths());

    // Execute builtin module script for sys & math
    // Other modules' .rox files are executed during lazy loading
    ptr<Thread> initThread = make_ptr<Thread>();
    thread = initThread;
    executeBuiltinModuleScript("sys.rox", getBuiltinModuleType(toUnicodeString("sys")));

    // Export pure Roxal functions from sys module to globals
    // (sys predates the module system and registers symbols directly as globals)
    {
        Value sysModule = getBuiltinModuleType(toUnicodeString("sys"));
        auto& sysVars = asModuleType(sysModule)->vars;
        for (const char* name : {"filter", "map", "reduce"}) {
            auto maybeFunc = sysVars.load(toUnicodeString(name));
            if (maybeFunc.has_value() && isClosure(maybeFunc.value())) {
                globals.storeGlobal(toUnicodeString(name), maybeFunc.value());
            }
        }
        // Export suffix functions and types from sys to globals.
        // If registeredSuffixes is empty (module loaded from cache), rebuild
        // it by scanning function annotations for @suffix.
        ObjModuleType* sysMod = asModuleType(sysModule);
        if (sysMod->registeredSuffixes.empty()) {
            sysVars.forEach([&](const VariablesMap::NameValue& nv) {
                if (isClosure(nv.second)) {
                    ObjFunction* fn = asFunction(asClosure(nv.second)->function);
                    for (const auto& annot : fn->annotations) {
                        if (annot->name == "suffix" && annot->args.size() == 1) {
                            if (auto s = dynamic_ptr_cast<ast::Str>(annot->args[0].second))
                                sysMod->registeredSuffixes[s->str] = nv.first;
                        }
                    }
                }
            });
        }
        for (const auto& [suffix, funcName] : sysMod->registeredSuffixes) {
            auto maybeFunc = sysVars.load(funcName);
            if (maybeFunc.has_value())
                globals.storeGlobal(funcName, maybeFunc.value());
        }
        // Also export quantity and _dimension types
        for (const char* name : {"quantity", "_dimension"}) {
            auto maybeType = sysVars.load(toUnicodeString(name));
            if (maybeType.has_value())
                globals.storeGlobal(toUnicodeString(name), maybeType.value());
        }
    }

    executeBuiltinModuleScript("math.rox", getBuiltinModuleType(toUnicodeString("math")));
    thread = nullptr;

    // Reset thread ids so the first real VM thread starts at 2
    Thread::resetIdCounter(1);

    // Initialize dataflow engine as builtin actor
    //  NB: the math module creates _vecSignal example, which may have created dataflow instance already
    //      (which is the reason math is eagerly loaded still - the init order appears to impact clock signals)
    dataflowEngine = df::DataflowEngine::instance();
    dataflowEngine->markNetworkModified();
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

    vmConstructed.store(true, std::memory_order_release);

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

    for (auto& entry : builtinMethods) {
        for (auto& method : entry.second) {
            method.second.defaultValues.clear();
            method.second.declFunction = Value::nilVal();
        }
    }
    builtinMethods.clear();

    // Call unloading hook for all loaded modules before clearing
    for (auto& mod : builtinModules) {
        if (mod)
            mod->onModuleUnloading(*this);
    }
    builtinModules.clear();
    lazyModuleRegistry.clear();

    conditionalInterruptClosure = Value::nilVal();
    replModuleValue = Value::nilVal();
    pendingRTClosure_ = Value::nilVal();

    if (dataflowEngine)
        dataflowEngine->clear();


    // Release the main thread before final garbage collection so any
    // objects referenced through its stacks and handlers can be reclaimed
    thread.reset();

    // Release REPL thread resources before reporting potential leaks
    replThread.reset();

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
        if (std::find(this->modulePaths.begin(), this->modulePaths.end(), path) == this->modulePaths.end()) {
            this->modulePaths.push_back(path);
            #ifdef ROXAL_ENABLE_GRPC
            if (grpcModule)
                grpcModule->addProtoPath(path);
            #endif
        }
    }
}

void VM::setScriptArguments(const std::vector<std::string>& args)
{
    scriptArguments = args;

    // Update the global 'args' list
    std::vector<Value> argValues;
    argValues.reserve(args.size());
    for (const auto& arg : args) {
        argValues.push_back(Value::stringVal(toUnicodeString(arg)));
    }
    globals.storeGlobal(toUnicodeString("args"), Value::listVal(argValues));
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




ExecutionStatus VM::run(std::istream& source, const std::string& name)
{
    // Setup: compile and prepare the initial call frame
    ExecutionStatus setupResult = setup(source, name);
    if (setupResult != ExecutionStatus::OK)
        return setupResult;

    // Establish RT main thread for script mode (runs on current C++ thread, not spawned)
    auto& rtMgr = RTCallbackManager::instance();
    if (!rtMgr.isMainThreadSet()) {
        rtMgr.setMainThread();
    }

    // Execute directly on the host thread
    ExecutionStatus result = ExecutionStatus::OK;
    inSynchronousExecution_.store(true, std::memory_order_release);
    try {
        auto [execResult, value] = execute();
        result = execResult;
    } catch (...) {
        // Ensure cleanup runs even if execute() throws (e.g. from queueCall
        // runtime errors).  Without this, joinAllThreads/thread.reset are
        // skipped, causing static/thread_local destruction order issues.
        inSynchronousExecution_.store(false, std::memory_order_release);
        joinAllThreads();
        thread.reset();
        freeObjects();
        throw;
    }
    inSynchronousExecution_.store(false, std::memory_order_release);

    // Join any other threads spawned during execution (actors, etc.)
    ExecutionStatus joinResult = joinAllThreads();

    if (joinResult != ExecutionStatus::OK || runtimeErrorFlag.load())
        result = ExecutionStatus::RuntimeError;

    if (exitRequested.load())
        result = ExecutionStatus::OK;

    #if defined(DEBUG_TRACE_EXECUTION)
    // globals dump disabled (VariablesMap API changed)
    #endif

    thread.reset();
    freeObjects();

    return result;
}


ExecutionStatus VM::setup(std::istream& source, const std::string& name)
{
    Value function { Value::nilVal() }; // ObjFunction

    runtimeErrorFlag = false;

    try {
        RoxalCompiler compiler {};
        compiler.setOutputBytecodeDisassembly(outputBytecodeDisassembly);
        compiler.setCacheReadEnabled(cacheReadsEnabled());
        compiler.setCacheWriteEnabled(cacheWritesEnabled());
        compiler.setModulePaths(modulePaths);
        compiler.setModuleResolverVM(this);

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
        return ExecutionStatus::CompileError;
    }

    if (function.isNil())
        return ExecutionStatus::CompileError;

    Value closureValue { Value::closureVal(function) };

    ptr<Thread> mainThread = make_ptr<Thread>();
    threads.store(mainThread->id(), mainThread);
    thread = mainThread;

    resetStack();
    push(closureValue);
    if (!call(asClosure(closureValue), CallSpec(0)))
        return ExecutionStatus::RuntimeError;

    return ExecutionStatus::OK;
}


std::pair<ExecutionStatus, Value> VM::runFor(TimeDuration duration)
{
    // Guard: if run()/runLine() is executing synchronously (e.g. --setup script),
    // don't enter execute() — the synchronous path already owns the VM.
    if (inSynchronousExecution_.load(std::memory_order_acquire))
        return { ExecutionStatus::OK, Value::nilVal() };

    // Check for pending closure from setupLine()
    auto state = rtState_.load(std::memory_order_acquire);

    if (state == RTState::Ready) {
        // Pick up the compiled closure
        Value closure;
        {
            std::lock_guard<std::mutex> lk(rtMutex_);
            closure = pendingRTClosure_;
            pendingRTClosure_ = Value::nilVal();
        }

        // Use persistent REPL thread
        if (!replThread)
            replThread = make_ptr<Thread>();
        thread = replThread;

        auto& rtMgr = RTCallbackManager::instance();
        if (!rtMgr.isMainThreadSet())
            rtMgr.setMainThread();

        resetStack();
        push(closure);
        if (!call(asClosure(closure), CallSpec(0))) {
            rtState_.store(RTState::Idle, std::memory_order_release);
            rtCondVar_.notify_one();
            return { ExecutionStatus::RuntimeError, Value::nilVal() };
        }
        rtState_.store(RTState::Executing, std::memory_order_release);

    } else if (state == RTState::Executing || state == RTState::Yielded) {
        // Resume previous work
        thread = replThread;

    } else {
        // Idle — fall through to check setup() path
    }

    // If no setupLine work, check for setup() path (existing behavior)
    if (state == RTState::Idle) {
        if (!hasMoreWork())
            return { ExecutionStatus::OK, Value::nilVal() };
        // thread is already set by setup()
    }

    // Execute with time budget
    auto deadline = TimePoint::currentTime() + duration;
    auto [status, value] = execute(deadline);

    // Only manage RT state if we're in the setupLine path
    if (state != RTState::Idle) {
        if (status == ExecutionStatus::Yielded) {
            rtState_.store(RTState::Yielded, std::memory_order_release);
        } else {
            // Completed or error — transition to Idle and wake setupLine()
            rtState_.store(RTState::Idle, std::memory_order_release);
            rtCondVar_.notify_one();
        }
    }

    if (runtimeErrorFlag.load())
        return { ExecutionStatus::RuntimeError, Value::nilVal() };

    return { status, value };
}

bool VM::hasMoreWork() const
{
    if (!thread) return false;
    if (thread->frames.empty()) return false;
    return true;
}

bool VM::isBlocked() const
{
    if (!thread) return false;
    return thread->threadSleep.load() || thread->awaitedFuture.isNonNil();
}

TimePoint VM::blockedUntil() const
{
    if (!thread) return TimePoint::max();
    if (thread->threadSleep.load()) return thread->threadSleepUntil.load();
    return TimePoint::max();  // future-blocked has no known deadline
}


ExecutionStatus VM::runLine(std::istream& linestream,
                                  bool replMode,
                                  const std::string& sourceNameOverride)
{
    Value function { Value::nilVal() }; // ObjFunction

    runtimeErrorFlag = false;

    static RoxalCompiler compiler {};
    compiler.setOutputBytecodeDisassembly(outputBytecodeDisassembly);
    compiler.setCacheReadEnabled(cacheReadsEnabled());
    compiler.setCacheWriteEnabled(cacheWritesEnabled());
    compiler.setModulePaths(modulePaths);
    compiler.setReplMode(replMode);
    compiler.setModuleResolverVM(this);

    try {
        function = compiler.compile(linestream, "cli", replModuleValue, sourceNameOverride);

    } catch (std::exception& e) {
        return ExecutionStatus::CompileError;
    }

    if (function.isNil())
        return ExecutionStatus::CompileError;

    if (replModuleValue.isNil())
        replModuleValue = asFunction(function)->moduleType.strongRef();

    lineMode = true;
    lineStream = &linestream;
    compiler.setReplMode(false);

    Value closure = Value::closureVal(function); // ObjClosure

    if (!replThread) {
        replThread = make_ptr<Thread>();
    }

    thread = replThread;

    // Establish RT main thread for REPL mode (runs on current C++ thread, not spawned)
    auto& rtMgr = RTCallbackManager::instance();
    if (!rtMgr.isMainThreadSet()) {
        rtMgr.setMainThread();
    }

    resetStack();

    inSynchronousExecution_.store(true, std::memory_order_release);
    auto resultPair = invokeClosure(asClosure(closure), {});
    inSynchronousExecution_.store(false, std::memory_order_release);

    ExecutionStatus result = resultPair.first;
    if (runtimeErrorFlag.load())
        result = ExecutionStatus::RuntimeError;

    #if defined(DEBUG_TRACE_EXECUTION)
    // globals dump disabled (VariablesMap API changed)
    #endif

    thread.reset();

    return result;
}

ExecutionStatus VM::setupLine(std::istream& linestream,
                              bool replMode,
                              const std::string& sourceNameOverride)
{
    // Persistent compiler (same pattern as runLine)
    static RoxalCompiler compiler {};
    compiler.setOutputBytecodeDisassembly(outputBytecodeDisassembly);
    compiler.setCacheReadEnabled(cacheReadsEnabled());
    compiler.setCacheWriteEnabled(cacheWritesEnabled());
    compiler.setModulePaths(modulePaths);
    compiler.setReplMode(replMode);
    compiler.setModuleResolverVM(this);

    Value function { Value::nilVal() };
    runtimeErrorFlag = false;

    try {
        function = compiler.compile(linestream, "cli", replModuleValue, sourceNameOverride);
    } catch (std::exception& e) {
        return ExecutionStatus::CompileError;
    }

    if (function.isNil())
        return ExecutionStatus::CompileError;

    if (replModuleValue.isNil())
        replModuleValue = asFunction(function)->moduleType.strongRef();

    compiler.setReplMode(false);

    Value closure = Value::closureVal(function);

    // Hand off to RT thread
    {
        std::unique_lock<std::mutex> lk(rtMutex_);
        // Wait for previous work to finish
        rtCondVar_.wait(lk, [this]{
            return rtState_.load(std::memory_order_acquire) == RTState::Idle;
        });
        pendingRTClosure_ = closure;
        rtState_.store(RTState::Ready, std::memory_order_release);
    }
    rtCondVar_.notify_one();

    return ExecutionStatus::OK;
}

void VM::waitForRTCompletion()
{
    std::unique_lock<std::mutex> lk(rtMutex_);
    rtCondVar_.wait(lk, [this]{
        return rtState_.load(std::memory_order_acquire) == RTState::Idle;
    });
}

ObjModuleType* VM::replModuleType() const
{
    if (replModuleValue.isNil())
        return nullptr;
    return asModuleType(replModuleValue);
}

thread_local ptr<Thread> VM::thread;

bool VM::call(ObjClosure* closure, const CallSpec& callSpec)
{

    // closure,frame pair for any param default value 'func' calls
    std::vector<std::pair<Value,CallFrame>> defValFrames {};

    // Check if function has variadic parameter
    assert(asFunction(closure->function)->funcType.has_value());
    ptr<type::Type> calleeType { asFunction(closure->function)->funcType.value() };
    bool hasVariadic = calleeType->func.has_value() && calleeType->func.value().hasVariadic();
    size_t regularArity = asFunction(closure->function)->arity;

    // fast-path: if callee supplied all arguments by position and none are missing,
    //  nothing special to do (but not for variadic functions which need arg collection)
    bool paramDefaultAndArgsReorderNeeded = hasVariadic || !(callSpec.allPositional && callSpec.argCount == regularArity);

    CallFrame callframe {};
    auto argCount = callSpec.argCount;

    if (paramDefaultAndArgsReorderNeeded) {

        callframe.reorderArgs = callSpec.paramPositions(calleeType, true);

        // Handle variadic args: collect extra args into a list
        Value variadicList = Value::nilVal();
        size_t variadicArgCount = 0;
        // Count regular params that actually have args assigned (vs using defaults)
        // This is needed because named args can leave gaps in regular params
        size_t regularArgsAssigned = 0;
        for (size_t i = 0; i < regularArity && i < callframe.reorderArgs.size(); i++) {
            if (callframe.reorderArgs[i] >= 0) {
                regularArgsAssigned++;
            }
        }
        if (hasVariadic && argCount > regularArgsAssigned) {
            variadicArgCount = argCount - regularArgsAssigned;
            // Create list and collect variadic args from stack
            variadicList = Value::listVal();
            ObjList* list = asList(variadicList);

            // Args are on stack: [callee][arg0][arg1]...[argN-1] <- stackTop
            // Variadic args are the last variadicArgCount args
            Value* variadicStart = &(*(thread->stackTop - variadicArgCount));
            for (size_t i = 0; i < variadicArgCount; i++) {
                list->append(variadicStart[i]);
            }

            // Pop variadic args from stack (they're now in the list)
            for (size_t i = 0; i < variadicArgCount; i++) {
                pop();
            }
            argCount = regularArgsAssigned;  // Now only regular args remain
        }

        // handle execution of default param expression 'func' for params not supplied
        // For variadic functions, we need to enter this block even if regular args are satisfied
        // because we still need to handle the variadic param (either empty list or collected args)
        if (argCount < regularArity || hasVariadic) {
            auto paramTypes { calleeType->func.value().params };
            // for each missing arg
            for(int16_t paramIndex = 0; paramIndex < callframe.reorderArgs.size(); paramIndex++) {
                if (callframe.reorderArgs[paramIndex] == -1) { // -1 -> not supplied in callSpec

                    // lookup param name hash
                    auto param { paramTypes.at(paramIndex) };
                    #ifdef DEBUG_BUILD
                    assert(param.has_value());
                    #endif

                    // For variadic param, create empty list (no default func lookup)
                    if (param.value().variadic) {
                        push(Value::listVal());
                        callframe.reorderArgs[paramIndex] = argCount;
                        argCount++;
                        continue;
                    }

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
                else if (callframe.reorderArgs[paramIndex] == -2) {
                    // -2 indicates variadic param with args to collect
                    // The variadic list was already created and args collected above
                    // Push the list and record its position
                    push(variadicList);
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
        else if (argCount > regularArity && !hasVariadic) {
            runtimeError("Passed "+std::to_string(argCount)+" arguments for function "
                        +toUTF8StdString(asFunction(closure->function)->name)+" which has "
                        +std::to_string(regularArity)+" parameters.");
            return false;
        }

        // Verify final arg count
        if (hasVariadic) {
            assert(argCount == calleeType->func.value().params.size());
        }
        else {
            assert(argCount == regularArity);
        }
    }

    if (thread->frames.size() > callFrameLimit) {
        reportStackOverflow();
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

        if (thread->frames.size() > callFrameLimit) {
            reportStackOverflow();
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
        // Special handling for tensor - supports varargs ints + named params
        if (builtinType == ValueType::Tensor && callSpec.argCount > 0) {
            // Hash codes for named param lookup
            static const uint16_t dtypeHash = toUnicodeString("dtype").hashCode() & 0x7fff;
            static const uint16_t dataHash = toUnicodeString("data").hashCode() & 0x7fff;
            static const uint16_t shapeHash = toUnicodeString("shape").hashCode() & 0x7fff;

            std::vector<int64_t> shape;
            std::vector<double> data;
            TensorDType dtype = TensorDType::Float64;
            bool hasData = false;

            // Process all arguments
            for (size_t i = 0; i < callSpec.argCount; ++i) {
                Value arg = *(argBegin + i);
                bool isNamed = !callSpec.allPositional && !callSpec.args[i].positional;

                if (isNamed) {
                    uint16_t hash = callSpec.args[i].paramNameHash & 0x7fff;
                    if (hash == dtypeHash) {
                        if (isString(arg))
                            dtype = tensorDTypeFromString(toUTF8StdString(asStringObj(arg)->s));
                        else
                            throw std::runtime_error("tensor dtype must be a string");
                    } else if (hash == dataHash) {
                        if (isList(arg)) {
                            auto dataList = asList(arg)->getElements();
                            data.reserve(dataList.size());
                            for (const auto& v : dataList) {
                                if (!v.isNumber())
                                    throw std::runtime_error("tensor data elements must be numeric");
                                data.push_back(v.isInt() ? static_cast<double>(v.asInt()) : v.asReal());
                            }
                            hasData = true;
                        } else {
                            throw std::runtime_error("tensor data must be a list");
                        }
                    } else if (hash == shapeHash) {
                        if (isList(arg)) {
                            auto shapeList = asList(arg)->getElements();
                            shape.reserve(shapeList.size());
                            for (const auto& v : shapeList) {
                                if (!v.isInt())
                                    throw std::runtime_error("tensor shape elements must be integers");
                                shape.push_back(v.asInt());
                            }
                        } else {
                            throw std::runtime_error("tensor shape must be a list");
                        }
                    }
                    // Ignore unknown named params (or could throw)
                } else {
                    // Positional argument
                    if (arg.isInt()) {
                        // Int -> shape dimension
                        shape.push_back(arg.asInt());
                    } else if (isList(arg) && shape.empty()) {
                        // First positional list -> shape list
                        auto shapeList = asList(arg)->getElements();
                        shape.reserve(shapeList.size());
                        for (const auto& v : shapeList) {
                            if (!v.isInt())
                                throw std::runtime_error("tensor shape elements must be integers");
                            shape.push_back(v.asInt());
                        }
                    } else if (isList(arg)) {
                        // Subsequent list -> data (backward compat)
                        auto dataList = asList(arg)->getElements();
                        data.reserve(dataList.size());
                        for (const auto& v : dataList) {
                            if (!v.isNumber())
                                throw std::runtime_error("tensor data elements must be numeric");
                            data.push_back(v.isInt() ? static_cast<double>(v.asInt()) : v.asReal());
                        }
                        hasData = true;
                    } else if (isString(arg)) {
                        // Positional string -> dtype (backward compat)
                        dtype = tensorDTypeFromString(toUTF8StdString(asStringObj(arg)->s));
                    } else if (isTensor(arg) && shape.empty()) {
                        // Copy constructor
                        *(thread->stackTop - callSpec.argCount - 1) = Value(asTensor(arg)->clone(nullptr));
                        popN(callSpec.argCount);
                        return true;
                    } else if (isVector(arg) && shape.empty()) {
                        // tensor(vector) → 1D tensor
                        auto vec = asVector(arg);
                        std::vector<int64_t> vecShape = { static_cast<int64_t>(vec->length()) };
                        std::vector<double> vecData(vec->vec().data(), vec->vec().data() + vec->length());
                        *(thread->stackTop - callSpec.argCount - 1) = Value::tensorVal(vecShape, vecData, TensorDType::Float64);
                        popN(callSpec.argCount);
                        return true;
                    } else if (isMatrix(arg) && shape.empty()) {
                        // tensor(matrix) → 2D tensor
                        auto mat = asMatrix(arg);
                        int64_t rows = mat->mat().rows();
                        int64_t cols = mat->mat().cols();
                        std::vector<int64_t> matShape = { rows, cols };
                        std::vector<double> matData;
                        matData.reserve(rows * cols);
                        for (int64_t r = 0; r < rows; ++r)
                            for (int64_t c = 0; c < cols; ++c)
                                matData.push_back(mat->mat()(r, c));
                        *(thread->stackTop - callSpec.argCount - 1) = Value::tensorVal(matShape, matData, TensorDType::Float64);
                        popN(callSpec.argCount);
                        return true;
                    } else {
                        throw std::runtime_error("tensor constructor: unexpected argument type");
                    }
                }
            }

            if (shape.empty())
                throw std::runtime_error("tensor constructor requires shape");

            Value result = hasData
                ? Value::tensorVal(shape, data, dtype)
                : Value::tensorVal(shape, dtype);
            *(thread->stackTop - callSpec.argCount - 1) = result;
            popN(callSpec.argCount);
            return true;
        }

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

        // Check for user-defined conversion operator as fallback before construct()
        if (callSpec.argCount == 1 && (isObjectInstance(*argBegin) || isActorInstance(*argBegin))) {
            Value arg = *argBegin;
            // Remove callee+arg from stack; tryConvertValue manages stack for async
            popN(callSpec.argCount + 1);
            auto outcome = tryConvertValue(arg, Value::typeVal(builtinType), false, /*implicitCall=*/false,
                                           Thread::PendingConversion::Kind::TypeConversion);
            if (outcome.result == ConversionResult::NeedsAsyncFrame)
                return true;
            if (outcome.result == ConversionResult::ConvertedSync) {
                push(outcome.convertedValue);
                return true;
            }
            // No conversion operator — restore stack and fall through to construct()
            push(Value::typeVal(builtinType)); // callee placeholder
            push(arg);
            argBegin = thread->stackTop - 1;
            argEnd = thread->stackTop;
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
                    if (!resolveValue(arg))
                        return false;
                    constArgs[pname] = arg;
                }
            }
        }

        // Check closure upvalues for mutable reference type captures.
        // DF funcs run on the dataflow thread — mutable captures would be
        // unsafely shared between threads.
        ObjClosure* cls = asClosure(closureVal);
        for (size_t i = 0; i < cls->upvalues.size(); ++i) {
            if (cls->upvalues[i].isNil()) continue;
            Value captured = *asUpvalue(cls->upvalues[i])->location;
            if (captured.isObj() && !captured.isConst()) {
                runtimeError("Dataflow function '" + toUTF8StdString(functionObj->name)
                    + "' captures a mutable reference variable; "
                      "captured variables must be const or primitive types.");
                return false;
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
            case ObjType::EventType: {
                ObjEventType* eventType = asEventType(callee);
                size_t payloadCount = eventType->payloadProperties.size();
                std::string eventName = toUTF8StdString(eventType->name);
                bool strict = false;
                if (!thread->frames.empty())
                    strict = (thread->frames.end() - 1)->strict;

                auto argBegin = thread->stackTop - callSpec.argCount;
                std::unordered_map<int32_t, Value> payload;
                std::vector<bool> assigned(payloadCount, false);
                // Initialize payload with default values
                for (size_t i = 0; i < payloadCount; ++i) {
                    const auto& prop = eventType->payloadProperties[i];
                    payload[prop.name.hashCode()] = prop.initialValue;
                }

                auto orderedProps = eventType->orderedPayloadProperties();
                auto assignValue = [&](const ObjEventType::PayloadPropertyView& entry, Value value) -> bool {
                    if (assigned[entry.index]) {
                        runtimeError("Multiple values provided for payload property '" +
                                     toUTF8StdString(entry.property->name) +
                                     "' when constructing event '" + eventName + "'.");
                        return false;
                    }
                    const Value& typeSpec = entry.property->type;
                    if (!value.isNil() && !typeSpec.isNil())
                        value = toType(typeSpec, value, strict);
                    assigned[entry.index] = true;
                    payload[entry.property->name.hashCode()] = value;
                    return true;
                };

                bool ok = true;
                if (callSpec.allPositional) {
                    if (callSpec.argCount > payloadCount) {
                        runtimeError("Event '" + eventName + "' expects at most " +
                                     std::to_string(payloadCount) + " argument" +
                                     (payloadCount == 1 ? "" : "s") + " but " +
                                     std::to_string(callSpec.argCount) + " were provided.");
                        ok = false;
                    } else {
                        for (size_t i = 0; i < callSpec.argCount && ok; ++i) {
                            if (!assignValue(orderedProps[i], *(argBegin + i)))
                                ok = false;
                        }
                    }
                } else {
                    size_t positionalIndex = 0;
                    for (size_t i = 0; i < callSpec.argCount && ok; ++i) {
                        const auto& spec = callSpec.args[i];
                        Value value = *(argBegin + i);
                        if (spec.positional) {
                            while (positionalIndex < orderedProps.size() &&
                                   assigned[orderedProps[positionalIndex].index])
                                ++positionalIndex;
                            if (positionalIndex >= orderedProps.size()) {
                                runtimeError("Too many positional arguments when constructing event '" +
                                             eventName + "'.");
                                ok = false;
                                break;
                            }
                            if (!assignValue(orderedProps[positionalIndex], value)) {
                                ok = false;
                                break;
                            }
                            ++positionalIndex;
                        } else {
                            bool ambiguous = false;
                            auto entry = eventType->findPayloadPropertyByHash15(
                                static_cast<uint16_t>(spec.paramNameHash & 0x7fff), ambiguous);
                            if (ambiguous) {
                                runtimeError("Ambiguous named argument when constructing event '" +
                                             eventName + "'; multiple payload properties share that name hash.");
                                ok = false;
                                break;
                            }
                            if (!entry.has_value()) {
                                runtimeError("Unknown named argument when constructing event '" +
                                             eventName + "'.");
                                ok = false;
                                break;
                            }
                            if (!assignValue(*entry, value)) {
                                ok = false;
                                break;
                            }
                        }
                    }
                }

                if (!ok)
                    return false;

                Value instance = Value::eventInstanceVal(Value::objRef(eventType), std::move(payload));
                *(thread->stackTop - callSpec.argCount - 1) = instance;
                popN(callSpec.argCount);
                return true;
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
                                else if (builtinToValueType(params[0]->type.value()->builtin) == std::optional(ValueType::Dict))
                                    initAcceptsDict = true;
                            }
                        }
                    }

                    if (initClosureObj != nullptr && !(dictArg && !initAcceptsDict)) {
                        if (!type->isActor) {
                            bool isNative = initFuncObj != nullptr && initFuncObj->builtinInfo;
                            Value calleeVal;
                            if (isNative) {
                                const auto& info = *initFuncObj->builtinInfo;
                                calleeVal = Value::boundNativeVal(inst, info.function,
                                                                  initFuncObj->funcType.has_value() &&
                                                                     initFuncObj->funcType.value()->func.has_value() ?
                                                                     initFuncObj->funcType.value()->func->isProc : false,
                                                                  initFuncObj->funcType.has_value() ?
                                                                     initFuncObj->funcType.value() : nullptr,
                                                                  info.defaultValues,
                                                                  Value::objRef(initFuncObj));
                            } else {
                                calleeVal = Value::boundMethodVal(inst, initMethod->closure);
                            }
                            *(thread->stackTop - callSpec.argCount - 1) = calleeVal;
                            bool ok = callValue(calleeVal, callSpec);
                            // Skip instance restoration if native call was deferred (continuation active)
                            // In deferred case, processNativeDefaultParamDispatch handles this
                            if (isNative && !thread->nativeContinuation.active)
                                *(thread->stackTop - 1) = inst; // native init returns instance
                            return ok;
                        } else {
                        bool isNativeInit = initFuncObj != nullptr && initFuncObj->builtinInfo;
                        Value calleeVal;
                        if (isNativeInit) {
                            const auto& info = *initFuncObj->builtinInfo;
                            calleeVal = Value::boundNativeVal(inst, info.function,
                                                              initFuncObj->funcType.has_value() &&
                                                                  initFuncObj->funcType.value()->func.has_value()
                                                                  ? initFuncObj->funcType.value()->func->isProc
                                                                  : false,
                                                              initFuncObj->funcType.has_value()
                                                                  ? initFuncObj->funcType.value()
                                                                  : nullptr,
                                                              info.defaultValues,
                                                              Value::objRef(initFuncObj));
                        } else {
                            auto boundInit = newBoundMethodObj(inst, initMethod->closure);
                            calleeVal = Value::objVal(std::move(boundInit));
                        }
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

                            // Track setter calls needed
                            struct DictSetterCall {
                                Value closure;
                                Value value;
                                CallFrame frame;
                            };
                            std::vector<DictSetterCall> setterFrames;

                            for(const auto& kv : argDict->items()) {
                                if (!isString(kv.first))
                                    continue;
                                ObjString* keyStr = asStringObj(kv.first);
                                int32_t hash = keyStr->hash;

                                // First check if there's a setter method for this property
                                icu::UnicodeString setterName = UnicodeString("__set_") + keyStr->s;
                                Value setterNameValue = Value::stringVal(setterName);
                                ObjString* setterNameStr = asStringObj(setterNameValue);
                                auto setterIt = type->methods.find(setterNameStr->hash);

                                if (setterIt != type->methods.end()) {
                                    // Property has a setter - queue the call
                                    // Type conversion will be handled by the setter
                                    const auto& methodInfo = setterIt->second;
                                    Value setterClosure = methodInfo.closure;

                                    CallFrame setterFrame{};
                                    setterFrame.closure = Value::objRef(asClosure(setterClosure));
                                    setterFrame.startIp = setterFrame.ip = asFunction(asClosure(setterClosure)->function)->chunk->code.begin();
                                    setterFrame.strict = asFunction(asClosure(setterClosure)->function)->strict;

                                    setterFrames.push_back(DictSetterCall{setterClosure, kv.second, setterFrame});
                                    continue;
                                }

                                // No setter - look for direct property
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
                                // Direct assignment
                                objInst->assignProperty(hash, val);
                            }
                            pop();

                            // Push setter frames if any
                            if (!setterFrames.empty()) {
                                Value savedInstance = pop();
                                thread->pendingConstructorInstance = savedInstance;
                                thread->pendingSetterCount = static_cast<int>(setterFrames.size());

                                CallFrames::iterator parentFrame = thread->frames.size() > 0 ? thread->frames.end() - 1 : thread->frames.end();

                                for (auto& setterCall : setterFrames) {
                                    Value instForSetter = Value::objRef(objInst);
                                    push(instForSetter);
                                    push(setterCall.value);

                                    auto& frame = setterCall.frame;
                                    frame.slots = &(*(thread->stackTop - 2));
                                    frame.parent = parentFrame;
                                    thread->pushFrame(frame);
                                    thread->frameStart = true;
                                }
                            }
                        } else if (!type->isActor && initMethod == nullptr) {
                            ObjectInstance* objInst = asObjectInstance(inst);

                            std::vector<ObjObjectType::PublicPropertyView> publicProps =
                                type->orderedPublicProperties();

                            std::string typeName = toUTF8StdString(type->name);

                            bool strictConv = false;
                            if (thread->frames.size() >= 1)
                                strictConv = (thread->frames.end()-1)->strict;

                            auto argBegin = thread->stackTop - callSpec.argCount;

                            // Track property assignments: key -> (value, property_name, has_setter)
                            struct PropertyAssignment {
                                Value value;
                                icu::UnicodeString propertyName;
                                bool callSetter;
                            };
                            std::unordered_map<int32_t, PropertyAssignment> assignedValues;
                            assignedValues.reserve(callSpec.argCount);
                            std::unordered_set<int32_t> assignedKeys;

                            // Helper that enforces duplicate/named argument validation and performs
                            // type conversion before storing the value for later assignment.
                            auto assignValue = [&](const ObjObjectType::PublicPropertyView& entry, Value value) -> bool {
                                if (assignedKeys.contains(entry.key)) {
                                    runtimeError("Multiple values provided for property '" +
                                                 toUTF8StdString(entry.property->name) +
                                                 "' when constructing type '" + typeName + "'.");
                                    return false;
                                }
                                if (!entry.property->type.isNil() && isTypeSpec(entry.property->type)) {
                                    ObjTypeSpec* ts = asTypeSpec(entry.property->type);
                                    if (ts->typeValue != ValueType::Nil) {
                                        try {
                                            value = toType(entry.property->type, value, strictConv);
                                        } catch (std::exception& e) {
                                            runtimeError(e.what());
                                            return false;
                                        }
                                    }
                                }
                                assignedKeys.insert(entry.key);
                                assignedValues.emplace(entry.key, PropertyAssignment{value, entry.property->name, false});
                                return true;
                            };

                            bool ok = true;
                            if (callSpec.allPositional) {
                                // No named parameters were present, so the argument order follows the
                                // order of declaration for public properties.
                                if (callSpec.argCount > publicProps.size()) {
                                    runtimeError("Type '" + typeName + "' constructor expects at most " +
                                                 std::to_string(publicProps.size()) +
                                                 " argument" + (publicProps.size() == 1 ? "" : "s") +
                                                 " but " + std::to_string(callSpec.argCount) + " were provided.");
                                    ok = false;
                                } else {
                                    for (size_t i = 0; i < callSpec.argCount && ok; ++i) {
                                        if (!assignValue(publicProps[i], *(argBegin + i)))
                                            ok = false;
                                    }
                                }
                            } else {
                                // When mixed positional/named arguments are present we need to skip
                                // properties that already received a value via a named parameter.
                                size_t positionalIndex = 0;
                                for (size_t i = 0; i < callSpec.argCount && ok; ++i) {
                                    const auto& spec = callSpec.args[i];
                                    Value value = *(argBegin + i);
                                    if (spec.positional) {
                                        while (positionalIndex < publicProps.size() &&
                                               assignedKeys.contains(publicProps[positionalIndex].key))
                                            ++positionalIndex;
                                        if (positionalIndex >= publicProps.size()) {
                                            runtimeError("Too many positional arguments when constructing type '" +
                                                         typeName + "'.");
                                            ok = false;
                                            break;
                                        }
                                        if (!assignValue(publicProps[positionalIndex], value)) {
                                            ok = false;
                                            break;
                                        }
                                        ++positionalIndex;
                                    } else {
                                        bool ambiguous = false;
                                        auto entry = type->findPublicPropertyByHash15(
                                            static_cast<uint16_t>(spec.paramNameHash & 0x7fff),
                                            ambiguous);
                                        if (ambiguous) {
                                            runtimeError("Ambiguous named argument when constructing type '" +
                                                         typeName +
                                                         "'; multiple public properties share that name hash.");
                                            ok = false;
                                            break;
                                        }
                                        if (entry.has_value()) {
                                            // Normal case: property found in public properties
                                            if (!assignValue(*entry, value)) {
                                                ok = false;
                                                break;
                                            }
                                        } else {
                                            // Named parameter didn't match a public property.
                                            // Check if it matches a property with a setter method.
                                            // Search through all methods for one matching "__set_<name>" where <name> hash matches parameter hash
                                            icu::UnicodeString propertyName;
                                            bool foundSetter = false;
                                            int32_t setterMethodHash = 0;

                                            for (const auto& methodPair : type->methods) {
                                                const auto& method = methodPair.second;
                                                ObjFunction* func = asFunction(asClosure(method.closure)->function);
                                                icu::UnicodeString methodName = func->name;

                                                // Check if method name starts with "__set_"
                                                if (methodName.startsWith("__set_")) {
                                                    // Extract property name by removing "__set_" prefix
                                                    icu::UnicodeString propName = methodName.tempSubString(6); // Skip "__set_"

                                                    // Compute hash of property name
                                                    ObjString* propNameStr = asStringObj(Value::stringVal(propName));
                                                    uint16_t propHash = static_cast<uint16_t>(propNameStr->hash & 0x7fff);

                                                    if (propHash == static_cast<uint16_t>(spec.paramNameHash & 0x7fff)) {
                                                        propertyName = propName;
                                                        setterMethodHash = methodPair.first;
                                                        foundSetter = true;
                                                        break;
                                                    }
                                                }
                                            }

                                            if (!foundSetter) {
                                                runtimeError("Unknown named argument when constructing type '" +
                                                             typeName + "'.");
                                                ok = false;
                                                break;
                                            }

                                            // Property has a setter - store for later
                                            // Use setter method hash as key
                                            int32_t propKey = setterMethodHash;

                                            if (assignedKeys.contains(propKey)) {
                                                runtimeError("Multiple values provided for property '" +
                                                             toUTF8StdString(propertyName) +
                                                             "' when constructing type '" + typeName + "'.");
                                                ok = false;
                                                break;
                                            }

                                            assignedKeys.insert(propKey);
                                            assignedValues.emplace(propKey, PropertyAssignment{value, propertyName, true});
                                        }
                                    }
                                }
                            }

                            if (!ok)
                                return false;

                            // Process property assignments: direct assignment for properties without setters,
                            // frame batching for properties with setters (similar to default parameter pattern)
                            struct SetterCall {
                                Value closure;
                                Value value;
                                CallFrame frame;
                            };
                            std::vector<SetterCall> setterFrames;

                            for (const auto& kv : assignedValues) {
                                const auto& assignment = kv.second;

                                if (assignment.callSetter) {
                                    // Property has a setter - prepare to call it
                                    icu::UnicodeString setterName = UnicodeString("__set_") + assignment.propertyName;
                                    Value setterNameValue = Value::stringVal(setterName);
                                    ObjString* setterNameStr = asStringObj(setterNameValue);
                                    auto setterIt = type->methods.find(setterNameStr->hash);

                                    #ifdef DEBUG_BUILD
                                    assert(setterIt != type->methods.end());
                                    #endif

                                    const auto& methodInfo = setterIt->second;
                                    Value setterClosure = methodInfo.closure;

                                    // Create frame for setter call (similar to default parameter frames)
                                    CallFrame setterFrame{};
                                    setterFrame.closure = Value::objRef(asClosure(setterClosure));
                                    setterFrame.startIp = setterFrame.ip = asFunction(asClosure(setterClosure)->function)->chunk->code.begin();
                                    setterFrame.strict = asFunction(asClosure(setterClosure)->function)->strict;

                                    // Save closure, value, and frame for later
                                    setterFrames.push_back(SetterCall{setterClosure, assignment.value, setterFrame});
                                } else {
                                    // Property without setter - direct assignment to backing field
                                    objInst->assignProperty(kv.first, assignment.value);
                                }
                            }

                            popN(callSpec.argCount);

                            // Push setter frames if any (they execute after object is created)
                            if (!setterFrames.empty()) {
                                // Save instance for later - after setters execute, we'll clean up their results
                                // and push the instance back
                                Value savedInstance = pop(); // Remove instance from stack
                                thread->pendingConstructorInstance = savedInstance;
                                thread->pendingSetterCount = static_cast<int>(setterFrames.size());

                                // Setter frames should return to the current frame (the one with OpCode::Call)
                                CallFrames::iterator parentFrame = thread->frames.size() > 0 ? thread->frames.end() - 1 : thread->frames.end();

                                for (auto& setterCall : setterFrames) {
                                    // Push instance and value for this setter call
                                    Value instForSetter = Value::objRef(objInst);
                                    push(instForSetter);
                                    push(setterCall.value);

                                    // Update frame slots to point to current stack position
                                    auto& frame = setterCall.frame;
                                    frame.slots = &(*(thread->stackTop - 2)); // Point to instance (receiver)
                                    frame.parent = parentFrame;
                                    thread->pushFrame(frame);
                                    thread->frameStart = true;
                                }
                            }
                        } else if (callSpec.argCount != 0) {
                            runtimeError("Expected 0 arguments for type instantiation, provided " +
                                         std::to_string(callSpec.argCount));
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
                if (function->builtinInfo) {
                    const auto& info = *function->builtinInfo;
                    ptr<type::Type> funcType = function->funcType.has_value()
                        ? function->funcType.value() : nullptr;
                    if (nativeCallTimingEnabled_)
                        nativeCallContext_ = function->name;
                    return callNativeFn(info.function, funcType,
                                        info.defaultValues, callSpec,
                                        false, Value::nilVal(), closure->function,
                                        info.resolveArgMask);
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
                if (nativeCallTimingEnabled_)
                    nativeCallContext_ = UnicodeString("native");
                return callNativeFn(native, nativeObj->funcType,
                                    nativeObj->defaultValues, callSpec,
                                    false, Value::nilVal(), Value::nilVal(),
                                    nativeObj->resolveArgMask);
            }
            case ObjType::BoundNative: {
                Value boundValue = callee;
                ObjBoundNative* bound { asBoundNative(boundValue) };

                // Extract resolveArgMask from declFunction if it has builtinInfo
                uint32_t resolveMask = 0;
                if (isFunction(bound->declFunction)) {
                    ObjFunction* declFunc = asFunction(bound->declFunction);
                    if (declFunc->builtinInfo)
                        resolveMask = declFunc->builtinInfo->resolveArgMask;
                }

                if (nativeCallTimingEnabled_) {
                    if (isFunction(bound->declFunction))
                        nativeCallContext_ = asFunction(bound->declFunction)->name;
                    else
                        nativeCallContext_ = UnicodeString("bound-native");
                }

                if (!isActorInstance(bound->receiver)) {
                    thread->currentBoundCall = boundValue;
                    BoundCallGuard guard(thread.get());
                    *(thread->stackTop - callSpec.argCount - 1) = bound->receiver;
                    NativeFn native = bound->function;
                    return callNativeFn(native, bound->funcType,
                                        bound->defaultValues, callSpec,
                                        true, bound->receiver,
                                        bound->declFunction, resolveMask);
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
                                            bound->declFunction, resolveMask);
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

std::pair<ExecutionStatus,Value> VM::invokeClosure(ObjClosure* closure,
                                                    const std::vector<Value>& args,
                                                    TimePoint deadline)
{
    // Push closure first, then arguments (to match OpCode::Call stack layout)
    push(Value::objRef(closure));
    for(const auto& a : args)
        push(a);
    CallSpec spec(args.size());

    // Native closures (builtinInfo) must go through callNativeFn, not call(),
    // because call() sets up a bytecode frame but native closures have no bytecodes.
    ObjFunction* function = asFunction(closure->function);
    if (function->builtinInfo) {
        const auto& info = *function->builtinInfo;
        ptr<type::Type> funcType = function->funcType.has_value()
            ? function->funcType.value() : nullptr;
        if (nativeCallTimingEnabled_)
            nativeCallContext_ = function->name;
        if (!callNativeFn(info.function, funcType, info.defaultValues, spec,
                          false, Value::nilVal(), closure->function))
            return { ExecutionStatus::RuntimeError, Value::nilVal() };
        // callNativeFn stores result in the closure slot and pops args.
        // The result is now at the top of the stack.
        Value result = peek(0);
        pop();
        return { ExecutionStatus::OK, result };
    }

    if(!call(closure, spec))
        return { ExecutionStatus::RuntimeError, Value::nilVal() };

    auto result = execute(deadline);  // Pass deadline to execute()

    // Note: execute() should have handled the cleanup when the function returned,
    // but for safety in nested calls, we don't need additional cleanup here
    // since the call() and execute() sequence manages the stack properly

    return result;
}



bool VM::invokeFromType(ObjObjectType* type, ObjString* name, const CallSpec& callSpec,
                        const Value& receiver)
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

    // Const enforcement for linkMethod-registered native methods
    if (receiver.isConst()) {
        ObjFunction* func = asFunction(asClosure(method)->function);
        if (func->builtinInfo && !func->builtinInfo->noMutateSelf) {
            runtimeError("Cannot call mutating method '%s' on const value.",
                         toUTF8StdString(name->s).c_str());
            return false;
        }
    }

    return call(asClosure(method), callSpec);
}


Value VM::findOperatorMethod(ObjObjectType* type, int32_t hash)
{
    ObjObjectType* t = type;
    while (t) {
        auto it = t->methods.find(hash);
        if (it != t->methods.end())
            return it->second.closure;
        t = t->superType.isNil() ? nullptr : asObjectType(t->superType);
    }
    return Value::nilVal();
}


// Annotation mode for user-defined conversion operators
enum class ImplicitMode {
    ExplicitOnly,    // no annotation — explicit conversion required everywhere
    Everywhere,      // @implicit — implicit conversion allowed in all contexts
    NonstrictOnly    // @implicit(nonstrict_only=true) — implicit in non-strict only
};

static ImplicitMode getImplicitMode(const Value& closureVal)
{
    auto* funcObj = asFunction(asClosure(closureVal)->function);
    for (const auto& annot : funcObj->annotations) {
        if (annot->name == "implicit") {
            for (const auto& arg : annot->args) {
                if (arg.first == toUnicodeString("nonstrict_only")) {
                    if (auto b = dynamic_ptr_cast<ast::Bool>(arg.second))
                        if (b->value) return ImplicitMode::NonstrictOnly;
                }
            }
            return ImplicitMode::Everywhere;
        }
    }
    return ImplicitMode::ExplicitOnly;
}

static bool hasExplicitAnnotation(const Value& closureVal)
{
    auto* funcObj = asFunction(asClosure(closureVal)->function);
    for (const auto& annot : funcObj->annotations)
        if (annot->name == "explicit") return true;
    return false;
}

Value VM::findConversionMethod(const Value& instanceType, int32_t hash, bool implicitCall)
{
    Value closure = findOperatorMethod(asObjectType(instanceType), hash);
    if (closure.isNil())
        return Value::nilVal();

    if (implicitCall) {
        ImplicitMode mode = getImplicitMode(closure);
        if (mode == ImplicitMode::ExplicitOnly)
            return Value::nilVal();  // no @implicit — require explicit conversion
        if (mode == ImplicitMode::NonstrictOnly) {
            bool strictCtx = !thread->frames.empty()
                             && (thread->frames.end()-1)->strict;
            if (strictCtx)
                return Value::nilVal();  // nonstrict_only in strict context — block
        }
        // ImplicitMode::Everywhere always proceeds
    }
    return closure;
}


bool VM::canConvertToType(const Value& val, const Value& targetTypeSpec, bool implicitCall) const
{
    // 1. Already the target type?
    if (val.is(targetTypeSpec))
        return true;

    if (!isTypeSpec(targetTypeSpec))
        return false;

    ObjTypeSpec* ts = asTypeSpec(targetTypeSpec);

    // 2. For builtin target types, check if the source can convert via builtin rules
    if (ts->typeValue != ValueType::Object && ts->typeValue != ValueType::Actor
        && ts->typeValue != ValueType::Nil) {

        // Builtin-to-builtin: these generally succeed (int→real, etc.)
        if (!val.isObj() || isString(val))
            return true;

        // Object/actor → builtin: check for user-defined conversion operator
        if (isObjectInstance(val) || isActorInstance(val)) {
            Value instType = isObjectInstance(val)
                ? asObjectInstance(val)->instanceType
                : asActorInstance(val)->instanceType;
            UnicodeString convName = UnicodeString("operator->") + toUnicodeString(to_string(ts->typeValue));
            int32_t convHash = convName.hashCode();
            // Use const_cast since findConversionMethod isn't const but doesn't modify state
            // when only checking (implicitCall logic reads thread which is safe)
            Value closure = const_cast<VM*>(this)->findConversionMethod(instType, convHash, implicitCall);
            if (!closure.isNil())
                return true;
        }
        return false;
    }

    // 3. For object/actor target types: check constructor-based auto-conversion
    if (ts->typeValue == ValueType::Object || ts->typeValue == ValueType::Actor) {
        ObjObjectType* targetType = asObjectType(targetTypeSpec);

        // Find init method on target type
        ObjObjectType* tInit = targetType;
        const ObjObjectType::Method* initMethod = nullptr;
        while (tInit != nullptr && initMethod == nullptr) {
            auto it = tInit->methods.find(asStringObj(initString)->hash);
            if (it != tInit->methods.end())
                initMethod = &it->second;
            else
                tInit = tInit->superType.isNil() ? nullptr : asObjectType(tInit->superType);
        }

        if (initMethod != nullptr && isClosure(initMethod->closure)) {
            ObjFunction* initFunc = asFunction(asClosure(initMethod->closure)->function);

            // Check: single required param (arity == 1) and not @explicit
            if (initFunc->arity == 1 && !hasExplicitAnnotation(initMethod->closure)) {
                // Constructor auto-conversion eligible
                return true;
            }
        }
    }

    return false;
}


VM::ConversionOutcome VM::tryConvertValue(
    const Value& val,
    const Value& targetTypeSpec,
    bool strict,
    bool implicitCall,
    Thread::PendingConversion::Kind pendingKind,
    const Value& savedContext)
{
    // 1. Already the target type?
    if (val.is(targetTypeSpec))
        return { ConversionResult::AlreadyCorrectType, Value::nilVal() };

    // Resolve target type: accept both ObjTypeSpec and inline type tags
    ValueType targetVT = ValueType::Nil;
    if (isTypeSpec(targetTypeSpec)) {
        targetVT = asTypeSpec(targetTypeSpec)->typeValue;
    } else if (targetTypeSpec.isType()) {
        targetVT = targetTypeSpec.asType();
    } else {
        return { ConversionResult::Failed, Value::nilVal() };
    }

    ObjTypeSpec* ts = isTypeSpec(targetTypeSpec) ? asTypeSpec(targetTypeSpec) : nullptr;

    // 2. Constructor auto-conversion for Object/Actor target types.
    //    Takes precedence over conversion operators.
    //    Eligible when target type has init with arity==1 and no @explicit.
    if (ts && (targetVT == ValueType::Object || targetVT == ValueType::Actor)) {
        ObjObjectType* targetType = asObjectType(targetTypeSpec);
        ObjObjectType* tInit = targetType;
        const ObjObjectType::Method* initMethod = nullptr;
        while (tInit && !initMethod) {
            auto it = tInit->methods.find(asStringObj(initString)->hash);
            if (it != tInit->methods.end())
                initMethod = &it->second;
            else
                tInit = tInit->superType.isNil() ? nullptr : asObjectType(tInit->superType);
        }
        if (initMethod && isClosure(initMethod->closure)) {
            ObjFunction* initFunc = asFunction(asClosure(initMethod->closure)->function);
            if (initFunc->arity == 1 && !hasExplicitAnnotation(initMethod->closure)) {
                // Auto-construct: set up callValue frame
                push(targetTypeSpec);  // callee (type constructor)
                push(val);             // argument
                callValue(targetTypeSpec, CallSpec(1));
                // The PendingConversion is not needed here because callValue for a
                // type constructor leaves the constructed instance on the stack when
                // the init frame returns (the VM's existing constructor machinery
                // handles this). The caller should treat this like NeedsAsyncFrame.
                return { ConversionResult::NeedsAsyncFrame, Value::nilVal() };
            }
        }
        // Object/Actor target with no eligible constructor
        return { ConversionResult::Failed, Value::nilVal() };
    }

    // 3. User-defined conversion operator (source is Object/Actor, target is builtin)
    if ((isObjectInstance(val) || isActorInstance(val))
        && targetVT != ValueType::Nil) {
        Value instType = isObjectInstance(val)
            ? asObjectInstance(val)->instanceType
            : asActorInstance(val)->instanceType;
        UnicodeString convName = UnicodeString("operator->") + toUnicodeString(to_string(targetVT));
        int32_t convHash = convName.hashCode();

        // Recursion guard
        bool inProgress = false;
        for (const auto& g : thread->conversionInProgress)
            if (g.receiver.is(val, false)) { inProgress = true; break; }

        if (!inProgress) {
            Value closure = findConversionMethod(instType, convHash, implicitCall);
            if (!closure.isNil()) {
                // Set up async conversion call
                thread->pendingConversions.push_back({
                    pendingKind, savedContext, val, thread->frames.size()
                });
                thread->conversionInProgress.push_back({val, thread->frames.size()});
                push(val); // push as receiver for method call
                call(asClosure(closure), CallSpec(0));
                return { ConversionResult::NeedsAsyncFrame, Value::nilVal() };
            }
        }
    }

    // 4. Builtin conversion fallback
    try {
        Value converted = toType(targetTypeSpec, val, strict);
        return { ConversionResult::ConvertedSync, converted };
    } catch (std::exception&) {
        return { ConversionResult::Failed, Value::nilVal() };
    }
}


bool VM::tryDispatchBinaryOperator(const OperatorHashes& hashes)
{
    Value& rhs = peek(0);
    Value& lhs = peek(1);

    // Fast bail: neither is a user-defined object instance → no overload possible
    // isObjectInstance checks isObj() then obj->type == ObjType::Instance,
    // so strings, lists, vectors, etc. are excluded (they have different ObjTypes).
    if (!isObjectInstance(lhs) && !isObjectInstance(rhs))
        return false;

    Value methodClosure;
    bool swapped = false;

    // Try LHS first: operator<sym> or loperator<sym>
    if (isObjectInstance(lhs)) {
        auto* type = asObjectType(asObjectInstance(lhs)->instanceType);
        methodClosure = findOperatorMethod(type, hashes.op);
        if (methodClosure.isNil())
            methodClosure = findOperatorMethod(type, hashes.lop);
    }

    // If LHS didn't have it, try RHS: operator<sym> (commutative, swap) or roperator<sym>
    if (methodClosure.isNil() && isObjectInstance(rhs)) {
        auto* type = asObjectType(asObjectInstance(rhs)->instanceType);
        methodClosure = findOperatorMethod(type, hashes.op);
        if (!methodClosure.isNil()) {
            swapped = true;
        } else {
            methodClosure = findOperatorMethod(type, hashes.rop);
            if (!methodClosure.isNil()) swapped = true;
        }
    }

    if (methodClosure.isNil())
        return false;

    // Check parameter type compatibility: if the operator method declares a type
    // for its parameter, verify the argument is compatible before dispatching.
    // This prevents e.g. quantity.operator+(other :quantity) from being dispatched
    // when the other operand is a string.
    {
        Value arg = swapped ? lhs : rhs;
        ObjFunction* fn = asFunction(asClosure(methodClosure)->function);
        if (fn->funcType.has_value() && fn->funcType.value()->func.has_value()) {
            auto& params = fn->funcType.value()->func.value().params;
            if (!params.empty() && params[0].has_value() && params[0]->type.has_value()) {
                auto& paramType = params[0]->type.value();
                // For object/actor parameter types, check if arg is that type
                if (paramType->builtin == type::BuiltinType::Object && paramType->obj.has_value()) {
                    if (!arg.is(Value::nilVal()) && isObjectInstance(arg)) {
                        auto* argType = asObjectType(asObjectInstance(arg)->instanceType);
                        auto& expectedName = paramType->obj.value().name;
                        // Walk supertype chain for compatibility
                        bool compatible = false;
                        auto* t = argType;
                        while (t) {
                            if (t->name == expectedName) { compatible = true; break; }
                            t = t->superType.isNil() ? nullptr : asObjectType(t->superType);
                        }
                        if (!compatible)
                            return false;  // parameter type mismatch — skip this operator
                    } else if (!isObjectInstance(arg)) {
                        return false;  // operator expects object type, arg is not an object
                    }
                }
            }
        }
    }

    // Set up stack: [receiver, arg]
    Value r = pop();
    Value l = pop();
    if (!swapped) {
        push(l);  // receiver = lhs
        push(r);  // arg = rhs
    } else {
        push(r);  // receiver = rhs
        push(l);  // arg = lhs
    }

    CallSpec callSpec{1};
    // Even if call() fails, return true to prevent fall-through to built-in dispatch
    // (the error is already set by call())
    call(asClosure(methodClosure), callSpec);
    return true;
}


bool VM::tryDispatchUnaryOperator(int32_t hash)
{
    Value& operand = peek(0);

    if (!isObjectInstance(operand))
        return false;

    auto* type = asObjectType(asObjectInstance(operand)->instanceType);
    Value methodClosure = findOperatorMethod(type, hash);
    if (methodClosure.isNil())
        return false;

    // Stack already has [receiver]. Call with 0 args.
    CallSpec callSpec{0};
    call(asClosure(methodClosure), callSpec);
    return true;
}


bool VM::invoke(ObjString* name, const CallSpec& callSpec)
{
    Value receiver { peek(callSpec.argCount) };

    if (isObjectInstance(receiver)) {

        ObjectInstance* instance = asObjectInstance(receiver);

        // check to ensure name isn't a prop with a func in it
        auto* prop = instance->findProperty(name->hash);
        if (prop) { // it is a prop
            Value value { prop->value };
            *(thread->stackTop - callSpec.argCount - 1) = value;
            return callValue(value, callSpec);
        }

        return invokeFromType(asObjectType(instance->instanceType), name, callSpec, receiver);
    }
    else if (isActorInstance(receiver)) {
        ActorInstance* instance = asActorInstance(receiver);

        // check to ensure name isn't a prop with a func in it
        auto* prop = instance->findProperty(name->hash);
        if (prop) { // it is a prop
            Value value { prop->value };
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

                // Const enforcement: reject mutating methods on const receivers
                if (receiver.isConst() && !methodInfo.noMutateSelf) {
                    runtimeError("Cannot call mutating method '%s' on const value.",
                                 toUTF8StdString(name->s).c_str());
                    return false;
                }

                if (nativeCallTimingEnabled_)
                    nativeCallContext_ = name->s;

                if (std::this_thread::get_id() == instance->thread_id) {
                    // Same thread - call directly
                    if (methodInfo.funcType) {
                        return callNativeFn(fn, methodInfo.funcType,
                                            methodInfo.defaultValues, callSpec,
                                            true, receiver, methodInfo.declFunction,
                                            methodInfo.resolveArgMask);
                    } else {
                        return callNativeFn(fn, nullptr, {}, callSpec,
                                            true, receiver, methodInfo.declFunction,
                                            methodInfo.resolveArgMask);
                    }
                } else {
                    // Different thread - queue the call
                    Value callee = Value::boundNativeVal(receiver, fn, methodInfo.isProc,
                                                         methodInfo.funcType, methodInfo.defaultValues,
                                                         methodInfo.declFunction);
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

                    // Const enforcement: reject mutating methods on const receivers
                    if (receiver.isConst() && !methodInfo.noMutateSelf) {
                        runtimeError("Cannot call mutating method '%s' on const value.",
                                     toUTF8StdString(name->s).c_str());
                        return false;
                    }

                    if (nativeCallTimingEnabled_)
                        nativeCallContext_ = name->s;
                    if (methodInfo.funcType) {
                        return callNativeFn(fn, methodInfo.funcType,
                                            methodInfo.defaultValues, callSpec,
                                            true, receiver, methodInfo.declFunction,
                                            methodInfo.resolveArgMask);
                    } else {
                        return callNativeFn(fn, nullptr, {}, callSpec,
                                            true, receiver, methodInfo.declFunction,
                                            methodInfo.resolveArgMask);
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
                bool isConstAccess = indexable.isConst();
                Value index = pop();
                try {
                    Value sublist { list->index(index) };
                    // MVCC: propagate const + resolve snapshot for reference-type elements
                    if (isConstAccess && sublist.isObj() && !sublist.isConst()) {
                        auto* token = list->control->snapshotToken;
                        if (token) {
                            // For integer indexing, cache the frozen child back into the list element
                            if (index.isNumber()) {
                                auto idx = index.asInt();
                                auto len = list->length();
                                if (idx < 0) idx = len - (-idx);
                                sublist = resolveConstChild(sublist, token);
                                if (idx >= 0 && idx < len)
                                    list->cacheElement(idx, sublist);
                            } else {
                                sublist = resolveConstChild(sublist, token);
                            }
                        }
                    }
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
            case ObjType::Tensor: {
                ObjTensor* t = asTensor(indexable);
                std::vector<Value> indices;
                indices.reserve(subscriptCount);
                for (int i = 0; i < subscriptCount; ++i)
                    indices.push_back(pop());
                std::reverse(indices.begin(), indices.end());
                try {
                    Value elt = t->index(indices);
                    pop(); // discard indexable
                    push(elt);
                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return false;
                }
                return true;
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
                // MVCC: propagate const + resolve snapshot for reference-type values
                if (indexable.isConst() && result.isObj() && !result.isConst()) {
                    auto* token = dict->control->snapshotToken;
                    if (token) {
                        result = resolveConstChild(result, token);
                        dict->cacheValue(index, result);
                    }
                }
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
        runtimeError("Only strings, lists, ranges, vectors, dicts, matrices, tensors, and signals can be indexed, not type "+objTypeName(indexable.asObj())+".");
        return false;
    }
    runtimeError("Only strings, lists, ranges,vectors, dicts, matrices, tensors, and signals can be indexed, not type "+indexable.typeName()+".");
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
                    if (isRange(index) && !isList(value)) {
                        if (!resolveValue(value))
                            return false;
                    }
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
                    if (isRange(index) && !isVector(value)) {
                        if (!resolveValue(value))
                            return false;
                    }
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
                        if (isRange(r) && !isMatrix(value)) {
                            if (!resolveValue(value))
                                return false;
                        }
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
                        if ((isRange(row) || isRange(col)) && !isMatrix(value)) {
                            if (!resolveValue(value))
                                return false;
                        }
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
            case ObjType::Tensor: {
                ObjTensor* t = asTensor(indexable);
                std::vector<Value> indices;
                indices.reserve(subscriptCount);
                for (int i = 0; i < subscriptCount; ++i)
                    indices.push_back(pop());
                std::reverse(indices.begin(), indices.end());
                try {
                    t->setIndex(indices, value);
                    pop(); // discard indexable
                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return false;
                }
                return true;
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
        runtimeError("Only strings, lists, vectors, dicts, matrices, tensors, and signals can be indexed for assignment, not type "+objTypeName(indexable.asObj())+".");
        return false;
    }

    runtimeError("Only strings, lists, vectors, dicts, matrices, tensors, and signals can be indexed for assignment, not type "+indexable.typeName()+".");
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

    if (isClosure(method) && asFunction(asClosure(method)->function)->builtinInfo) {
        ObjClosure* cl = asClosure(method);
        ObjFunction* func = asFunction(cl->function);
        const auto& info = *func->builtinInfo;

        // Const enforcement for linkMethod-registered native methods
        if (peek(0).isConst() && !info.noMutateSelf) {
            runtimeError("Cannot call mutating method '%s' on const value.",
                         toUTF8StdString(name->s).c_str());
            return BindResult::Private; // reuse error return path
        }

        Value boundNative { Value::boundNativeVal(peek(0), info.function,
                                                  func->funcType.has_value() &&
                                                      func->funcType.value()->func.has_value() ?
                                                      func->funcType.value()->func->isProc : false,
                                                  func->funcType.has_value() ?
                                                      func->funcType.value() : nullptr,
                                                  info.defaultValues,
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


    // Flag event handler return so processEventDispatch() can advance
    // to the next handler.
    if (returningFrame.isEventHandler)
        thread->eventHandlerJustReturned = true;

    // Flag continuation callback return so processContinuationDispatch() can
    // process the result and continue iteration or finalize.
    if (returningFrame.isContinuationCallback)
        thread->continuationCallbackReturned = true;

    Value result = pop();
    closeUpvalues(returningFrame.slots);


    thread->popFrame();

    if (!thread->frames.empty()) {
        auto slotsOffset = returningFrame.slots - &*thread->stack.begin();
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
    int typeObjOffset = 4;
    if (!isObjectType(peek(typeObjOffset)))
        typeObjOffset = 3;
    #ifdef DEBUG_BUILD
    if (!isObjectType(peek(typeObjOffset)))
        throw std::runtime_error("Can't create property without object or actor type on stack");
    #endif
    ObjObjectType* objType = asObjectType(peek(typeObjOffset));
    #ifdef DEBUG_BUILD
    if (objType->isInterface)
        throw std::runtime_error("Can't create property for an interface");
    #endif

    if (objType->properties.contains(name->hash))
        throw std::runtime_error("Duplicate property '"+name->toStdString()+"' declared in type "+(objType->isActor?"actor":"object")+" "+toUTF8StdString(objType->name));

    const Value& propertyType { peek(typeObjOffset - 1) };
    Value propertyInitial { peek(typeObjOffset - 2) };
    Value accessVal { peek(typeObjOffset - 3) };
    Value constVal { Value::falseVal() };
    bool hasConstFlag = (typeObjOffset == 4);
    if (hasConstFlag)
        constVal = peek(0);

    if (!propertyInitial.isNil()) {
        // if the property type is specified, convert the initial value (if given) to the declared propType
        if (!propertyType.isNil() && isTypeSpec(propertyType)) {
            ObjTypeSpec* typeSpec = asTypeSpec(propertyType);
            if (typeSpec->typeValue != ValueType::Nil)
                // TODO: implement & use a canConvertToType()
                propertyInitial = toType(propertyType, propertyInitial, /*strict=*/false);
        }

        // Replace signal with template signal (not added to engine) for type member defaults
        // Note: the original signal was already added to engine during expression evaluation,
        // but will be GC'd when no longer referenced. Template signals are never added to engine.
        if (isSignal(propertyInitial)) {
            auto sig = asSignal(propertyInitial)->signal;
            ptr<df::Signal> templateSig;
            if (sig->isClockSignal()) {
                templateSig = df::Signal::newClockSignalTemplate(sig->frequency());
            } else if (sig->isSourceSignal()) {
                templateSig = df::Signal::newSourceSignalTemplate(sig->frequency(), sig->lastValue());
            } else {
                throw std::runtime_error("cannot use derived signals as member defaults");
            }
            propertyInitial = Value::signalVal(templateSig);
        }
    }

    ast::Access access = (!accessVal.isNil() && accessVal.isBool() && accessVal.asBool()) ? ast::Access::Private : ast::Access::Public;
    bool isConst = (!constVal.isNil() && constVal.isBool() && constVal.asBool());
    ObjObjectType::Property property{};
    property.name = name->s;
    property.type = propertyType;
    property.initialValue = propertyInitial;
    property.access = access;
    property.isConst = isConst;
    property.ownerType = Value::objRef(objType).weakRef();
    objType->properties[name->hash] = property;
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
    popN(hasConstFlag ? 4 : 3);
}


void VM::defineEventPayload(ObjString* name)
{
    #ifdef DEBUG_BUILD
    if (!isEventType(peek(2)))
        throw std::runtime_error("Can't declare event payload without event type on stack");
    #endif

    ObjEventType* eventType = asEventType(peek(2));

    if (eventType->propertyLookup.contains(name->hash))
        throw std::runtime_error("Duplicate event payload '" + name->toStdString() + "' declared in event '" + toUTF8StdString(eventType->name) + "'");

    Value propertyType = peek(1);
    Value initialValue = peek(0);

    if (!initialValue.isNil() && !propertyType.isNil() && isTypeSpec(propertyType)) {
        ObjTypeSpec* spec = asTypeSpec(propertyType);
        if (spec->typeValue != ValueType::Nil) {
            bool strict = false;
            if (!thread->frames.empty())
                strict = (thread->frames.end() - 1)->strict;
            initialValue = toType(propertyType, initialValue, strict);
        }
    }

    // Events cannot have signal members (enforced at compile-time and runtime)
    if (!initialValue.isNil() && isSignal(initialValue)) {
        throw std::runtime_error("events cannot have signal members");
    }

    ObjEventType::PayloadProperty payload { name->s, propertyType, initialValue };
    eventType->payloadProperties.push_back(payload);
    eventType->propertyLookup[name->hash] = eventType->payloadProperties.size() - 1;

    popN(2);
}


void VM::extendEventType()
{
    Value superVal = peek(1);
    Value subVal = peek(0);

    if (!isEventType(superVal) || !isEventType(subVal))
        throw std::runtime_error("Event inheritance requires event types");

    ObjEventType* superType = asEventType(superVal);
    ObjEventType* subType = asEventType(subVal);

    subType->superType = Value::objRef(superType);
    subType->payloadProperties.clear();
    subType->payloadProperties.reserve(superType->payloadProperties.size());
    subType->propertyLookup.clear();

    for (size_t i = 0; i < superType->payloadProperties.size(); ++i) {
        const auto& prop = superType->payloadProperties[i];
        subType->payloadProperties.push_back(prop);
        subType->propertyLookup[prop.name.hashCode()] = i;
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
                      std::vector<Value> defaults,
                      uint32_t resolveArgMask)
{
    UnicodeString uname { toUnicodeString(name) };
    Value funcVal { Value::nativeVal(function, nullptr, funcType, defaults) };
    if (resolveArgMask)
        asNative(funcVal)->resolveArgMask = resolveArgMask;
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


void VM::enableOpcodeProfiling(std::string filePath)
{
#ifndef DEBUG_BUILD
    throw std::runtime_error("Opcode profiling is only available in debug builds.");
#else
    std::filesystem::path path = filePath.empty() ? std::filesystem::path("opcode_profile.json")
                                                  : std::filesystem::path(filePath);

    opcodeProfilePath = std::filesystem::absolute(path);

    for (auto& counter : opcodeProfileCounts)
        counter.store(0, std::memory_order_relaxed);

    if (!opcodeProfilePath.empty()) {
        std::error_code ec;
        if (std::filesystem::exists(opcodeProfilePath, ec)) {
            if (ec) {
                std::cerr << "Warning: unable to check opcode profile file '" << opcodeProfilePath << "': " << ec.message() << std::endl;
            } else {
                std::ifstream in(opcodeProfilePath);
                if (in) {
                    std::ostringstream buffer;
                    buffer << in.rdbuf();
                    std::string contents = buffer.str();
                    std::string err;
                    auto json = json11::Json::parse(contents, err);
                    if (err.empty() && json.is_object()) {
                        for (const auto& kv : json.object_items()) {
                            size_t opcodeIndex = 0;
                            try {
                                opcodeIndex = static_cast<size_t>(std::stoul(kv.first));
                            } catch (const std::exception&) {
                                continue;
                            }
                            if (opcodeIndex >= opcodeProfileCounts.size())
                                continue;

                            uint64_t value = 0;
                            const auto& jvalue = kv.second;
                            if (jvalue.is_number()) {
                                double num = jvalue.number_value();
                                if (num >= 0.0) {
                                    value = static_cast<uint64_t>(num);
                                }
                            } else if (jvalue.is_string()) {
                                try {
                                    value = std::stoull(jvalue.string_value());
                                } catch (const std::exception&) {
                                    continue;
                                }
                            }

                            opcodeProfileCounts[opcodeIndex].store(value, std::memory_order_relaxed);
                        }
                    } else if (!err.empty()) {
                        std::cerr << "Warning: failed to parse opcode profile file '" << opcodeProfilePath << "': " << err << std::endl;
                    }
                } else {
                    std::cerr << "Warning: failed to open opcode profile file '" << opcodeProfilePath << "' for reading" << std::endl;
                }
            }
        } else if (ec) {
            std::cerr << "Warning: unable to check opcode profile file '" << opcodeProfilePath << "': " << ec.message() << std::endl;
        }
    }

    opcodeProfilingEnabled.store(true, std::memory_order_release);
#endif
}

void VM::writeOpcodeProfile()
{
#ifndef DEBUG_BUILD
    throw std::runtime_error("Opcode profiling is only available in debug builds.");
#else
    if (!opcodeProfilingEnabled.load(std::memory_order_acquire))
        return;

    std::error_code ec;
    const auto parent = opcodeProfilePath.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            std::cerr << "Warning: failed to create directory for opcode profile file '" << opcodeProfilePath
                      << "': " << ec.message() << std::endl;
            return;
        }
    }

    json11::Json::object obj;
    for (size_t i = 0; i < opcodeProfileCounts.size(); ++i) {
        auto value = opcodeProfileCounts[i].load(std::memory_order_relaxed);
        obj[std::to_string(i)] = json11::Json(static_cast<double>(value));
    }

    std::ofstream out(opcodeProfilePath, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "Warning: failed to open opcode profile file '" << opcodeProfilePath
                  << "' for writing." << std::endl;
        return;
    }

    json11::Json json(obj);
    out << json.dump();
    if (!out) {
        std::cerr << "Warning: failed to write opcode profile file '" << opcodeProfilePath << "'." << std::endl;
    }
#endif
}


std::pair<ExecutionStatus,Value> VM::execute(TimePoint deadline)
{
    if (thread->frames.empty() ||
        asFunction(asClosure(thread->frames.back().closure)->function)->chunk->code.size() == 0)
        return std::make_pair(ExecutionStatus::OK, Value::nilVal()); // nothing to execute

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

    // Deadline-based yielding support
    const bool hasDeadline = (deadline != TimePoint::max());
    auto yieldReturn = std::make_pair(ExecutionStatus::Yielded, Value::nilVal());
    nativeCallDeadline_ = deadline; // expose to callNativeFn() for timing

    // Reference to RT callback manager for instruction loop callback checks
    auto& rtMgr = RTCallbackManager::instance();

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

    auto errorReturn = std::make_pair(ExecutionStatus::RuntimeError,Value::nilVal());


    //
    //  main dispatch loop

    for(;;) {

        // Local alias for the Thread field so existing handler code can use the
        // same name.  The Thread field is also accessed by tryAwait* helpers.
        auto& instructionStart = thread->instructionStart;

        if (runtimeErrorFlag.load())
            return errorReturn;

        // Constructor setter cleanup: after setter frames execute and return,
        // clean up their results and push the saved instance
        if (thread->pendingSetterCount > 0 && thread->frames.size() == frame_depth_on_entry) {
            // All setter frames have returned, clean up
            popN(thread->pendingSetterCount);
            push(thread->pendingConstructorInstance);
            thread->pendingSetterCount = 0;
            thread->pendingConstructorInstance = Value::nilVal();
        }

        // Pending conversion operator cleanup: after conversion method returns,
        // complete the deferred operation (e.g. string concatenation)
        if (!thread->pendingConversions.empty()
            && thread->frames.size() == thread->pendingConversions.back().frameDepth) {
            auto pending = thread->pendingConversions.back();
            thread->pendingConversions.pop_back();
            Value converted = pop();
            // Remove receiver from recursion guard
            auto& inProgress = thread->conversionInProgress;
            for (auto it = inProgress.begin(); it != inProgress.end(); ++it) {
                if (it->receiver.is(pending.convReceiver, false)) {
                    inProgress.erase(it);
                    break;
                }
            }
            if (pending.kind == Thread::PendingConversion::Kind::Concat) {
                UnicodeString lhs = asUString(pending.savedLHS);
                UnicodeString rhs = isString(converted)
                    ? asUString(converted)
                    : toUnicodeString(toString(converted));
                push(Value::stringVal(lhs + rhs));
            }
            else if (pending.kind == Thread::PendingConversion::Kind::TypeConversion) {
                // Conversion method returned the converted value — push it
                push(converted);
            }
        }

        // Clean up stale conversion recursion guards (for explicit TargetType(obj) calls
        // where there is no PendingConversion to trigger cleanup)
        if (!thread->conversionInProgress.empty()) {
            auto& guards = thread->conversionInProgress;
            guards.erase(
                std::remove_if(guards.begin(), guards.end(),
                    [&](const Thread::ConversionGuard& g) {
                        return thread->frames.size() <= g.frameDepth;
                    }),
                guards.end());
        }

        if (exitRequested.load())
            return std::make_pair(ExecutionStatus::OK,Value::nilVal());

        // if we're 'sleeping' don't execute any instructions
        //  (we may have been woken up by an event or a spurious wakeup, in which case we'll re-block below)
        if (thread->threadSleep)
           goto postInstructionDispatch;

        // if awaiting a future, check if it resolved; otherwise keep sleeping
        if (thread->awaitedFuture.isNonNil()) {
            ObjFuture* fut = asFuture(thread->awaitedFuture);
            if (fut->future.wait_for(std::chrono::microseconds(0)) != std::future_status::ready)
                goto postInstructionDispatch; // still pending
            thread->awaitedFuture = Value::nilVal(); // resolved, clear and proceed
        }


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
                return std::make_pair(ExecutionStatus::RuntimeError,Value::nilVal());
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
                        auto& paramType = paramOpt->type.value();
                        Value& arg = *(frame->slots + 1 + pi);

                        // User-defined Object/Actor parameter types: check type match.
                        // Constructor auto-conversion for function params is handled by
                        // the compiler emitting ToTypeSpec opcodes (TODO: future improvement).
                        if ((paramType->builtin == type::BuiltinType::Object
                             || paramType->builtin == type::BuiltinType::Actor)
                            && paramType->obj.has_value()) {
                            auto& typeName = paramType->obj.value().name;
                            auto typeVal = asModuleType(
                                asFunction(asClosure(frame->closure)->function)->moduleType
                            )->vars.load(typeName);
                            if (typeVal.has_value() && isTypeSpec(typeVal.value()) && !arg.is(typeVal.value())) {
                                runtimeError("unable to convert " + to_string(arg.type())
                                             + " to " + toUTF8StdString(typeName));
                                return std::make_pair(ExecutionStatus::RuntimeError, Value::nilVal());
                            }
                            continue;
                        }

                        auto vt = builtinToValueType(paramType->builtin);
                        if (!vt.has_value()) continue;
                        try {
                            arg = toType(vt.value(), arg, strictConv);
                        } catch(std::exception& e) {
                            runtimeError(e.what());
                            return std::make_pair(ExecutionStatus::RuntimeError,Value::nilVal());
                        }
                    }
                }
            }


        }


        // Save IP before reading the opcode so we can rewind if the
        // instruction needs to wait on an unresolved future.
        instructionStart = frame->ip;

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

        #ifdef DEBUG_BUILD
        if (opcodeProfilingEnabled.load(std::memory_order_relaxed)) {
            size_t opcodeIndex = static_cast<size_t>(instruction);
            if (opcodeIndex < opcodeProfileCounts.size())
                opcodeProfileCounts[opcodeIndex].fetch_add(1, std::memory_order_relaxed);
        }
        #endif

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
            case OpCode::GetPropSignal: {
                Value& inst { peek(0) };
                ObjString* name = readString();

                {
                    auto s = tryAwaitValue(inst);
                    if (s == FutureStatus::Pending) goto postInstructionDispatch;
                    if (s == FutureStatus::Error) return errorReturn;
                }

                std::string signalName = toUTF8StdString(name->s);

                if (isObjectInstance(inst)) {
                    ObjectInstance* objInst = asObjectInstance(inst);
                    ObjObjectType* type = asObjectType(objInst->instanceType);
                    auto* prop = objInst->findProperty(name->hash);
                    if (prop) {
                        ast::Access propAccess = ast::Access::Public;
                        Value ownerT = objInst->instanceType.weakRef();
                        auto pit = type->properties.find(name->hash);
                        if (pit != type->properties.end()) {
                            propAccess = pit->second.access;
                            ownerT = pit->second.ownerType;
                        }
                        if (!isAccessAllowed(ownerT, propAccess)) {
                            runtimeError("Cannot access private member '%s'", toUTF8StdString(name->s).c_str());
                            return errorReturn;
                        }

                        Value result = prop->value;
                        if (!isSignal(result))
                            result = objInst->ensurePropertySignal(name->hash, signalName);
                        pop();
                        push(result);
                        break;
                    }

                    auto br = bindMethod(type, name);
                    if (br == BindResult::Bound) {
                        runtimeError("'changes' requires a property when using object member access.");
                        return errorReturn;
                    }
                    if (br == BindResult::Private)
                        return errorReturn;

                    runtimeError("Undefined method or property '"+toUTF8StdString(name->s)+"' for instance type '"+toUTF8StdString(type->name)+"'.");
                    return errorReturn;
                } else if (isActorInstance(inst)) {
                    ActorInstance* actorInst = asActorInstance(inst);
                    ObjObjectType* type = asObjectType(actorInst->instanceType);
                    auto* prop = actorInst->findProperty(name->hash);
                    if (prop) {
                        ast::Access propAccess = ast::Access::Public;
                        Value ownerT = actorInst->instanceType.weakRef();
                        auto pit = type->properties.find(name->hash);
                        if (pit != type->properties.end()) {
                            propAccess = pit->second.access;
                            ownerT = pit->second.ownerType;
                        }
                        if (!isAccessAllowed(ownerT, propAccess)) {
                            runtimeError("Cannot access private member '%s'", toUTF8StdString(name->s).c_str());
                            return errorReturn;
                        }

                        Value result = prop->value;
                        if (!isSignal(result))
                            result = actorInst->ensurePropertySignal(name->hash, signalName);
                        pop();
                        push(result);
                        break;
                    }

                    auto br = bindMethod(type, name);
                    if (br == BindResult::Bound) {
                        runtimeError("'changes' requires a property when using object member access.");
                        return errorReturn;
                    }
                    if (br == BindResult::Private)
                        return errorReturn;

                    // Check builtin methods (actors, vectors, matrices, etc.)
                    auto vt = inst.type();
                    auto mit = builtinMethods.find(vt);
                    if (mit != builtinMethods.end()) {
                        auto methodIt = mit->second.find(name->hash);
                        if (methodIt != mit->second.end()) {
                            runtimeError("'changes' requires a property when using object member access.");
                            return errorReturn;
                        }
                    }

                    runtimeError("Undefined method or property '"+toUTF8StdString(name->s)+"' for instance type '"+toUTF8StdString(type->name)+"'.");
                    return errorReturn;
                }

                runtimeError("Only object and actor instances have properties (string keys only).");
                return errorReturn;
            }
            case OpCode::MoveProp: {
                Value& inst { peek(0) };
                ObjString* name = readString();
                inst.resolveSignal();
                if (runtimeErrorFlag.load()) return errorReturn;
                VariablesMap::MonitoredValue* prop = nullptr;
                if (isObjectInstance(inst)) {
                    prop = asObjectInstance(inst)->findProperty(name->hash);
                } else if (isActorInstance(inst)) {
                    prop = asActorInstance(inst)->findProperty(name->hash);
                } else {
                    runtimeError("Cannot move property from non-object value");
                    return errorReturn;
                }
                if (!prop) {
                    runtimeError("Undefined property '"+toUTF8StdString(name->s)+"'");
                    return errorReturn;
                }
                Value value = prop->value;
                pop();
                push(value);
                prop->value = Value::nilVal();
                break;
            }
            case OpCode::GetProp: {
                Value& inst { peek(0) };
                ObjString* name = readString();

                // Resolve futures first (but NOT signals - we need to check for signal properties)
                {
                    auto s = tryAwaitFuture(inst);
                    if (s == FutureStatus::Pending) goto postInstructionDispatch;
                    if (s == FutureStatus::Error) return errorReturn;
                }

                // Check for signal properties AFTER resolving futures but BEFORE resolving signals
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
                                                             methodInfo.funcType, methodInfo.defaultValues,
                                                             methodInfo.declFunction) };
                            pop();
                            push(bm);
                            break;
                        }
                    }

                    runtimeError("Undefined property '"+toUTF8StdString(name->s)+"' for signal.");
                    return errorReturn;
                }

                // Now resolve signals for non-signal types
                inst.resolveSignal();
                if (runtimeErrorFlag.load())
                    return errorReturn;
                if (isEventInstance(inst)) {
                    ObjEventInstance* eventInst = asEventInstance(inst);
                    if (!eventInst->typeHandle.isAlive() || !isEventType(eventInst->typeHandle)) {
                        runtimeError("Event instance is no longer associated with a live event type.");
                        return errorReturn;
                    }

                    // Look up property directly in payload map by name hash
                    auto pit = eventInst->payload.find(name->hash);
                    if (pit != eventInst->payload.end()) {
                        Value result = pit->second;
                        // Propagate const transitively: event payload refs inherit const from event.
                        // Use constRef() (not createFrozenSnapshot) to preserve object identity
                        // for 'is' checks while still blocking mutation through const enforcement.
                        if (inst.isConst() && result.isObj() && !result.isConst()) {
                            result = result.constRef();
                        }
                        pop();
                        push(result);
                        break;
                    }

                    auto mit = builtinMethods.find(inst.type());
                    if (mit != builtinMethods.end()) {
                        auto methodIt = mit->second.find(name->hash);
                        if (methodIt != mit->second.end()) {
                            const BuiltinMethodInfo& methodInfo = methodIt->second;
                            Value bound { Value::boundNativeVal(inst, methodInfo.function, methodInfo.isProc,
                                                                methodInfo.funcType, methodInfo.defaultValues,
                                                                methodInfo.declFunction) };
                            pop();
                            push(bound);
                            break;
                        }
                    }

                    runtimeError("Undefined property '" + toUTF8StdString(name->s) + "' for event instance.");
                    return errorReturn;
                }

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
                        // MVCC: propagate const + resolve snapshot for reference-type values
                        if (inst.isConst() && result.isObj() && !result.isConst()) {
                            auto* token = dict->control->snapshotToken;
                            if (token) {
                                result = resolveConstChild(result, token);
                                dict->cacheValue(key, result);
                            }
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

                    // Check if this property has a getter method
                    // Optimization: skip accessor search for properties starting with '_'
                    // (backing fields cannot have accessors)
                    if (!name->s.startsWith("_")) {
                        icu::UnicodeString getterName = UnicodeString("__get_") + name->s;
                        Value getterNameValue = Value::stringVal(getterName);
                        ObjString* getterNameStr = asStringObj(getterNameValue);
                        auto getterIt = asObjectType(objInst->instanceType)->methods.find(getterNameStr->hash);
                        if (getterIt != asObjectType(objInst->instanceType)->methods.end()) {
                            // Property has a getter - invoke it instead of direct access
                            // Stack: [instance]
                            // Call __get_<property>() with instance as receiver
                            CallSpec callSpec{0}; // 0 arguments
                            if (!invoke(getterNameStr, callSpec))
                                return errorReturn;
                            frame = thread->frames.end()-1;
                            break;
                        }
                    }

                    // is it an instance property?
                    auto* prop = objInst->findProperty(name->hash);
                    if (prop) { // exists
                        Value result = prop->value;
                        // MVCC const resolution: materialize frozen clone for reference-type children
                        if (inst.isConst() && result.isObj() && !result.isConst()) {
                            auto* token = objInst->control->snapshotToken;
                            if (token)
                                result = resolveConstChild(result, token, &prop->value);
                        }
                        pop();
                        push(result);
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
                    auto* prop = actorInst->findProperty(name->hash);
                    if (prop) {
                        Value result = prop->value;
                        // MVCC const resolution
                        if (inst.isConst() && result.isObj() && !result.isConst()) {
                            auto* token = actorInst->control->snapshotToken;
                            if (token)
                                result = resolveConstChild(result, token, &prop->value);
                        }
                        pop();
                        push(result);
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
                                                                          methodInfo.funcType, methodInfo.defaultValues,
                                                                          methodInfo.declFunction) };
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
                            // Const enforcement: reject mutating methods on const receivers
                            if (inst.isConst() && !methodInfo.noMutateSelf) {
                                runtimeError("Cannot call mutating method '%s' on const value.",
                                             toUTF8StdString(name->s).c_str());
                                return errorReturn;
                            }
                            Value bm { Value::boundNativeVal(inst, methodInfo.function, methodInfo.isProc,
                                                             methodInfo.funcType, methodInfo.defaultValues,
                                                             methodInfo.declFunction) };
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

                if (inst.isNil())
                    runtimeError("Attempted member or property access on nil");
                else
                    runtimeError("Only object and actor instances have methods and only object, actor, and dictionary instances have properties (string keys only).");
#ifdef DEBUG_BUILD
                if (inst.isObj()) {
                    std::cerr << "GetProp fallback objType=" << int(objType(inst)) << std::endl;
                }
#endif
                return errorReturn;
                break;
            }
            case OpCode::GetPropCheck: {
                Value& inst { peek(0) };
                ObjString* name = readString();

                // Resolve futures first (but NOT signals - we need to check for signal properties)
                {
                    auto s = tryAwaitFuture(inst);
                    if (s == FutureStatus::Pending) goto postInstructionDispatch;
                    if (s == FutureStatus::Error) return errorReturn;
                }

                // Check for signal properties AFTER resolving futures but BEFORE resolving signals
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
                                                             methodInfo.funcType, methodInfo.defaultValues,
                                                             methodInfo.declFunction) };
                            pop();
                            push(bm);
                            break;
                        }
                    }

                    runtimeError("Undefined property '"+toUTF8StdString(name->s)+"' for signal.");
                    return errorReturn;
                }

                // Now resolve signals for non-signal types
                inst.resolveSignal();
                if (runtimeErrorFlag.load())
                    return errorReturn;
                if (isEventInstance(inst)) {
                    ObjEventInstance* eventInst = asEventInstance(inst);
                    if (!eventInst->typeHandle.isAlive() || !isEventType(eventInst->typeHandle)) {
                        runtimeError("Event instance is no longer associated with a live event type.");
                        return errorReturn;
                    }

                    // Look up property directly in payload map by name hash
                    auto pit = eventInst->payload.find(name->hash);
                    if (pit != eventInst->payload.end()) {
                        Value result = pit->second;
                        // Propagate const transitively: event payload refs inherit const from event.
                        // Use constRef() (not createFrozenSnapshot) to preserve object identity
                        // for 'is' checks while still blocking mutation through const enforcement.
                        if (inst.isConst() && result.isObj() && !result.isConst()) {
                            result = result.constRef();
                        }
                        pop();
                        push(result);
                        break;
                    }

                    auto mit = builtinMethods.find(inst.type());
                    if (mit != builtinMethods.end()) {
                        auto methodIt = mit->second.find(name->hash);
                        if (methodIt != mit->second.end()) {
                            const BuiltinMethodInfo& methodInfo = methodIt->second;
                            Value bound { Value::boundNativeVal(inst, methodInfo.function, methodInfo.isProc,
                                                                methodInfo.funcType, methodInfo.defaultValues,
                                                                methodInfo.declFunction) };
                            pop();
                            push(bound);
                            break;
                        }
                    }

                    runtimeError("Undefined property '" + toUTF8StdString(name->s) + "' for event instance.");
                    return errorReturn;
                }
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
                        // MVCC: propagate const + resolve snapshot for reference-type values
                        if (inst.isConst() && result.isObj() && !result.isConst()) {
                            auto* token = dict->control->snapshotToken;
                            if (token) {
                                result = resolveConstChild(result, token);
                                dict->cacheValue(key, result);
                            }
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
                    auto* prop = objInst->findProperty(name->hash);
                    if (prop) {
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
                        {
                            Value result = prop->value;
                            // MVCC const resolution
                            if (inst.isConst() && result.isObj() && !result.isConst()) {
                                auto* token = objInst->control->snapshotToken;
                                if (token)
                                    result = resolveConstChild(result, token, &prop->value);
                            }
                            pop();
                            push(result);
                        }
                        break;
                    } else {
                        // Check if this property has a getter method
                        icu::UnicodeString getterName = UnicodeString("__get_") + name->s;
                        Value getterNameValue = Value::stringVal(getterName);
                        ObjString* getterNameStr = asStringObj(getterNameValue);
                        auto getterIt = t->methods.find(getterNameStr->hash);
                        if (getterIt != t->methods.end()) {
                            // Property has a getter - invoke it instead of direct access
                            CallSpec callSpec{0}; // 0 arguments
                            if (!invoke(getterNameStr, callSpec))
                                return errorReturn;
                            frame = thread->frames.end()-1;
                            break;
                        }

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
                    auto* prop = actorInst->findProperty(name->hash);
                    if (prop) {
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
                        {
                            Value result = prop->value;
                            // MVCC const resolution
                            if (inst.isConst() && result.isObj() && !result.isConst()) {
                                auto* token = actorInst->control->snapshotToken;
                                if (token)
                                    result = resolveConstChild(result, token, &prop->value);
                            }
                            pop();
                            push(result);
                        }
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
                                                                          methodInfo.funcType, methodInfo.defaultValues,
                                                                          methodInfo.declFunction) };
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
                            // Const enforcement: reject mutating methods on const receivers
                            if (inst.isConst() && !methodInfo.noMutateSelf) {
                                runtimeError("Cannot call mutating method '%s' on const value.",
                                             toUTF8StdString(name->s).c_str());
                                return errorReturn;
                            }
                            Value bm { Value::boundNativeVal(inst, methodInfo.function, methodInfo.isProc,
                                                             methodInfo.funcType, methodInfo.defaultValues,
                                                             methodInfo.declFunction) };
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

                if (inst.isNil())
                    runtimeError("Attempted member or property access on nil");
                else
                    runtimeError("Only object and actor instances have methods and only object, actor, and dictionary instances have properties (string keys only).");
                return errorReturn;
                break;
            }
            case OpCode::SetProp: {
                Value& inst { peek(1) };
                ObjString* name = readString();

                if (inst.isConst()) {
                    runtimeError("Cannot mutate const: assignment to '%s'", toUTF8StdString(name->s).c_str());
                    return errorReturn;
                }

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

                if (isEventInstance(inst)) {
                    runtimeError("Cannot assign to property '" + toUTF8StdString(name->s) + "' of event instance.");
                    return errorReturn;
                }

                {
                    auto s = tryAwaitValue(inst);
                    if (s == FutureStatus::Pending) goto postInstructionDispatch;
                    if (s == FutureStatus::Error) return errorReturn;
                }
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
                    ObjObjectType* t = asObjectType(objInst->instanceType);

                    // Check if this property has a setter method
                    // Optimization: skip accessor search for properties starting with '_'
                    // (backing fields cannot have accessors)
                    if (!name->s.startsWith("_")) {
                        icu::UnicodeString setterName = UnicodeString("__set_") + name->s;
                        Value setterNameValue = Value::stringVal(setterName);
                        ObjString* setterNameStr = asStringObj(setterNameValue);
                        auto setterIt = t->methods.find(setterNameStr->hash);
                        if (setterIt != t->methods.end()) {
                            // Property has a setter - invoke it instead of direct assignment
                            // Stack: [instance, value]
                            // Call __set_<property>(value) with instance as receiver and value as arg
                            CallSpec callSpec{1}; // 1 argument
                            if (!invoke(setterNameStr, callSpec))
                                return errorReturn;
                            frame = thread->frames.end()-1;
                            break;
                        }
                    }

                    const auto& properties { t->properties };
                    auto propertyIt = properties.find(name->hash);
                    if (propertyIt != properties.end() && propertyIt->second.isConst) {
                        runtimeError("Cannot assign to constant '" + toUTF8StdString(name->s) + "' of object type '" + toUTF8StdString(t->name) + "'");
                        return errorReturn;
                    }
                    Value value { peek(0) };

                    if (!value.isNil()) {
                        bool strictConv = asFunction(asClosure(frame->closure)->function)->strict;
                        // if type object specified the property type in the declaration,
                        //  convert the value to that type (if possible)
                        if (propertyIt != properties.end()) {
                            const auto& prop { propertyIt->second };
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


                    // Clone vector/matrix/tensor for by-value semantics
                    objInst->assignProperty(name->hash, cloneIfValueSemantics(value));
                    popN(2); // pop original value & instance
                    push(value); // value (possibly converted)
                    break;
                } else if (isActorInstance(inst)) {
                    ActorInstance* actorInst = asActorInstance(inst);
                    ObjObjectType* tA = asObjectType(actorInst->instanceType);
                    const auto& properties { tA->properties };
                    auto propertyIt = properties.find(name->hash);
                    if (propertyIt != properties.end() && propertyIt->second.isConst) {
                        runtimeError("Cannot assign to constant '" + toUTF8StdString(name->s) + "' of actor type '" + toUTF8StdString(tA->name) + "'");
                        return errorReturn;
                    }
                    Value value { peek(0) };

                    if (!value.isNil()) {
                        bool strictConv = asFunction(asClosure(frame->closure)->function)->strict;
                        if (propertyIt != properties.end()) {
                            const auto& prop { propertyIt->second };
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

                    // Clone vector/matrix/tensor for by-value semantics
                    actorInst->assignProperty(name->hash, cloneIfValueSemantics(value));
                    popN(2);
                    push(value);
                    break;
                } else if (isModuleType(inst)) {
                    auto moduleType = asModuleType(inst);

                    if (moduleType->constVars.find(name->hash) != moduleType->constVars.end()) {
                        runtimeError("Cannot assign to module constant '" + toUTF8StdString(name->s) + "'");
                        return errorReturn;
                    }

                    auto& vars { moduleType->vars };

                    // TODO: consider if we should allow setting module vars from another module
                    //  (maybe only if !strict?)

                    if (vars.exists(name->hash)) {
                        Value value { peek(0) };

                        // Clone vector/matrix/tensor for by-value semantics
                        vars.store(name->hash, name->s, cloneIfValueSemantics(value), /*overwrite=*/true);
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

                if (inst.isConst()) {
                    runtimeError("Cannot mutate const: assignment to '%s'", toUTF8StdString(name->s).c_str());
                    return errorReturn;
                }

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

                {
                    auto s = tryAwaitValue(inst);
                    if (s == FutureStatus::Pending) goto postInstructionDispatch;
                    if (s == FutureStatus::Error) return errorReturn;
                }
                if (isEventInstance(inst)) {
                    runtimeError("Cannot assign to property '" + toUTF8StdString(name->s) + "' of event instance.");
                    return errorReturn;
                }
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

                    // Check if this property has a setter method
                    icu::UnicodeString setterName = UnicodeString("__set_") + name->s;
                    Value setterNameValue = Value::stringVal(setterName);
                    ObjString* setterNameStr = asStringObj(setterNameValue);
                    auto setterIt = t->methods.find(setterNameStr->hash);
                    if (setterIt != t->methods.end()) {
                        // Property has a setter - invoke it instead of direct assignment
                        // Stack: [instance, value]
                        // Call __set_<property>(value) with instance as receiver and value as arg
                        CallSpec callSpec{1}; // 1 argument
                        if (!invoke(setterNameStr, callSpec))
                            return errorReturn;
                        frame = thread->frames.end()-1;
                        break;
                    }

                    const auto& properties { t->properties };
                    auto propertyIt = properties.find(name->hash);
                    if (propertyIt != properties.end() && propertyIt->second.isConst) {
                        runtimeError("Cannot assign to constant '" + toUTF8StdString(name->s) + "' of object type '" + toUTF8StdString(t->name) + "'");
                        return errorReturn;
                    }

                    Value value { peek(0) };

                    if (!value.isNil()) {
                        bool strictConv = asFunction(asClosure(frame->closure)->function)->strict;
                        if (propertyIt != properties.end()) {
                            const auto& prop { propertyIt->second };
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

                    auto pit = propertyIt;
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

                    // Clone vector/matrix/tensor for by-value semantics
                    objInst->assignProperty(name->hash, cloneIfValueSemantics(value));
                    popN(2);
                    push(value);
                    break;
                } else if (isActorInstance(inst)) {
                    ActorInstance* actorInst = asActorInstance(inst);
                    ObjObjectType* tA = asObjectType(actorInst->instanceType);
                    const auto& properties { tA->properties };
                    auto propertyIt = properties.find(name->hash);
                    if (propertyIt != properties.end() && propertyIt->second.isConst) {
                        runtimeError("Cannot assign to constant '" + toUTF8StdString(name->s) + "' of actor type '" + toUTF8StdString(tA->name) + "'");
                        return errorReturn;
                    }

                    Value value { peek(0) };

                    if (!value.isNil()) {
                        bool strictConv = asFunction(asClosure(frame->closure)->function)->strict;
                        if (propertyIt != properties.end()) {
                            const auto& prop { propertyIt->second };
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

                    auto pit = propertyIt;
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

                    // Clone vector/matrix/tensor for by-value semantics
                    actorInst->assignProperty(name->hash, cloneIfValueSemantics(value));
                    popN(2);
                    push(value);
                    break;
                } else if (isModuleType(inst)) {
                    auto moduleType = asModuleType(inst);

                    if (moduleType->constVars.find(name->hash) != moduleType->constVars.end()) {
                        runtimeError("Cannot assign to module constant '" + toUTF8StdString(name->s) + "'");
                        return errorReturn;
                    }

                    auto& vars { moduleType->vars };

                    if (vars.exists(name->hash)) {
                        Value value { peek(0) };

                        // Clone vector/matrix/tensor for by-value semantics
                        vars.store(name->hash, name->s, cloneIfValueSemantics(value), /*overwrite=*/true);
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
                    return std::make_pair(ExecutionStatus::RuntimeError,Value::nilVal());

                break;
            }
            case OpCode::Equal: {
                if (tryAwaitFutures(peek(0), peek(1)) != FutureStatus::Resolved)
                    goto postInstructionDispatch;

                if (tryDispatchBinaryOperator(opHashEq)) {
                    frame = thread->frames.end() - 1;
                    break;
                }

                try {
                    binaryOp([&](Value a, Value b) -> Value { return equal(a, b, frame->strict); });
                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return errorReturn;
                }
                break;
            }
            case OpCode::Is: {
                {
                    auto s = tryAwaitValues(peek(0), peek(1));
                    if (s == FutureStatus::Pending) goto postInstructionDispatch;
                    if (s == FutureStatus::Error) return errorReturn;
                }
                Value b = pop();
                Value a = pop();
                push(Value::boolVal(a.is(b, frame->strict)));
                break;
            }
            case OpCode::In: {
                {
                    auto s = tryAwaitValues(peek(0), peek(1));
                    if (s == FutureStatus::Pending) goto postInstructionDispatch;
                    if (s == FutureStatus::Error) return errorReturn;
                }
                Value container = pop();
                Value needle = pop();

                bool result = false;

                if (isList(container)) {
                    ObjList* list = asList(container);
                    for (int32_t i = 0; i < list->length(); i++) {
                        if (needle.equals(list->getElement(i), frame->strict)) {
                            result = true;
                            break;
                        }
                    }
                }
                else if (isDict(container)) {
                    ObjDict* dict = asDict(container);
                    result = dict->contains(needle);
                }
                else if (isString(container)) {
                    if (!isString(needle)) {
                        runtimeError("'in' for string requires string operand on left side");
                        return errorReturn;
                    }
                    ObjString* haystack = asStringObj(container);
                    ObjString* needleStr = asStringObj(needle);
                    result = (haystack->s.indexOf(needleStr->s) >= 0);
                }
                else if (isRange(container)) {
                    ObjRange* range = asRange(container);
                    if (!needle.isNumber()) {
                        runtimeError("'in' for range requires numeric operand on left side");
                        return errorReturn;
                    }
                    double val = needle.asReal();
                    double start = range->start.isNil() ? 0 : range->start.asReal();
                    double stop = range->stop.asReal();
                    double step = range->step.isNil() ? 1.0 : range->step.asReal();

                    if (step > 0) {
                        bool inBounds = range->closed
                            ? (val >= start && val <= stop)
                            : (val >= start && val < stop);
                        if (inBounds && step != 1.0) {
                            double offset = val - start;
                            result = (fmod(offset, step) == 0);
                        } else {
                            result = inBounds;
                        }
                    } else {
                        // Negative step (reverse range)
                        bool inBounds = range->closed
                            ? (val <= start && val >= stop)
                            : (val <= start && val > stop);
                        if (inBounds && step != -1.0) {
                            double offset = start - val;
                            result = (fmod(offset, -step) == 0);
                        } else {
                            result = inBounds;
                        }
                    }
                }
                else {
                    runtimeError("'in' operator requires list, dict, string, or range on right side");
                    return errorReturn;
                }

                push(Value::boolVal(result));
                break;
            }
            case OpCode::Greater: {
                if (tryAwaitFutures(peek(0), peek(1)) != FutureStatus::Resolved)
                    goto postInstructionDispatch;

                if (tryDispatchBinaryOperator(opHashGt)) {
                    frame = thread->frames.end() - 1;
                    break;
                }

                try {
                    binaryOp([](Value a, Value b) -> Value { return greater(a,b); });
                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return errorReturn;
                }
                break;
            }
            case OpCode::Less: {
                if (tryAwaitFutures(peek(0), peek(1)) != FutureStatus::Resolved)
                    goto postInstructionDispatch;

                if (tryDispatchBinaryOperator(opHashLt)) {
                    frame = thread->frames.end() - 1;
                    break;
                }

                try {
                    binaryOp([](Value a, Value b) -> Value { return less(a,b); });
                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return errorReturn;
                }
                break;
            }
            case OpCode::GreaterEqual: {
                if (tryAwaitFutures(peek(0), peek(1)) != FutureStatus::Resolved)
                    goto postInstructionDispatch;

                if (tryDispatchBinaryOperator(opHashGe)) {
                    frame = thread->frames.end() - 1;
                    break;
                }

                try {
                    binaryOp([](Value a, Value b) -> Value { return greaterEqual(a,b); });
                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return errorReturn;
                }
                break;
            }
            case OpCode::LessEqual: {
                if (tryAwaitFutures(peek(0), peek(1)) != FutureStatus::Resolved)
                    goto postInstructionDispatch;

                if (tryDispatchBinaryOperator(opHashLe)) {
                    frame = thread->frames.end() - 1;
                    break;
                }

                try {
                    binaryOp([](Value a, Value b) -> Value { return lessEqual(a,b); });
                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return errorReturn;
                }
                break;
            }
            case OpCode::NotEqual: {
                if (tryAwaitFutures(peek(0), peek(1)) != FutureStatus::Resolved)
                    goto postInstructionDispatch;

                if (tryDispatchBinaryOperator(opHashNe)) {
                    frame = thread->frames.end() - 1;
                    break;
                }

                try {
                    binaryOp([&](Value a, Value b) -> Value { return notEqual(a, b, frame->strict); });
                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return errorReturn;
                }
                break;
            }
            case OpCode::Add: {
                if (tryAwaitFutures(peek(0), peek(1)) != FutureStatus::Resolved)
                    goto postInstructionDispatch;

                // String concatenation takes priority when LHS is a string
                // (string behaves as if it has a built-in operator+)
                if (isString(peek(1))) {
                    // Check for @implicit operator string() on RHS before falling to concatenate()
                    if (!isString(peek(0)) && (isObjectInstance(peek(0)) || isActorInstance(peek(0)))) {
                        Value rhs = pop();
                        Value lhs = pop();
                        auto outcome = tryConvertValue(rhs, Value::typeVal(ValueType::String),
                                                       false, /*implicitCall=*/true,
                                                       Thread::PendingConversion::Kind::Concat, lhs);
                        if (outcome.result == ConversionResult::NeedsAsyncFrame) {
                            frame = thread->frames.end() - 1;
                            break;
                        }
                        if (outcome.result == ConversionResult::ConvertedSync) {
                            push(Value::stringVal(asUString(lhs) + (isString(outcome.convertedValue)
                                ? asUString(outcome.convertedValue)
                                : toUnicodeString(toString(outcome.convertedValue)))));
                            break;
                        }
                        // No conversion — push back and fall through to concatenate()
                        push(lhs);
                        push(rhs);
                    }
                    concatenate();
                } else {
                    if (tryDispatchBinaryOperator(opHashAdd)) {
                        frame = thread->frames.end() - 1;
                        break;
                    }
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
                if (tryAwaitFutures(peek(0), peek(1)) != FutureStatus::Resolved)
                    goto postInstructionDispatch;

                if (tryDispatchBinaryOperator(opHashSub)) {
                    frame = thread->frames.end() - 1;
                    break;
                }

                try {
                    binaryOp([](Value l, Value r) -> Value { return subtract(l, r); });
                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return errorReturn;
                }
                break;
            }
            case OpCode::Multiply: {
                if (tryAwaitFutures(peek(0), peek(1)) != FutureStatus::Resolved)
                    goto postInstructionDispatch;

                if (tryDispatchBinaryOperator(opHashMul)) {
                    frame = thread->frames.end() - 1;
                    break;
                }

                try {
                    binaryOp([](Value l, Value r) -> Value { return multiply(l, r); });
                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return errorReturn;
                }
                break;
            }
            case OpCode::Divide: {
                if (tryAwaitFutures(peek(0), peek(1)) != FutureStatus::Resolved)
                    goto postInstructionDispatch;

                if (tryDispatchBinaryOperator(opHashDiv)) {
                    frame = thread->frames.end() - 1;
                    break;
                }

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
                if (tryAwaitFuture(operand) != FutureStatus::Resolved)
                    goto postInstructionDispatch;

                if (tryDispatchUnaryOperator(opHashNeg)) {
                    frame = thread->frames.end() - 1;
                    break;
                }

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
                if (tryAwaitFutures(peek(0), peek(1)) != FutureStatus::Resolved)
                    goto postInstructionDispatch;

                if (tryDispatchBinaryOperator(opHashMod)) {
                    frame = thread->frames.end() - 1;
                    break;
                }

                try {
                    binaryOp([](Value a, Value b) -> Value { return mod(a,b); });
                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return errorReturn;
                }
                break;
            }
            case OpCode::And: {
                if (tryAwaitFutures(peek(0), peek(1)) != FutureStatus::Resolved)
                    goto postInstructionDispatch;
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
                if (tryAwaitFutures(peek(0), peek(1)) != FutureStatus::Resolved)
                    goto postInstructionDispatch;
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
                if (tryAwaitFutures(peek(0), peek(1)) != FutureStatus::Resolved)
                    goto postInstructionDispatch;
                try {
                    binaryOp([](Value a, Value b) -> Value { return band(a,b); });
                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return errorReturn;
                }
                break;
            }
            case OpCode::BitOr: {
                if (tryAwaitFutures(peek(0), peek(1)) != FutureStatus::Resolved)
                    goto postInstructionDispatch;
                try {
                    binaryOp([](Value a, Value b) -> Value { return bor(a,b); });
                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return errorReturn;
                }
                break;
            }
            case OpCode::BitXor: {
                if (tryAwaitFutures(peek(0), peek(1)) != FutureStatus::Resolved)
                    goto postInstructionDispatch;
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
                if (tryAwaitFuture(operand) != FutureStatus::Resolved)
                    goto postInstructionDispatch;
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
            case OpCode::MakeConst: {
                Value& top = peek(0);
                top = createFrozenSnapshot(top);
                break;
            }
            case OpCode::CopyInto: {
                Value rhs = pop();
                Value lhs = pop();
                if (lhs.isConst()) {
                    runtimeError("Cannot mutate const: copy-into (<-) on const value");
                    return errorReturn;
                }
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
                {
                    auto s = tryAwaitValue(peek(0));
                    if (s == FutureStatus::Pending) goto postInstructionDispatch;
                    if (s == FutureStatus::Error) return errorReturn;
                }
                if (isFalsey(peek(0)))
                    frame->ip += jumpDist;
                break;
            }
            case OpCode::JumpIfTrue: {
                uint16_t jumpDist = readShort();
                {
                    auto s = tryAwaitValue(peek(0));
                    if (s == FutureStatus::Pending) goto postInstructionDispatch;
                    if (s == FutureStatus::Error) return errorReturn;
                }
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
                {
                    auto s = tryAwaitValue(callee);
                    if (s == FutureStatus::Pending) goto postInstructionDispatch;
                    if (s == FutureStatus::Error) return errorReturn;
                }

                // Resolve future args for typed parameters (closures only).
                // Native functions use resolveArgMask; this handles Roxal-implemented
                // functions whose typed params should implicitly resolve futures.
                if (isClosure(callee)) {
                    ObjFunction* fn = asFunction(asClosure(callee)->function);
                    if (fn->funcType.has_value()) {
                        auto& ft = fn->funcType.value();
                        if (ft->func.has_value()) {
                            auto& params = ft->func->params;
                            for (size_t i = 0; i < params.size() && i < (size_t)callSpec.argCount; i++) {
                                if (params[i].has_value() && params[i]->type.has_value()) {
                                    Value& arg = peek(callSpec.argCount - 1 - i);
                                    if (isFuture(arg)) {
                                        auto s = tryAwaitFuture(arg);
                                        if (s != FutureStatus::Resolved)
                                            goto postInstructionDispatch;
                                    }
                                }
                            }
                        }
                    }
                }

                if (!callValue(callee, callSpec))
                    return errorReturn;

                // Check if a native function needs a future arg resolved
                if (thread->awaitedFuture.isNonNil()) {
                    frame->ip = instructionStart;
                    goto postInstructionDispatch;
                }

                frame = thread->frames.end()-1;

                // Constructor setter cleanup: if callValue() pushed setter frames,
                // they will execute and return, leaving their results on stack.
                // We detect this and clean up after all setters have executed.
                // The cleanup happens later when pendingSetterCount is checked
                // at the start of VM loop iterations.

                break;
            }
            case OpCode::Index: {
                uint8_t argCount = readByte();
                if (tryAwaitFuture(peek(argCount)) != FutureStatus::Resolved)
                    goto postInstructionDispatch;
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

                // Check if a native function needs a future arg resolved
                if (thread->awaitedFuture.isNonNil()) {
                    frame->ip = instructionStart;
                    goto postInstructionDispatch;
                }

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

                    // For nested execute() calls, only terminate when we return BELOW the entry depth.
                    // Use < (not <=) because we should only return if we've popped BELOW the entry depth,
                    // which means we've returned from the function that execute() was started for.
                    // Using <= would cause early return when a called function returns, even if the
                    // caller still has code to execute.
                    if (thread->execute_depth > 1 && thread->frames.size() < frame_depth_on_entry) {
                        Value returnVal = pop();

                        if (consumePendingObjectCleanup()) {
                            freeObjects(); // Drain pending objects on scope exit for deterministic destruction
                        }

                        if (thread->execute_depth > 0) thread->execute_depth--;
                        return std::make_pair(ExecutionStatus::OK,returnVal);
                    }

                    // For top-level execute(), use original termination logic
                    if (thread->execute_depth == 1 && thread->frames.empty()) {
                        Value returnVal = pop();

                        if (consumePendingObjectCleanup()) {
                            freeObjects();
                        }

                        if (thread->execute_depth > 0) thread->execute_depth--;
                        return std::make_pair(ExecutionStatus::OK,returnVal);
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

                    // For nested execute() calls, only terminate when we return BELOW the entry depth
                    // Use < (not <=) to avoid early return when a called function returns
                    if (thread->execute_depth > 1 && thread->frames.size() < frame_depth_on_entry) {
                        if (consumePendingObjectCleanup()) {
                            freeObjects(); // Drain pending objects on scope exit for deterministic destruction
                        }

                        if (thread->execute_depth > 0) thread->execute_depth--;
                        return std::make_pair(ExecutionStatus::OK,result);
                    }

                    // For top-level execute(), use original termination logic
                    if (thread->execute_depth == 1 && thread->frames.empty()) {
                        if (consumePendingObjectCleanup()) {
                            freeObjects();
                        }

                        if (thread->execute_depth > 0) thread->execute_depth--;
                        return std::make_pair(ExecutionStatus::OK,result);
                    }

                    // For continuation callbacks, push result to stack like OpCode::Return
                    // so processContinuationDispatch can pop it
                    if (thread->continuationCallbackReturned) {
                        push(result);
                        frame = thread->frames.end() - 1;
                        if (frame->ip == frame->startIp)
                            thread->frameStart = true;
                        break;
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
            case OpCode::MoveLocal: {
                uint16_t slot = singleByteArg ? readByte() : readShort();
                #ifdef DEBUG_BUILD
                auto stackIndex = (frame->slots - &thread->stack[0]) + slot;
                if (stackIndex >= thread->stack.size())
                    throw std::runtime_error("Stack overflow access");
                #endif
                push(frame->slots[slot]);
                frame->slots[slot] = Value::nilVal();
                break;
            }
            case OpCode::SetLocal: {
                uint16_t slot = singleByteArg ? readByte() : readShort();
                #ifdef DEBUG_BUILD
                auto stackIndex = (frame->slots - &thread->stack[0]) + slot;
                if (stackIndex >= thread->stack.size())
                    throw std::runtime_error("Stack overflow access");
                #endif
                frame->slots[slot] = cloneIfValueSemantics(peek(0));
                break;
            }
            case OpCode::SetIndex: {
                uint8_t argCount = readByte();
                if (tryAwaitFuture(peek(argCount)) != FutureStatus::Resolved)
                    goto postInstructionDispatch;
                if (peek(argCount).isConst()) {
                    runtimeError("Cannot mutate const: index assignment");
                    return errorReturn;
                }
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
            case OpCode::DefineModuleConst: {
                ObjString* name = readString();
                moduleType()->constVars.insert(name->hash);
                // Clone vector/matrix/tensor for by-value semantics
                moduleVars().store(name->hash, name->s, cloneIfValueSemantics(pop()));
                break;
            }
            case OpCode::DefineModuleVar: {
                ObjString* name = readString();
                // Clone vector/matrix/tensor for by-value semantics
                moduleVars().store(name->hash, name->s, cloneIfValueSemantics(pop()));
                break;
            }
            case OpCode::GetModuleVar: {
                ObjString* name = readString();
                auto& vars { moduleVars() };
                auto optValue { vars.load(name->hash) };
                if (optValue.has_value()) {
                    Value value = optValue.value();
                    if (onDataflowThread_ && value.isObj())
                        value = value.constRef();
                    push(value);
                }
                else {
                    runtimeError("Undefined variable '"+name->toStdString()+"'");
                    return errorReturn;
                }
                break;
            }
            case OpCode::MoveModuleVar: {
                ObjString* name = readString();
                if (onDataflowThread_) {
                    runtimeError("Cannot modify module variable '" + name->toStdString() + "' from dataflow function");
                    return errorReturn;
                }
                auto& vars { moduleVars() };
                auto optValue { vars.load(name->hash) };
                if (!optValue.has_value()) {
                    runtimeError("Undefined variable '"+name->toStdString()+"'");
                    return errorReturn;
                }
                push(optValue.value());
                vars.storeIfExists(name->hash, name->s, Value::nilVal());
                break;
            }
            case OpCode::GetModuleVarSignal: {
                ObjString* name = readString();
                auto& vars { moduleVars() };
                auto optValue { vars.load(name->hash) };
                if (!optValue.has_value()) {
                    runtimeError("Undefined variable '"+name->toStdString()+"'");
                    return errorReturn;
                }

                Value value = optValue.value();

                // If the value is a future that resolves to a signal, resolve it first
                if (isFuture(value)) {
                    auto s = tryAwaitFuture(value);
                    if (s == FutureStatus::Pending) goto postInstructionDispatch;
                    if (s == FutureStatus::Error) return errorReturn;
                    // Update the module variable with the resolved value
                    if (isSignal(value)) {
                        vars.store(name->hash, name->s, value);
                    }
                }

                if (isSignal(value)) {
                    push(value);
                    break;
                }

                Value signal = vars.ensureSignal(name->hash, name->s, name->toStdString());
                if (signal.isNil()) {
                    runtimeError("Cannot monitor variable '" + name->toStdString() + "'");
                    return errorReturn;
                }
                push(signal);
                break;
            }
            case OpCode::SetModuleVar: {
                ObjString* name = readString();
                if (onDataflowThread_) {
                    runtimeError("Cannot modify module variable '" + name->toStdString() + "' from dataflow function");
                    return errorReturn;
                }
                if (moduleType()->constVars.find(name->hash) != moduleType()->constVars.end()) {
                    runtimeError("Cannot assign to module constant '" + toUTF8StdString(name->s) + "'");
                    return errorReturn;
                }
                auto& vars { moduleVars() };
                // set new value, but leave it on stack (as assignment is an expression)
                // Clone vector/matrix/tensor for by-value semantics
                bool stored = vars.storeIfExists(name->hash, name->s, cloneIfValueSemantics(peek(0)));
                if (!stored) { // not stored, since not existing
                    runtimeError("Undefined variable '"+name->toStdString()+"'");
                    return errorReturn;
                }
                break;
            }
            case OpCode::SetNewModuleVar: {
                ObjString* name = readString();
                if (onDataflowThread_) {
                    runtimeError("Cannot modify module variable '" + name->toStdString() + "' from dataflow function");
                    return errorReturn;
                }
                auto& vars { moduleVars() };

                // only automatic declaration of globals on assignment when
                //   at module level scope
                bool moduleScope = true; // FIXME: set false if not in module scope

                bool exists = vars.exists(name->hash);
                if (!exists && !moduleScope) {
                    runtimeError("Undefined variable '"+name->toStdString()+"'");
                    return errorReturn;
                }

                if (moduleType()->constVars.find(name->hash) != moduleType()->constVars.end()) {
                    runtimeError("Cannot assign to module constant '" + toUTF8StdString(name->s) + "'");
                    return errorReturn;
                }

                // Clone vector/matrix/tensor for by-value semantics
                vars.store(name->hash, name->s, cloneIfValueSemantics(peek(0)), /*overwrite=*/true);

                break;
            }
            case OpCode::GetUpvalue: {
                uint16_t slot = singleByteArg ? readByte() : readShort();
                push(*asUpvalue(asClosure(frame->closure)->upvalues[slot])->location);
                break;
            }
            case OpCode::SetUpvalue: {
                uint16_t slot = singleByteArg ? readByte() : readShort();
                // Clone vector/matrix/tensor for by-value semantics
                *(asUpvalue(asClosure(frame->closure)->upvalues[slot])->location) = cloneIfValueSemantics(peek(0));
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
                        mat(r,c) = vec->vec()[c];
                }
                for(int i=0; i<rowCount; ++i) pop();
                push(Value::matrixVal(mat));
                break;
            }
            case OpCode::IfDictToKeys: {
                Value& maybeDict = peek(0);
                if (!isDict(maybeDict)) {
                    auto s = tryAwaitValue(maybeDict);
                    if (s == FutureStatus::Pending) goto postInstructionDispatch;
                    if (s == FutureStatus::Error) return errorReturn;
                }
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
                if (!isDict(maybeDict)) {
                    auto s = tryAwaitValue(maybeDict);
                    if (s == FutureStatus::Pending) goto postInstructionDispatch;
                    if (s == FutureStatus::Error) return errorReturn;
                }
                if (isDict(maybeDict)) {
                    Value d { maybeDict };
                    pop();
                    auto vecItemPairs { asDict(d)->items() };
                    Value listItems { Value::listVal() };
                    for(const auto& item : vecItemPairs) {
                        Value itemList { Value::listVal() };
                        asList(itemList)->append(item.first);
                        asList(itemList)->append(item.second);
                        asList(listItems)->append(itemList);
                    }
                    push(listItems);
                }
                break;
            }
            case OpCode::ToType:
            case OpCode::ToTypeStrict: {
                bool strict = (instruction == OpCode::ToTypeStrict);
                uint8_t typeByte = readByte();
                ValueType targetVT = ValueType(typeByte);
                Value val = pop(); // remove from stack; tryConvertValue manages stack for async

                Value typeSpec = Value::typeSpecVal(targetVT);
                auto outcome = tryConvertValue(val, typeSpec, strict, /*implicitCall=*/true,
                                               Thread::PendingConversion::Kind::TypeConversion);
                switch (outcome.result) {
                    case ConversionResult::AlreadyCorrectType:
                        push(val);
                        break;
                    case ConversionResult::ConvertedSync:
                        push(outcome.convertedValue);
                        break;
                    case ConversionResult::NeedsAsyncFrame:
                        frame = thread->frames.end() - 1;
                        break;
                    case ConversionResult::Failed:
                        runtimeError("unable to convert " + to_string(val.type())
                                     + " to " + to_string(targetVT));
                        return errorReturn;
                }
                break;
            }
            case OpCode::ToTypeSpec:
            case OpCode::ToTypeSpecStrict: {
                bool strict = (instruction == OpCode::ToTypeSpecStrict);
                Value typeSpec = pop();
                Value val = pop(); // remove from stack; tryConvertValue manages stack for async

                auto outcome = tryConvertValue(val, typeSpec, strict, /*implicitCall=*/true,
                                               Thread::PendingConversion::Kind::TypeConversion);
                switch (outcome.result) {
                    case ConversionResult::AlreadyCorrectType:
                        push(val);
                        break;
                    case ConversionResult::ConvertedSync:
                        push(outcome.convertedValue);
                        break;
                    case ConversionResult::NeedsAsyncFrame:
                        // tryConvertValue set up the call frame; result will land on stack
                        frame = thread->frames.end() - 1;
                        break;
                    case ConversionResult::Failed:
                        runtimeError("unable to convert " + to_string(val.type())
                                     + " to " + (isTypeSpec(typeSpec) ? to_string(asTypeSpec(typeSpec)->typeValue) : "type"));
                        return errorReturn;
                }
                break;
            }
            case OpCode::EventOn: {
                uint8_t modeByte = readByte();
                // mode encoding: bits 0-1 = base mode, bit 2 = target filter present
                uint8_t baseMode = modeByte & 0x03;
                bool hasTargetFilter = (modeByte & 0x04) != 0;
                bool requireChangesKeyword = (baseMode == 1) || (baseMode == 3);
                bool disallowSignalTargets = (baseMode == 2);
                bool matchOnBecomes = (baseMode == 3);

                Value closureVal = pop();
                Value targetFilterVal = hasTargetFilter ? pop() : Value::nilVal();
                Value matchVal = matchOnBecomes ? pop() : Value::nilVal();
                Value eventVal = pop();
                if (!isClosure(closureVal)) {
                    runtimeError("EVENT_ON expects event/signal and closure");
                    return errorReturn;
                }

                ObjEventType* ev = nullptr;
                if (isSignal(eventVal)) {
                    if (disallowSignalTargets) {
                        runtimeError("signal handlers must use 'changes'");
                        return errorReturn;
                    }
                    ObjSignal* sigObj = asSignal(eventVal);
                    ev = sigObj->ensureChangeEventType();
                    Value signalVal = eventVal; // save signal ref before overwriting
                    eventVal = sigObj->changeEventType;
                    thread->eventToSignal[eventVal.weakRef()] = signalVal.weakRef();
                } else if (isEventType(eventVal)) {
                    if (matchOnBecomes) {
                        runtimeError("'becomes' is only valid with signals");
                        return errorReturn;
                    }
                    if (requireChangesKeyword) {
                        runtimeError("'changes' is only valid with signals");
                        return errorReturn;
                    }
                    ev = asEventType(eventVal);
                } else {
                    runtimeError("EVENT_ON expects event/signal and closure");
                    return errorReturn;
                }

                // record this handler on the current thread
                Value key = eventVal.weakRef();
                thread->eventHandlers[key].push_back(Thread::HandlerRegistration{
                    closureVal,
                    matchOnBecomes ? std::make_optional(matchVal) : std::nullopt,
                    hasTargetFilter ? std::make_optional(targetFilterVal) : std::nullopt
                });

                // track the handler thread and subscribe the closure to the event
                auto* closure = asClosure(closureVal);
                closure->handlerThread = thread;
                ev->subscribers.push_back(closureVal.weakRef());
                break;
            }
            case OpCode::EventOff: {
                Value closureVal = pop();
                Value eventVal = pop();
                if (!isClosure(closureVal) || !(isEventType(eventVal) || isSignal(eventVal))) {
                    runtimeError("EVENT_OFF expects event/signal and closure");
                    return errorReturn;
                }

                ObjEventType* ev = nullptr;
                if (isEventType(eventVal)) {
                    ev = asEventType(eventVal);
                } else {
                    ObjSignal* sigObj = asSignal(eventVal);
                    ev = sigObj->ensureChangeEventType();
                    eventVal = sigObj->changeEventType;
                    thread->eventToSignal.erase(eventVal.weakRef());
                }

                Value key = eventVal.weakRef();
                auto it = thread->eventHandlers.find(key);
                if (it != thread->eventHandlers.end()) {
                    auto& handlers = it->second;
                    for(auto hit = handlers.begin(); hit != handlers.end(); ) {
                        if (hit->closure.isAlive() && asClosure(hit->closure) == asClosure(closureVal))
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
            case OpCode::EventType: {
                ObjString* name = readString();
                Value tv { Value::objVal(newEventTypeObj(name->s)) };
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
            case OpCode::EventPayload: {
                try {
                    defineEventPayload(readString());
                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return errorReturn;
                }
                break;
            }
            case OpCode::EventExtend: {
                try {
                    extendEventType();
                } catch (std::exception& e) {
                    runtimeError(e.what());
                    return errorReturn;
                }
                pop();
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
                    const auto& firstElement { symbolsListObj->getElement(0) };
                    if (isString(firstElement) && asStringObj(firstElement)->s == "*") {
                        fromModuleType->vars.forEach(
                            [&](const VariablesMap::NameValue& nameValue) {
                                //const icu::UnicodeString& name { nameValue.first };
                                toModuleType->vars.store(nameValue);
                                //std::cout << "declaring " << toUTF8StdString(nameValue.first) << " into " << toUTF8StdString(toModuleType->name) << std::endl;
                            });
                    }
                    else { // import the symbols explicitly listed
                        for(const auto& symbol : symbolsListObj->getElements()) {
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
                return std::make_pair(ExecutionStatus::RuntimeError,Value::nilVal());
                break;
        }

        // RT Callback check and deadline check - after every instruction
        // mayNeedInvocation() returns false immediately if not main thread or no callbacks
        if (hasDeadline) {
            auto now = TimePoint::currentTime();
            if (rtMgr.mayNeedInvocation()) {
                rtMgr.checkAndInvokeCallbacks(now);
            }
            if (now >= deadline) {
                if (thread->execute_depth > 0) thread->execute_depth--;
                return yieldReturn;
            }
        } else if (rtMgr.mayNeedInvocation()) {
            rtMgr.checkAndInvokeCallbacks(TimePoint::currentTime());
        }

        postInstructionDispatch:

        if (valueGC.isCollectionRequested()) {
            valueGC.safepoint(*thread);
        }

        // are we supposed to be sleeping?  If so, block until the sleep time is over
        //  or until we get a wakeup signal (for a possible event)
        // if we've slept for long enough, reset the flag and continue execution
        if (thread->threadSleep) {
            auto now = TimePoint::currentTime();
            if (now >= thread->threadSleepUntil) {
                thread->threadSleep = false;
            }
            else {
                // If deadline-limited, yield instead of blocking
                if (hasDeadline) {
                    if (thread->execute_depth > 0) thread->execute_depth--;
                    return yieldReturn;
                }

                auto sleepTarget = thread->threadSleepUntil.load();

                // If RT callbacks are registered, wake at their deadline instead
                // so we can service them promptly even while sleeping
                if (rtMgr.hasCallbacks()) {
                    auto rtDeadline = rtMgr.nextDeadline();
                    if (rtDeadline < sleepTarget) {
                        sleepTarget = rtDeadline;
                    }
                }

                auto waitTime = sleepTarget - now;
                if (waitTime.microSecs() > 0) {
                    std::unique_lock<std::mutex> lk(thread->sleepMutex);
                    thread->sleepCondVar.wait_for(lk,
                        std::chrono::microseconds(waitTime.microSecs()));
                }

                // Invoke any due RT callbacks after waking
                if (rtMgr.hasCallbacks()) {
                    rtMgr.checkAndInvokeCallbacks(TimePoint::currentTime());
                }
                // Note: threadSleep stays true if we haven't reached original sleep target
            }
        }

        // Sleep while awaiting a future (separate from threadSleep to avoid
        // interfering with wait() builtin semantics).  The 1ms is a polling
        // fallback; normally wakeWaiters() → Thread::wake() unblocks sooner.
        if (thread->awaitedFuture.isNonNil()) {
            ObjFuture* fut = asFuture(thread->awaitedFuture);
            if (fut->future.wait_for(std::chrono::microseconds(0)) == std::future_status::ready) {
                thread->awaitedFuture = Value::nilVal();
            } else {
                // If deadline-limited, yield instead of blocking
                if (hasDeadline) {
                    if (thread->execute_depth > 0) thread->execute_depth--;
                    return yieldReturn;
                }
                std::unique_lock<std::mutex> lk(thread->sleepMutex);
                thread->sleepCondVar.wait_for(lk, std::chrono::milliseconds(1));
            }
        }

        if (thread->pendingWaitFor.isNonNil()) {
            Value& waitTarget = thread->pendingWaitFor;
            if (isList(waitTarget)) {
                ObjList* list = asList(waitTarget);
                bool allResolved = true;
                for (auto& element : list->getElements()) {
                    auto s = tryResolveValue(element);
                    if (s == FutureStatus::Error)
                        return errorReturn;
                    if (s == FutureStatus::Pending) {
                        thread->awaitedFuture = element;
                        allResolved = false;
                        break;
                    }
                }
                if (allResolved)
                    thread->pendingWaitFor = Value::nilVal();
            } else if (isFuture(waitTarget)) {
                auto s = tryResolveValue(waitTarget);
                if (s == FutureStatus::Error)
                    return errorReturn;
                if (s == FutureStatus::Pending) {
                    thread->awaitedFuture = waitTarget;
                } else {
                    thread->pendingWaitFor = Value::nilVal();
                }
            } else {
                thread->pendingWaitFor = Value::nilVal();
            }
        }

        if (!processEventDispatch())
            return errorReturn;

        if (!processContinuationDispatch())
            return errorReturn;

        // Refresh frame pointer after processing events/continuations, as
        // frames may have been pushed onto or popped from the frame stack
        if (!thread->frames.empty())
            frame = thread->frames.end()-1;

        if (consumePendingObjectCleanup()) {
            freeObjects();
        }

    } // for

    if (thread->execute_depth > 0) thread->execute_depth--;
    return std::make_pair(ExecutionStatus::OK, Value::nilVal());

}


bool VM::processPendingEvents()
{

    if (exitRequested.load()) return false;

    if (thread->pendingEventCount.load(std::memory_order_acquire) == 0)
        return true;

    Thread::PendingEvent tev;

    // Drop events that are no longer alive or have no handlers
    while(thread->pendingEvents.pop_if([&](const Thread::PendingEvent& e){
                return !e.eventType.isAlive() ||
                        thread->eventHandlers.count(e.eventType) == 0;
            }, tev)) {
        size_t previous = thread->pendingEventCount.fetch_sub(1, std::memory_order_acq_rel);
        assert(previous > 0);
        thread->eventHandlers.erase(tev.eventType);
    }

    if (thread->pendingEventCount.load(std::memory_order_acquire) == 0)
        return true;

    auto now = TimePoint::currentTime();
    if (thread->pendingEvents.pop_if([&](const Thread::PendingEvent& e){
            return e.when <= now && e.eventType.isAlive() &&
                    thread->eventHandlers.count(e.eventType) > 0;
        }, tev)) {
        size_t previous = thread->pendingEventCount.fetch_sub(1, std::memory_order_acq_rel);
        assert(previous > 0);
        auto handlersIt = thread->eventHandlers.find(tev.eventType);
        if (handlersIt != thread->eventHandlers.end()) {
            for(const auto& handler : handlersIt->second) {
                // Check target filter before invoking handler
                if (handler.targetFilter.has_value() && isEventInstance(tev.instance)) {
                    auto* inst = asEventInstance(tev.instance);
                    static const int32_t targetHash = toUnicodeString("target").hashCode();
                    auto it = inst->payload.find(targetHash);
                    if (it == inst->payload.end()) {
                        continue;  // No target property, skip this handler
                    }
                    const Value& eventTarget = it->second;
                    if (!eventTarget.equals(handler.targetFilter.value(), /*strict=*/false)) {
                        continue;  // Target doesn't match filter, skip this handler
                    }
                }

                // Check matchValue filter for 'becomes' handlers
                if (handler.matchValue.has_value() && isEventInstance(tev.instance)) {
                    auto* inst = asEventInstance(tev.instance);
                    static const int32_t valueHash = toUnicodeString("value").hashCode();
                    auto it = inst->payload.find(valueHash);
                    if (it == inst->payload.end()) {
                        continue;
                    }
                    const Value& sample = it->second;
                    if (!sample.equals(handler.matchValue.value(), /*strict=*/false)) {
                        continue;
                    }
                }

                auto closureObj = asClosure(handler.closure);

                auto prevThreadSleep = thread->threadSleep.load();
                auto prevThreadSleepUntil = thread->threadSleepUntil.load();

                thread->threadSleep = false;

                if (handler.closure == conditionalInterruptClosure) {
                    bool raise = true;
                    auto sigIt = thread->eventToSignal.find(tev.eventType);
                    if (sigIt != thread->eventToSignal.end()) {
                        Value sigVal = sigIt->second;
                        if (!sigVal.isAlive()) {
                            thread->eventToSignal.erase(sigIt);
                            raise = false;
                        } else {
                            Value sigStrong = sigVal.strongRef();
                            if (!sigStrong.isNil() && isSignal(sigStrong)) {
                                ObjSignal* sigObj = asSignal(sigStrong);
                                Value cur = sigObj->signal->lastValue();
                                if (cur.isBool() && cur.asBool()) {
                                    raise = true;
                                } else {
                                    raise = false;
                                }
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
                    // Skip handler if closure has been cleaned up (function is nil)
                    if (closureObj->function.isNil()) {
                        continue;
                    }

                    std::vector<Value> handlerArgs;
                    int arity = asFunction(closureObj->function)->arity;
                    if (arity > 0) {
                        // Check if event instance is nil or no longer valid
                        if (tev.instance.isNil()) {
                            continue;
                        }
                        // Ensure we have a strong reference to the event instance
                        Value strongInstance = tev.instance.strongRef();
                        if (strongInstance.isNil() || !isEventInstance(strongInstance)) {
                            continue;
                        }
                        handlerArgs.push_back(strongInstance);
                    }
                    auto result = invokeClosure(closureObj, handlerArgs);
                    assert(!thread->threadSleep);

                    if (result.first != ExecutionStatus::OK)
                        return false;

                    thread->threadSleep = prevThreadSleep;
                    thread->threadSleepUntil = prevThreadSleepUntil;
                }
            }
        }
    }
    return true;
}


bool VM::invokeNextEventHandler()
{
    auto& dispatch = thread->eventDispatch;
    auto& tev = dispatch.currentEvent;

    while (dispatch.nextHandlerIndex < dispatch.handlerSnapshot.size()) {
        const auto& handler = dispatch.handlerSnapshot[dispatch.nextHandlerIndex];
        dispatch.nextHandlerIndex++;

        // Check target filter before invoking handler
        if (handler.targetFilter.has_value() && isEventInstance(tev.instance)) {
            auto* inst = asEventInstance(tev.instance);
            static const int32_t targetHash = toUnicodeString("target").hashCode();
            auto it = inst->payload.find(targetHash);
            if (it == inst->payload.end()) {
                continue;  // No target property, skip this handler
            }
            const Value& eventTarget = it->second;
            if (!eventTarget.equals(handler.targetFilter.value(), /*strict=*/false)) {
                continue;  // Target doesn't match filter, skip this handler
            }
        }

        // Check matchValue filter for 'becomes' handlers
        if (handler.matchValue.has_value() && isEventInstance(tev.instance)) {
            auto* inst = asEventInstance(tev.instance);
            static const int32_t valueHash = toUnicodeString("value").hashCode();
            auto it = inst->payload.find(valueHash);
            if (it == inst->payload.end()) {
                continue;
            }
            const Value& sample = it->second;
            if (!sample.equals(handler.matchValue.value(), /*strict=*/false)) {
                continue;
            }
        }

        // ConditionalInterrupt: handle inline (no frame push needed)
        if (handler.closure == conditionalInterruptClosure) {
            bool raise = true;
            auto sigIt = thread->eventToSignal.find(tev.eventType);
            if (sigIt != thread->eventToSignal.end()) {
                Value sigVal = sigIt->second;
                if (!sigVal.isAlive()) {
                    thread->eventToSignal.erase(sigIt);
                    raise = false;
                } else {
                    Value sigStrong = sigVal.strongRef();
                    if (!sigStrong.isNil() && isSignal(sigStrong)) {
                        ObjSignal* sigObj = asSignal(sigStrong);
                        Value cur = sigObj->signal->lastValue();
                        if (cur.isBool() && cur.asBool()) {
                            raise = true;
                        } else {
                            raise = false;
                        }
                    } else {
                        raise = false;
                    }
                }
            }
            if (raise) {
                // Finish the dispatch before raising the exception so that
                // unwindFrame does not need to clear it redundantly.
                dispatch.active = false;
                // Clear threadSleep so the exception handler runs immediately
                // (matches original processPendingEvents behaviour where
                // threadSleep was set to false before every handler).
                thread->threadSleep = false;
                Value excType = globals.load(toUnicodeString("ConditionalInterrupt")).value();
                Value exc = Value::exceptionVal(Value::nilVal(), excType);
                raiseException(exc);
                // raiseException modified the frame/IP state directly;
                // return true so execute() continues with the exception handler.
                return true;
            }
            continue;
        }

        auto closureObj = asClosure(handler.closure);

        // Skip handler if closure has been cleaned up (function is nil)
        if (closureObj->function.isNil()) {
            continue;
        }

        // Save sleep state before invoking handler
        dispatch.prevThreadSleep = thread->threadSleep.load();
        dispatch.prevThreadSleepUntil = thread->threadSleepUntil.load();
        thread->threadSleep = false;

        // Push closure + args (same stack layout as invokeClosure / OpCode::Call)
        int arity = asFunction(closureObj->function)->arity;
        push(Value::objRef(closureObj));
        if (arity > 0) {
            if (tev.instance.isNil()) {
                pop(); // pop closure
                continue;
            }
            Value strongInstance = tev.instance.strongRef();
            if (strongInstance.isNil() || !isEventInstance(strongInstance)) {
                pop(); // pop closure
                continue;
            }
            push(strongInstance);
        }

        CallSpec spec(arity > 0 ? 1 : 0);
        if (!call(closureObj, spec))
            return false;

        // Mark the new frame as an event handler so opReturn can flag it
        thread->frames.back().isEventHandler = true;
        return true;  // frame pushed; main loop will execute it
    }

    return false;  // no more applicable handlers
}


bool VM::processEventDispatch()
{
    if (exitRequested.load()) return false;

    auto& dispatch = thread->eventDispatch;

    // Phase 1: If an event handler just returned, clean up and try the next handler
    if (thread->eventHandlerJustReturned) {
        thread->eventHandlerJustReturned = false;

        // Discard the handler's return value (event handlers are procs → nil)
        pop();

        assert(!thread->threadSleep);

        // Restore sleep state that was saved before the handler was invoked
        thread->threadSleep = dispatch.prevThreadSleep;
        thread->threadSleepUntil = dispatch.prevThreadSleepUntil;

        // Try to invoke the next handler for the current event
        if (invokeNextEventHandler())
            return true;  // next handler frame pushed

        // All handlers for this event have been processed
        dispatch.active = false;
    }

    // Phase 2: Check for new pending events
    if (thread->pendingEventCount.load(std::memory_order_acquire) == 0)
        return true;

    Thread::PendingEvent tev;

    // Drop events that are no longer alive or have no handlers
    while(thread->pendingEvents.pop_if([&](const Thread::PendingEvent& e){
                return !e.eventType.isAlive() ||
                        thread->eventHandlers.count(e.eventType) == 0;
            }, tev)) {
        size_t previous = thread->pendingEventCount.fetch_sub(1, std::memory_order_acq_rel);
        assert(previous > 0);
        thread->eventHandlers.erase(tev.eventType);
    }

    if (thread->pendingEventCount.load(std::memory_order_acquire) == 0)
        return true;

    auto now = TimePoint::currentTime();
    if (thread->pendingEvents.pop_if([&](const Thread::PendingEvent& e){
            return e.when <= now && e.eventType.isAlive() &&
                    thread->eventHandlers.count(e.eventType) > 0;
        }, tev)) {
        size_t previous = thread->pendingEventCount.fetch_sub(1, std::memory_order_acq_rel);
        assert(previous > 0);
        auto handlersIt = thread->eventHandlers.find(tev.eventType);
        if (handlersIt != thread->eventHandlers.end()) {
            // Start a new event dispatch: snapshot the handler list and invoke
            // the first applicable handler.
            dispatch.active = true;
            dispatch.currentEvent = tev;
            dispatch.handlerSnapshot = handlersIt->second;
            dispatch.nextHandlerIndex = 0;

            if (invokeNextEventHandler())
                return true;  // first handler frame pushed

            // No applicable handlers after filtering
            dispatch.active = false;
        }
    }

    return true;
}


bool VM::pushContinuationCall(ObjClosure* closure, const std::vector<Value>& args)
{
    push(Value::objRef(closure));
    for (const auto& arg : args)
        push(arg);

    CallSpec spec(args.size());
    if (!call(closure, spec))
        return false;

    thread->frames.back().isContinuationCallback = true;
    return true;
}

void VM::clearContinuation()
{
    thread->nativeContinuation.clear();
    thread->continuationCallbackReturned = false;
}

bool VM::processContinuationDispatch()
{
    if (!thread->continuationCallbackReturned)
        return true;

    thread->continuationCallbackReturned = false;
    auto& cont = thread->nativeContinuation;

    if (!cont.active || !cont.onComplete)
        return true;  // No active continuation

    // Get closure return value from stack
    Value result = pop();

    // Invoke handler - it may push another frame or finalize
    bool ok = cont.onComplete(*this, result);

    // If handler didn't push another continuation frame, we're done
    if (thread->frames.empty() ||
        !thread->frames.back().isContinuationCallback) {
        // Write final result to the original call's result slot and restore stack
        if (cont.resultSlot) {
            // The handler pushed the final result; pop it, then pop the original
            // method call's args and receiver (to properly clean up Values),
            // then push the final result back
            Value finalResult = pop();
            // Pop items from current position down to (and including) the result slot
            // stackBase is one past resultSlot, so we pop (stackTop - stackBase + 1) items
            size_t itemsToPop = static_cast<size_t>(thread->stackTop - cont.stackBase) + 1;
            popN(itemsToPop);
            push(finalResult);
        }
        clearContinuation();
    }

    return ok;
}

bool VM::processNativeDefaultParamDispatch(Value defaultValue)
{

    auto& state = thread->nativeDefaultParamState;
    if (!state.active)
        return true;

    // Store the result in the args buffer at the correct position
    size_t paramIdx = state.closureParamIndices[state.nextClosureIndex];
    size_t bufferIdx = paramIdx + (state.includeReceiver ? 1 : 0);
    state.argsBuffer[bufferIdx] = defaultValue;

    // Apply type conversion if needed
    const auto& params = state.funcType->func.value().params;
    if (params[paramIdx].has_value() && params[paramIdx]->type.has_value()) {
        auto vt = builtinToValueType(params[paramIdx]->type.value()->builtin);
        if (vt.has_value()) {
            bool strictConv = false;
            if (thread->frames.size() >= 1)
                strictConv = (thread->frames.end()-1)->strict;
            state.argsBuffer[bufferIdx] = toType(vt.value(), state.argsBuffer[bufferIdx], strictConv);
        }
    }

    // Move to next closure default
    state.nextClosureIndex++;

    // More closure defaults to evaluate?
    if (state.nextClosureIndex < state.closureParamIndices.size()) {
        size_t nextParamIdx = state.closureParamIndices[state.nextClosureIndex];
        auto it = state.paramDefaultFuncs.find(params[nextParamIdx]->nameHashCode);
        Value defFunc = it->second;
        Value defClosure = Value::closureVal(defFunc);

        // Check for captured variables (not allowed in default params)
        if (asClosure(defClosure)->upvalues.size() > 0) {
            auto paramName = params[nextParamIdx]->name;
            state.clear();
            runtimeError("Captured variables in default parameter '" + toUTF8StdString(paramName) +
                        "' value expressions are not allowed.");
            return false;
        }

        // Push closure and call it using continuation mechanism
        push(defClosure);
        if (!call(asClosure(defClosure), CallSpec(0))) {
            state.clear();
            clearContinuation();
            return false;
        }
        thread->frames.back().isContinuationCallback = true;
        return true;  // Continue with next closure frame
    }

    // All defaults evaluated - clear continuation and call the native function
    clearContinuation();
    NativeFn fn = state.nativeFunc;
    size_t actual = state.argsBuffer.size();
    Value* buf = state.argsBuffer.data();

    // Non-blocking resolution of future args indicated by mask
    if (state.resolveArgMask) {
        for (size_t i = 0; i < actual && state.resolveArgMask >> i; ++i) {
            if ((state.resolveArgMask & (1u << i)) && isFuture(buf[i])) {
                auto s = buf[i].tryResolveFuture();
                if (s == FutureStatus::Pending) {
                    thread->awaitedFuture = buf[i];
                    // Can't defer further - state is consumed. This is an edge case.
                    // For now, clear and error.
                    state.clear();
                    runtimeError("Cannot await future in native function with deferred default params");
                    return false;
                }
                if (s == FutureStatus::Error) {
                    state.clear();
                    return false;
                }
            }
        }
    }

    ArgsView view{buf, actual};
    Value result { fn(*this, view) };

    // For init methods (proc that returns void on ObjectInstance), the result should be the instance
    // Native init returns nil, but we want to leave the instance on the stack
    // Check if this is a proc (not func) - init is always a proc
    bool isInitMethod = state.includeReceiver &&
                        isObjectInstance(state.receiver) &&
                        state.funcType &&
                        state.funcType->func.has_value() &&
                        state.funcType->func.value().isProc;
    Value finalResult = result;
    if (isInitMethod) {
        finalResult = state.receiver;
    } else {
    }

    // Clean up original call args from stack and store result.
    // After processContinuationDispatch pops the closure result, the stack has the
    // receiver and original args. We need to replace them with the final result.
    size_t argCount = state.originalArgCount;
    // Stack: [receiver, <args>...] - write result to receiver slot, pop args
    *(thread->stackTop - argCount - 1) = finalResult;
    popN(argCount);

    state.clear();
    return true;
}


bool VM::needsAsyncConversion(const Value& val, ptr<type::Type> paramType)
{
    if (!paramType)
        return false;

    auto vt = builtinToValueType(paramType->builtin);

    // Object/Actor target type: check for constructor auto-conversion
    if (paramType->builtin == type::BuiltinType::Object
        || paramType->builtin == type::BuiltinType::Actor) {
        if (!paramType->obj.has_value())
            return false;
        // Look up the target type in module variables
        auto& typeName = paramType->obj.value().name;
        Value typeVal = Value::nilVal();
        if (!thread->frames.empty()) {
            auto moduleType = asFunction(asClosure(thread->frames.back().closure)->function)->moduleType;
            if (!moduleType.isNil()) {
                auto found = asModuleType(moduleType)->vars.load(typeName);
                if (found.has_value())
                    typeVal = found.value();
            }
        }
        if (typeVal.isNil() || !isTypeSpec(typeVal))
            return false;
        // If value already matches, no conversion needed
        if (val.is(typeVal))
            return false;
        // Check if constructor auto-conversion is possible
        return canConvertToType(val, typeVal, true);
    }

    // Builtin target type: check if source is object/actor with conversion operator
    if (vt.has_value() && (isObjectInstance(val) || isActorInstance(val))) {
        Value instType = isObjectInstance(val)
            ? asObjectInstance(val)->instanceType
            : asActorInstance(val)->instanceType;
        UnicodeString convName = UnicodeString("operator->") + toUnicodeString(to_string(vt.value()));
        int32_t convHash = convName.hashCode();
        Value closure = findConversionMethod(instType, convHash, /*implicitCall=*/true);
        return !closure.isNil();
    }

    return false;
}


bool VM::pushParamConversionFrame(const Value& val, ptr<type::Type> paramType)
{
    auto vt = builtinToValueType(paramType->builtin);

    // User-defined conversion operator (object/actor → builtin)
    if (vt.has_value() && (isObjectInstance(val) || isActorInstance(val))) {
        Value instType = isObjectInstance(val)
            ? asObjectInstance(val)->instanceType
            : asActorInstance(val)->instanceType;
        UnicodeString convName = UnicodeString("operator->") + toUnicodeString(to_string(vt.value()));
        int32_t convHash = convName.hashCode();
        Value closure = findConversionMethod(instType, convHash, /*implicitCall=*/true);
        if (!closure.isNil()) {
            thread->conversionInProgress.push_back({val, thread->frames.size()});
            push(val); // push as receiver for method call
            if (!call(asClosure(closure), CallSpec(0)))
                return false;
            thread->frames.back().isContinuationCallback = true;
            return true;
        }
    }

    // Constructor auto-conversion (for object/actor target types)
    if (paramType->builtin == type::BuiltinType::Object
        || paramType->builtin == type::BuiltinType::Actor) {
        if (paramType->obj.has_value()) {
            auto& typeName = paramType->obj.value().name;
            Value typeVal = Value::nilVal();
            if (!thread->frames.empty()) {
                auto moduleType = asFunction(asClosure(thread->frames.back().closure)->function)->moduleType;
                if (!moduleType.isNil()) {
                    auto found = asModuleType(moduleType)->vars.load(typeName);
                    if (found.has_value())
                        typeVal = found.value();
                }
            }
            if (!typeVal.isNil() && isTypeSpec(typeVal)) {
                push(typeVal);  // callee (type constructor)
                push(val);     // argument
                if (!callValue(typeVal, CallSpec(1)))
                    return false;
                thread->frames.back().isContinuationCallback = true;
                return true;
            }
        }
    }

    return false;
}


bool VM::processNativeParamConversion(Value convertedValue)
{
    auto& state = thread->nativeParamConversionState;
    if (!state.active)
        return true;

    // Store the converted value in the args buffer
    size_t paramIdx = state.conversionParamIndices[state.nextConversionIndex];
    size_t bufferIdx = paramIdx + (state.includeReceiver ? 1 : 0);
    state.argsBuffer[bufferIdx] = convertedValue;

    // Clean up conversion recursion guard
    auto& guards = thread->conversionInProgress;
    if (!guards.empty()) {
        guards.erase(
            std::remove_if(guards.begin(), guards.end(),
                [&](const Thread::ConversionGuard& g) {
                    return thread->frames.size() <= g.frameDepth;
                }),
            guards.end());
    }

    // Move to next conversion
    state.nextConversionIndex++;

    // More conversions to do?
    if (state.nextConversionIndex < state.conversionParamIndices.size()) {
        size_t nextParamIdx = state.conversionParamIndices[state.nextConversionIndex];
        size_t nextBufIdx = nextParamIdx + (state.includeReceiver ? 1 : 0);
        const auto& params = state.funcType->func.value().params;
        Value val = state.argsBuffer[nextBufIdx];
        if (!pushParamConversionFrame(val, params[nextParamIdx]->type.value())) {
            state.clear();
            clearContinuation();
            runtimeError("Failed to convert parameter for native function call");
            return false;
        }
        return true;  // Continue with next conversion frame
    }

    // All conversions done — call the native function
    clearContinuation();
    NativeFn fn = state.nativeFunc;
    size_t actual = state.argsBuffer.size();
    Value* buf = state.argsBuffer.data();

    // Non-blocking resolution of future args
    if (state.resolveArgMask) {
        for (size_t i = 0; i < actual && state.resolveArgMask >> i; ++i) {
            if ((state.resolveArgMask & (1u << i)) && isFuture(buf[i])) {
                auto s = buf[i].tryResolveFuture();
                if (s == FutureStatus::Pending) {
                    thread->awaitedFuture = buf[i];
                    state.clear();
                    runtimeError("Cannot await future in native function with deferred param conversion");
                    return false;
                }
                if (s == FutureStatus::Error) {
                    state.clear();
                    return false;
                }
            }
        }
    }

    ArgsView view{buf, actual};
    Value result { fn(*this, view) };

    // Check if this is an init method (proc returning instance)
    bool isInitMethod = state.includeReceiver &&
                        isObjectInstance(state.receiver) &&
                        state.funcType &&
                        state.funcType->func.has_value() &&
                        state.funcType->func.value().isProc;
    Value finalResult = result;
    if (isInitMethod)
        finalResult = state.receiver;

    // Clean up original call args from stack
    size_t argCount = state.originalArgCount;
    *(thread->stackTop - argCount - 1) = finalResult;
    popN(argCount);

    state.clear();
    return true;
}


void VM::unwindFrame()
{
    auto f = thread->frames.back();
    // If an event handler frame is being unwound (e.g. by raiseException),
    // clear the dispatch state so the event dispatch machinery does not
    // attempt to invoke the next handler for the same event.
    if (f.isEventHandler && thread->eventDispatch.active) {
        thread->eventDispatch.active = false;
        thread->eventHandlerJustReturned = false;
    }
    // If a continuation callback frame is being unwound, clear the continuation state
    // and clean up the original method call's stack area (receiver + args)
    if (f.isContinuationCallback && thread->nativeContinuation.active) {
        auto& cont = thread->nativeContinuation;
        if (cont.resultSlot) {
            // The original method call's args are below this frame's slots.
            // We need to mark them for cleanup. We can't pop them now (frame's stack
            // area hasn't been popped yet), so we adjust the frame's slots pointer
            // to include the original args. This way, the normal unwinding will
            // pop everything including the original args.
            // Calculate: slots should be at resultSlot (to include receiver)
            f.slots = cont.resultSlot;
        }
        clearContinuation();
    }
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

    if (thread && thread->nativeCallDepth > 0)
        thread->exceptionJumpPending.store(true, std::memory_order_relaxed);

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
    thread->stack.resize(stackLimit);
    thread->stackTop = thread->stack.begin();

    thread->frames.clear();
    thread->frames.reserve(callFrameLimit);
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


void VM::reportStackOverflow()
{
    size_t frameCount = thread ? thread->frames.size() : 0;
    size_t stackDepth = 0;
    if (thread) {
        stackDepth = static_cast<size_t>(thread->stackTop - thread->stack.begin());
    }

    std::string message {
        "Stack overflow (call frames: " + std::to_string(frameCount) + "/" +
        std::to_string(callFrameLimit) + ", stack depth: " +
        std::to_string(stackDepth) + "/" + std::to_string(stackLimit) + ")."
    };

    runtimeError(message);
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
    // Modules may pre-register builtin methods (e.g., sys.Time helpers) before
    // the VM installs its core methods. Guard against skipping this setup only
    // when the canonical "list.append" hook is already present.
    const auto appendHash = toUnicodeString("append").hashCode();
    auto listIt = builtinMethods.find(ValueType::List);
    if (listIt != builtinMethods.end() && listIt->second.find(appendHash) != listIt->second.end()) {
        return;
    }

    // noMutateSelf / noMutateArgs flags:
    //   noMutateSelf=true  → method reads but doesn't mutate receiver
    //   noMutateArgs bits  → bit N set means arg N is read-only (not mutated)
    // These flags enable the VM to skip snapshot isolation for const dispatch.

    // Vector methods — all read-only on self
    defineBuiltinMethod(ValueType::Vector, "norm", std::mem_fn(&VM::vector_norm_builtin),
                        false, nullptr, {}, Value::nilVal(), /*noMutateSelf=*/true);
    defineBuiltinMethod(ValueType::Vector, "sum", std::mem_fn(&VM::vector_sum_builtin),
                        false, nullptr, {}, Value::nilVal(), /*noMutateSelf=*/true);
    defineBuiltinMethod(ValueType::Vector, "min", std::mem_fn(&VM::vector_min_builtin),
                        false, nullptr, {}, Value::nilVal(), /*noMutateSelf=*/true);
    defineBuiltinMethod(ValueType::Vector, "max", std::mem_fn(&VM::vector_max_builtin),
                        false, nullptr, {}, Value::nilVal(), /*noMutateSelf=*/true);
    defineBuiltinMethod(ValueType::Vector, "normalized", std::mem_fn(&VM::vector_normalized_builtin),
                        false, nullptr, {}, Value::nilVal(), /*noMutateSelf=*/true);
    defineBuiltinMethod(ValueType::Vector, "dot", std::mem_fn(&VM::vector_dot_builtin),
                        false, nullptr, {}, Value::nilVal(), /*noMutateSelf=*/true, /*noMutateArgs=*/0x1);

    // Matrix methods — all read-only on self
    defineBuiltinMethod(ValueType::Matrix, "rows", std::mem_fn(&VM::matrix_rows_builtin),
                        false, nullptr, {}, Value::nilVal(), /*noMutateSelf=*/true);
    defineBuiltinMethod(ValueType::Matrix, "cols", std::mem_fn(&VM::matrix_cols_builtin),
                        false, nullptr, {}, Value::nilVal(), /*noMutateSelf=*/true);
    defineBuiltinMethod(ValueType::Matrix, "transpose", std::mem_fn(&VM::matrix_transpose_builtin),
                        false, nullptr, {}, Value::nilVal(), /*noMutateSelf=*/true);
    defineBuiltinMethod(ValueType::Matrix, "determinant", std::mem_fn(&VM::matrix_determinant_builtin),
                        false, nullptr, {}, Value::nilVal(), /*noMutateSelf=*/true);
    defineBuiltinMethod(ValueType::Matrix, "inverse", std::mem_fn(&VM::matrix_inverse_builtin),
                        false, nullptr, {}, Value::nilVal(), /*noMutateSelf=*/true);
    defineBuiltinMethod(ValueType::Matrix, "trace", std::mem_fn(&VM::matrix_trace_builtin),
                        false, nullptr, {}, Value::nilVal(), /*noMutateSelf=*/true);
    defineBuiltinMethod(ValueType::Matrix, "norm", std::mem_fn(&VM::matrix_norm_builtin),
                        false, nullptr, {}, Value::nilVal(), /*noMutateSelf=*/true);
    defineBuiltinMethod(ValueType::Matrix, "sum", std::mem_fn(&VM::matrix_sum_builtin),
                        false, nullptr, {}, Value::nilVal(), /*noMutateSelf=*/true);
    defineBuiltinMethod(ValueType::Matrix, "min", std::mem_fn(&VM::matrix_min_builtin),
                        false, nullptr, {}, Value::nilVal(), /*noMutateSelf=*/true);
    defineBuiltinMethod(ValueType::Matrix, "max", std::mem_fn(&VM::matrix_max_builtin),
                        false, nullptr, {}, Value::nilVal(), /*noMutateSelf=*/true);

    // Tensor methods — all read-only on self
    defineBuiltinMethod(ValueType::Tensor, "min", std::mem_fn(&VM::tensor_min_builtin),
                        false, nullptr, {}, Value::nilVal(), /*noMutateSelf=*/true);
    defineBuiltinMethod(ValueType::Tensor, "max", std::mem_fn(&VM::tensor_max_builtin),
                        false, nullptr, {}, Value::nilVal(), /*noMutateSelf=*/true);
    defineBuiltinMethod(ValueType::Tensor, "sum", std::mem_fn(&VM::tensor_sum_builtin),
                        false, nullptr, {}, Value::nilVal(), /*noMutateSelf=*/true);

    // List methods — append mutates self; filter/map/reduce read-only on self
    defineBuiltinMethod(ValueType::List, "append", std::mem_fn(&VM::list_append_builtin),
                        false, nullptr, {}, Value::nilVal(), /*noMutateSelf=*/false, /*noMutateArgs=*/0x1);
    defineBuiltinMethod(ValueType::List, "filter", std::mem_fn(&VM::list_filter_builtin),
                        false, nullptr, {}, Value::nilVal(), /*noMutateSelf=*/true, /*noMutateArgs=*/0x1);
    defineBuiltinMethod(ValueType::List, "map", std::mem_fn(&VM::list_map_builtin),
                        false, nullptr, {}, Value::nilVal(), /*noMutateSelf=*/true, /*noMutateArgs=*/0x1);
    defineBuiltinMethod(ValueType::List, "reduce", std::mem_fn(&VM::list_reduce_builtin),
                        false, nullptr, {}, Value::nilVal(), /*noMutateSelf=*/true, /*noMutateArgs=*/0x3);

#ifdef ROXAL_ENABLE_REGEX
    // String methods — all read-only on self and args
    defineBuiltinMethod(ValueType::String, "match", std::mem_fn(&VM::string_match_builtin),
                        false, nullptr, {}, Value::nilVal(), /*noMutateSelf=*/true, /*noMutateArgs=*/0x1);
    defineBuiltinMethod(ValueType::String, "search", std::mem_fn(&VM::string_search_builtin),
                        false, nullptr, {}, Value::nilVal(), /*noMutateSelf=*/true, /*noMutateArgs=*/0x1);
    defineBuiltinMethod(ValueType::String, "replace", std::mem_fn(&VM::string_replace_builtin),
                        false, nullptr, {}, Value::nilVal(), /*noMutateSelf=*/true, /*noMutateArgs=*/0x3);
    defineBuiltinMethod(ValueType::String, "split", std::mem_fn(&VM::string_split_builtin),
                        false, nullptr, {}, Value::nilVal(), /*noMutateSelf=*/true, /*noMutateArgs=*/0x1);
#endif

    // Signal methods — run/stop/tick/set mutate self; freq is read-only
    defineBuiltinMethod(ValueType::Signal, "run", std::mem_fn(&VM::signal_run_builtin));
    defineBuiltinMethod(ValueType::Signal, "stop", std::mem_fn(&VM::signal_stop_builtin));
    defineBuiltinMethod(ValueType::Signal, "tick", std::mem_fn(&VM::signal_tick_builtin));
    defineBuiltinMethod(ValueType::Signal, "freq", std::mem_fn(&VM::signal_freq_builtin),
                        false, nullptr, {}, Value::nilVal(), /*noMutateSelf=*/true);
    defineBuiltinMethod(ValueType::Signal, "set", std::mem_fn(&VM::signal_set_builtin),
                        false, nullptr, {}, Value::nilVal(), /*noMutateSelf=*/false, /*noMutateArgs=*/0x1);
    defineBuiltinMethod(ValueType::Signal, "on_changed", std::mem_fn(&VM::signal_on_changed_builtin), true);

    // Event methods — all mutate self (register/remove handlers)
    defineBuiltinMethod(ValueType::Event, "emit", std::mem_fn(&VM::event_emit_builtin), true,
                        nullptr, {}, Value::nilVal(), /*noMutateSelf=*/false, /*noMutateArgs=*/0x1);
    defineBuiltinMethod(ValueType::Object, "emit", std::mem_fn(&VM::event_emit_builtin), true,
                        nullptr, {}, Value::nilVal(), /*noMutateSelf=*/false, /*noMutateArgs=*/0x1);
    defineBuiltinMethod(ValueType::Event, "when", std::mem_fn(&VM::event_when_builtin), true);
    defineBuiltinMethod(ValueType::Event, "remove", std::mem_fn(&VM::event_remove_builtin), true);

    // Actor dataflow methods — all mutate state (procs)
    defineBuiltinMethod(ValueType::Actor, "tick", std::mem_fn(&VM::dataflow_tick_native), true);  // proc
    defineBuiltinMethod(ValueType::Actor, "run", std::mem_fn(&VM::dataflow_run_native), true);   // proc
    defineBuiltinMethod(ValueType::Actor, "runFor", std::mem_fn(&VM::dataflow_run_for_native), true,
                        nullptr, {}, Value::nilVal(), /*noMutateSelf=*/false, /*noMutateArgs=*/0x1);
}

void VM::defineBuiltinMethod(ValueType type, const std::string& name, NativeFn fn,
                             bool isProc,
                             ptr<type::Type> funcType,
                             std::vector<Value> defaults,
                             Value declFunction,
                             bool noMutateSelf,
                             uint32_t noMutateArgs)
{
    auto us = toUnicodeString(name);
    builtinMethods[type][us.hashCode()] = BuiltinMethodInfo(fn, isProc, funcType,
                                                            std::move(defaults), declFunction,
                                                            0, noMutateSelf, noMutateArgs);
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
    defineBuiltinProperty(ValueType::Object, "detail", &VM::exception_detail_getter);
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

Value VM::exception_detail_getter(Value& receiver)
{
#ifdef DEBUG_BUILD
    if (!isException(receiver))
        throw std::invalid_argument("exception.detail property on non-exception");
#endif
    if (!isException(receiver)) {
        runtimeError("Undefined property 'detail'");
        return Value::nilVal();
    }
    ObjException* ex = asException(receiver);
    return ex->detail;
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

bool VM::resolveValue(Value& value)
{
    value.resolve();
    return !runtimeErrorFlag.load();
}

FutureStatus VM::tryResolveValue(Value& value)
{
    auto status = value.tryResolveFuture();
    if (status == FutureStatus::Resolved)
        value.resolveSignal();
    return status;
}

FutureStatus VM::tryAwaitFuture(Value& v)
{
    auto s = v.tryResolveFuture();
    if (s == FutureStatus::Pending) {
        thread->awaitedFuture = v;
        (thread->frames.end() - 1)->ip = thread->instructionStart;
    }
    return s;
}

FutureStatus VM::tryAwaitFutures(Value& a, Value& b)
{
    auto s = a.tryResolveFuture();
    if (s == FutureStatus::Resolved)
        s = b.tryResolveFuture();
    if (s == FutureStatus::Pending) {
        thread->awaitedFuture = isFuture(a) ? a : b;
        (thread->frames.end() - 1)->ip = thread->instructionStart;
    }
    return s;
}

FutureStatus VM::tryAwaitValue(Value& v)
{
    auto s = tryResolveValue(v);
    if (s == FutureStatus::Pending) {
        thread->awaitedFuture = v;
        (thread->frames.end() - 1)->ip = thread->instructionStart;
    }
    return s;
}

FutureStatus VM::tryAwaitValues(Value& a, Value& b)
{
    auto s = tryResolveValue(a);
    if (s == FutureStatus::Resolved)
        s = tryResolveValue(b);
    if (s == FutureStatus::Pending) {
        thread->awaitedFuture = isFuture(a) ? a : b;
        (thread->frames.end() - 1)->ip = thread->instructionStart;
    }
    return s;
}

Value VM::event_emit_builtin(ArgsView args)
{
    if (args.empty())
        throw std::invalid_argument("event.emit expects an event receiver");

    const Value& receiver = args[0];

    auto parseTime = [](const Value& candidate) {
        if (!candidate.isNumber())
            throw std::invalid_argument("event.emit time argument must be numeric microseconds");
        return TimePoint::microSecs(candidate.asInt());
    };

    TimePoint when = TimePoint::currentTime();
    Value eventType = Value::nilVal();
    Value instance = Value::nilVal();
    ObjEventType* ev = nullptr;

    if (isEventType(receiver)) {
        throw std::invalid_argument("event.emit expects an event instance; call the event type to create one");
    } else if (isEventInstance(receiver)) {
        if (args.size() > 2)
            throw std::invalid_argument("event.emit expects optional time argument in microseconds");

        ObjEventInstance* inst = asEventInstance(receiver);
        if (!inst->typeHandle.isAlive() || !isEventType(inst->typeHandle))
            return Value::nilVal();

        eventType = inst->typeHandle;
        ev = asEventType(eventType);

        if (args.size() == 2)
            when = parseTime(args[1]);

        if (ev->subscribers.empty())
            return Value::nilVal();

        instance = receiver;
    } else {
        throw std::invalid_argument("event.emit expects an event instance receiver");
    }

    if (!ev)
        ev = asEventType(eventType);

    // Event instances are implicitly const once emitted (spec: Event Implicit Const).
    // Freeze before dispatch so all handlers receive a const snapshot.
    instance = createFrozenSnapshot(instance);

    Value eventWeak = eventType.weakRef();
    scheduleEventHandlers(eventWeak, ev, instance, when);

    return Value::nilVal();
}

Value VM::event_when_builtin(ArgsView args)
{
    if (args.size() != 2 || !isEventType(args[0]) || !isClosure(args[1]))
        throw std::invalid_argument("event.when expects event and closure argument");

    Value eventVal = args[0];
    Value closureVal = args[1];

    Value key = eventVal.weakRef();
    thread->eventHandlers[key].push_back(Thread::HandlerRegistration{closureVal, std::nullopt});

    ObjEventType* ev = asEventType(eventVal);
    ObjClosure* closure = asClosure(closureVal);
    if (closure->function.isNonNil()) {
        ObjFunction* fn = asFunction(closure->function);
        if (fn->arity > 1)
            throw std::invalid_argument("event handler must accept at most one argument");
    }
    closure->handlerThread = thread;
    ev->subscribers.push_back(closureVal.weakRef());

    return Value::nilVal();
}

Value VM::event_remove_builtin(ArgsView args)
{
    if (args.size() != 2 || !(isEventType(args[0]) || isSignal(args[0])) || !isClosure(args[1]))
        throw std::invalid_argument("event.remove expects event/signal and closure argument");

    Value eventVal = args[0];
    Value closureVal = args[1];

    ObjEventType* ev = nullptr;
    if (isEventType(eventVal)) {
        ev = asEventType(eventVal);
    } else {
        ObjSignal* sigObj = asSignal(eventVal);
        ev = sigObj->ensureChangeEventType();
        eventVal = sigObj->changeEventType;
        thread->eventToSignal.erase(eventVal.weakRef());
    }

    Value key = eventVal.weakRef();
    auto it = thread->eventHandlers.find(key);
    if (it != thread->eventHandlers.end()) {
        auto& handlers = it->second;
        for(auto hit = handlers.begin(); hit != handlers.end(); ) {
            if (hit->closure.isAlive() && asClosure(hit->closure) == asClosure(closureVal))
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
    double n = vec->vec().norm();
    return Value::realVal(n);
}

Value VM::vector_sum_builtin(ArgsView args)
{
    if (args.size() != 1 || !isVector(args[0]))
        throw std::invalid_argument("vector.sum expects no arguments");

    ObjVector* vec = asVector(args[0]);
    double s = vec->vec().sum();
    return Value::realVal(s);
}

Value VM::vector_normalized_builtin(ArgsView args)
{
    if (args.size() != 1 || !isVector(args[0]))
        throw std::invalid_argument("vector.normalized expects no arguments");

    ObjVector* vec = asVector(args[0]);
    Eigen::VectorXd nvec = vec->vec().normalized();
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

    double d = v1->vec().dot(v2->vec());
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
    Eigen::MatrixXd tr = mat->mat().transpose();
    return Value::matrixVal(tr);
}

Value VM::matrix_determinant_builtin(ArgsView args)
{
    if (args.size() != 1 || !isMatrix(args[0]))
        throw std::invalid_argument("matrix.determinant expects no arguments");

    ObjMatrix* mat = asMatrix(args[0]);
    if (mat->rows() != mat->cols())
        throw std::invalid_argument("matrix.determinant requires a square matrix");

    double det = mat->mat().determinant();
    return Value::realVal(det);
}

Value VM::matrix_inverse_builtin(ArgsView args)
{
    if (args.size() != 1 || !isMatrix(args[0]))
        throw std::invalid_argument("matrix.inverse expects no arguments");

    ObjMatrix* mat = asMatrix(args[0]);
    if (mat->rows() != mat->cols())
        throw std::invalid_argument("matrix.inverse requires a square matrix");

    Eigen::MatrixXd inv = mat->mat().inverse();
    return Value::matrixVal(inv);
}

Value VM::matrix_trace_builtin(ArgsView args)
{
    if (args.size() != 1 || !isMatrix(args[0]))
        throw std::invalid_argument("matrix.trace expects no arguments");

    ObjMatrix* mat = asMatrix(args[0]);
    double tr = mat->mat().trace();
    return Value::realVal(tr);
}

Value VM::matrix_norm_builtin(ArgsView args)
{
    if (args.size() != 1 || !isMatrix(args[0]))
        throw std::invalid_argument("matrix.norm expects no arguments");

    ObjMatrix* mat = asMatrix(args[0]);
    double n = mat->mat().norm();
    return Value::realVal(n);
}

Value VM::matrix_sum_builtin(ArgsView args)
{
    if (args.size() != 1 || !isMatrix(args[0]))
        throw std::invalid_argument("matrix.sum expects no arguments");

    ObjMatrix* mat = asMatrix(args[0]);
    double s = mat->mat().sum();
    return Value::realVal(s);
}

Value VM::vector_min_builtin(ArgsView args)
{
    if (args.size() != 1 || !isVector(args[0]))
        throw std::invalid_argument("vector.min expects no arguments");

    ObjVector* vec = asVector(args[0]);
    return Value::realVal(vec->vec().minCoeff());
}

Value VM::vector_max_builtin(ArgsView args)
{
    if (args.size() != 1 || !isVector(args[0]))
        throw std::invalid_argument("vector.max expects no arguments");

    ObjVector* vec = asVector(args[0]);
    return Value::realVal(vec->vec().maxCoeff());
}

Value VM::matrix_min_builtin(ArgsView args)
{
    if (args.size() != 1 || !isMatrix(args[0]))
        throw std::invalid_argument("matrix.min expects no arguments");

    ObjMatrix* mat = asMatrix(args[0]);
    return Value::realVal(mat->mat().minCoeff());
}

Value VM::matrix_max_builtin(ArgsView args)
{
    if (args.size() != 1 || !isMatrix(args[0]))
        throw std::invalid_argument("matrix.max expects no arguments");

    ObjMatrix* mat = asMatrix(args[0]);
    return Value::realVal(mat->mat().maxCoeff());
}

Value VM::tensor_min_builtin(ArgsView args)
{
    if (args.size() != 1 || !isTensor(args[0]))
        throw std::invalid_argument("tensor.min expects no arguments");

    ObjTensor* t = asTensor(args[0]);
    int64_t n = t->numel();
    if (n == 0) throw std::invalid_argument("tensor.min on empty tensor");
    double minVal = t->at(0);
    for (int64_t i = 1; i < n; ++i)
        minVal = std::min(minVal, t->at(i));
    return Value::realVal(minVal);
}

Value VM::tensor_max_builtin(ArgsView args)
{
    if (args.size() != 1 || !isTensor(args[0]))
        throw std::invalid_argument("tensor.max expects no arguments");

    ObjTensor* t = asTensor(args[0]);
    int64_t n = t->numel();
    if (n == 0) throw std::invalid_argument("tensor.max on empty tensor");
    double maxVal = t->at(0);
    for (int64_t i = 1; i < n; ++i)
        maxVal = std::max(maxVal, t->at(i));
    return Value::realVal(maxVal);
}

Value VM::tensor_sum_builtin(ArgsView args)
{
    if (args.size() != 1 || !isTensor(args[0]))
        throw std::invalid_argument("tensor.sum expects no arguments");

    ObjTensor* t = asTensor(args[0]);
    int64_t n = t->numel();
    double s = 0.0;
    for (int64_t i = 0; i < n; ++i)
        s += t->at(i);
    return Value::realVal(s);
}

Value VM::list_append_builtin(ArgsView args)
{
    if (args.size() != 2 || !isList(args[0]))
        throw std::invalid_argument("list.append expects single argument");

    // TODO: Signal values should be resolved when passed as function arguments
    // Currently signals may not be resolved immediately, requiring workarounds like arithmetic (0 + signal)
    ObjList* list = asList(args[0]);
    list->append(args[1]);
    return Value::nilVal();
}

Value VM::list_filter_builtin(ArgsView args)
{
    if (args.size() != 2 || !isList(args[0]))
        throw std::invalid_argument("list.filter expects single predicate argument");

    if (!isClosure(args[1]))
        throw std::invalid_argument("list.filter: argument must be a function");

    ObjList* inputList = asList(args[0]);
    ObjClosure* predicate = asClosure(args[1]);

    // Empty list - return immediately
    if (inputList->empty())
        return Value::listVal();

    // Detect callback arity
    int arity = asFunction(predicate->function)->arity;

    // Create continuation state: {list, pred, result, index, arity}
    Value state = Value::dictVal();
    asDict(state)->store(Value::stringVal(toUnicodeString("list")), args[0]);
    asDict(state)->store(Value::stringVal(toUnicodeString("pred")), args[1]);
    asDict(state)->store(Value::stringVal(toUnicodeString("result")), Value::listVal());
    asDict(state)->store(Value::stringVal(toUnicodeString("index")), Value::intVal(0));
    asDict(state)->store(Value::stringVal(toUnicodeString("arity")), Value::intVal(arity));

    auto& cont = thread->nativeContinuation;
    cont.active = true;
    cont.state = state;
    // Set up stack cleanup: result goes to receiver slot, stack shrinks by arg count
    cont.resultSlot = &*(thread->stackTop - args.size());  // receiver position
    cont.stackBase = thread->stackTop - (args.size() - 1); // one past result slot
    cont.onComplete = [](VM& vm, Value callbackResult) -> bool {
        auto& st = vm.thread->nativeContinuation.state;
        auto* d = asDict(st);
        ObjList* list = asList(d->at(Value::stringVal(toUnicodeString("list"))));
        ObjList* result = asList(d->at(Value::stringVal(toUnicodeString("result"))));
        int idx = d->at(Value::stringVal(toUnicodeString("index"))).asInt();
        int arity = d->at(Value::stringVal(toUnicodeString("arity"))).asInt();

        // Process result - append element if predicate returned true
        auto elts = list->getElements();
        if (isTruthy(callbackResult))
            result->append(elts[idx]);

        // Advance index
        idx++;
        d->store(Value::stringVal(toUnicodeString("index")), Value::intVal(idx));

        // More iterations?
        if (static_cast<size_t>(idx) < elts.size()) {
            ObjClosure* pred = asClosure(d->at(Value::stringVal(toUnicodeString("pred"))));
            std::vector<Value> callArgs;
            callArgs.push_back(elts[idx]);
            if (arity >= 2)
                callArgs.push_back(Value::intVal(idx));
            return vm.pushContinuationCall(pred, callArgs);
        }

        // Done - push final result
        vm.push(d->at(Value::stringVal(toUnicodeString("result"))));
        return true;
    };

    // Push first callback
    auto elts = inputList->getElements();
    std::vector<Value> callArgs;
    callArgs.push_back(elts[0]);
    if (arity >= 2)
        callArgs.push_back(Value::intVal(0));

    if (!pushContinuationCall(predicate, callArgs))
        throw std::runtime_error("list.filter: failed to invoke predicate");

    // Return nil placeholder - actual result pushed by continuation
    return Value::nilVal();
}

Value VM::list_map_builtin(ArgsView args)
{
    if (args.size() != 2 || !isList(args[0]))
        throw std::invalid_argument("list.map expects single transform argument");

    if (!isClosure(args[1]))
        throw std::invalid_argument("list.map: argument must be a function");

    ObjList* inputList = asList(args[0]);
    ObjClosure* transform = asClosure(args[1]);

    // Empty list - return immediately
    if (inputList->empty())
        return Value::listVal();

    // Detect callback arity
    int arity = asFunction(transform->function)->arity;

    // Create continuation state: {list, transform, result, index, arity}
    Value state = Value::dictVal();
    asDict(state)->store(Value::stringVal(toUnicodeString("list")), args[0]);
    asDict(state)->store(Value::stringVal(toUnicodeString("transform")), args[1]);
    asDict(state)->store(Value::stringVal(toUnicodeString("result")), Value::listVal());
    asDict(state)->store(Value::stringVal(toUnicodeString("index")), Value::intVal(0));
    asDict(state)->store(Value::stringVal(toUnicodeString("arity")), Value::intVal(arity));

    auto& cont = thread->nativeContinuation;
    cont.active = true;
    cont.state = state;
    // Set up stack cleanup: result goes to receiver slot, stack shrinks by arg count
    cont.resultSlot = &*(thread->stackTop - args.size());  // receiver position
    cont.stackBase = thread->stackTop - (args.size() - 1); // one past result slot
    cont.onComplete = [](VM& vm, Value callbackResult) -> bool {
        auto& st = vm.thread->nativeContinuation.state;
        auto* d = asDict(st);
        ObjList* list = asList(d->at(Value::stringVal(toUnicodeString("list"))));
        ObjList* result = asList(d->at(Value::stringVal(toUnicodeString("result"))));
        int idx = d->at(Value::stringVal(toUnicodeString("index"))).asInt();
        int arity = d->at(Value::stringVal(toUnicodeString("arity"))).asInt();

        // Process result - append transformed value
        result->append(callbackResult);

        // Advance index
        idx++;
        d->store(Value::stringVal(toUnicodeString("index")), Value::intVal(idx));

        // More iterations?
        auto elts = list->getElements();
        if (static_cast<size_t>(idx) < elts.size()) {
            ObjClosure* transform = asClosure(d->at(Value::stringVal(toUnicodeString("transform"))));
            std::vector<Value> callArgs;
            callArgs.push_back(elts[idx]);
            if (arity >= 2)
                callArgs.push_back(Value::intVal(idx));
            return vm.pushContinuationCall(transform, callArgs);
        }

        // Done - push final result
        vm.push(d->at(Value::stringVal(toUnicodeString("result"))));
        return true;
    };

    // Push first callback
    auto elts = inputList->getElements();
    std::vector<Value> callArgs;
    callArgs.push_back(elts[0]);
    if (arity >= 2)
        callArgs.push_back(Value::intVal(0));

    if (!pushContinuationCall(transform, callArgs))
        throw std::runtime_error("list.map: failed to invoke transform");

    // Return nil placeholder - actual result pushed by continuation
    return Value::nilVal();
}

Value VM::list_reduce_builtin(ArgsView args)
{
    if (args.size() != 3 || !isList(args[0]))
        throw std::invalid_argument("list.reduce expects reducer function and initial value");

    if (!isClosure(args[1]))
        throw std::invalid_argument("list.reduce: first argument must be a function");

    ObjList* inputList = asList(args[0]);
    ObjClosure* reducer = asClosure(args[1]);

    // Empty list - return initial value immediately
    if (inputList->empty())
        return args[2];

    // Detect callback arity
    int arity = asFunction(reducer->function)->arity;

    // Create continuation state: {list, reducer, accumulator, index, arity}
    Value state = Value::dictVal();
    asDict(state)->store(Value::stringVal(toUnicodeString("list")), args[0]);
    asDict(state)->store(Value::stringVal(toUnicodeString("reducer")), args[1]);
    asDict(state)->store(Value::stringVal(toUnicodeString("accumulator")), args[2]);
    asDict(state)->store(Value::stringVal(toUnicodeString("index")), Value::intVal(0));
    asDict(state)->store(Value::stringVal(toUnicodeString("arity")), Value::intVal(arity));

    auto& cont = thread->nativeContinuation;
    cont.active = true;
    cont.state = state;
    // Set up stack cleanup: result goes to receiver slot, stack shrinks by arg count
    cont.resultSlot = &*(thread->stackTop - args.size());  // receiver position
    cont.stackBase = thread->stackTop - (args.size() - 1); // one past result slot
    cont.onComplete = [](VM& vm, Value callbackResult) -> bool {
        auto& st = vm.thread->nativeContinuation.state;
        auto* d = asDict(st);
        ObjList* list = asList(d->at(Value::stringVal(toUnicodeString("list"))));
        int idx = d->at(Value::stringVal(toUnicodeString("index"))).asInt();
        int arity = d->at(Value::stringVal(toUnicodeString("arity"))).asInt();

        // Process result - update accumulator
        d->store(Value::stringVal(toUnicodeString("accumulator")), callbackResult);

        // Advance index
        idx++;
        d->store(Value::stringVal(toUnicodeString("index")), Value::intVal(idx));

        // More iterations?
        auto elts = list->getElements();
        if (static_cast<size_t>(idx) < elts.size()) {
            ObjClosure* reducer = asClosure(d->at(Value::stringVal(toUnicodeString("reducer"))));
            std::vector<Value> callArgs;
            callArgs.push_back(callbackResult);  // new accumulator
            callArgs.push_back(elts[idx]);
            if (arity >= 3)
                callArgs.push_back(Value::intVal(idx));
            return vm.pushContinuationCall(reducer, callArgs);
        }

        // Done - push final accumulator
        vm.push(callbackResult);
        return true;
    };

    // Push first callback
    auto elts = inputList->getElements();
    std::vector<Value> callArgs;
    callArgs.push_back(args[2]);  // initial accumulator
    callArgs.push_back(elts[0]);
    if (arity >= 3)
        callArgs.push_back(Value::intVal(0));

    if (!pushContinuationCall(reducer, callArgs))
        throw std::runtime_error("list.reduce: failed to invoke reducer");

    // Return nil placeholder - actual result pushed by continuation
    return Value::nilVal();
}

#ifdef ROXAL_ENABLE_REGEX
Value VM::string_match_builtin(ArgsView args)
{
    if (args.size() != 2 || !isString(args[0]))
        throw std::invalid_argument("string.match expects regex pattern argument");

    std::string subject = toUTF8StdString(asStringObj(args[0])->s);

    // Get the regex wrapper - either from a Regex object or compile a string pattern
    RegexWrapper* wrapper = nullptr;
    bool ownsWrapper = false;

    if (isObjectInstance(args[1])) {
        ObjectInstance* inst = asObjectInstance(args[1]);
        Value fpVal = inst->getProperty("_this");
        if (!fpVal.isNil() && isForeignPtr(fpVal)) {
            wrapper = static_cast<RegexWrapper*>(asForeignPtr(fpVal)->ptr);
        }
    } else if (isString(args[1])) {
        std::string pattern = toUTF8StdString(asStringObj(args[1])->s);
        wrapper = ModuleRegex::compilePattern(pattern, "");
        ownsWrapper = true;
    }

    if (!wrapper)
        throw std::invalid_argument("string.match expects Regex object or pattern string");

    pcre2_match_data* matchData = pcre2_match_data_create_from_pattern(wrapper->code, nullptr);
    int rc = pcre2_match(
        wrapper->code,
        reinterpret_cast<PCRE2_SPTR>(subject.c_str()),
        subject.length(),
        0, 0, matchData, nullptr
    );

    if (rc < 0) {
        pcre2_match_data_free(matchData);
        if (ownsWrapper) delete wrapper;
        return Value::nilVal();
    }

    PCRE2_SIZE* ovector = pcre2_get_ovector_pointer(matchData);

    // Build result list with the match and groups
    Value resultVal = Value::listVal();
    ObjList* result = asList(resultVal);

    for (int i = 0; i < rc; i++) {
        PCRE2_SIZE start = ovector[2*i];
        PCRE2_SIZE end = ovector[2*i + 1];
        if (start == PCRE2_UNSET) {
            result->append(Value::nilVal());
        } else {
            std::string matchStr = subject.substr(start, end - start);
            result->append(Value::stringVal(toUnicodeString(matchStr)));
        }
    }

    pcre2_match_data_free(matchData);
    if (ownsWrapper) delete wrapper;
    return resultVal;
}

Value VM::string_search_builtin(ArgsView args)
{
    if (args.size() != 2 || !isString(args[0]))
        throw std::invalid_argument("string.search expects regex pattern argument");

    std::string subject = toUTF8StdString(asStringObj(args[0])->s);

    RegexWrapper* wrapper = nullptr;
    bool ownsWrapper = false;

    if (isObjectInstance(args[1])) {
        ObjectInstance* inst = asObjectInstance(args[1]);
        Value fpVal = inst->getProperty("_this");
        if (!fpVal.isNil() && isForeignPtr(fpVal)) {
            wrapper = static_cast<RegexWrapper*>(asForeignPtr(fpVal)->ptr);
        }
    } else if (isString(args[1])) {
        std::string pattern = toUTF8StdString(asStringObj(args[1])->s);
        wrapper = ModuleRegex::compilePattern(pattern, "");
        ownsWrapper = true;
    }

    if (!wrapper)
        throw std::invalid_argument("string.search expects Regex object or pattern string");

    pcre2_match_data* matchData = pcre2_match_data_create_from_pattern(wrapper->code, nullptr);
    int rc = pcre2_match(
        wrapper->code,
        reinterpret_cast<PCRE2_SPTR>(subject.c_str()),
        subject.length(),
        0, 0, matchData, nullptr
    );

    int64_t index = -1;
    if (rc >= 0) {
        PCRE2_SIZE* ovector = pcre2_get_ovector_pointer(matchData);
        index = static_cast<int64_t>(ovector[0]);
    }

    pcre2_match_data_free(matchData);
    if (ownsWrapper) delete wrapper;
    return Value::intVal(index);
}

Value VM::string_replace_builtin(ArgsView args)
{
    if (args.size() != 3 || !isString(args[0]))
        throw std::invalid_argument("string.replace expects pattern and replacement arguments");

    std::string subject = toUTF8StdString(asStringObj(args[0])->s);

    RegexWrapper* wrapper = nullptr;
    bool ownsWrapper = false;

    if (isObjectInstance(args[1])) {
        ObjectInstance* inst = asObjectInstance(args[1]);
        Value fpVal = inst->getProperty("_this");
        if (!fpVal.isNil() && isForeignPtr(fpVal)) {
            wrapper = static_cast<RegexWrapper*>(asForeignPtr(fpVal)->ptr);
        }
    } else if (isString(args[1])) {
        std::string pattern = toUTF8StdString(asStringObj(args[1])->s);
        wrapper = ModuleRegex::compilePattern(pattern, "");
        ownsWrapper = true;
    }

    if (!wrapper)
        throw std::invalid_argument("string.replace expects Regex object or pattern string");

    if (!isString(args[2]))
        throw std::invalid_argument("string.replace expects replacement string");

    std::string replacement = toUTF8StdString(asStringObj(args[2])->s);

    // Use PCRE2 substitute for replacement
    uint32_t options = PCRE2_SUBSTITUTE_OVERFLOW_LENGTH;
    if (wrapper->global)
        options |= PCRE2_SUBSTITUTE_GLOBAL;

    PCRE2_SIZE outlen = subject.length() * 2 + replacement.length() + 1;
    std::vector<PCRE2_UCHAR> output(outlen);

    int rc = pcre2_substitute(
        wrapper->code,
        reinterpret_cast<PCRE2_SPTR>(subject.c_str()),
        subject.length(),
        0,  // start offset
        options,
        nullptr,  // match data
        nullptr,  // match context
        reinterpret_cast<PCRE2_SPTR>(replacement.c_str()),
        replacement.length(),
        output.data(),
        &outlen
    );

    if (rc == PCRE2_ERROR_NOMEMORY) {
        // Retry with larger buffer
        output.resize(outlen + 1);
        rc = pcre2_substitute(
            wrapper->code,
            reinterpret_cast<PCRE2_SPTR>(subject.c_str()),
            subject.length(),
            0, options, nullptr, nullptr,
            reinterpret_cast<PCRE2_SPTR>(replacement.c_str()),
            replacement.length(),
            output.data(),
            &outlen
        );
    }

    if (ownsWrapper) delete wrapper;

    if (rc < 0) {
        // On error, return original string
        return args[0];
    }

    std::string result(reinterpret_cast<char*>(output.data()), outlen);
    return Value::stringVal(toUnicodeString(result));
}

Value VM::string_split_builtin(ArgsView args)
{
    if (args.size() != 2 || !isString(args[0]))
        throw std::invalid_argument("string.split expects regex pattern argument");

    std::string subject = toUTF8StdString(asStringObj(args[0])->s);

    RegexWrapper* wrapper = nullptr;
    bool ownsWrapper = false;

    if (isObjectInstance(args[1])) {
        ObjectInstance* inst = asObjectInstance(args[1]);
        Value fpVal = inst->getProperty("_this");
        if (!fpVal.isNil() && isForeignPtr(fpVal)) {
            wrapper = static_cast<RegexWrapper*>(asForeignPtr(fpVal)->ptr);
        }
    } else if (isString(args[1])) {
        std::string pattern = toUTF8StdString(asStringObj(args[1])->s);
        wrapper = ModuleRegex::compilePattern(pattern, "");
        ownsWrapper = true;
    }

    if (!wrapper)
        throw std::invalid_argument("string.split expects Regex object or pattern string");

    Value resultVal = Value::listVal();
    ObjList* result = asList(resultVal);

    pcre2_match_data* matchData = pcre2_match_data_create_from_pattern(wrapper->code, nullptr);
    PCRE2_SIZE offset = 0;

    while (offset < subject.length()) {
        int rc = pcre2_match(
            wrapper->code,
            reinterpret_cast<PCRE2_SPTR>(subject.c_str()),
            subject.length(),
            offset, 0, matchData, nullptr
        );

        if (rc < 0) {
            // No more matches - add rest of string
            std::string rest = subject.substr(offset);
            result->append(Value::stringVal(toUnicodeString(rest)));
            break;
        }

        PCRE2_SIZE* ovector = pcre2_get_ovector_pointer(matchData);
        PCRE2_SIZE matchStart = ovector[0];
        PCRE2_SIZE matchEnd = ovector[1];

        // Add part before match
        std::string part = subject.substr(offset, matchStart - offset);
        result->append(Value::stringVal(toUnicodeString(part)));

        // Handle zero-length matches
        if (matchEnd == matchStart) {
            offset = matchEnd + 1;
        } else {
            offset = matchEnd;
        }
    }

    pcre2_match_data_free(matchData);
    if (ownsWrapper) delete wrapper;
    return resultVal;
}
#endif // ROXAL_ENABLE_REGEX

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

Value VM::signal_on_changed_builtin(ArgsView args)
{
    if (args.size() != 2 || !isSignal(args[0]) || !isClosure(args[1]))
        throw std::invalid_argument("signal.on_changed expects signal and closure argument");

    Value signalVal = args[0];
    Value closureVal = args[1];

    ObjSignal* sigObj = asSignal(signalVal);
    ObjEventType* ev = sigObj->ensureChangeEventType();  // Lazy create SignalChanged event type
    Value eventVal = sigObj->changeEventType;

    // Register handler on current thread
    Value key = eventVal.weakRef();
    thread->eventHandlers[key].push_back(Thread::HandlerRegistration{closureVal, std::nullopt});

    // Validate closure arity (0 or 1 arguments allowed)
    ObjClosure* closure = asClosure(closureVal);
    if (closure->function.isNonNil()) {
        ObjFunction* fn = asFunction(closure->function);
        if (fn->arity > 1)
            throw std::invalid_argument("signal change handler must accept 0 or 1 arguments");
    }

    // Subscribe closure to event
    closure->handlerThread = thread;
    ev->subscribers.push_back(closureVal.weakRef());

    // Track the signal for this event
        thread->eventToSignal[eventVal.weakRef()] = signalVal.weakRef();

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



ptr<BuiltinModule> VM::getBuiltinModule(const icu::UnicodeString& name)
{
    // Check eagerly-loaded modules first (e.g., sys)
    for (auto& m : builtinModules) {
        Value mt = m->moduleType();
        if (mt.isNil() || !isModuleType(mt))
            continue; // helper-only modules (e.g., grpc) do not expose a module type
        if (asModuleType(mt)->name == name)
            return m;
    }

    // Check lazy registry and trigger loading if registered
    return lazyModuleRegistry.ensureLoaded(name, *this);
}

Value VM::getBuiltinModuleType(const icu::UnicodeString& name)
{
    // Check eagerly-loaded modules first
    for (auto& m : builtinModules) {
        Value mt = m->moduleType();
        if (mt.isNil() || !isModuleType(mt))
            continue;
        if (asModuleType(mt)->name == name)
            return mt;
    }

    // Check lazy registry and trigger loading if registered
    if (lazyModuleRegistry.isRegistered(name)) {
        auto mod = lazyModuleRegistry.ensureLoaded(name, *this);
        if (mod)
            return mod->moduleType();
    }

    return Value::nilVal();
}

void VM::executeBuiltinModuleScript(const std::string& path, Value moduleType)
{
    debug_assert_msg(isModuleType(moduleType),"is ObjModuleType");
    std::filesystem::path openedPath;
    std::ifstream in;

    std::vector<std::string> searchRoots = modulePaths;
    for (const auto& candidate : VM::defaultModuleSearchPaths()) {
        if (std::find(searchRoots.begin(), searchRoots.end(), candidate) == searchRoots.end())
            searchRoots.push_back(candidate);
    }

    std::filesystem::path requested(path);
    std::vector<std::filesystem::path> candidates;
    auto addCandidate = [&](const std::filesystem::path& candidate) {
        if (candidate.empty())
            return;
        if (std::find(candidates.begin(), candidates.end(), candidate) != candidates.end())
            return;
        candidates.push_back(candidate);
    };

    if (requested.is_absolute()) {
        addCandidate(requested);
    } else {
        addCandidate(requested);
        for (const auto& root : searchRoots)
            addCandidate(std::filesystem::path(root) / requested);
    }

    for (const auto& candidate : candidates) {
        std::ifstream candidateStream(candidate);
        if (candidateStream.is_open()) {
            in = std::move(candidateStream);
            openedPath = candidate;
            break;
        }
    }

    if (!in.is_open()) {
        std::ostringstream oss;
        oss << "Cannot open builtin module script '" << path << "'";
        if (!candidates.empty()) {
            oss << " (searched:";
            bool first = true;
            for (const auto& candidate : candidates) {
                oss << (first ? " " : ", ") << candidate.string();
                first = false;
            }
            oss << ")";
        }
        runtimeError(oss.str());
        return;
    }

    std::filesystem::path cacheSourcePath;
    try {
        cacheSourcePath = std::filesystem::canonical(std::filesystem::absolute(openedPath));
    } catch (...) {
        cacheSourcePath.clear();
    }

    RoxalCompiler compiler;
    compiler.setOutputBytecodeDisassembly(false);
    compiler.setCacheReadEnabled(cacheReadsEnabled());
    compiler.setCacheWriteEnabled(cacheWritesEnabled());
    compiler.setModulePaths(modulePaths);
    compiler.setModuleResolverVM(this);

    Value fn { Value::nilVal() };
    bool loadedFromCache = false;
    if (!cacheSourcePath.empty()) {
        Value cached = compiler.loadFileCache(cacheSourcePath);
        if (cached.isNonNil()) {
            fn = cached;
            loadedFromCache = true;
        }
    }

    if (!loadedFromCache) {
        fn = compiler.compile(in, openedPath.string(), moduleType);
        if (!fn.isNil() && !cacheSourcePath.empty())
            compiler.storeFileCache(cacheSourcePath, fn);
    }

    if (fn.isNil())
        return;

    Value closure { Value::closureVal(fn) };
    ptr<Thread> t = make_ptr<Thread>();
    thread = t;
    resetStack();
    invokeClosure(asClosure(closure), {});
    thread = nullptr;
}

void VM::registerBuiltinModule(ptr<BuiltinModule> module)
{
    // Note: Module-specific pointer registration (grpcModule, ddsModule) is now
    // handled by onModuleLoaded() hooks, called during lazy loading
    builtinModules.push_back(module);
    if (module) {
        appendModulePaths(module->additionalModulePaths());
    }
}

#ifdef ROXAL_ENABLE_GRPC
Value VM::importProtoModule(const std::string& path)
{
    if (!grpcModule)
        lazyModuleRegistry.ensureLoaded(toUnicodeString("grpc"), *this);
    if (!grpcModule)
        throw std::runtime_error("gRPC module not initialized");
    return grpcModule->importProto(path);
}
#endif
#ifdef ROXAL_ENABLE_DDS
Value VM::importIdlModule(const std::string& path)
{
    if (!ddsModule)
        lazyModuleRegistry.ensureLoaded(toUnicodeString("dds"), *this);
    if (!ddsModule)
        throw std::runtime_error("DDS module not initialized");
    return ddsModule->importIdl(path);
}
#endif

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

ExecutionStatus VM::joinAllThreads(uint64_t skipId)
{
    ExecutionStatus combined = ExecutionStatus::OK;
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
                if (t->result != ExecutionStatus::OK)
                    combined = ExecutionStatus::RuntimeError;
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
