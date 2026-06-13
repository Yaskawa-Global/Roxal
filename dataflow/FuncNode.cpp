#include "FuncNode.h"

#include "core/common.h"
#include <algorithm>
#include "compiler/VM.h"
#include "compiler/Thread.h"
#include "DataflowEngine.h"
#include <stdexcept>
#include <iostream>
#include <regex>

using namespace df;

namespace {

// RAII guard to set/clear the dataflow thread flag on VM
struct DataflowThreadGuard {
    DataflowThreadGuard() { roxal::VM::setOnDataflowThread(true); }
    ~DataflowThreadGuard() { roxal::VM::setOnDataflowThread(false); }
};

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
  : m_name(name), m_operatorSignalsCalled(false), closure(closure_), constArgs(constArgs_), signalArgs(signalArgs_), m_overrideOutputSignals(outputSignals)
{
    m_outputNames = {DataflowEngine::uniqueFuncName("result")};
    if (roxal::isClosure(closure) && asFunction(asClosure(closure)->function)->funcType.has_value()) {
        auto funcTypePtr = asFunction(asClosure(closure)->function)->funcType.value();
        if (funcTypePtr->func.has_value()) {
            const auto& funcType = funcTypePtr->func.value();

            if (!funcType.returnTypes.empty()) {
                if (funcType.returnTypes.size() == 1)
                    m_outputNames = {DataflowEngine::uniqueFuncName("result")};
                else {
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
            // compute max input frequency
            double maxFreq = 0.0;
            for (auto& sig : signalArgs)
                maxFreq = std::max(maxFreq, sig->frequency());

            if (maxFreq <= 0.0)
                maxFreq = 1.0;

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
  : m_name(name), m_operatorSignalsCalled(false), closure(Value::nilVal()),
    nativeFunc(nativeFunc_), constArgs(constArgs_), signalArgs(signalArgs_), m_overrideOutputSignals(outputSignals)
{
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
    double maxFreq = 0.0;
    for (auto& sig : signalArgs)
        maxFreq = std::max(maxFreq, sig->frequency());
    if (maxFreq <= 0.0)
        maxFreq = 1.0;
    createOutputSignals(maxFreq);
    m_operatorSignalsCalled = true;
}

FuncNode::~FuncNode()
{
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
            addOutput(names[i], outputSignal);
        }
        m_overrideOutputSignals.clear();
    } else {
        // create output signals(freq may be updated as inputs added)
        for(size_t i=0;i<names.size();++i) {
            auto outputSignal = Signal::newSignal(freq, initialValueForOutput(i), names[i]);
            outputSignal->m_suppressInitialChange = true;
            addOutput(names[i], outputSignal);
        }
    }
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

FuncExecResult FuncNode::conditionallyExecute(TimePoint time, TimePoint deadline)
{
    using roxal::VM;
    using roxal::Value;
    using roxal::ExecutionStatus;
    using roxal::asClosure;
    using roxal::isList;
    using roxal::asList;
    using roxal::isFuture;

    // If we have a pending future-based yield (e.g. from a previous tick where
    // the non-budgeted evaluateNetwork() path didn't handle the Yielded result),
    // try to resume it instead of re-executing.
    if (m_funcYieldState.active && !m_funcYieldState.pendingOutputFutures.empty()) {
        return resumeExecution(deadline);
    }

    if (!m_operatorSignalsCalled && !inputNames().empty())
        throw std::runtime_error("FuncNode '"+name()+"' not all inputs connected");

    #ifdef DEBUG_BUILD
    if (!inputsAvailableAt(time))
        throw std::runtime_error("FuncNode '"+name()+"' inputs not available at time "+time.humanString());
    #endif

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

    // FIXME: if not pure, don't execute every time, just on our expected period (GCD)
    // NB: don't compare inputs if unnecessary (expensive)
    bool execute = !isPure() || (previousInputValues != inputValues);

    if (!execute) {
        previousInputValues = inputValues;
        return FuncExecResult::NotExecuted;
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
        std::vector<Value> args;
        size_t sigIdx = 0;
        for (const auto& pname : paramNames) {
            auto cit = constArgs.find(pname);
            if (cit != constArgs.end()) {
                args.push_back(cit->second);
            } else {
                if (sigIdx < inputValues.size())
                    args.push_back(inputValues[sigIdx++]);
                else
                    args.push_back(Value::nilVal());
            }
        }

        DataflowThreadGuard dfGuard;
        auto result = vm.invokeClosure(asClosure(closure), args, deadline);

        if (result.first == ExecutionStatus::Yielded) {
            // VM yielded due to deadline - save state for resumption
            m_funcYieldState.active = true;
            m_funcYieldState.inputValues = inputValues;
            m_funcYieldState.executionThread = VM::thread;
            m_funcYieldState.executionTime = time;
            return FuncExecResult::Yielded;
        }

        if (result.first != ExecutionStatus::OK) {
            return FuncExecResult::Error;
        }

        // Process return value into output values (same logic as operator())
        if (!m_outputNames.empty() && m_outputNames.size() > 1) {
            if (isList(result.second)) {
                auto list = asList(result.second);
                for (size_t i = 0; i < m_outputNames.size(); ++i) {
                    if (i < list->length())
                        outputValues.push_back(list->getElement(i));
                    else
                        outputValues.push_back(Value::nilVal());
                }
            } else {
                outputValues.push_back(result.second);
            }
        } else {
            outputValues.push_back(result.second);
        }
    }

    // Check if any output is a future (async native fn like predict).
    // If so, yield and let resumeExecution() poll for completion.
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

    return FuncExecResult::Completed;
}

FuncExecResult FuncNode::resumeExecution(TimePoint deadline)
{
    using roxal::VM;
    using roxal::Value;
    using roxal::ExecutionStatus;
    using roxal::isList;
    using roxal::asList;
    using roxal::isFuture;
    using roxal::asFuture;

    if (!m_funcYieldState.active) {
        return FuncExecResult::Error;
    }

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

        TimePoint time = m_funcYieldState.executionTime;
        Values inputValues = m_funcYieldState.inputValues;
        m_funcYieldState.active = false;
        m_funcYieldState.pendingOutputFutures.clear();

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

        return FuncExecResult::Completed;
    }

    // Case 2: VM thread yield (existing code)
    auto& vm = VM::instance();

    // Switch to the saved execution thread and resume
    auto savedThread = VM::thread;
    VM::thread = m_funcYieldState.executionThread;

    auto remaining = deadline - TimePoint::currentTime();
    DataflowThreadGuard dfGuard;
    auto [result, returnValue] = vm.runFor(remaining);

    if (result == ExecutionStatus::Yielded) {
        // Still not complete - keep yield state active
        VM::thread = savedThread;
        return FuncExecResult::Yielded;
    }

    // Execution completed - process outputs
    m_funcYieldState.active = false;
    TimePoint time = m_funcYieldState.executionTime;
    Values inputValues = m_funcYieldState.inputValues;

    if (result != ExecutionStatus::OK) {
        VM::thread = savedThread;
        return FuncExecResult::Error;
    }

    VM::thread = savedThread;

    // Process return value into output values
    Values outputValues;
    if (!m_outputNames.empty() && m_outputNames.size() > 1) {
        if (isList(returnValue)) {
            auto list = asList(returnValue);
            for (size_t i = 0; i < m_outputNames.size(); ++i) {
                if (i < list->length())
                    outputValues.push_back(list->getElement(i));
                else
                    outputValues.push_back(Value::nilVal());
            }
        } else {
            outputValues.push_back(returnValue);
        }
    } else {
        outputValues.push_back(returnValue);
    }

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

    return FuncExecResult::Completed;
}

void FuncNode::invokeExecutionCallbacks(TimePoint time, const Values& inputValues, const Values& outputValues)
{
    for (const auto& callback : executionCallbacks) {
        #ifdef DEBUG_BUILD
        try {
            callback(time, ptr_from_this(), inputValues, outputValues);
        } catch(const std::exception& e) {
            std::cerr << "Exception in funcnode "+name()+" callback " << e.what() << std::endl;
        }
        #else
        try { callback(time, ptr_from_this(), inputValues, outputValues); } catch(...) {}
        #endif
    }
}

Values FuncNode::operator()(const Values& inputValues)
{
    using namespace roxal;

    auto& vm = VM::instance();

    std::vector<Value> args;
    size_t sigIdx = 0;
    for (const auto& pname : paramNames) {
        auto cit = constArgs.find(pname);
        if (cit != constArgs.end()) {
            args.push_back(cit->second);
        } else {
            if (sigIdx < inputValues.size())
                args.push_back(inputValues[sigIdx++]);
            else
                args.push_back(Value::nilVal());
        }
    }

    if (nativeFunc) {
        return nativeFunc(args);
    }
    DataflowThreadGuard dfGuard;
    auto result = vm.invokeClosure(roxal::asClosure(closure), args);

    if (!m_outputNames.empty() && m_outputNames.size() > 1) {
        if (roxal::isList(result.second)) {
            auto list = roxal::asList(result.second);
            Values outs;
            outs.reserve(m_outputNames.size());
            for (size_t i = 0; i < m_outputNames.size(); ++i) {
                if (i < list->length())
                    outs.push_back(list->getElement(i));
                else
                    outs.push_back(Value::nilVal());
            }
            return outs;
        }
    }

    return { result.second };
}
