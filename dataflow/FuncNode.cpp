#include "FuncNode.h"

#include "core/common.h"
#include <algorithm>
#include "compiler/VM.h"
#include "DataflowEngine.h"
#include <stdexcept>
#include <iostream>
#include <regex>

using namespace df;

FuncNode::FuncNode(const std::string& name,
                   const roxal::Value& closure_,
                   const ConstArgMap& constArgs_,
                   const std::vector<ptr<Signal>>& signalArgs_,
                   const std::vector<ptr<Signal>>& outputSignals)
  : m_name(name), m_operatorSignalsCalled(false), closure(closure_), constArgs(constArgs_), signalArgs(signalArgs_), m_overrideOutputSignals(outputSignals)
{
    m_outputNames = {DataflowEngine::uniqueFuncName("result")};
    if (roxal::isClosure(closure) && roxal::asClosure(closure)->function->funcType.has_value()) {
        auto funcTypePtr = roxal::asClosure(closure)->function->funcType.value();
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

    InputPort inputPort = {name, signal, index, defaultValue, TimePoint()};
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
    if (!m_overrideOutputSignals.empty()) {
        size_t count = std::min(m_overrideOutputSignals.size(), outputNames().size());
        for(size_t i=0;i<count;++i) {
            auto sig = m_overrideOutputSignals[i];
            sig->setFrequency(freq);
            addOutput(outputNames()[i], sig);
        }
        for(size_t i=count;i<outputNames().size();++i) {
            auto outputSignal = Signal::newSignal(freq, roxal::Value(), outputNames()[i]);
            addOutput(outputNames()[i], outputSignal);
        }
        m_overrideOutputSignals.clear();
    } else {
        // create output signals(freq may be updated as inputs added)
        for(auto& outName : outputNames()) {
            auto outputSignal = Signal::newSignal(freq, roxal::Value(), outName);
            addOutput(outName, outputSignal);
        }
    }
}

void FuncNode::updateOutputSignals(double freq)
{
    for(auto& output : m_outputs)
        output.signal->setFrequency(freq);
}

void FuncNode::addToEngine()
{
    DataflowEngine::instance()->addFunc(shared_from_this());
}

bool FuncNode::inputsAvailableAt(TimePoint time) const
{
    bool inputsAvailable = true;
    for (const auto& input : m_inputs) {
        TimeDuration latency = input.signal->period() * -input.index;
        auto timeNeeded = time - latency;
        // input value isn't available if it isn't valid yet for the time we need it, or it is nil (never updated - at start of run) and has no default
        if (((input.latestAvailableTime < timeNeeded) || input.signal->valueAt(timeNeeded).isNil()) && !input.defaultValue.has_value() ) {
            inputsAvailable = false;
            break;
        }
    }
    return inputsAvailable;
}

bool FuncNode::conditionallyExecute(TimePoint time)
{
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
        if (inputValue.has_value() || input.defaultValue.has_value()) {
            if (inputValue.has_value() && !inputValue.value().isNil())
                inputValues.push_back(inputValue.value());
            else
                inputValues.push_back(input.defaultValue.value());
        }
        else
            throw std::runtime_error("FuncNode '"+name()+"' signal '"+input.signal->name()+"' not available at time "+inputTime.humanString());
    }

    // FIXME: if not pure, don't execute every time, just on our expected period (GCD)
    // NB: don't compare inputs if unnecessary (expensive)
    bool execute = !isPure() || (previousInputValues != inputValues);

    if (execute) {
        Values outputValues;
        try {
          outputValues = operator()(inputValues);
        } catch (std::exception& e) {
            throw std::runtime_error("FuncNode '"+name()+"' operator() threw exception: "+std::string(e.what()));
        }
        if (outputValues.size() != m_outputs.size())
            throw std::runtime_error("FuncNode '"+name()+"' operator returned "+std::to_string(outputValues.size())+" values, expected "+std::to_string(m_outputs.size())+" values");
        previousOutputValues = outputValues;
        invokeExecutionCallbacks(time, inputValues, outputValues);

        // now update the outputs
        int index = 0;
        for (const auto& output : m_outputs) {
            output.signal->setValueAt(time, outputValues[index]);
            index++;
        }
    }
    previousInputValues = inputValues;

    return execute;
}

void FuncNode::invokeExecutionCallbacks(TimePoint time, const Values& inputValues, const Values& outputValues)
{
    for (const auto& callback : executionCallbacks) {
        #ifdef DEBUG_BUILD
        try {
            callback(time, shared_from_this(), inputValues, outputValues);
        } catch(const std::exception& e) {
            std::cerr << "Exception in funcnode "+name()+" callback " << e.what() << std::endl;
        }
        #else
        try { callback(time, shared_from_this(), inputValues, outputValues); } catch(...) {}
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
    auto result = vm.callAndExec(roxal::asClosure(closure), args);

    if (!m_outputNames.empty() && m_outputNames.size() > 1) {
        if (roxal::isList(result.second)) {
            auto list = roxal::asList(result.second);
            Values outs;
            outs.reserve(m_outputNames.size());
            for (size_t i = 0; i < m_outputNames.size(); ++i) {
                if (i < list->elts.size())
                    outs.push_back(list->elts.at(i));
                else
                    outs.push_back(Value::nilVal());
            }
            return outs;
        }
    }

    return { result.second };
}
