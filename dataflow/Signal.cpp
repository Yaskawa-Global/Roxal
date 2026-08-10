#include <time.h>
#include <algorithm>
#include <atomic>
#include <numeric>
#include <iterator>
#include <cmath>
#include <optional>

#include "Signal.h"
#include "DataflowEngine.h"
#include <stdexcept>
#include <iostream>

using namespace df;

namespace {

// Publish-safe snapshot of a value stored into a signal: value-semantics
// types (tensor/vector/matrix/orient) COW-clone exactly like assignment;
// list/dict payloads are frozen (createFrozenSnapshot) so producer and
// samplers never share a writable reference across threads.  Object/actor
// payloads deliberately pass through (e.g. DDS message flows).
roxal::Value snapshotForSignal(const roxal::Value& v)
{
    if (v.isConst())
        return v;
    if (roxal::isList(v) || roxal::isDict(v))
        return roxal::createFrozenSnapshot(v);
    return roxal::cloneIfValueSemantics(v);
}

}


uint64_t df::nextGraphId()
{
    static std::atomic<uint64_t> counter { 1 };
    return counter.fetch_add(1, std::memory_order_relaxed);
}


ptr<Signal> Signal::newClockSignal(double freq, std::optional<std::string> name)
{
    auto s = ptr<Signal>::from_raw(new Signal(freq, Value(0), name)); // direct new needed
    s->isClock = true;
    s->isSource = true;
    s->clockCount = 0;
    DataflowEngine::instance()->addSignal(s);
    return s;
}


ptr<Signal> Signal::newSignal(double freq, Value initial, std::optional<std::string> name)
{
    auto s = ptr<Signal>::from_raw(new Signal(freq, initial, name));
    s->isClock = false;
    s->isSource = false;
    DataflowEngine::instance()->addSignal(s);
    return s;
}

ptr<Signal> Signal::newSourceSignal(double freq, Value initial, std::optional<std::string> name,
                                    Domain domain)
{
    auto s = ptr<Signal>::from_raw(new Signal(freq, initial, name));
    s->isClock = false;
    s->isSource = true;
    // Set before engine registration: a registered signal's domain may only
    // change via DataflowEngine::setSignalDomain (rebuilds read it under the
    // engine mutex).
    s->m_domain = domain;
    DataflowEngine::instance()->addSignal(s);
    return s;
}

ptr<Signal> Signal::newClockSignalTemplate(double freq, std::optional<std::string> name)
{
    auto s = ptr<Signal>::from_raw(new Signal(freq, Value(0), name)); // direct new needed
    s->isClock = true;
    s->isSource = true;
    s->clockCount = 0;
    // NOT added to engine - this is a template for type member defaults
    return s;
}

ptr<Signal> Signal::newSourceSignalTemplate(double freq, Value initial, std::optional<std::string> name)
{
    auto s = ptr<Signal>::from_raw(new Signal(freq, initial, name));
    s->isClock = false;
    s->isSource = true;
    // NOT added to engine - this is a template for type member defaults
    return s;
}


Signal::Signal(double freq, Value initial, std::optional<std::string> name)
    : m_frequency(freq), m_maxHistoryPeriods(2), m_id(nextGraphId())
{
    m_name = name.value_or("source_signal");

    if (freq <= 0.0) {
        m_eventDriven = true;
        m_frequency = 0.0;
        m_period = TimeDuration::zero();
    } else {
        double period_us = 1000000.0 / m_frequency;
        double period_round = std::round(period_us);
        if (period_us < 1.0 || std::fabs(period_us - period_round) > 1e-9) {
            throw std::invalid_argument(
                "clock frequency " + std::to_string(freq) +
                " not representable as whole microseconds");
        }
        m_period = TimeDuration::microSecs(static_cast<int64_t>(period_round));
    }

    // Snapshot on store (value-semantics COW clone; list/dict frozen): the
    // creator keeping its reference and mutating in place must never alter
    // what samplers observe.
    values[TimePoint::zero()] = snapshotForSignal(initial);
}


void Signal::addValueChangedCallback(std::function<void(TimePoint, ptr<Signal>, const Value&)> callback)
{
    m_changeNotifier.addCallback(std::move(callback));
}


void Signal::invokeValueChangedCallbacks(TimePoint t, const Value& v)
{
    m_changeNotifier.notify(t, ptr_from_this(), v);
}



Signal::~Signal()
{
}

void Signal::clearValues()
{
    std::lock_guard<std::recursive_mutex> lock(m_valuesMutex);
    values.clear();
}

void Signal::trace(roxal::ValueVisitor& visitor) const
{
    // GC tracing can run while a non-VM thread (e.g. the DDS reader-signal
    // thread) is mutating the map.
    std::lock_guard<std::recursive_mutex> lock(m_valuesMutex);
    for (const auto& entry : values) {
        visitor.visit(entry.second);
    }
}


void Signal::setFrequency(double freq)
{
    if (freq <= 0.0)
        throw std::invalid_argument("Signal frequency must be positive");

    m_eventDriven = false;
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
    std::lock_guard<std::recursive_mutex> lock(m_valuesMutex);
    auto it = values.find(t);
    if (it != values.end())
        return it->second;

    // For non-clock source signals, treat the last value prior to t as the
    // value at t.  These signals maintain their last set value until changed
    // explicitly, so consumers should consider that value available at all
    // future tick times.
    if (isSource && !isClock && !values.empty() && t >= values.begin()->first) {
        auto it_before = values.upper_bound(t);
        if (it_before != values.begin()) {
            --it_before;
            return it_before->second;
        }
    }

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

    std::lock_guard<std::recursive_mutex> lock(m_valuesMutex);
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
    auto engine = DataflowEngine::instance();
    auto tickStart = engine->tickStart();

    if (!m_eventDriven) {
        auto age = t - tickStart;
#ifdef DEBUG_BUILD
        // Diagnostic only, on stderr: stdout is byte-compared by the test
        // harness.  KNOWN BENIGN MISFIRE: the check is phase-relative to
        // tickStart, which rebases concurrently on network rebuilds -- an
        // absolutely grid-aligned write (e.g. t=200ms, period=100ms) can be
        // flagged when tickStart momentarily sits at an odd phase of this
        // signal's period.  (An absolute check would misfire instead on
        // set()'s deliberate tickStart+period scheduling.)
        if (t >= tickStart && (age % m_period != TimeDuration::zero())) {
            std::cerr << "setValueAt Signal " + name() + " for time " + t.humanString() +
                " not a multiple of period " + m_period.humanString() << std::endl;
        }
#endif
    }

    // Snapshot on store, exactly like assignment does for value-semantics
    // types (COW, O(1)); list/dict payloads are frozen so the setter keeping
    // its reference and mutating in place can never alter what samplers (or
    // change-event payloads) observe — mutation attempts fail loudly instead.
    // Const (frozen) values need no snapshot at all — immutability makes the
    // reference itself safe to share, so publishing a const tensor is free.
    Value stored = snapshotForSignal(v);

    bool notifyChange = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_valuesMutex);
        assert(!values.empty());
        bool valueChanged = (lastValueBefore(t) != stored);

        // if we're adding a newer time, remove the oldest
        if (t > values.rbegin()->first) {
            if (values.size() >= m_maxHistoryPeriods)
                values.erase(values.begin()); // map keys are sorted by time, so remove oldest/smallest value
        }

        values[t] = stored;

        if (valueChanged) {
            if (m_suppressInitialChange)
                m_suppressInitialChange = false;  // first value is initialization, not a change
            else
                notifyChange = true;
        }
    }

    // Callbacks and engine notification run outside m_valuesMutex: callbacks
    // can be arbitrarily heavy (event scheduling, DDS writes) and the engine
    // takes its own m_mutex -- holding m_valuesMutex across that would invert
    // the engine-then-signal lock order the tick path uses.
    if (notifyChange)
        invokeValueChangedCallbacks(t, stored);

    if (m_eventDriven) {
        engine->updateSignalConsumerInputAvailability(ptr_from_this(), t);
        if (isSource && !isDerived)
            engine->processEventDrivenSignalUpdate(ptr_from_this(), t);
    }
}

void Signal::set(const Value& v)
{
    if (m_eventDriven) {
        TimePoint now = TimePoint::currentTime();
        setValueAt(now, v);
        return;
    }

    TimePoint t = DataflowEngine::instance()->tickStart();

    // Update the value at the next tick boundary for this signal. This avoids
    // races with the dataflow engine thread which may already be processing the
    // current tick when set() is called from another thread.
    TimePoint nextTick = t + m_period;

    setValueAt(nextTick, v);
    DataflowEngine::instance()->updateSignalConsumerInputAvailability(ptr_from_this(), nextTick);
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
    std::lock_guard<std::recursive_mutex> lock(m_valuesMutex);
    if (values.empty())
        throw std::runtime_error("Signal has no values.");
    return values.rbegin()->second;
}

TimePoint Signal::latestSampleTime() const
{
    std::lock_guard<std::recursive_mutex> lock(m_valuesMutex);
    if (values.empty())
        return TimePoint::zero();
    return values.rbegin()->first;
}

Value Signal::valueAtIndex(int index, std::optional<TimePoint> referenceTime) const
{
    if (index > 0)
        throw std::invalid_argument(
            "Signal index must be 0 or negative (sig[-1] is the value one period ago). "
            "To sample a signal use a cast like real(sig); a func returning a list "
            "produces ONE list-valued signal - declare '-> [T, ...]' for multiple outputs");

    std::lock_guard<std::recursive_mutex> lock(m_valuesMutex);
    if (values.empty())
        throw std::runtime_error("Signal has no values.");

    if (m_eventDriven) {
        if (index == 0)
            return lastValue();
        throw std::invalid_argument("Event-driven signals do not support history indices");
    }

    int stepsBack = -index;

    TimePoint reference = referenceTime.value_or(values.rbegin()->first);
    TimePoint t = reference - m_period * stepsBack;
    // If t predates the earliest recorded value, return the
    // initial value instead of throwing an exception.
    if (t < values.begin()->first)
        return values.begin()->second;

    return valueAt(t);
}

ptr<Signal> Signal::indexedSignal(int index)
{
    if (index > 0)
        throw std::invalid_argument(
            "Signal index must be 0 or negative (sig[-1] is the value one period ago). "
            "To sample a signal use a cast like real(sig); a func returning a list "
            "produces ONE list-valued signal - declare '-> [T, ...]' for multiple outputs");

    if (index == 0)
        return ptr_from_this();

    if (m_eventDriven)
        throw std::invalid_argument("Event-driven signals do not support delayed indices");

    Value initial;
    try {
        // Use the signal's latest sample time as reference, not the engine's
        // tickStart. The tickStart is the *next* tick boundary, but we want to
        // index relative to the signal's actual current value.
        TimePoint reference = latestSampleTime();
        initial = valueAtIndex(index, reference);
    } catch(...) {
        initial = Value();
    }

    // Create a new signal that mirrors this one but with a time delay.
    // The old standalone DataflowEngine supported latency by storing the
    // desired index on FuncInputInfo.  Here we emulate that behaviour by
    // generating a separate Signal updated whenever the source updates.
    auto newSig = ptr<Signal>::from_raw(new Signal(m_frequency, initial, m_name + "[" + std::to_string(index) + "]"));
    newSig->isClock = false;
    newSig->isSource = false;
    newSig->isDerived = true;
    newSig->baseSignal = ptr_from_this();
    newSig->baseIndex = index;
    newSig->m_eventDriven = m_eventDriven;
    newSig->setInternal(isInternal());
    newSig->setMaxHistoryPeriods(std::max(m_maxHistoryPeriods, -index + 1));
    newSig->setSrcOrigin(m_srcName, m_srcLine, m_srcCol);  // inherits the base's origin
    DataflowEngine::instance()->addSignal(newSig);

    return newSig;
}



Value Signal::lastValueBefore(TimePoint t) const
{
    std::lock_guard<std::recursive_mutex> lock(m_valuesMutex);
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
