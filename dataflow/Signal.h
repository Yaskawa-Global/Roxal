#pragma once

#include <map>

#include "core/common.h"
#include "core/TimePoint.h"
#include "core/TimeDuration.h"

#include "Value.h"

namespace df {

class DataflowEngine;

using roxal::ptr;
using roxal::TimePoint;
using roxal::TimeDuration;


class Signal
   : public std::enable_shared_from_this<Signal>
{
public:
    static ptr<Signal> newClockSignal(double freq, std::optional<std::string> name = {});
    static ptr<Signal> newSignal(double freq, Value initial, std::optional<std::string> name = {});

    const std::string& name() const { return m_name; }

    ptr<Signal> rename(const std::string& newName) { m_name = newName; return shared_from_this(); }

    void addValueChangedCallback(std::function<void(TimePoint, ptr<Signal>,const Value&)> callback);

    virtual ~Signal();

    double frequency() const { return m_frequency; }
    void setFrequency(double freq);

    // Get and set value at a specific time point
    std::optional<Value> valueIfAvailableAt(TimePoint t) const;
    Value valueAt(TimePoint t) const;
    void setValueAt(TimePoint t, const Value& v);

    // For source signals: generate next value
    void tick(TimePoint t);

    // Start/stop running of source signals
    void run() { running = true; }
    void stop() { running = false; }
    
    // set initial value without advancing clock state (for evaluation)
    void evaluate(TimePoint t);

    // Get the period of the signal based on its frequency
    TimeDuration period() const { return m_period; }

    bool isSourceSignal() const { return isSource; }

    // Get the last value of the signal
    Value lastValue() const;

    // Value at a negative index relative to the most recent value
    //   index 0 -> last value
    //   index -1 -> value from one period ago
    // Throws if index > 0 or history is insufficient
    Value valueAtIndex(int index) const;

    // Return a new signal that is this signal delayed by -index periods
    //   index 0 -> this signal
    //   index -1 -> signal one period ago
    // Throws if index > 0
    ptr<Signal> indexedSignal(int index);

    // The last value before a specific time
    Value lastValueBefore(TimePoint t) const;

protected:
    Signal(double freq, Value initial, std::optional<std::string> name = {});

    void invokeValueChangedCallbacks(TimePoint t, const Value& v);

    void setMaxHistoryPeriods(int maxHistoryPeriods)
    {
        m_maxHistoryPeriods = maxHistoryPeriods;
    }


    std::string m_name;

    // mapping from time to value (sorted by time)
    //  only stores m_maxHistoryPeriods values on m_period boundaries
    std::map<TimePoint, Value> values; // Time to Value mapping

    double m_frequency; // Frequency in Hz
    TimeDuration m_period;

    bool isSource; // True if this is a source signal (e.g. constant at freq)
    bool isClock;  // True if this signal counts up at freq

    bool running = false; // True if source signal should advance each tick

    uint64_t clockCount = 0;

    std::vector<std::function<void(TimePoint, ptr<Signal>, const Value&)>> valueChangedCallbacks;

    friend class DataflowEngine;
    friend class Func;

private:
    // only store this many previous period values
    //  (e.g. is an input references [-1] for this signal, need to keep 2 values)
    // computed in buildSignalConsumers()
    int m_maxHistoryPeriods;

};


typedef std::vector<ptr<Signal>> Signals;


}
