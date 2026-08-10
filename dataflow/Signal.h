#pragma once

#include <map>
#include <mutex>
#include <optional>

#include "core/common.h"
#include "core/TimePoint.h"
#include "core/TimeDuration.h"

#include "Value.h"
#include "compiler/ChangeNotifier.h"

namespace df {

class DataflowEngine;

using roxal::ptr;
using roxal::weak_ptr;
using roxal::TimePoint;
using roxal::TimeDuration;


class Signal
   : public roxal::enable_ptr_from_this<Signal>
{
public:
    // Execution domain.  RT (default): the signal's island belongs to the
    // shared periodic schedule -- ticked by the engine's clock, or by an
    // embedding host's tickFor() budget slices.  Background: the island is
    // kept OFF that schedule and serviced by the engine's own thread with
    // no tick budget -- for periodic work whose cost can't fit an RT slice
    // (perception, logging, telemetry decimation).  Declared per source
    // signal; an island is background if ANY of its signals declares it
    // (see DataflowEngine::buildNetworkCacheData).
    enum class Domain { RT, Background };

    static ptr<Signal> newClockSignal(double freq, std::optional<std::string> name = {});
    static ptr<Signal> newSignal(double freq, Value initial, std::optional<std::string> name = {});
    static ptr<Signal> newSourceSignal(double freq, Value initial = Value(),
                                      std::optional<std::string> name = {},
                                      Domain domain = Domain::RT);

    // Template signal creation (not added to engine) - for use as type member default initializers
    static ptr<Signal> newClockSignalTemplate(double freq, std::optional<std::string> name = {});
    static ptr<Signal> newSourceSignalTemplate(double freq, Value initial = Value(),
                                               std::optional<std::string> name = {});

    const std::string& name() const { return m_name; }

    ptr<Signal> rename(const std::string& newName) { m_name = newName; return ptr_from_this(); }

    // Stable process-unique graph id (shared counter with FuncNode).  Names
    // are neither unique nor immutable; introspection keys on this instead.
    uint64_t id() const { return m_id; }

    // Source location that created this signal (best effort; line 0 = unknown).
    // Lets introspection correlate the live network back to the program text.
    const std::string& srcName() const { return m_srcName; }
    size_t srcLine() const { return m_srcLine; }
    size_t srcCol() const { return m_srcCol; }
    bool hasSrcOrigin() const { return m_srcLine != 0; }
    void setSrcOrigin(const std::string& name, size_t line, size_t col)
    {
        m_srcName = name; m_srcLine = line; m_srcCol = col;
    }

    // The x[-1] delayed-signal relation (see indexedSignal)
    bool derived() const { return isDerived; }
    ptr<Signal> derivedBase() const { return baseSignal.lock(); }
    int derivedIndex() const { return baseIndex; }

    void addValueChangedCallback(std::function<void(TimePoint, ptr<Signal>,const Value&)> callback);

    void trace(roxal::ValueVisitor& visitor) const;

    bool isInternal() const { return m_internal; }
    void setInternal(bool internal) { m_internal = internal; }

    virtual ~Signal();

    double frequency() const { return m_frequency; }
    void setFrequency(double freq);

    // Get and set value at a specific time point
    std::optional<Value> valueIfAvailableAt(TimePoint t) const;
    Value valueAt(TimePoint t) const;
    void setValueAt(TimePoint t, const Value& v);
    void set(const Value& v);

    // Release the buffered time->Value history.  Called during VM/engine
    // teardown (DataflowEngine::clear) to drop Object-backed Values (e.g.
    // tensor-valued signals) while their target Objs are still alive -- if
    // they linger into VM::freeObjects() the Values decRef already-freed Objs
    // (use-after-free / heap corruption at shutdown).  Callers must ensure no
    // producer thread will setValueAt() afterwards (DDS reader threads are
    // stopped before clear()).
    void clearValues();

    // For source signals: generate next value
    void tick(TimePoint t);

    // Advance the clock by one tick on the next period boundary
    void tickOnce() { tickPending = true; }

    // Start/stop running of source signals
    void run() { running = true; }
    void stop() { running = false; }

    // set initial value without advancing clock state (for evaluation)
    void evaluate(TimePoint t);

    // Get the period of the signal based on its frequency
    TimeDuration period() const { return m_period; }

    bool isEventDriven() const { return m_eventDriven; }

    bool isSourceSignal() const { return isSource; }
    bool isClockSignal() const { return isClock; }
    bool isRunning() const { return running; }

    // Domain access.  Post-registration domain CHANGES must go through
    // DataflowEngine::setSignalDomain -- both m_domain and the derived
    // m_inBackgroundIsland are read/written under the engine mutex (cache
    // rebuilds and the tick paths hold it), so a bare setDomain() on a
    // registered signal races an in-flight rebuild.
    Domain domain() const { return m_domain; }
    void setDomain(Domain d) { m_domain = d; }

    // Derived at network-cache build: true if this signal's island resolved
    // to the background domain.  Read by the periodic tick paths (under the
    // engine mutex) to keep background sources off the shared schedule.
    bool inBackgroundIsland() const { return m_inBackgroundIsland; }
    void setInBackgroundIsland(bool b) { m_inBackgroundIsland = b; }

    // Get the last value of the signal
    Value lastValue() const;
    // Retrieve the timestamp of the most recent sample if available.
    TimePoint latestSampleTime() const;
    // True once the signal carries any sample at all (a clock that has never
    // been evaluated carries none).
    bool hasValues() const {
        std::lock_guard<std::recursive_mutex> lock(m_valuesMutex);
        return !values.empty();
    }

    // Value at a negative index relative to the most recent value
    //   index 0 -> last value
    //   index -1 -> value from one period ago
    // Throws if index > 0 or history is insufficient
    Value valueAtIndex(int index, std::optional<TimePoint> referenceTime = std::nullopt) const;

    // Return a new signal that is this signal delayed by -index periods
    //   index 0 -> this signal
    //   index -1 -> signal one period ago
    // Throws if index > 0
    ptr<Signal> indexedSignal(int index);

    // The last value before a specific time
    Value lastValueBefore(TimePoint t) const;

    // number of stored history values
    size_t valuesCount() const
    {
        std::lock_guard<std::recursive_mutex> lock(m_valuesMutex);
        return values.size();
    }

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

    // Guards `values`: it is written by the engine thread (ticks), by script
    // threads (set) and by the DDS reader-signal thread, and read by any
    // sampling thread plus GC tracing. Recursive because accessors nest
    // (setValueAt -> lastValueBefore, valueAtIndex -> valueAt). Lock order
    // where both are held: engine m_mutex first, then this. Never hold this
    // across change callbacks or DataflowEngine calls.
    mutable std::recursive_mutex m_valuesMutex;

    double m_frequency; // Frequency in Hz
    TimeDuration m_period;

    bool m_eventDriven = false;

    bool isSource; // True if this is a source signal (e.g. constant at freq)
    bool isClock;  // True if this signal counts up at freq

    bool running = false; // True if source signal should advance each tick

    Domain m_domain = Domain::RT;
    bool m_inBackgroundIsland = false;  // derived at network-cache build

    // if true, advance once on next engine tick then clear
    bool tickPending = false;

    uint64_t clockCount = 0;

    roxal::ChangeNotifier m_changeNotifier;   // shared with bare property observers

    friend class DataflowEngine;
    friend class FuncNode;
    friend class Func;

private:
    // only store this many previous period values
    //  (e.g. is an input references [-1] for this signal, need to keep 2 values)
    // computed in buildSignalConsumers()
    int m_maxHistoryPeriods;

    bool isDerived = false; // true if this signal represents another signal delayed
    weak_ptr<Signal> baseSignal;  // base signal for derived signals
    int baseIndex = 0;       // index relative to base signal (negative)

    bool m_internal = false;

    uint64_t m_id;

    std::string m_srcName;
    size_t m_srcLine = 0;
    size_t m_srcCol = 0;

    // When true, the first setValueAt() call will set the value but skip
    // invoking valueChangedCallbacks.  This ensures the initial value of a
    // FuncNode output signal is treated as initialization rather than a change
    // — regardless of whether the producing FuncNode completes synchronously
    // or asynchronously (via a future).
    bool m_suppressInitialChange = false;

};



typedef std::vector<ptr<Signal>> Signals;


// Next id from the process-wide graph-id counter shared by Signal and
// FuncNode (introspection keys both node kinds in one id space).
uint64_t nextGraphId();

}
