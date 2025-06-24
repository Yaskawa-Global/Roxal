#include <time.h>
#include <algorithm>
#include <numeric>
#include <iterator>

#include "Signal.h"
#include "DataflowEngine.h"
#include <stdexcept>
#include <iostream>

using namespace df;


ptr<Signal> Signal::newClockSignal(double freq, std::optional<std::string> name)
{
    auto s = std::shared_ptr<Signal>(new Signal(freq, Value(0), name)); // direct new needed
    s->isClock = true;
    s->isSource = true;
    s->clockCount = 0;
    DataflowEngine::instance()->addSignal(s);
    return s;
}


ptr<Signal> Signal::newSignal(double freq, Value initial, std::optional<std::string> name)
{
    auto s = std::shared_ptr<Signal>(new Signal(freq, initial, name));
    s->isClock = false;
    s->isSource = false;
    DataflowEngine::instance()->addSignal(s);
    return s;
}


Signal::Signal(double freq, Value initial, std::optional<std::string> name)
    : m_frequency(freq), m_maxHistoryPeriods(1)
{
    m_name = name.value_or("source_signal");
    assert(freq > 0.0);
    m_period = TimeDuration::microSecs(int64_t(1000000ULL / m_frequency));
    values[TimePoint::zero()] = initial;
}


void Signal::addValueChangedCallback(std::function<void(TimePoint, ptr<Signal>, const Value&)> callback)
{
    valueChangedCallbacks.push_back(callback);
}


void Signal::invokeValueChangedCallbacks(TimePoint t, const Value& v)
{
    for (auto& callback : valueChangedCallbacks) {
        #ifdef DEBUG_BUILD
        try {
            callback(t, shared_from_this(), v);
        } catch(const std::exception& e) {
            std::cerr << "Exception in signal "+name()+" callback " << e.what() << std::endl;
        }
        #else
        try {
            callback(t, shared_from_this(), v);
        } catch(...) {}
        #endif
    }
}



Signal::~Signal()
{
}


void Signal::setFrequency(double freq)
{
    m_frequency = freq;
    m_period = TimeDuration::microSecs(int64_t(1000000ULL / m_frequency));
}


std::optional<Value> Signal::valueIfAvailableAt(TimePoint t) const
{
    auto it = values.find(t);
    if (it != values.end())
        return it->second;
    else
        return std::nullopt;
}


Value Signal::valueAt(TimePoint t) const
{
    #if 0
    // check if the requested time is a multiple of the history
    auto age = t - DataflowEngine::instance()->tickStart();
    if (age % m_period != TimeDuration::zero())
        std::cout << "valueAt Signal " + name() + " for time " + t.humanString() + " not a multiple of period " + m_period.humanString() << std::endl;
    #endif

    auto it = values.find(t);
    if (it != values.end()) {
        return it->second;
    } else {
        // If exact time not found, find the last value before t
        auto it_before = values.upper_bound(t);
        if (it_before != values.begin()) {
            --it_before;
            return it_before->second;
        } else {
            throw std::runtime_error("Value not available at time on signal " + m_name);
        }
    }
}

void Signal::setValueAt(TimePoint t, const Value& v)
{
    auto age = t - DataflowEngine::instance()->tickStart();
    if (age % m_period != TimeDuration::zero()) {
        std::cout << "setValueAt Signal " + name() + " for time " + t.humanString() + " not a multiple of period " + m_period.humanString() << std::endl;
    }

    assert(!values.empty());
    bool valueChanged = (lastValueBefore(t) != v);

    // if we're adding a newer time, remove the oldest
    if (t > values.rbegin()->first) {
        if (values.size() >= m_maxHistoryPeriods)
            values.erase(values.begin()); // map keys are sorted by time, so remove oldest/smallest value
    }

    values[t] = v;

    if (valueChanged)
        invokeValueChangedCallbacks(t, v);
}


void Signal::tick(TimePoint t)
{
    if (isClock) {
        Value val;
        if (running) {
            // For clock signals, generate a new incrementing value
            ++clockCount;
            val = Value{int32_t(clockCount)}; // FIXME: !!! 64->31 bits
        } else {
            // Not running - repeat last value
            val = lastValue();
        }
        setValueAt(t, val);
    }
}


void Signal::evaluate(TimePoint t)
{
    if (isClock) {
        // For clock signals, set initial value without advancing clockCount
        // Use current clockCount value (starts at 0)
        Value val {int32_t(clockCount)};
        setValueAt(t, val);
    }
}



Value Signal::lastValue() const
{
    if (values.empty())
        throw std::runtime_error("Signal has no values.");
    return values.rbegin()->second;
}



Value Signal::lastValueBefore(TimePoint t) const
{
    auto it = values.lower_bound(t);

    if (it == values.begin()) {
        // The earliest time in values is not less than t
        return it->second; // Return the initial value
    }

    if (it == values.end()) {
        // All times are before t; return the last value
        --it;
        return it->second;
    }

    // Return the value just before t
    --it;
    return it->second;
}
