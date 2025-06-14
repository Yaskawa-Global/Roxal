#pragma once

#include <string>

#include "Utils.h"

#include "Signal.h"


namespace df {

typedef std::vector<std::string> Names;


// transform a set of signals into another set of signals (abstract)
class Func
  : public std::enable_shared_from_this<Func>
{
public:
    Func(const std::string& name);
    virtual ~Func() {}

    template<typename FuncType, typename... Args>
    static ptr<FuncType> newFunc(const std::string& funcName, Args&&... args) {
        static_assert(std::is_base_of_v<Func, FuncType>, "FuncType must be derived from Func");

        auto func = core::make_ptr<FuncType>(funcName, std::forward<Args>(args)...);
        func->addToEngine();
        return func;
    }

    const std::string& name() const;

    virtual Names inputNames() const = 0;
    virtual Names outputNames() const = 0;

    // set an input's default value, for use if the signal to which it is connected doesn't have a value available (e.g. <0 index)
    void setInputDefault(const std::string inputName, const Value& defaultValue);

    Signals outputs() const; // in order of outputNames()

    // Indicate if the function is pure (no side effects, depends only on inputs)
    //  (the engine doesn't need to re-execute it if the inputs haven't changed)
    virtual bool isPure() const { return false; }

    // override this to execute the transformation function
    //  it should take a set of values (in inputNames() order) and return a set of values (in outputNames() order)
    virtual Values operator()(const Values& inputValues) = 0;

    void addExecutionCallback(std::function<void(TimePoint, ptr<Func>, const Values&, const Values&)> callback);

    typedef std::map<std::string, std::string> ParamMap;

    // a Func is a transformation function from inputs signals to output signals,
    //  hence, call it (which connects the input signals to the output signals via this Node)
    //    If signalsToParams is not specified, assumes signals are in order of inputNames(), otherwise
    //     it will map the provided signal's names to the input names via the provided mapping
    //     The source signal name may be appended with a [n] to specify n periods of the signal delay in the input signal
    //  Returns Signals in order of outputNames()
    virtual Signals operator()(const Signals& signals, const std::optional<ParamMap>& signalsToParams = std::nullopt);


protected:
    std::string m_name;

    bool m_operatorSignalsCalled; // true if operator(Signals) has been called

    // Structures to represent inputs and outputs with latency
    struct InputPort {
        std::string name;
        ptr<Signal> signal;
        // latency added to input signal (multiple of signal's period: -1 -> one signal period ago, -2 -> two signal periods ago, etc.)
        int index;
        std::optional<Value> defaultValue;  // value to use for input port if signal has no value available
        TimePoint latestAvailableTime; // Latest time when input is available
    };

    struct OutputPort {
        std::string name;
        ptr<Signal> signal;
    };

    // Input and output ports
    std::vector<InputPort> m_inputs;
    std::vector<OutputPort> m_outputs;


    //
    // Add input and output ports

    // index=-1 -> previous period's value
    // defaultValue -> value to use if the output signal connected to this input doesn't have any value yet (e.g. for index<0)
    void addInput(const std::string& name, ptr<Signal> signal, int index = 0, std::optional<Value> defaultValue = std::nullopt);

    void addOutput(const std::string& name, ptr<Signal> signal);


    // create default output signals based on output names
    void createOutputSignals(double freq);

    // update freq of all output signals
    void updateOutputSignals(double freq);

    // For pure functions, store previous inputs and outputs
    Values previousInputValues;
    Values previousOutputValues;

    void addToEngine();

    bool inputsAvailableAt(TimePoint time) const;

    // period this function will be executed at, as computed from the periods of its input signals (in precomputeFuncPeriods())
    TimeDuration m_period;


    // gather inputs from signals we consume, and execute the function if they have changed,
    //  or we're impure
    // if executed, update output signal values
    //  return true, if executed
    bool conditionallyExecute(TimePoint time);

    void invokeExecutionCallbacks(TimePoint time, const Values& inputValues, const Values& outputValues);

    std::vector<std::function<void(TimePoint, ptr<Func>,const Values&, const Values&)>> executionCallbacks;

    friend class DataflowEngine;
};


std::string funcNames(const std::vector<ptr<Func>>& funcs);




//
// Some built-in convenience functions

// Takes a single Vector input and splits it into a set individual output values
class Split : public Func
{
public:
    Split(const std::string& name, int dimension);

    virtual Names inputNames() const { return {"input"}; }
    virtual Names outputNames() const;

    using Func::operator();

    virtual Values operator()(const Values& inputValues) override;

    bool isPure() const override { return true; }

protected:
    int m_dim;
};


// takes a set of input Values (of same type) and concatenates them into a vector value
class Join : public Func
{
public:
    Join(const std::string& name, int dimension);

    virtual Names inputNames() const;
    virtual Names outputNames() const { return {"output"}; }

    using Func::operator();

    virtual Values operator()(const Values& inputValues) override;

    bool isPure() const override { return true; }

protected:
    int m_dim;
};


// Add two values (must be compatible types, like both numberic or both vectors of the same dimension)
class Add : public Func
{
public:
    Add(const std::string& name);

    virtual Names inputNames() const { return {"lhs", "rhs"}; };
    virtual Names outputNames() const { return {"sum"}; }

    using Func::operator();

    virtual Values operator()(const Values& inputValues) override;

    bool isPure() const override { return true; }
};


// Subtract two values (must be compatible types, like both numeric or both vectors of the same dimension)
class Subtract : public Func
{
public:
    Subtract(const std::string& name);

    virtual Names inputNames() const { return {"lhs", "rhs"}; };
    virtual Names outputNames() const { return {"difference"}; }

    using Func::operator();

    virtual Values operator()(const Values& inputValues) override;

    bool isPure() const override { return true; }
};



// Multiply two values (must be compatible types, like both numeric or a number and a vector)
class Multiply : public Func
{
public:
    Multiply(const std::string& name);

    virtual Names inputNames() const { return {"lhs", "rhs"}; };
    virtual Names outputNames() const { return {"product"}; }

    using Func::operator();

    virtual Values operator()(const Values& inputValues) override;

    bool isPure() const override { return true; }
};




} // namespace df
