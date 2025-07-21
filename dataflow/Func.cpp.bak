#include "Func.h"

#include <stdexcept>
#include <iostream>
#include <regex>

#include "core/common.h"

#include "DataflowEngine.h"


using namespace df;



Func::Func(const std::string& name)
    : m_name(name), m_operatorSignalsCalled(false)
{
}


const std::string& Func::name() const
{
    return m_name;
}


void Func::addInput(const std::string& name, ptr<Signal> signal, int index, std::optional<Value> defaultValue)
{
    if (index > 0)
        throw std::invalid_argument("Func '"+name+"' addInput() index must be 0 or negative (index=-1 -> previous period's value)");

    InputPort inputPort = {name, signal, index, defaultValue, TimePoint()};
    m_inputs.push_back(inputPort);
}

void Func::addOutput(const std::string& name, ptr<Signal> signal)
{
    OutputPort outputPort = {name, signal};
    m_outputs.push_back(outputPort);
}


void Func::setInputDefault(const std::string inputName, const Value& defaultValue)
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
        throw std::invalid_argument("Func '"+name()+"' setInputDefault() input '"+inputName+"' not found");
}


Signals Func::outputs() const
{
    if (m_outputs.empty())
        const_cast<Func*>(this)->createOutputSignals(1.0); // default to 1Hz - will be updated once inputs are added
    std::vector<ptr<Signal>> outputSignals;
    for (auto& output : m_outputs)
        outputSignals.push_back(output.signal);
    return outputSignals;
}




void Func::addExecutionCallback(std::function<void(TimePoint, ptr<Func>, const Values&, const Values&)> callback)
{
    executionCallbacks.push_back(callback);
}


Signals Func::operator()(const Signals& signals, const std::optional<ParamMap>& signalsToParams)
{
    if (m_operatorSignalsCalled)
        throw std::runtime_error("Func::operator(signals) called twice - func '"+name()+"'");

    if (signals.size() != inputNames().size())
        throw std::invalid_argument("Func '"+name()+"' requires "+std::to_string(inputNames().size())+" input signals, but got "+std::to_string(signals.size())+" signals");

    if (signalsToParams.has_value()) {
        if (signalsToParams.value().size() != inputNames().size())
            throw std::invalid_argument("Func '"+name()+"' requires "+std::to_string(inputNames().size())+" input signals, but map has "+std::to_string(signals.size())+" signals");

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
                        throw std::invalid_argument("Func '"+name()+"' input signal '"+inputSignalName+"' not found in signals: "+roxal::join(inputNames(), ", "));
                    addInput(inputName, inputSignal, index);
                    break;
                }
            }
        }
        if (m_inputs.size() != inputNames().size())
            throw std::invalid_argument("Func '"+name()+"' not all inputs supplied");

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


void Func::createOutputSignals(double freq)
{
    m_outputs.clear();
    // create output signals(freq may be updated as inputs added)
    for(auto& outputName : outputNames()) {
        auto outputSignal = Signal::newSignal(freq, Value(), outputName);
        addOutput(outputName, outputSignal);
    }
}


void Func::updateOutputSignals(double freq)
{
    for(auto& output : m_outputs)
        output.signal->setFrequency(freq);
}



void Func::addToEngine()
{
    DataflowEngine::instance()->addFunc(shared_from_this());
}


bool Func::inputsAvailableAt(TimePoint time) const
{
    bool inputsAvailable = true;
    for (const auto& input : m_inputs) {
        TimeDuration latency = input.signal->period() * -input.index;
        auto timeNeeded = time - latency;
        // input value isn't available if it isn't valid yet for the time we need it, or it is nil (never updated - at start of run) and has no default
        if (((input.latestAvailableTime < timeNeeded) || input.signal->valueAt(timeNeeded).isNil()) && !input.defaultValue.has_value() ) {
            inputsAvailable = false;
            // std::cout << "Func " << name() << " input " << input.name << "[" << input.index << "] (" << input.signal->name() <<") not available "
            //           << " at time:" << time.humanString() << " needed:" << timeNeeded.humanString() << " latestAvailableTime:" << input.latestAvailableTime.humanString()
            //           << " output is null? " << input.signal->valueAt(timeNeeded).isNil() << " has default? " << input.defaultValue.has_value()
            //           << std::endl;
            break;
        }
    }
    //std::cout << "Func " << name() << " inputsAvailable: " << inputsAvailable << std::endl;
    return inputsAvailable;
}


bool Func::conditionallyExecute(TimePoint time)
{
    if (!m_operatorSignalsCalled && !inputNames().empty())
        throw std::runtime_error("Func '"+name()+"' not all inputs connected");

    #ifdef DEBUG_BUILD
    if (!inputsAvailableAt(time))
        throw std::runtime_error("Func '"+name()+"' inputs not available at time "+time.humanString());
    #endif


    Values inputValues;
    for (const auto& input : m_inputs) {
        TimeDuration latency = input.signal->period() * -input.index;
        auto inputTime = time - latency;
        auto inputValue = input.signal->valueIfAvailableAt(inputTime);
        if (inputValue.has_value() || input.defaultValue.has_value()) {
            // std::cout << "conditionally execute " << name() << " at " << time.humanString() << " with input " << input.name << "[" << input.index << "] (" << input.signal->name() <<") "
            //           << "value? " << (inputValue.has_value() ? "yes" : "no") << inputValue.value() << " default? " << (input.defaultValue.has_value() ? "yes" : "no")
            //           << "  " << (inputValue.has_value() ? inputValue.value() : (input.defaultValue.has_value() ? input.defaultValue.value() : Value())) << std::endl;
            if (inputValue.has_value() && !inputValue.value().isNil())
                inputValues.push_back(inputValue.value());
            else
                inputValues.push_back(input.defaultValue.value());
        }
        else
            throw std::runtime_error("Func '"+name()+"' signal '"+input.signal->name()+"' not available at time "+inputTime.humanString());
    }

    // FIXME: if not pure, don't execute every time, just on our expected period (GCD)
    // NB: don't compare inputs if unnecessary (expensive)
    bool execute = !isPure() || (previousInputValues != inputValues);

    if (execute) {

        #if 0
        std::cout << "executing " << name() << " at " << time.humanString() << " with inputs " << join(inputValues) << std::endl;
        #endif
        Values outputValues;
        try {
          outputValues = operator()(inputValues);
        } catch (std::exception& e) {
            throw std::runtime_error("Func '"+name()+"' operator() threw exception: "+std::string(e.what()));
        }
        if (outputValues.size() != m_outputs.size())
            throw std::runtime_error("Func '"+name()+"' operator returned "+std::to_string(outputValues.size())+" values, expected "+std::to_string(m_outputs.size())+" values");
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



void Func::invokeExecutionCallbacks(TimePoint time, const Values& inputValues, const Values& outputValues)
{
    for (const auto& callback : executionCallbacks) {
        #ifdef DEBUG_BUILD
        try {
            callback(time, shared_from_this(), inputValues, outputValues);
        } catch(const std::exception& e) {
            std::cerr << "Exception in func "+name()+" callback " << e.what() << std::endl;
        }
        #else
        try { callback(time, shared_from_this(), inputValues, outputValues); } catch(...) {}
        #endif
    }
}


std::string funcNames(const std::vector<ptr<Func>>& funcs)
{
    std::vector<std::string> names;
    names.reserve(funcs.size());
    for (auto& func : funcs)
        names.push_back(func->name());
    return roxal::join(names);
}




//
// Convenience Funcs


// Split

Split::Split(const std::string& name, int dimension)
  : Func(name), m_dim(dimension)
{}

Names Split::outputNames() const
{
    Names outNames {};
    for (int i = 0; i < m_dim; i++)
        outNames.push_back("output"+std::to_string(i));
    return outNames;
}


Values Split::operator()(const Values& inputValues)
{
    if (inputValues.size() != 1)
        throw std::runtime_error("Split "+name()+": expected 1 input value, got "+std::to_string(inputValues.size()));
    if (!roxal::isVector(inputValues[0]))
        throw std::runtime_error("Split "+name()+": input value is not a vector");

    const auto& inVec = roxal::asVector(inputValues[0])->vec;

    Values outValues {};
    for (int i = 0; i < m_dim; i++)
        outValues.push_back(Value(inVec(i)));

    return outValues;
}



// Join

Join::Join(const std::string& name, int dimension)
  : Func(name), m_dim(dimension)
{}

Names Join::inputNames() const
{
    Names outNames {};
    for (int i = 0; i < m_dim; i++)
        outNames.push_back("input"+std::to_string(i));
    return outNames;
}


Values Join::operator()(const Values& inputValues)
{
    if (inputValues.size() != m_dim)
        throw std::runtime_error("Join "+name()+": expected "+std::to_string(m_dim)+" input values, got "+std::to_string(inputValues.size()));

    Eigen::VectorXd outelts(m_dim);
    for (int i = 0; i < m_dim; i++)
        outelts(i) = inputValues.at(i).asReal();

    return {Value(roxal::vectorVal(outelts))};
}



// Add

Add::Add(const std::string& name)
  : Func(name)
{}


Values Add::operator()(const Values& inputValues)
{
    if (inputValues.size() != 2)
        throw std::runtime_error("Add "+name()+": expected 2 input values, got "+std::to_string(inputValues.size()));

    const auto& lhs { inputValues.at(0) };
    const auto& rhs { inputValues.at(1) };

    if (lhs.isNil() || rhs.isNil())
        throw std::runtime_error("Add "+name()+": input value is nil");

    if (lhs.isNumber() && rhs.isNumber()) {
        if (lhs.isInt() && rhs.isInt()) {
            return {Value(lhs.asInt() + rhs.asInt())};
        }
        return {Value(lhs.asReal() + rhs.asReal())};
    }
    else if (roxal::isVector(lhs) && roxal::isVector(rhs)) {
        if (df::vectorSize(lhs) != df::vectorSize(rhs))
            throw std::runtime_error("Add"+name()+": input vectors are not the same size");
        return {vecAdd(lhs, rhs)};
    }
    else
        throw std::runtime_error("Add "+name()+": input values are not numbers or vectors");
}



// Subtract

Subtract::Subtract(const std::string& name)
  : Func(name)
{}


Values Subtract::operator()(const Values& inputValues)
{
    if (inputValues.size() != 2)
        throw std::runtime_error("Subtract "+name()+": expected 2 input values, got "+std::to_string(inputValues.size()));

    const auto& lhs { inputValues.at(0) };
    const auto& rhs { inputValues.at(1) };

    if (lhs.isNil() || rhs.isNil())
        throw std::runtime_error("Subtract "+name()+": input value is nil");

    if (lhs.isNumber() && rhs.isNumber()) {
        if (lhs.isInt() && rhs.isInt()) {
            return {Value(lhs.asInt() - rhs.asInt())};
        }
        return {Value(lhs.asReal() - rhs.asReal())};
    }
    else if (roxal::isVector(lhs) && roxal::isVector(rhs)) {
        if (df::vectorSize(lhs) != df::vectorSize(rhs))
            throw std::runtime_error("Subtract"+name()+": input vectors are not the same size");
        return {vecSub(lhs, rhs)};
    }
    else
        throw std::runtime_error("Subtract "+name()+": input values are not numbers or vectors");
}




// Multiply

Multiply::Multiply(const std::string& name)
  : Func(name)
{}


Values Multiply::operator()(const Values& inputValues)
{
    if (inputValues.size() != 2)
        throw std::runtime_error("Multiply "+name()+": expected 2 input values, got "+std::to_string(inputValues.size()));

    const auto& lhs { inputValues.at(0) };
    const auto& rhs { inputValues.at(1) };

    if (lhs.isNil() || rhs.isNil())
        throw std::runtime_error("Multiply "+name()+": input value is nil");

    if (lhs.isNumber() && rhs.isNumber()) {
        if (lhs.isInt() && rhs.isInt()) {
            return {Value(lhs.asInt() * rhs.asInt())};
        }
        return {Value(lhs.asReal() * rhs.asReal())};
    }
    else if (lhs.isNumber() && roxal::isVector(rhs)) {
        if (lhs.isInt())
            return {vecMult(lhs.asInt(), rhs)};
        return {vecMult(lhs.asReal(), rhs)};
    }
    else if (roxal::isVector(lhs) && rhs.isNumber()) {
        if (rhs.isInt())
            return {vecMult(rhs.asInt(), lhs)};
        return {vecMult(rhs.asReal(), lhs)};
    }
    else
        throw std::runtime_error("Multiply "+name()+": input values are not numbers or a number and a vector");
}
