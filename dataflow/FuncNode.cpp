#include "FuncNode.h"

#include "core/common.h"
#include <algorithm>
#include "compiler/VM.h"
#include "compiler/Thread.h"
#include "compiler/ExecutionDomain.h"
#include "DataflowEngine.h"
#include <stdexcept>
#include <iostream>
#include <regex>

using namespace df;

namespace {

// RAII guard: this thread is running a node body (see VM::inDataflowBody)
struct DataflowBodyGuard {
    DataflowBodyGuard() { roxal::VM::setInDataflowBody(true); }
    ~DataflowBodyGuard() { roxal::VM::setInDataflowBody(false); }
};

// An exception Value of the builtin UnavailableError type (or a subtype).
bool isUnavailableError(const roxal::Value& exception)
{
    const roxal::Value& type = roxal::asException(exception)->exType;
    if (!roxal::isObjectType(type))
        return false;
    auto unavailable = roxal::VM::instance().loadGlobal(roxal::toUnicodeString("UnavailableError"));
    return unavailable && roxal::isObjectType(*unavailable)
        && roxal::isSubtypeOf(roxal::asObjectType(type), roxal::asObjectType(*unavailable));
}

// The domain of the thread constructing a node: the run (or session, or
// service) the node belongs to.
weak_ptr<roxal::ExecutionDomain> creatingDomain()
{
    if (roxal::VM::thread)
        return weak_ptr<roxal::ExecutionDomain>(roxal::VM::thread->domain);
    return weak_ptr<roxal::ExecutionDomain>();
}

std::optional<roxal::ValueType> valueTypeForBuiltin(roxal::type::BuiltinType builtin)
{
    using roxal::type::BuiltinType;
    switch (builtin) {
        case BuiltinType::Nil:   return roxal::ValueType::Nil;
        case BuiltinType::Bool:  return roxal::ValueType::Bool;
        case BuiltinType::Byte:  return roxal::ValueType::Byte;
        case BuiltinType::Int:   return roxal::ValueType::Int;
        case BuiltinType::Real:
        case BuiltinType::Number: return roxal::ValueType::Real;
        case BuiltinType::String: return roxal::ValueType::String;
        case BuiltinType::Range:  return roxal::ValueType::Range;
        case BuiltinType::List:   return roxal::ValueType::List;
        case BuiltinType::Dict:   return roxal::ValueType::Dict;
        case BuiltinType::Vector: return roxal::ValueType::Vector;
        case BuiltinType::Matrix: return roxal::ValueType::Matrix;
        case BuiltinType::Event:  return roxal::ValueType::Event;
        case BuiltinType::Type:   return roxal::ValueType::Type;
        default:
            return std::nullopt;
    }
}

std::optional<roxal::Value> defaultValueForReturnType(const ptr<roxal::type::Type>& typeSpec)
{
    if (!typeSpec)
        return std::nullopt;

    auto valueType = valueTypeForBuiltin(typeSpec->builtin);
    if (!valueType.has_value())
        return std::nullopt;

    try {
        return roxal::defaultValue(valueType.value());
    } catch (...) {
        return std::nullopt;
    }
}

}

FuncNode::FuncNode(const std::string& name,
                   const roxal::Value& closure_,
                   const ConstArgMap& constArgs_,
                   const std::vector<ptr<Signal>>& signalArgs_,
                   const std::vector<ptr<Signal>>& outputSignals)
  : m_name(name), m_operatorSignalsCalled(false), m_id(nextGraphId()), closure(closure_), constArgs(constArgs_), signalArgs(signalArgs_), m_overrideOutputSignals(outputSignals)
{
    m_ownerDomain = creatingDomain();
    m_outputNames = {DataflowEngine::uniqueFuncName("result")};
    if (roxal::isClosure(closure) && asFunction(asClosure(closure)->function)->funcType.has_value()) {
        auto funcTypePtr = asFunction(asClosure(closure)->function)->funcType.value();
        if (funcTypePtr->func.has_value()) {
            const auto& funcType = funcTypePtr->func.value();

            if (!funcType.returnTypes.empty()) {
                if (funcType.returnTypes.size() == 1)
                    m_outputNames = {DataflowEngine::uniqueFuncName("result")};
                else {
                    // multi-return: one output port per declared return type
                    m_outputNames.clear();
                    for (size_t i = 0; i < funcType.returnTypes.size(); ++i)
                        m_outputNames.push_back("result" + std::to_string(i));
                }
            }

            initializeOutputDefaults(funcType.returnTypes);

            size_t sigIndex = 0;
            for (const auto& param : funcType.params) {
                std::string pname;
                if (param.has_value())
                    pname = roxal::toUTF8StdString(param->name);
                else
                    pname = std::to_string(paramNames.size());
                m_inputNames.push_back(pname);
                paramNames.push_back(pname);

                auto it = constArgs.find(pname);
                if (it == constArgs.end()) {
                    if (sigIndex < signalArgs.size()) {
                        addInput(pname, signalArgs[sigIndex]);
                        paramSignalIndex.push_back(int(sigIndex));
                        sigIndex++;
                    } else {
                        paramSignalIndex.push_back(-1);
                    }
                } else {
                    paramSignalIndex.push_back(-1);
                }
            }
            // Compute max input frequency.  All-event-driven inputs
            // (every frequency 0) produce an EVENT-DRIVEN output (freq 0),
            // matching the operator()(Signals) path -- a periodic output
            // on an event island is incoherent (its writes happen at set()
            // times, not on any grid).
            double maxFreq = 0.0;
            for (auto& sig : signalArgs)
                maxFreq = std::max(maxFreq, sig->frequency());

            createOutputSignals(maxFreq);
            m_operatorSignalsCalled = true;
        }
    }

    if (m_outputDefaults.size() != outputNames().size())
        initializeOutputDefaults(std::vector<ptr<roxal::type::Type>>{});
}

FuncNode::FuncNode(const std::string& name,
                   NativeFunc nativeFunc_,
                   const std::vector<std::string>& paramNames_,
                   const ConstArgMap& constArgs_,
                   const std::vector<ptr<Signal>>& signalArgs_,
                   const Names& outputNames_,
                   const std::vector<ptr<Signal>>& outputSignals)
  : m_name(name), m_operatorSignalsCalled(false), m_id(nextGraphId()), closure(Value::nilVal()),
    nativeFunc(nativeFunc_), constArgs(constArgs_), signalArgs(signalArgs_), m_overrideOutputSignals(outputSignals)
{
    m_ownerDomain = creatingDomain();
    m_outputNames = outputNames_;
    if (m_outputNames.empty())
        m_outputNames = {DataflowEngine::uniqueFuncName("result")};
    initializeOutputDefaults(std::vector<ptr<roxal::type::Type>>{});
    paramNames = paramNames_;
    size_t sigIndex = 0;
    for(const auto& pname : paramNames) {
        m_inputNames.push_back(pname);
        auto it = constArgs.find(pname);
        if (it == constArgs.end()) {
            if (sigIndex < signalArgs.size()) {
                addInput(pname, signalArgs[sigIndex]);
                paramSignalIndex.push_back(int(sigIndex));
                sigIndex++;
            } else {
                paramSignalIndex.push_back(-1);
            }
        } else {
            paramSignalIndex.push_back(-1);
        }
    }
    // All-event-driven inputs -> event-driven output (see the closure ctor).
    double maxFreq = 0.0;
    for (auto& sig : signalArgs)
        maxFreq = std::max(maxFreq, sig->frequency());
    createOutputSignals(maxFreq);
    m_operatorSignalsCalled = true;
}

FuncNode::~FuncNode()
{
}

void FuncNode::trace(roxal::ValueVisitor& visitor) const
{
    auto visitValue = [&](const roxal::Value& v) {
        if (v.isObj())
            visitor.visit(v);
    };

    visitValue(closure);
    for (const auto& entry : constArgs)
        visitValue(entry.second);
    for (const auto& in : m_inputs) {
        if (in.defaultValue.has_value())
            visitValue(in.defaultValue.value());
    }
    for (const auto& v : previousInputValues)
        visitValue(v);
    for (const auto& v : previousOutputValues)
        visitValue(v);
    for (const auto& v : m_funcYieldState.inputValues)
        visitValue(v);
    for (const auto& v : m_funcYieldState.pendingOutputFutures)
        visitValue(v);
    for (const auto& f : m_funcYieldState.forwards)
        visitValue(f.inner);
    visitValue(m_funcYieldState.bodyModule);
    // Per-output default values (returned by initialValueForOutput) are
    // retained Values too.
    for (const auto& d : m_outputDefaults) {
        if (d.has_value())
            visitValue(d.value());
    }
    // A yielded execution preserves interpreter state on a Thread that may
    // not be in the VM's thread registry -- run the COMPLETE thread root
    // scan on it (stack, frames incl. tail args, upvalues, event-dispatch,
    // native continuations, wait state, ...), not a partial copy.
    if (m_funcYieldState.executionThread) {
        roxal::SimpleMarkSweepGC::visitSingleThreadRoots(
            *m_funcYieldState.executionThread, visitor);
    }
}

const std::string& FuncNode::name() const
{
    return m_name;
}

void FuncNode::addInput(const std::string& name, ptr<Signal> signal, int index, std::optional<roxal::Value> defaultValue)
{
    if (index > 0)
        throw std::invalid_argument("FuncNode '"+m_name+"' addInput() index must be 0 or negative (index=-1 -> previous period's value)");

    TimePoint available = TimePoint::zero();
    if (signal)
        available = signal->latestSampleTime();
    InputPort inputPort = {name, signal, index, defaultValue, available};
    m_inputs.push_back(inputPort);
}

void FuncNode::reassignInput(const std::string& name, ptr<Signal> newSignal)
{
    for(auto& input : m_inputs) {
        if (input.name == name) {
            auto oldSignal = input.signal;
            input.signal = newSignal;
            for(auto& signalArg : signalArgs) {
                if (signalArg == oldSignal) {
                    signalArg = newSignal;
                }
            }
        }
    }
}


void FuncNode::addOutput(const std::string& name, ptr<Signal> signal)
{
    OutputPort outputPort = {name, signal};
    m_outputs.push_back(outputPort);
}

void FuncNode::setInputDefault(const std::string inputName, const roxal::Value& defaultValue)
{
    bool found = false;
    for (auto& input : m_inputs) {
        if (input.name == inputName) {
            input.defaultValue = defaultValue;
            found = true;
            break;
        }
    }

    if (!found)
        throw std::invalid_argument("FuncNode '"+name()+"' setInputDefault() input '"+inputName+"' not found");
}

Signals FuncNode::outputs() const
{
    if (m_outputs.empty())
        const_cast<FuncNode*>(this)->createOutputSignals(1.0); // default to 1Hz - will be updated once inputs are added
    std::vector<ptr<Signal>> outputSignals;
    for (auto& output : m_outputs)
        outputSignals.push_back(output.signal);
    return outputSignals;
}

void FuncNode::addExecutionCallback(std::function<void(TimePoint, ptr<FuncNode>, const Values&, const Values&)> callback)
{
    executionCallbacks.push_back(callback);
}

Signals FuncNode::operator()(const Signals& signals, const std::optional<ParamMap>& signalsToParams)
{
    if (m_operatorSignalsCalled)
        throw std::runtime_error("FuncNode::operator(signals) called twice - func '"+name()+"'");

    if (signals.size() != inputNames().size())
        throw std::invalid_argument("FuncNode '"+name()+"' requires "+std::to_string(inputNames().size())+" input signals, but got "+std::to_string(signals.size())+" signals");

    if (signalsToParams.has_value()) {
        if (signalsToParams.value().size() != inputNames().size())
            throw std::invalid_argument("FuncNode '"+name()+"' requires "+std::to_string(inputNames().size())+" input signals, but map has "+std::to_string(signals.size())+" signals");

        auto findSignalWithName = [signals](const std::string& name) -> ptr<Signal> {
            for (auto& signal : signals) {
                if (signal->name() == name)
                    return signal;
            }
            return nullptr;
        };

        // Parse input signal name like "mysignal", or "mysignal[-1]" into name & index
        //  (index references previous values of the signal - (-index periods of the signal ago))
        auto parseInputSignalName = [](const std::string& inputSignalNameSpec, std::string& name, int& index) {
            std::regex pattern(R"((\w+)(?:\[(-?\d+)\])?)");
            std::smatch matches;

            if (std::regex_match(inputSignalNameSpec, matches, pattern)) {
                name = matches[1].str(); // The name is always in the first capturing group

                if (matches[2].matched) {
                    // If there's a second capturing group, it's the latency
                    index = std::stoi(matches[2].str());
                } else {
                    index = 0; // No latency specified
                }
            } else {
                // The input doesn't match the expected pattern
                throw std::invalid_argument("Invalid input signal name format: " + inputSignalNameSpec);
            }
        };

        const auto& paramValues = roxal::mapValues(signalsToParams.value());
        for (auto& inputName : inputNames()) {

            // look for a provided input signal to map to this input
            for(auto& fromToName : signalsToParams.value()) {
                if (fromToName.second == inputName) {
                    std::string inputSignalName;
                    int index = 0;
                    parseInputSignalName(fromToName.first, inputSignalName, index);
                    auto inputSignal = findSignalWithName(inputSignalName);
                    if (!inputSignal)
                        throw std::invalid_argument("FuncNode '"+name()+"' input signal '"+inputSignalName+"' not found in signals: "+roxal::join(inputNames(), ", "));
                    addInput(inputName, inputSignal, index);
                    break;
                }
            }
        }
        if (m_inputs.size() != inputNames().size())
            throw std::invalid_argument("FuncNode '"+name()+"' not all inputs supplied");

    }
    else { // in-order mapping
        for(auto inputSignal : signals) {
            addInput(inputSignal->name(), inputSignal);
        }
    }

    // create all the output signals having a frequency that is the max of the input signals
    double maxFreq = 0.0;
    for (const auto& signal : signals)
        maxFreq = std::max(maxFreq, signal->frequency());

    // create output signals (if not already created)
    if (m_outputs.empty())
        createOutputSignals(maxFreq);
    else
        updateOutputSignals(maxFreq);

    m_operatorSignalsCalled = true;

    return outputs();
}

void FuncNode::createOutputSignals(double freq)
{
    m_outputs.clear();
    auto names = outputNames();
    if (!m_overrideOutputSignals.empty()) {
        size_t count = std::min(m_overrideOutputSignals.size(), names.size());
        for(size_t i=0;i<count;++i) {
            auto sig = m_overrideOutputSignals[i];
            sig->setFrequency(freq);
            sig->m_suppressInitialChange = true;
            addOutput(names[i], sig);
        }
        for(size_t i=count;i<names.size();++i) {
            auto outputSignal = Signal::newSignal(freq, initialValueForOutput(i), names[i]);
            outputSignal->m_suppressInitialChange = true;
            if (m_srcLine != 0)
                outputSignal->setSrcOrigin(m_srcName, m_srcLine, m_srcCol);
            addOutput(names[i], outputSignal);
        }
        m_overrideOutputSignals.clear();
    } else {
        // create output signals(freq may be updated as inputs added)
        for(size_t i=0;i<names.size();++i) {
            auto outputSignal = Signal::newSignal(freq, initialValueForOutput(i), names[i]);
            outputSignal->m_suppressInitialChange = true;
            if (m_srcLine != 0)
                outputSignal->setSrcOrigin(m_srcName, m_srcLine, m_srcCol);
            addOutput(names[i], outputSignal);
        }
    }
}

// Maps a body's return value onto this node's output ports.  A single-port
// node carries the value as-is (so a returned list is ONE list-valued output,
// per the '-> list' vs '-> [T..]' rule).  An N-port node splits a returned
// list element-wise, one per port.  Anything else -- a non-list, or a future
// still pending from an async native body -- passes through unchanged: a
// non-list then trips the caller's arity check with an honest count, and a
// future is distributed once it resolves (see resumeExecution).  A wrong-length
// list likewise reaches the arity check with its real element count rather
// than being padded or truncated silently; declared '-> [T..]' Roxal bodies
// can't produce one (CheckReturnList), so it only ever reports a native bug.
Values FuncNode::distributeReturnValue(const roxal::Value& returned) const
{
    if (m_outputNames.size() > 1 && roxal::isList(returned)) {
        auto* list = roxal::asList(returned);
        Values outs;
        outs.reserve(list->length());
        for (int32_t i = 0; i < list->length(); ++i)
            outs.push_back(list->getElement(i));
        return outs;
    }
    return { returned };
}

void FuncNode::initializeOutputDefaults(const std::vector<ptr<roxal::type::Type>>& returnTypes)
{
    const auto count = outputNames().size();
    m_outputDefaults.assign(count, std::nullopt);
    for (size_t i = 0; i < count && i < returnTypes.size(); ++i) {
        auto defaultValue = defaultValueForReturnType(returnTypes[i]);
        if (defaultValue.has_value())
            m_outputDefaults[i] = defaultValue;
    }
}

roxal::Value FuncNode::initialValueForOutput(size_t index) const
{
    if (index < m_outputDefaults.size() && m_outputDefaults[index].has_value())
        return m_outputDefaults[index].value();
    return roxal::Value();
}

void FuncNode::updateOutputSignals(double freq)
{
    for(auto& output : m_outputs)
        output.signal->setFrequency(freq);
}

void FuncNode::addToEngine()
{
    DataflowEngine::instance()->addFunc(ptr_from_this());
}

bool FuncNode::inputsAvailableAt(TimePoint time) const
{
    for (const auto& input : m_inputs) {
        TimeDuration latency = input.signal->period() * -input.index;
        auto timeNeeded = time - latency;
        auto availableTime = std::min(timeNeeded, input.signal->latestSampleTime());

        bool hasDefault = input.defaultValue.has_value();
        bool upToDate = input.latestAvailableTime >= availableTime;
        bool hasValue = false;

        try {
            auto sample = input.signal->valueAt(availableTime);
            hasValue = !sample.isNil();
        } catch (...) {
            hasValue = false;
        }

        if ((!upToDate || !hasValue) && !hasDefault) {
            return false;
        }
    }
    return true;
}

Values FuncNode::gatherInputs(TimePoint time) const
{
    Values inputValues;
    for (const auto& input : m_inputs) {
        TimeDuration latency = input.signal->period() * -input.index;
        auto inputTime = time - latency;
        auto inputValue = input.signal->valueIfAvailableAt(inputTime);
        if (inputValue.has_value()) {
            if (!inputValue.value().isNil()) {
                inputValues.push_back(inputValue.value());
            } else if (input.defaultValue.has_value()) {
                inputValues.push_back(input.defaultValue.value());
            } else {
                auto fallbackTime = std::min(inputTime, input.signal->latestSampleTime());
                auto fallback = input.signal->valueAt(fallbackTime);
                inputValues.push_back(fallback);
            }
        } else {
            auto fallbackTime = std::min(inputTime, input.signal->latestSampleTime());
            if (fallbackTime < TimePoint::zero() && !input.defaultValue.has_value())
                throw std::runtime_error("FuncNode '"+name()+"' signal '"+input.signal->name()+"' not available at time "+inputTime.humanString());

            try {
                auto fallback = input.signal->valueAt(fallbackTime);
                if (!fallback.isNil()) {
                    inputValues.push_back(fallback);
                } else if (input.defaultValue.has_value()) {
                    inputValues.push_back(input.defaultValue.value());
                } else {
                    throw std::runtime_error("FuncNode '"+name()+"' signal '"+input.signal->name()+"' not available at time "+inputTime.humanString());
                }
            } catch (...) {
                if (input.defaultValue.has_value())
                    inputValues.push_back(input.defaultValue.value());
                else
                    throw std::runtime_error("FuncNode '"+name()+"' signal '"+input.signal->name()+"' not available at time "+inputTime.humanString());
            }
        }
    }
    return inputValues;
}

FuncExecResult FuncNode::conditionallyExecute(TimePoint time, TimePoint deadline)
{
    using roxal::VM;
    using roxal::Value;
    using roxal::ExecutionStatus;
    using roxal::asClosure;
    using roxal::isFuture;

    // A pending future-based yield (an async native body such as ai.nn
    // predict, evaluated earlier at lift time, on a tick, or for an event):
    // poll it first.  Still pending: remember that a newer evaluation was
    // asked for, so the node runs again on its current inputs once the
    // futures resolve (see resumeExecution) instead of dropping this
    // update behind the in-flight one.  Resolved: deliver, then -- unless
    // the rerun already covered it -- fall through and evaluate afresh at
    // this newer time.  A held evaluation (see resumeExecution) is settled
    // the same way, but delivered nothing.
    //
    // Forwarding a stream (see FuncYieldState::Forward) is such a yield too,
    // but changed inputs may end it rather than wait behind it: the node
    // then evaluates on them here, as if nothing were pending.  Checked
    // before the poll, and again after one that found the futures resolved
    // to a stream -- forwarded only from then on.
    std::optional<Values> gathered;
    auto takeOverForwarding = [&] {
        if (m_funcYieldState.forwards.empty() || time <= m_funcYieldState.executionTime
            || !newInputEndsForwarding())
            return false;
        if (!gathered)
            gathered = gatherInputs(time);
        if (*gathered == m_funcYieldState.inputValues)
            return false;
        endForwarding();
        return true;
    };
    takeOverForwarding();

    bool delivered = false;   // a pending evaluation completed in this call
    if (hasPendingFutureYield()) {
        const TimePoint yieldedAt = m_funcYieldState.executionTime;
        auto result = resumeExecution(deadline, time);
        if (result != FuncExecResult::Completed && result != FuncExecResult::Held) {
            if (result == FuncExecResult::Yielded && time > yieldedAt
                && (!m_funcYieldState.rerunTime || time > *m_funcYieldState.rerunTime))
                m_funcYieldState.rerunTime = time;
            return result;
        }
        if (m_funcYieldState.active && !takeOverForwarding()) {
            // the rerun yielded anew, or a stream is forwarded and stays
            if (time > m_funcYieldState.executionTime
                && (!m_funcYieldState.rerunTime || time > *m_funcYieldState.rerunTime))
                m_funcYieldState.rerunTime = time;
            return FuncExecResult::Yielded;
        }
        if (time <= yieldedAt)
            return result == FuncExecResult::Held ? FuncExecResult::NotExecuted : result;
        delivered = result == FuncExecResult::Completed;
    }

    if (!m_operatorSignalsCalled && !inputNames().empty())
        throw std::runtime_error("FuncNode '"+name()+"' not all inputs connected");

    #ifdef DEBUG_BUILD
    if (!inputsAvailableAt(time))
        throw std::runtime_error("FuncNode '"+name()+"' inputs not available at time "+time.humanString());
    #endif

    Values inputValues = gathered ? std::move(*gathered) : gatherInputs(time);

    // FIXME: if not pure, don't execute every time, just on our expected period (GCD)
    // NB: don't compare inputs if unnecessary (expensive)
    bool execute = !isPure() || (previousInputValues != inputValues);

    if (!execute) {
        previousInputValues = inputValues;
        // (the resumed evaluation above did deliver: report it so the
        // caller marks the outputs available to their consumers)
        return delivered ? FuncExecResult::Completed : FuncExecResult::NotExecuted;
    }

    Values outputValues;

    if (nativeFunc) {
        // Native functions complete immediately - no deadline concern
        try {
            outputValues = operator()(inputValues);
        } catch (std::exception& e) {
            throw std::runtime_error("FuncNode '"+name()+"' nativeFunc threw exception: "+std::string(e.what()));
        }
    } else {
        // Closure execution - pass deadline to VM
        auto& vm = VM::instance();

        // Build args from inputValues (same logic as operator())
        std::vector<Value> args = assembleArgs(inputValues);

        if (!DataflowEngine::soundEvaluationContext(deadline)) {
            VM::emitDiagnostic(
                "FuncNode '" + name() + "': body not evaluated -- called outside "
                "the engine's execution context ("
                + std::string(VM::thread ? "on a thread that is not executing it"
                                         : "no thread bound")
                + ")", roxal::OutputSeverity::Error, "dataflow");
            return FuncExecResult::Error;
        }

        // A function reaches its module through a weak link, so a node can
        // outlive the module of the script that created it: the wrappers that
        // keep a node registered go after the module does, or never, if a
        // signal of the node is stored somewhere that outlives the script.
        // Such a body has nothing to run against.  Any other is held until it
        // is over, in case the script ends meanwhile -- and only that long:
        // the module's variables hold the node's own signals, so a node
        // holding its module for good would keep them in scope for ever.
        const Value& moduleLink = asFunction(asClosure(closure)->function)->moduleType;
        if (!moduleLink.isNil()) {
            m_funcYieldState.bodyModule = moduleLink.strongRef();
            if (m_funcYieldState.bodyModule.isNil())
                return FuncExecResult::Retired;
        }

        auto result = [&] {
            DataflowBodyGuard dfGuard;
            return vm.invokeClosure(asClosure(closure), args, deadline);
        }();
        // Whatever the result: a body can exit() and still come back
        // Yielded (the deadline check follows the call), and its context may
        // be discarded before it is ever resumed.
        carryOutcomeToOwner();

        if (roxal::isSuspended(result.first)) {
            // VM yielded (deadline) or was debugger-paused -- identical state
            // retention either way: closure, Thread and call state stay live
            // for resumption.
            m_funcYieldState.active = true;
            m_funcYieldState.inputValues = inputValues;
            m_funcYieldState.executionThread = VM::thread;
            m_funcYieldState.executionTime = time;
            return result.first == ExecutionStatus::Paused ? FuncExecResult::Paused
                                                           : FuncExecResult::Yielded;
        }

        m_funcYieldState.bodyModule = Value::nilVal();

        if (result.first != ExecutionStatus::OK) {
            return FuncExecResult::Error;
        }

        outputValues = distributeReturnValue(result.second);
    }

    return finishEvaluation(time, inputValues, outputValues);
}

FuncExecResult FuncNode::finishEvaluation(TimePoint time, const Values& inputValues,
                                          const Values& outputValues)
{
    // An output that is a future (an async native body such as predict, an
    // actor call): nothing is published yet.  The node keeps the futures and
    // is checked again on later evaluations (resumeExecution, Case 1),
    // without anything waiting for them.
    bool hasPendingFuture = false;
    for (const auto& v : outputValues) {
        if (isFuture(v)) { hasPendingFuture = true; break; }
    }
    if (hasPendingFuture) {
        m_funcYieldState.active = true;
        m_funcYieldState.inputValues = inputValues;
        m_funcYieldState.executionTime = time;
        m_funcYieldState.pendingOutputFutures = outputValues;
        m_funcYieldState.executionThread = nullptr;  // no VM thread for future-based yield
        return FuncExecResult::Yielded;
    }

    // A signal: its values are forwarded onto the ports, the node pending
    // until it stops -- unless it already has (a snapshot, published now).
    if (startForwarding(time, inputValues, outputValues))
        return m_funcYieldState.active ? FuncExecResult::Yielded : FuncExecResult::Completed;

    if (outputValues.size() != m_outputs.size())
        throw std::runtime_error("FuncNode '"+name()+"' returned "+std::to_string(outputValues.size())+" values, expected "+std::to_string(m_outputs.size())+" values");

    previousOutputValues = outputValues;
    previousInputValues = inputValues;
    invokeExecutionCallbacks(time, inputValues, outputValues);

    // Update the outputs
    int index = 0;
    for (const auto& output : m_outputs) {
        output.signal->setValueAt(time, outputValues[index]);
        index++;
    }
    m_lastDelivery = time;

    m_failureReported = false;
    m_holdReported = false;
    return FuncExecResult::Completed;
}

bool FuncNode::startForwarding(TimePoint time, const Values& inputValues, const Values& outputValues)
{
    using roxal::isSignal;
    using roxal::asSignal;

    std::vector<FuncYieldState::Forward> forwards;
    if (m_outputs.size() > 1 && outputValues.size() == 1 && isSignal(outputValues[0])) {
        // One signal for several ports: its values are lists, split per port.
        forwards.push_back({outputValues[0], -1});
    } else {
        for (size_t i = 0; i < outputValues.size(); ++i)
            if (isSignal(outputValues[i]))
                forwards.push_back({outputValues[i], static_cast<int>(i)});
        if (forwards.empty())
            return false;
        if (outputValues.size() != m_outputs.size())
            throw std::runtime_error("FuncNode '"+name()+"' returned "+std::to_string(outputValues.size())+" values, expected "+std::to_string(m_outputs.size())+" values");
    }

    // Each write and the stop wake the engine, which polls the node (on a
    // clocked island its ticks do).  The callbacks hold no Values.
    auto wake = DataflowEngine::instance()->asyncYieldWakeTarget();
    for (auto& f : forwards) {
        auto sig = asSignal(f.inner)->signal;
        f.writes = std::make_shared<std::atomic<uint64_t>>(0);
        f.changed = sig->subscribeValueChanged(
            [writes = f.writes, wake](TimePoint, ptr<Signal>, const roxal::Value&) {
                writes->fetch_add(1, std::memory_order_acq_rel);
                wake->futureResolved();
            });
        f.stopped = sig->subscribeStopped([wake] { wake->futureResolved(); });
    }

    auto& st = m_funcYieldState;
    st.active = true;
    st.inputValues = inputValues;
    st.executionTime = time;
    st.executionThread = nullptr;
    st.pendingOutputFutures.clear();
    st.forwards = std::move(forwards);
    previousInputValues = inputValues;

    publishForwards(time, &outputValues);

    bool running = false;
    for (const auto& f : st.forwards)
        running = running || !f.ended;
    if (!running) {
        detachForwards(false);
        st.forwards.clear();
        st.active = false;
    }
    return true;
}

bool FuncNode::publishForwards(std::optional<TimePoint> at, const Values* initialValues)
{
    using roxal::isSignal;
    using roxal::asSignal;
    auto& st = m_funcYieldState;

    // At the asking evaluation's time (a tick's), else now -- never before
    // the evaluation, nor before what was published last.
    TimePoint t = at ? *at : TimePoint::currentTime();
    if (t < st.executionTime)
        t = st.executionTime;
    if (m_lastDelivery && t < *m_lastDelivery)
        t = *m_lastDelivery;

    Values outs = previousOutputValues;
    if (outs.size() != m_outputs.size()) {
        outs.clear();
        for (size_t i = 0; i < m_outputs.size(); ++i)
            outs.push_back(initialValueForOutput(i));
    }
    bool changed = false;
    if (initialValues && initialValues->size() == m_outputs.size()) {
        for (size_t i = 0; i < initialValues->size(); ++i)
            if (!isSignal((*initialValues)[i])) {
                outs[i] = (*initialValues)[i];
                changed = true;
            }
    }

    for (auto& f : st.forwards) {
        if (f.ended)
            continue;
        auto sig = asSignal(f.inner)->signal;
        // Running first: a producer writes its last value before it stops,
        // so a value read after seeing it stopped is the last one.
        const bool running = sig->isRunning();
        const uint64_t writes = f.writes->load(std::memory_order_acquire);
        const roxal::Value value = sig->lastValue();
        const bool fresh = initialValues ? !value.isNil() : writes != f.seen;
        f.seen = writes;
        if (!running)
            f.ended = true;
        if (!fresh)
            continue;
        if (f.port >= 0) {
            outs[f.port] = value;
        } else {
            Values split = distributeReturnValue(value);
            if (split.size() != m_outputs.size()) {
                reportBodyFailure("FuncNode '" + name() + "' forwarded " + std::to_string(split.size())
                                  + " values, expected " + std::to_string(m_outputs.size()));
                for (auto& g : st.forwards)
                    g.ended = true;
                continue;
            }
            outs = std::move(split);
        }
        changed = true;
    }
    if (!changed)
        return false;

    previousOutputValues = outs;
    invokeExecutionCallbacks(t, st.inputValues, outs);
    for (size_t i = 0; i < m_outputs.size(); ++i)
        m_outputs[i].signal->setValueAt(t, outs[i]);
    m_lastDelivery = t;
    m_failureReported = false;
    m_holdReported = false;
    return true;
}

FuncExecResult FuncNode::pollForwards(TimePoint deadline, std::optional<TimePoint> at)
{
    auto& st = m_funcYieldState;
    const bool delivered = publishForwards(at);
    for (const auto& f : st.forwards)
        if (!f.ended)
            return delivered ? FuncExecResult::Completed : FuncExecResult::Yielded;

    // Every forwarded signal has stopped: the evaluation is over.  One asked
    // for meanwhile runs now, on the current inputs (held until here).
    const TimePoint time = st.executionTime;
    const std::optional<TimePoint> rerun = st.rerunTime;
    detachForwards(false);
    st.forwards.clear();
    st.active = false;
    st.rerunTime.reset();
    if (rerun && *rerun > time
        && conditionallyExecute(*rerun, deadline) == FuncExecResult::Error)
        return FuncExecResult::Error;
    return FuncExecResult::Completed;
}

void FuncNode::detachForwards(bool stopOwned)
{
    std::vector<ptr<Signal>> owned;
    for (auto& f : m_funcYieldState.forwards) {
        f.changed.cancel();
        f.stopped.cancel();
        if (stopOwned && roxal::isSignal(f.inner)) {
            auto sig = roxal::asSignal(f.inner)->signal;
            if (sig->isCallStream() && sig->isRunning())
                owned.push_back(sig);
        }
    }
    for (auto& sig : owned)
        sig->stop();
}

bool FuncNode::newInputEndsForwarding() const
{
    if (onInput() == OnInput::Restart)
        return true;
    // A call in flight is let finish.  (One that has stopped but was not
    // polled since still has its last value to publish: the poll does that,
    // and the evaluation follows it.)
    for (const auto& f : m_funcYieldState.forwards)
        if (!f.ended && roxal::asSignal(f.inner)->signal->isCallStream())
            return false;
    return true;
}

void FuncNode::endForwarding()
{
    detachForwards(/*stopOwned=*/true);
    m_funcYieldState.forwards.clear();
    m_funcYieldState.active = false;
    m_funcYieldState.rerunTime.reset();
}

FuncExecResult FuncNode::resumeExecution(TimePoint deadline, std::optional<TimePoint> at)
{
    using roxal::VM;
    using roxal::Value;
    using roxal::ExecutionStatus;
    using roxal::isFuture;
    using roxal::asFuture;

    if (!m_funcYieldState.active) {
        return FuncExecResult::Error;
    }

    // Case 0: forwarding a signal the body returned
    if (!m_funcYieldState.forwards.empty())
        return pollForwards(deadline, at);

    // Case 1: Future-based yield (async native fn like predict)
    if (!m_funcYieldState.pendingOutputFutures.empty()) {
        // Check if all futures are ready (non-blocking)
        for (auto& v : m_funcYieldState.pendingOutputFutures) {
            if (isFuture(v)) {
                auto* fut = asFuture(v);
                if (fut->future.wait_for(std::chrono::microseconds(0))
                        != std::future_status::ready)
                    return FuncExecResult::Yielded;  // still pending
            }
        }

        // All resolved — extract values
        Values outputValues;
        for (auto& v : m_funcYieldState.pendingOutputFutures) {
            if (isFuture(v))
                outputValues.push_back(asFuture(v)->asValue());
            else
                outputValues.push_back(v);
        }
        // A body that handed back ONE future for several ports (an async
        // native such as ai.nn predict on a multi-output model) delivers its
        // list only now, so the per-port split happens here rather than at
        // return time.
        if (outputValues.size() == 1 && m_outputs.size() > 1)
            outputValues = distributeReturnValue(outputValues[0]);

        TimePoint time = m_funcYieldState.executionTime;
        Values inputValues = m_funcYieldState.inputValues;
        const std::optional<TimePoint> rerun = m_funcYieldState.rerunTime;
        m_funcYieldState.active = false;
        m_funcYieldState.pendingOutputFutures.clear();
        m_funcYieldState.rerunTime.reset();

        // A failed async body resolves to an exception.  Any failure but an
        // UnavailableError fails the node as one whose body raises directly
        // does -- reported once, its outputs left at their last values, and
        // a rerun asked for meanwhile dropped with the evaluation it was
        // waiting behind.  An UnavailableError ("not now": a service
        // unreachable or too slow) HOLDS the node instead: every output keeps
        // its last value -- never some outputs and not others -- with one
        // warning per run of such failures.  The evaluation is settled but
        // delivered nothing, so the latest input still runs (the rerun) and
        // the last delivered inputs stay what a later evaluation compares
        // against: it retries.
        const Value* fatal = nullptr;
        const Value* unavailable = nullptr;
        for (const auto& v : outputValues) {
            if (!roxal::isException(v))
                continue;
            if (!isUnavailableError(v)) {
                fatal = &v;
                break;
            }
            if (!unavailable)
                unavailable = &v;
        }
        if (fatal) {
            const std::string what = "Uncaught exception: "
                + roxal::objExceptionToString(roxal::asException(*fatal));
            return reportBodyFailure(what) ? FuncExecResult::Error
                                           : FuncExecResult::NotExecuted;
        }
        if (unavailable) {
            reportHold(*unavailable);
            if (rerun && *rerun > time
                && conditionallyExecute(*rerun, deadline) == FuncExecResult::Error)
                return FuncExecResult::Error;
            return FuncExecResult::Held;
        }

        // OnInput::Restart, and the inputs changed while the call was still
        // starting (its future can't be cancelled): the stream is stopped as
        // it arrives, nothing of it published, and the evaluation waiting
        // behind it runs instead, below.
        const bool waiting = rerun && *rerun > time;
        bool superseded = false;
        if (waiting && onInput() == OnInput::Restart) {
            std::vector<ptr<Signal>> streams;
            for (const auto& v : outputValues)
                if (roxal::isSignal(v) && roxal::asSignal(v)->signal->isCallStream()
                    && roxal::asSignal(v)->signal->isRunning())
                    streams.push_back(roxal::asSignal(v)->signal);
            if (!streams.empty() && gatherInputs(*rerun) != inputValues) {
                for (auto& stream : streams)
                    stream->stop();
                superseded = true;
            }
        }

        // A future that resolved to a signal (a streaming call): forward it.
        // The evaluation goes on -- unless it was a snapshot, published and
        // done.  One asked for meanwhile is held until a call's stream ends;
        // behind plain signals it runs now, below, and takes over if its
        // inputs differ (see conditionallyExecute).
        if (superseded) {
            // nothing to publish
        } else if (startForwarding(time, inputValues, outputValues)) {
            if (m_funcYieldState.active && !(waiting && newInputEndsForwarding())) {
                m_funcYieldState.rerunTime = rerun;
                return FuncExecResult::Completed;
            }
        } else {
            if (outputValues.size() != m_outputs.size())
                throw std::runtime_error("FuncNode '"+name()+"' returned "+std::to_string(outputValues.size())+" values, expected "+std::to_string(m_outputs.size())+" values");

            previousOutputValues = outputValues;
            previousInputValues = inputValues;
            invokeExecutionCallbacks(time, inputValues, outputValues);

            int index = 0;
            for (const auto& output : m_outputs) {
                output.signal->setValueAt(time, outputValues[index]);
                index++;
            }
            m_lastDelivery = time;
            m_failureReported = false;
            m_holdReported = false;
        }

        // An evaluation requested while the futures were pending: run it
        // now on the current inputs (the pure gate skips it when they are
        // unchanged).  It typically yields anew -- hasPendingFutureYield()
        // tells the caller -- but the resumed evaluation itself completed.
        if (rerun && *rerun > time
            && conditionallyExecute(*rerun, deadline) == FuncExecResult::Error)
            return FuncExecResult::Error;

        return FuncExecResult::Completed;
    }

    // Case 2: VM thread yield (existing code)
    auto& vm = VM::instance();

    // Switch to the saved execution thread and resume
    auto savedThread = VM::thread;
    VM::thread = m_funcYieldState.executionThread;

    DataflowBodyGuard dfGuard;
    // Continue the suspended body on the thread it left.  execute() declines
    // on its own when a collection is pending and this thread must not delay
    // it, reporting Yielded, which resumption handles like any other.
    auto [result, returnValue] = vm.hasMoreWork()
        ? vm.execute(deadline)
        : std::make_pair(ExecutionStatus::OK, Value::nilVal());
    if (vm.hasRuntimeError())
        result = ExecutionStatus::RuntimeError;
    carryOutcomeToOwner();   // whatever the result -- see conditionallyExecute

    if (roxal::isSuspended(result)) {
        // Still not complete (deadline yield or debugger pause) -- keep the
        // yield state active; resumption retains identical state.
        VM::thread = savedThread;
        return result == ExecutionStatus::Paused ? FuncExecResult::Paused
                                                 : FuncExecResult::Yielded;
    }

    // Execution completed - process outputs
    m_funcYieldState.active = false;
    m_funcYieldState.bodyModule = Value::nilVal();
    TimePoint time = m_funcYieldState.executionTime;
    Values inputValues = m_funcYieldState.inputValues;

    if (result != ExecutionStatus::OK) {
        VM::thread = savedThread;
        return FuncExecResult::Error;
    }

    VM::thread = savedThread;

    // As if it had run straight through: a future it returned (an actor call,
    // predict) is kept pending, not published.
    return finishEvaluation(time, inputValues, distributeReturnValue(returnValue));
}

// Wherever a body runs -- the engine's host context (tickFor), its actor
// thread (event-driven islands, and every island standalone), or nested in
// another thread's own code -- its uncaught failure or exit() is the owning
// run's, as the user decided for host-driven evaluation and as it already is
// when the body runs in the owner's own domain (standalone, or nested in the
// owner's code: nothing to carry).  The engine discards its host context
// after a failure; the actor thread's domain is the default one, which with a
// driver attached belongs to the services -- left failed, every later
// evaluation there would bail -- so it is cleared once the failure is carried.
void FuncNode::carryOutcomeToOwner()
{
    using roxal::ExecutionDomain;
    roxal::Thread* t = roxal::VM::thread.get();
    if (!t)
        return;
    auto owner = ownerDomain();
    if (!owner || owner == t->domain)
        return;
    ExecutionDomain& here = *t->domain;
    const uint32_t raised = here.interrupts().load()
        & (ExecutionDomain::IntrRuntimeError | ExecutionDomain::IntrExit);
    if (!raised)
        return;
    auto& vm = roxal::VM::instance();
    vm.transferDomainOutcome(here, *owner);
    if (t->domain == vm.defaultDomain() && vm.embeddedDriverAttached()
        && (raised & ExecutionDomain::IntrRuntimeError)) {
        {
            std::lock_guard<std::mutex> lock(here.errorMutex);
            here.errorMessage.clear();
        }
        vm.clearDefaultDomainRuntimeError();
    }
}

void FuncNode::reportHold(const roxal::Value& exception)
{
    if (m_holdReported)
        return;
    m_holdReported = true;
    const roxal::Value message = roxal::asException(exception)->message;
    roxal::VM::emitDiagnostic(
        "FuncNode '" + name() + "' keeps its last outputs: "
        + (roxal::isString(message) ? roxal::toUTF8StdString(roxal::asStringObj(message)->s)
                                    : roxal::objExceptionToString(roxal::asException(exception))),
        roxal::OutputSeverity::Warning, "dataflow");
}

bool FuncNode::reportBodyFailure(const std::string& what)
{
    if (m_failureReported)
        return false;
    m_failureReported = true;
    if (ownerDomain() && roxal::VM::thread) {
        roxal::VM::instance().reportUncaughtInInvocation(what);
        carryOutcomeToOwner();
    } else {
        // Its run has ended: there is nothing left to fail, and raising the
        // error here would fail whichever domain this evaluation runs in.
        roxal::VM::emitDiagnostic(what, roxal::OutputSeverity::Error, "dataflow");
    }
    return true;
}

void FuncNode::abandonSuspendedExecution()
{
    if (!m_funcYieldState.active || !m_funcYieldState.executionThread)
        return;
    m_funcYieldState = FuncYieldState{};
}

void FuncNode::invokeExecutionCallbacks(TimePoint time, const Values& inputValues, const Values& outputValues)
{
    for (const auto& callback : executionCallbacks) {
        #ifdef DEBUG_BUILD
        try {
            callback(time, ptr_from_this(), inputValues, outputValues);
        } catch(const std::exception& e) {
            roxal::VM::emitDiagnostic(
                "Exception in funcnode " + name() + " callback " + e.what(),
                roxal::OutputSeverity::Error, "dataflow.callback");
        }
        #else
        try { callback(time, ptr_from_this(), inputValues, outputValues); } catch(...) {}
        #endif
    }
}

// The call's positional arguments: bound constants by parameter name, then
// the input signals' values in order. Trailing parameters that neither
// supplies are LEFT OUT rather than passed as nil, so the call applies their
// declared defaults exactly as a hand-written call with the same arguments
// would (a lifted `f(sig)` for `func f(a, t = 0.5)` used to pass t=nil).
std::vector<roxal::Value> FuncNode::assembleArgs(const Values& inputValues) const
{
    std::vector<roxal::Value> args;
    size_t sigIdx = 0;
    size_t supplied = 0;
    for (const auto& pname : paramNames) {
        auto cit = constArgs.find(pname);
        if (cit != constArgs.end()) {
            args.push_back(cit->second);
            supplied = args.size();
        } else if (sigIdx < inputValues.size()) {
            args.push_back(inputValues[sigIdx++]);
            supplied = args.size();
        } else {
            args.push_back(roxal::Value::nilVal());
        }
    }
    args.resize(supplied);
    return args;
}

Values FuncNode::operator()(const Values& inputValues)
{
    using namespace roxal;

    auto& vm = VM::instance();

    std::vector<Value> args = assembleArgs(inputValues);

    if (nativeFunc) {
        return nativeFunc(args);
    }
    DataflowBodyGuard dfGuard;
    auto result = vm.invokeClosure(roxal::asClosure(closure), args);

    return distributeReturnValue(result.second);
}
