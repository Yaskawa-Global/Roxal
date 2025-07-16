#include <time.h>
#include <algorithm>
#include <numeric>
#include <iterator>
#include <cmath>

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

ptr<Signal> Signal::newSourceSignal(double freq, Value initial, std::optional<std::string> name)
{
    auto s = std::shared_ptr<Signal>(new Signal(freq, initial, name));
    s->isClock = false;
    s->isSource = true;
    DataflowEngine::instance()->addSignal(s);
    return s;
}


Signal::Signal(double freq, Value initial, std::optional<std::string> name)
    : m_frequency(freq), m_maxHistoryPeriods(2)
{
    m_name = name.value_or("source_signal");
    assert(freq > 0.0);
    double period_us = 1000000.0 / m_frequency;
    double period_round = std::round(period_us);
    if (period_us < 1.0 || std::fabs(period_us - period_round) > 1e-9) {
        throw std::invalid_argument(
            "clock frequency " + std::to_string(freq) +
            " not representable as whole microseconds");
    }
    m_period = TimeDuration::microSecs(static_cast<int64_t>(period_round));
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
    double period_us = 1000000.0 / m_frequency;
    double period_round = std::round(period_us);
    if (period_us < 1.0 || std::fabs(period_us - period_round) > 1e-9) {
        throw std::invalid_argument(
            "clock frequency " + std::to_string(freq) +
            " not representable as whole microseconds");
    }
    m_period = TimeDuration::microSecs(static_cast<int64_t>(period_round));
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
#ifdef DEBUG_BUILD
    if (age % m_period != TimeDuration::zero()) {
        std::cout << "setValueAt Signal " + name() + " for time " + t.humanString() +
            " not a multiple of period " + m_period.humanString() << std::endl;
    }
#endif

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

void Signal::set(const Value& v)
{
    TimePoint t = DataflowEngine::instance()->tickStart();
    setValueAt(t, v);
    DataflowEngine::instance()->updateSignalConsumerInputAvailability(shared_from_this(), t);
}


void Signal::tick(TimePoint t)
{
    if (isClock) {
        Value val;
        if (running || tickPending) {
            ++clockCount;
            val = Value{int32_t(clockCount)}; // FIXME: !!! 64->31 bits
            tickPending = false;
        } else {
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

Value Signal::valueAtIndex(int index) const
{
    if (index > 0)
        throw std::invalid_argument("Signal index must be 0 or negative");

    if (values.empty())
        throw std::runtime_error("Signal has no values.");

    int stepsBack = -index;

    TimePoint lastTime = values.rbegin()->first;
    TimePoint t = lastTime - m_period * stepsBack;
    // If t predates the earliest recorded value, return the
    // initial value instead of throwing an exception.
    if (t < values.begin()->first)
        return values.begin()->second;

    return valueAt(t);
}

ptr<Signal> Signal::indexedSignal(int index)
{
    if (index > 0)
        throw std::invalid_argument("Signal index must be 0 or negative");

    if (index == 0)
        return shared_from_this();

    Value initial;
    try {
        initial = valueAtIndex(index);
    } catch(...) {
        initial = Value();
    }

    // Create a new signal that mirrors this one but with a time delay.
    // The old standalone DataflowEngine supported latency by storing the
    // desired index on FuncInputInfo.  Here we emulate that behaviour by
    // generating a separate Signal updated whenever the source updates.
    auto newSig = std::shared_ptr<Signal>(new Signal(m_frequency, initial, m_name + "[" + std::to_string(index) + "]"));
    // Indexed signals are derived from an existing signal and are neither
    // clocks nor source signals themselves.
    newSig->isClock = false;
    newSig->isSource = false;
    newSig->setMaxHistoryPeriods(std::max(m_maxHistoryPeriods, -index + 1));

    std::weak_ptr<Signal> weakNew = newSig;
    addValueChangedCallback([weakNew, index](TimePoint t, ptr<Signal> src, const Value& v){
        if (auto s = weakNew.lock()) {
            try {
                Value val = src->valueAtIndex(index);
                s->setValueAt(t, val);
            } catch(...) {
                s->setValueAt(t, Value());
            }

            // update availability even if value didn't change
            DataflowEngine::instance()->updateSignalConsumerInputAvailability(s, t);
        }
    });

    return newSig;
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
