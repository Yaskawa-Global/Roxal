#pragma once

#include "Signal.h"
#include "compiler/Object.h"
#include "compiler/Value.h"
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <functional>
#include <memory>

namespace df {

typedef std::vector<std::string> Names;

class FuncNode : public std::enable_shared_from_this<FuncNode>
{
public:
    using ConstArgMap = std::map<std::string, roxal::Value>;
    typedef std::map<std::string, std::string> ParamMap;

    FuncNode(const std::string& name,
             const roxal::Value& closure,
             const ConstArgMap& constArgs,
             const std::vector<ptr<Signal>>& signalArgs);

    using NativeFunc = std::function<Values(const Values&)>;

    FuncNode(const std::string& name,
             NativeFunc nativeFunc,
             const std::vector<std::string>& paramNames,
             const ConstArgMap& constArgs,
             const std::vector<ptr<Signal>>& signalArgs,
             const Names& outputNames = {"result"});

    virtual ~FuncNode() {}

    const std::string& name() const;
    
    Names inputNames() const { return m_inputNames; }
    Names outputNames() const { return m_outputNames.empty() ? Names{"result"} : m_outputNames; }
    bool isPure() const { return true; }
    
    // Core execution method
    virtual Values operator()(const Values& inputValues);

    // Signal connection method
    virtual Signals operator()(const Signals& signals, const std::optional<ParamMap>& signalsToParams = std::nullopt);

    // set an input's default value, for use if the signal to which it is connected doesn't have a value available (e.g. <0 index)
    void setInputDefault(const std::string inputName, const roxal::Value& defaultValue);

    Signals outputs() const; // in order of outputNames()

    void addExecutionCallback(std::function<void(TimePoint, ptr<FuncNode>, const Values&, const Values&)> callback);

    void addToEngine();

    roxal::Value closure;
    NativeFunc nativeFunc;
    ConstArgMap constArgs;
    std::vector<ptr<Signal>> signalArgs;

    // parameter names in order of declaration
    std::vector<std::string> paramNames;
    // index of signal argument for each param (-1 if constant)
    std::vector<int> paramSignalIndex;

protected:
    std::string m_name;
    bool m_operatorSignalsCalled; // true if operator(Signals) has been called

    // Structures to represent inputs and outputs with latency
    struct InputPort {
        std::string name;
        ptr<Signal> signal;
        // latency added to input signal (multiple of signal's period: -1 -> one signal period ago, -2 -> two signal periods ago, etc.)
        int index;
        std::optional<roxal::Value> defaultValue;  // value to use for input port if signal has no value available
        TimePoint latestAvailableTime; // Latest time when input is available
    };

    struct OutputPort {
        std::string name;
        ptr<Signal> signal;
    };

    // Input and output ports
    std::vector<InputPort> m_inputs;
    std::vector<OutputPort> m_outputs;
    Names m_outputNames;

    // For pure functions, store previous inputs and outputs
    Values previousInputValues;
    Values previousOutputValues;

    // period this function will be executed at, as computed from the periods of its input signals (in precomputeFuncPeriods())
    TimeDuration m_period;

    std::vector<std::function<void(TimePoint, ptr<FuncNode>, const Values&, const Values&)>> executionCallbacks;

    // Add input and output ports
    // index=-1 -> previous period's value
    // defaultValue -> value to use if the output signal connected to this input doesn't have any value yet (e.g. for index<0)
    void addInput(const std::string& name, ptr<Signal> signal, int index = 0, std::optional<roxal::Value> defaultValue = std::nullopt);

    void addOutput(const std::string& name, ptr<Signal> signal);

    // create default output signals based on output names
    void createOutputSignals(double freq);

    // update freq of all output signals
    void updateOutputSignals(double freq);

    bool inputsAvailableAt(TimePoint time) const;

    // gather inputs from signals we consume, and execute the function if they have changed,
    //  or we're impure
    // if executed, update output signal values
    //  return true, if executed
    bool conditionallyExecute(TimePoint time);

    void invokeExecutionCallbacks(TimePoint time, const Values& inputValues, const Values& outputValues);

private:
    Names m_inputNames;

    friend class DataflowEngine;
};

}
