#include "DataflowEngine.h"
#include "FuncNode.h"
#include "compiler/VM.h"
#include "compiler/Object.h"
#include "compiler/SimpleMarkSweepGC.h"
#include "core/common.h"

#include <optional>

#include <cstdlib>
#include <stdexcept>
#include <numeric>
#include <thread>
#include <iostream>
#include <string.h>
#include <deque>
#include <algorithm>
#include <limits>

using namespace df;


#include <atomic>

static std::atomic<uint64_t> gFuncCounter{0};

std::string DataflowEngine::uniqueFuncName(const std::string& base)
{
    // Single atomic op: increment-then-re-read let two concurrent lifts
    // observe the same count and mint duplicate names.
    const uint64_t n = ++gFuncCounter;
    if (n == 1)
        return base;
    return base + "#" + std::to_string(n);
}


#define TRACE_EXECUTION 0

#ifdef TRACE_EXECUTION
struct TraceEntry {
    TraceEntry(TimePoint occurred_, std::string log) : occurred(occurred_), log(log) {}
    TraceEntry(TimePoint occurred_, std::string log, std::optional<TimePoint> time_, std::optional<ptr<FuncNode>> func_, std::optional<ptr<Signal>> signal_)
      : occurred(occurred_), log(log), time(time_), func(func_), signal(signal_) {}

    TimePoint occurred;
    std::string log;
    std::optional<TimePoint> time;
    std::optional<ptr<FuncNode>> func;
    std::optional<ptr<Signal>> signal;

    std::string toString() const {
        std::stringstream ss;
        ss << occurred << " " << log;
        if (time.has_value()) ss << "  time:" << time.value();
        if (func.has_value()) ss << "  func: " << func.value()->name();
        if (signal.has_value()) ss << "  signal: " << signal.value()->name();
        return ss.str();
    }
};
std::vector<TraceEntry> globalTrace {};

void trace(const TraceEntry& t) {
    globalTrace.push_back(t);
}

void printTrace() {
    std::cout << "-------- Trace: --------" << std::endl;
    for (auto& t : globalTrace)
        std::cout << t.toString() << std::endl;
}
#endif


DataflowEngine::DataflowEngine()
{
    m_executionScheme = ExecutionScheme::Strict;
    m_networkModified = false;
    assert(TimeDuration::secs(1).frequency() == 1.0);
    m_runStart = TimePoint::zero();
    m_tickNumber = 0;
}


DataflowEngine::~DataflowEngine()
{
    clear();
}

void DataflowEngine::markNetworkModified()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_networkModified = true;
}


void DataflowEngine::addSignal(ptr<Signal> signal)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    signals.push_back(signal);
    m_networkModified = true;
}

void DataflowEngine::addFunc(ptr<FuncNode> func)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    funcs[func->name()] = func;
    m_networkModified = true;
}

void DataflowEngine::registerSignalWrapper(const ptr<Signal>& signal)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    signalWrapperRefs[signal]++;
}

size_t DataflowEngine::unregisterSignalWrapper(const ptr<Signal>& signal)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto it = signalWrapperRefs.find(signal);
    if (it == signalWrapperRefs.end())
        return 0;
    if (--it->second == 0) {
        signalWrapperRefs.erase(it);
        return 0;
    }
    return it->second;
}

size_t DataflowEngine::wrapperRefCount(const ptr<Signal>& signal) const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto it = signalWrapperRefs.find(signal);
    if (it == signalWrapperRefs.end())
        return 0;
    return it->second;
}

size_t DataflowEngine::consumerCount(const ptr<Signal>& signal) const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto it = signalConsumers.find(signal);
    if (it == signalConsumers.end())
        return 0;
    return it->second.size();
}

size_t DataflowEngine::signalRefCount(const ptr<Signal>& signal) const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    size_t count = 0;
    for (const auto& s : signals)
        if (s == signal)
            ++count;
    if (signalConsumers.find(signal) != signalConsumers.end())
        ++count;
    for (const auto& kv : funcs) {
        const auto& func = kv.second;
        for (const auto& ip : func->m_inputs)
            if (ip.signal == signal) ++count;
        for (const auto& op : func->m_outputs)
            if (op.signal == signal) ++count;
    }
    return count;
}

void DataflowEngine::removeSignal(const ptr<Signal>& signal, bool force)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    // Check if any function still references this signal
    bool used = false;
    for (const auto& kv : funcs) {
        const auto& func = kv.second;
        for (const auto& ip : func->m_inputs)
            if (ip.signal == signal) { used = true; break; }
        if (used) break;
        for (const auto& op : func->m_outputs)
            if (op.signal == signal) { used = true; break; }
        if (used) break;
    }

    if (used && !force)
        return;

    // Remove from signal list
    auto it = std::remove(signals.begin(), signals.end(), signal);
    if (it != signals.end())
        signals.erase(it, signals.end());
    else
        return;

    // Remove from signalConsumers mapping
    signalConsumers.erase(signal);

    // Remove references from funcs and mark any funcs with no outputs
    std::vector<ptr<FuncNode>> funcsToRemove;
    for (auto& kv : funcs) {
        auto& func = kv.second;
        auto& inputs = func->m_inputs;
        inputs.erase(std::remove_if(inputs.begin(), inputs.end(),
                        [&](const FuncNode::InputPort& p){ return p.signal == signal; }),
                     inputs.end());

        auto& outputs = func->m_outputs;
        outputs.erase(std::remove_if(outputs.begin(), outputs.end(),
                        [&](const FuncNode::OutputPort& p){ return p.signal == signal; }),
                      outputs.end());

        if (func->m_outputs.empty())
            funcsToRemove.push_back(func);
    }

    for (auto& f : funcsToRemove)
        removeFunc(f);

    m_networkModified = true;
}

void DataflowEngine::removeFunc(ptr<FuncNode> func)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    auto it = funcs.find(func->name());
    if (it != funcs.end())
        funcs.erase(it);
    else {
        for (auto it2 = funcs.begin(); it2 != funcs.end(); ++it2) {
            if (it2->second == func) { funcs.erase(it2); break; }
        }
    }

    // Remove func from signalConsumers lists
    for (auto itSig = signalConsumers.begin(); itSig != signalConsumers.end(); ) {
        auto& vec = itSig->second;
        vec.erase(std::remove_if(vec.begin(), vec.end(),
                   [&](const FuncInputInfo& fi){ return fi.func == func; }), vec.end());
        if (vec.empty())
            itSig = signalConsumers.erase(itSig);
        else
            ++itSig;
    }

    // After removing the function, attempt to remove its signals if unused
    for (const auto& input : func->m_inputs)
        removeSignal(input.signal,
                     wrapperRefCount(input.signal) == 0 && consumerCount(input.signal) == 0);
    for (const auto& output : func->m_outputs)
        removeSignal(output.signal,
                     wrapperRefCount(output.signal) == 0 && consumerCount(output.signal) == 0);

    m_networkModified = true;
}


void DataflowEngine::copyInto(const ptr<Signal>& lhs, const ptr<Signal>& rhs)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    // Ensure both signals are valid and have the same frequency
    if (!lhs || !rhs || lhs->frequency() != rhs->frequency())
        throw std::runtime_error("Signals have the same frequency");

    if (!lhs->isSourceSignal())
        throw std::runtime_error("lhs must be a source signal");

    // Copy attributes from rhs to lhs
    lhs->isSource = rhs->isSource;
    lhs->isClock = rhs->isClock;
    lhs->clockCount = rhs->clockCount;
    lhs->m_maxHistoryPeriods = rhs->m_maxHistoryPeriods;
    lhs->running = rhs->running;
    lhs->tickPending = rhs->tickPending;
    lhs->m_eventDriven = rhs->m_eventDriven;

    // Direct manipulation of the signals' values maps must be serialized with
    // concurrent producers (e.g. the DDS reader-signal thread). Snapshot under
    // the locks, operate on the snapshots, and never hold the locks across
    // setValueAt (it notifies callbacks/the engine after its own locking).
    std::map<TimePoint, Value> previousValues, rhsValues;
    {
        std::scoped_lock sigLock(lhs->m_valuesMutex, rhs->m_valuesMutex);
        previousValues = lhs->values;
        rhsValues = rhs->values;
        lhs->values = rhs->values;
    }
    for(const auto& rhsCallback : rhs->m_changeNotifier.callbacks)
        lhs->m_changeNotifier.addCallback(rhsCallback);

    lhs->isDerived = rhs->isDerived;
    lhs->baseSignal = rhs->baseSignal;
    lhs->baseIndex = rhs->baseIndex;

    // Update any derived signals that referenced lhs so they now reference rhs
    for (auto& sig : signals) {
        if (!sig->isDerived)
            continue;

        auto base = sig->baseSignal.lock();
        if (base == lhs)
            sig->baseSignal = rhs;
    }

    // Update any functions that use rhs as an input to use lhs instead
    for (const auto& kv : funcs) {
        auto& func = kv.second;
        for (auto& inputPort : func->m_inputs) {
            if (inputPort.signal == rhs)
                func->reassignInput(inputPort.name, lhs);
        }
    }

    bool rhsHasConcreteSample = std::any_of(
        rhsValues.begin(), rhsValues.end(),
        [](const std::pair<const TimePoint, Value>& sample) {
            return !sample.second.isNil();
        });

    // Explicit iterator guard rather than `empty() || begin()->...`: GCC 13's
    // -Wstringop-overflow can't see the short-circuit and reports a bogus
    // read past the map object for the empty case (observed in the
    // roxal-internal/FC build of this file).
    bool earliestSampleIsNil = true;
    if (auto firstSample = rhsValues.begin(); firstSample != rhsValues.end())
        earliestSampleIsNil = firstSample->second.isNil();

    if (rhsHasConcreteSample) {
        for (const auto& kv : rhsValues)
            lhs->setValueAt(kv.first, kv.second);

        if (earliestSampleIsNil && !previousValues.empty()) {
            auto fallback = previousValues.begin();
            std::lock_guard<std::recursive_mutex> lock(rhs->m_valuesMutex);
            rhs->values[fallback->first] = fallback->second;
        }
    } else if (!previousValues.empty()) {
        std::scoped_lock sigLock(lhs->m_valuesMutex, rhs->m_valuesMutex);
        lhs->values = previousValues;
        rhs->values = previousValues;
    }

    m_networkModified = true;
}

void DataflowEngine::clear()
{
    // Drop queued-but-unserviced event updates first: they hold Signal refs
    // (and Values inside those signals) that must not outlive VM teardown.
    {
        std::lock_guard<std::mutex> pendingLock(m_pendingEventMutex);
        m_pendingEventUpdates.clear();
    }
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    signalConsumers.clear();
    m_networkIslands.clear();
    signalWrapperRefs.clear();
    signals.clear();
    funcs.clear();
    m_networkModified = false;
    m_tickCallbacks.clear();
    m_tickStart = TimePoint::zero();
    m_tickPeriod = TimeDuration::zero();
    m_runStart = TimePoint::zero();
    m_tickNumber = 0;
    // Reset yield state to avoid stale references to cleared funcs
    m_yieldState = YieldState{};
}

void DataflowEngine::stop()
{
    m_shouldStop = true;
    wakeDrain();   // rouse an idle run() so it observes the stop promptly
}

void DataflowEngine::wakeDrain()
{
    // Notify without holding m_pendingEventMutex: the waiter re-checks its
    // predicate under the lock, and the timed wait covers a lost wake.
    m_pendingEventCv.notify_all();
}


TimeDuration DataflowEngine::tickPeriod() const
{
    if (m_networkModified)
        const_cast<DataflowEngine*>(this)->buildNetworkCacheData();

    return m_tickPeriod;
}

uint64_t DataflowEngine::currentTickNumber() const
{
    return m_tickNumber.load();
}



void DataflowEngine::setSignalDomain(const ptr<Signal>& signal, Signal::Domain domain)
{
    if (!signal)
        return;
    {
        // Every cache rebuild reads signal domains under m_mutex; writing
        // under it means an in-flight rebuild can never observe a torn or
        // mid-change value.
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        signal->setDomain(domain);
    }
    markNetworkModified();   // rerun the build with the new domain
}


void DataflowEngine::requestTickAndWait()
{
    const uint64_t target = m_tickRequests.fetch_add(1, std::memory_order_acq_rel) + 1;
    wakeDrain();
    // Yieldable wait: the engine thread executes the tick (it is the sole
    // periodic driver); this thread parks for any collection that starts
    // meanwhile -- the engine thread itself can park mid-tick, so a
    // non-parking waiter would deadlock the barrier.
    while (m_tickRequestsDone.load(std::memory_order_acquire) < target) {
        if (m_shouldStop.load(std::memory_order_relaxed))
            return;   // engine stopping: the tick will never run
        if (roxal::VM::thread && roxal::VM::thread->execute_depth > 0) {
            auto& gc = roxal::SimpleMarkSweepGC::instance();
            if (gc.isCollectionRequested())
                gc.safepoint(*roxal::VM::thread);
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}


bool DataflowEngine::servicePendingTickRequests()
{
    bool ticked = false;
    uint64_t want = m_tickRequests.load(std::memory_order_acquire);
    while (m_tickRequestsDone.load(std::memory_order_relaxed) < want) {
        tick(/*waitForTickStart=*/false);
        m_tickRequestsDone.fetch_add(1, std::memory_order_release);
        ticked = true;
        want = m_tickRequests.load(std::memory_order_acquire);
    }
    return ticked;
}


bool DataflowEngine::serviceBackgroundIslands(TimePoint& soonestDue)
{
    soonestDue = TimePoint::zero();
    if (!m_haveBackgroundIslands.load(std::memory_order_relaxed))
        return false;

    // Serialize against tickFor's periodic evaluation and the event path.
    // This runs on the engine thread, which may block -- but a contended
    // wait is covered as GC-safe blocking for the same reason as in
    // processEventDrivenSignalUpdate: the in-flight evaluator may be parked
    // at a safepoint while holding the mutex.
    std::unique_lock<std::recursive_mutex> evalLock(m_evalMutex, std::try_to_lock);
    if (!evalLock.owns_lock()) {
        roxal::SimpleMarkSweepGC::GCSafeBlockScope blockCover;
        evalLock.lock();
    }

    if (m_networkModified)
        buildNetworkCacheData();

    const TimePoint now = TimePoint::currentTime();

    // Under m_mutex: find due islands, tick their due sources, and advance
    // their schedules.  Evaluation happens on island COPIES outside m_mutex
    // (same pattern as tickFor) -- closures can run arbitrarily long.
    std::vector<std::pair<NetworkIsland, TimePoint>> due;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        for (auto& island : m_networkIslands) {
            if (!island.background || island.tickPeriod == TimeDuration::zero())
                continue;

            if (island.bgNextDue == TimePoint::zero())
                island.bgNextDue = nextPeriodOnPeriodBoundary(island.tickPeriod);

            if (now < island.bgNextDue) {
                if (soonestDue == TimePoint::zero() || island.bgNextDue < soonestDue)
                    soonestDue = island.bgNextDue;
                continue;
            }

            const TimePoint evalTime = island.bgNextDue;
            for (const auto& signal : island.signals) {
                if (!signal->isSourceSignal()
                    || signal->period() == TimeDuration::zero())
                    continue;
                if ((evalTime % signal->period()) == TimeDuration::zero()) {
                    signal->tick(evalTime);
                    updateSignalConsumerInputAvailability(signal, evalTime);
                }
            }

            // Best-effort schedule: if servicing fell behind (background
            // work may overrun its own period), skip the missed periods
            // rather than replaying them.
            do {
                island.bgNextDue = island.bgNextDue + island.tickPeriod;
            } while (island.bgNextDue <= now);
            if (soonestDue == TimePoint::zero() || island.bgNextDue < soonestDue)
                soonestDue = island.bgNextDue;

            due.emplace_back(island, evalTime);
        }
    }

    // No tick budget: background work runs to completion on this thread
    // (same unbudgeted evaluation as the event-driven path).
    for (auto& entry : due)
        evaluateIsland(entry.first,
                       resolveEvaluationTime(entry.first, entry.second),
                       TimePoint::max());

    return !due.empty();
}


void DataflowEngine::run() {
    m_shouldStop = false;

    // GC coverage for the whole loop.  The engine's actor thread reaches here
    // via a boundNative dispatch (no VM::execute frame) and is normally
    // already covered by the actor dispatch loop's persistent participant
    // (Thread::act) -- poll THAT here via pollCurrentThreadParticipant().
    // Only a bare foreign thread with no coverage at all gets a participant
    // of its own; a script VM thread calling run() is registered by its
    // outer execute() (VM::execute skips re-registration for participant
    // threads) and polls via safepoint().  Poll ONLY at points where no
    // un-stored Value locals are held (top of loop / gap loop).
    const bool needsParticipant =
        (roxal::VM::thread == nullptr || roxal::VM::thread->execute_depth == 0)
        && !roxal::SimpleMarkSweepGC::currentThreadIsExternalParticipant();
    std::optional<roxal::SimpleMarkSweepGC::ExternalParticipant> gcParticipant;
    if (needsParticipant)
        gcParticipant.emplace(roxal::SimpleMarkSweepGC::instance());
    auto gcPoll = [&] {
        if (roxal::SimpleMarkSweepGC::currentThreadIsExternalParticipant()) {
            roxal::SimpleMarkSweepGC::pollCurrentThreadParticipant();
        } else if (roxal::VM::thread && roxal::VM::thread->execute_depth > 0) {
            auto& gc = roxal::SimpleMarkSweepGC::instance();
            if (gc.isCollectionRequested())
                gc.safepoint(*roxal::VM::thread);
        }
    };

    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);

        if (m_networkModified)
            buildNetworkCacheData();

        if (m_tickPeriod == TimeDuration::zero())
            m_runStart = TimePoint::currentTime();
        else
            m_runStart = nextPeriodOnPeriodBoundary(m_tickPeriod);
    }

    while (!m_shouldStop) {
        gcPoll();   // no engine Value locals held here: safe park point

        if (m_networkModified) {
            buildNetworkCacheData();

            if (m_tickPeriod > TimeDuration::zero()) {
                m_runStart = nextPeriodOnPeriodBoundary(m_tickPeriod);
                m_tickNumber = 0;
            }
            continue;
        }

        if (m_hostDriven.load(std::memory_order_relaxed)
            || m_tickPeriod == TimeDuration::zero()) {
            // Purely event-driven network, OR a host drives the periodic
            // schedule via tickFor() (m_hostDriven): this loop reduces to
            // servicing updates handed off from non-VM producer threads
            // (see processEventDrivenSignalUpdate) plus any background-
            // domain periodic islands -- both evaluate here on the actor
            // thread, off the host's RT budget.  Deferred RT-lint scans
            // (queued by the tick path) also print from here.
            if (m_rtLintPending.exchange(false, std::memory_order_relaxed))
                rtLintIslands();
            bool didWork = processPendingEventUpdates();
            TimePoint bgDue;
            didWork |= serviceBackgroundIslands(bgDue);
            didWork |= servicePendingTickRequests();
            if (!didWork) {
                // Idle: sleep on the pending-event condvar instead of a 1ms
                // poll -- truly dormant with no traffic, ~us delivery with.
                // The predicate also wakes for stop() (teardown join stays
                // prompt) and for a GC request (the collection barrier must
                // not wait out this sleep; VM::wakeAllThreadsForGC calls
                // wakeDrain()).  The timed fallback covers any missed wake
                // -- shortened when a background island comes due sooner.
                auto waitDur = std::chrono::microseconds(10000);
                if (bgDue != TimePoint::zero()) {
                    auto us = (bgDue - TimePoint::currentTime()).microSecs();
                    if (us < 10000)
                        waitDur = std::chrono::microseconds(us > 0 ? us : 0);
                }
                std::unique_lock<std::mutex> lk(m_pendingEventMutex);
                m_pendingEventCv.wait_for(lk, waitDur, [&]{
                    return m_shouldStop.load(std::memory_order_relaxed)
                        || !m_pendingEventUpdates.empty()
                        || m_tickRequests.load(std::memory_order_relaxed)
                               != m_tickRequestsDone.load(std::memory_order_relaxed)
                        || roxal::SimpleMarkSweepGC::instance().isCollectionRequested();
                });
            }
            continue;
        }

        m_tickStart = m_runStart + m_tickPeriod*m_tickNumber;
        if (m_tickStart < TimePoint::currentTime()) {
            m_runStart = nextPeriodOnPeriodBoundary(m_tickPeriod);
            m_tickNumber = 0;
            continue;
        }

        // Service handed-off event updates and background islands at least
        // once per tick cycle (at fast tick rates, e.g. 1kHz, the gap loop
        // below never runs), then keep servicing at 1ms granularity while
        // waiting out longer tick gaps, taking the final (<1ms) stretch as
        // a precise sleep so tick timing is unchanged.  (Background islands
        // are excluded from m_tickPeriod's grid, so tick() below never
        // reaches them -- this is their only servicing in engine-driven
        // mode too.)
        processPendingEventUpdates();
        {
            TimePoint bgDue;
            serviceBackgroundIslands(bgDue);
        }
        servicePendingTickRequests();
        // Re-check m_shouldStop AND m_hostDriven throughout the tick gap -- the
        // outer loop only re-checks them once per tick, but this gap can span a
        // whole tick period (and sleepUntil below waits out the rest):
        //  * m_shouldStop: if the engine clock is simulator time that stops
        //    advancing once the sim process exits, TimePoint::currentTime()
        //    never reaches m_tickStart, so a stop() requested during the gap is
        //    never observed and VM teardown's join of this thread hangs forever.
        //  * m_hostDriven: a host (e.g. FC's RT loop) may call tickFor() and
        //    take over the periodic schedule after run() has already entered
        //    this branch (it starts false, before the host's first tickFor).
        //    Bail so the outer loop takes the host-driven branch (draining
        //    events) instead of this thread sitting dormant until a far-future
        //    m_tickStart -- and double-driving the schedule if it ever wakes.
        while (!m_shouldStop
               && !m_hostDriven.load(std::memory_order_relaxed)
               && TimePoint::currentTime() + TimeDuration::milliSecs(1) < m_tickStart) {
            gcPoll();   // gap can span a whole tick period: stay parkable
            bool gapWork = processPendingEventUpdates();
            TimePoint bgDue;
            gapWork |= serviceBackgroundIslands(bgDue);
            gapWork |= servicePendingTickRequests();
            if (!gapWork)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (m_shouldStop || m_hostDriven.load(std::memory_order_relaxed))
            continue;  // stop, or the host took over -> re-evaluate via outer loop
        sleepUntil(m_tickStart);
        tick(false);
    }
}

void DataflowEngine::tick(bool waitForTickStart)
{
    // Engine-thread periodic driver (run() and the _dataflow_tick() request
    // path are the only callers).  Serialize against event-driven island
    // evaluation and a host's tickFor; a contended wait is covered as
    // GC-safe blocking (the in-flight evaluator can park at a safepoint
    // while holding the mutex).
    std::unique_lock<std::recursive_mutex> evalLock(m_evalMutex, std::try_to_lock);
    if (!evalLock.owns_lock()) {
        roxal::SimpleMarkSweepGC::GCSafeBlockScope blockCover;
        evalLock.lock();
    }

    if (m_networkModified)
        buildNetworkCacheData();

    if (m_tickPeriod == TimeDuration::zero())
        return;

    if (m_runStart == TimePoint::zero()) // not set
        m_runStart = nextPeriodOnPeriodBoundary(m_tickPeriod);



    m_tickStart = m_runStart + m_tickPeriod*m_tickNumber;
    auto nextTickStart = m_tickStart + m_tickPeriod;

    if (waitForTickStart)
        sleepUntil(m_tickStart);

    #if 0
    std::cout << "tick " << tick << " @ " << m_tickStart.humanString() << std::endl;
    #endif


    // tick all the source signals that need it on this loop tick
    std::vector<ptr<Signal>> signalsToCheck {};
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        signalsToCheck = signals; // copy to avoid holding the lock while ticking signals
    }


    {
        std::lock_guard<std::recursive_mutex> sourceLock(m_mutex);
        for(const auto& signal : signals) {
            if (signal->isSourceSignal()) {
                if (signal->period() == TimeDuration::zero())
                    continue;

                // Background-island sources tick on their own schedule in
                // serviceBackgroundIslands() -- never here (double-advance).
                if (signal->inBackgroundIsland())
                    continue;

                // should this source tick now?
                if ((m_tickStart % signal->period()) == TimeDuration::zero()) { // yes

                    #if 0
                    Value previousValue = signal->lastValue();
                    #endif

                    signal->tick(m_tickStart);

                    #if 0
                    Value currentValue = signal->lastValue();
                    bool valueChanged = (currentValue != previousValue);
                    std::cout << "ticked source signal " << signal->name();
                    if (valueChanged)
                                std::cout << " changed from " << previousValue << " to " << currentValue;
                    else
                                std::cout << " unchanged at " << currentValue;
                    std::cout << std::endl;
                    #endif

                    updateSignalConsumerInputAvailability(signal, m_tickStart);

                }
            }
        }
    }

    evaluateNetwork(m_tickStart);

    #if 0
    std::cout << "iterations for tick " << tick << ": " << iterations << std::endl;
    #endif

    invokeTickCallbacks();
    if (TimePoint::currentTime() > nextTickStart) {
        std::string message = "Engine tick period "+m_tickPeriod.load().humanString()+" exceeded after tick callbacks invoked";

        if (m_executionScheme == ExecutionScheme::Strict)
            throw std::runtime_error(message);
        else
            std::cerr << message << std::endl;
    }

    m_tickNumber++;
}


void DataflowEngine::refreshDerivedSignals(const NetworkIsland& island, TimePoint time)
{
    for (const auto& signal : island.signals) {
        if (!signal->isDerived)
            continue;

        auto src = signal->baseSignal.lock();
        if (!src)
            continue;

        Value val;
        try {
            if (src->period() == TimeDuration::zero() && signal->baseIndex != 0)
                val = src->lastValue();
            else
                val = src->valueAtIndex(signal->baseIndex, time);
        } catch (...) {
            val = Value();
        }

        signal->setValueAt(time, val);
        updateSignalConsumerInputAvailability(signal, time);
    }
}

void DataflowEngine::evaluateNetwork(TimePoint evaluationTime)
{
    std::vector<NetworkIsland> islandsCopy;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        islandsCopy = m_networkIslands;
    }

    for (const auto& island : islandsCopy) {
        TimePoint islandTime = resolveEvaluationTime(island, evaluationTime);
        evaluateIsland(island, islandTime);
    }
}


void DataflowEngine::recordNodeOverrun(
    const std::string& nodeName, TimeDuration cost, TimeDuration overBudget)
{
    bool firstOccurrence = false;
    {
        std::lock_guard<std::mutex> lock(m_nodeOverrunMutex);
        auto& rec = m_nodeOverruns[nodeName];
        rec.nodeName = nodeName;
        rec.cost = cost;
        rec.overBudget = overBudget;
        rec.occurrences++;
        firstOccurrence = m_nodeOverrunWarned.insert(nodeName).second;
    }

    // One stderr line per node, ever.  Printing from the ticking thread is
    // itself slow, but this fires only on a cycle that already overran, and
    // only the first time -- repeat offenders just bump the record for the
    // host to drain.
    if (firstOccurrence) {
        std::cerr << "DataflowEngine: node '" << nodeName
                  << "' overran the tick budget: cost " << cost.humanString()
                  << ", " << overBudget.humanString()
                  << " past the deadline (repeats aggregated; drain via "
                     "consumeNodeOverruns())" << std::endl;
    }
}


std::vector<DataflowEngine::NodeOverrun> DataflowEngine::consumeNodeOverruns()
{
    std::vector<NodeOverrun> result;
    std::lock_guard<std::mutex> lock(m_nodeOverrunMutex);
    result.reserve(m_nodeOverruns.size());
    for (auto& entry : m_nodeOverruns)
        result.push_back(std::move(entry.second));
    m_nodeOverruns.clear();
    return result;
}


void DataflowEngine::rtLintIslands()
{
    static const bool lintEnabled = [] {
        const char* env = std::getenv("ROXAL_RT_LINT");
        return !(env && env[0] == '0');
    }();
    if (!lintEnabled)
        return;

    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    for (const auto& island : m_networkIslands) {
        // Event-driven islands aren't on the host's periodic schedule
        if (island.tickPeriod == TimeDuration::zero())
            continue;

        std::string scriptNodes;
        for (const auto& func : island.funcs) {
            if (func->closure.isNil())
                continue;   // native nodes are the expected RT payload
            if (!scriptNodes.empty())
                scriptNodes += ", ";
            scriptNodes += "'" + func->name() + "'";
        }
        if (scriptNodes.empty())
            continue;

        if (!m_rtLintAdvised.insert(scriptNodes).second)
            continue;

        std::cerr << "DataflowEngine: advisory: periodic island (period "
                  << island.tickPeriod.humanString()
                  << ") on the host tick schedule contains script node(s) "
                  << scriptNodes
                  << " -- script execution is budget-sliced, but a single"
                     " non-yieldable stretch can overrun the tick; if this"
                     " work doesn't need the periodic schedule, move it to"
                     " an event-driven or background island"
                     " (ROXAL_RT_LINT=0 silences this)" << std::endl;
    }
}


DataflowEngine::TickResult DataflowEngine::tickFor(TimeDuration budget)
{
    // The host is driving the periodic schedule from here on: the engine's
    // own run()/runFor() loops must stop ticking periodic islands (see
    // m_hostDriven) or the two drivers race on the same islands.
    if (!m_hostDriven.exchange(true, std::memory_order_relaxed))
        m_rtLintPending.store(true, std::memory_order_relaxed);    // first latch: lint the already-built network (on the engine thread)

    // Single-evaluator guard: serialize periodic evaluation here with the
    // actor thread's event-driven evaluation (processEventDrivenSignalUpdate)
    // so they never touch engine state concurrently.  TRY-lock: an RT host
    // must never block on an in-flight event-island evaluation (which can run
    // arbitrarily long, e.g. ANN inference) -- on contention, return Busy and
    // let the host retry next cycle (one late tick, same tolerance as GC
    // lateness).  recursive_mutex::try_lock succeeds re-entrantly, so the
    // internal resume path is unaffected.  Released on every return,
    // including the Yielded budget-slice path, so the actor thread gets the
    // engine between our slices.
    std::unique_lock<std::recursive_mutex> evalLock(m_evalMutex, std::try_to_lock);
    if (!evalLock.owns_lock())
        return TickResult::Busy;

    auto deadline = TimePoint::currentTime() + budget;

    // If we have yielded work, check for overrun then resume
    if (m_yieldState.active) {
        // Check if we've overrun the tick period
        auto elapsed = TimePoint::currentTime() - m_yieldState.tickTime;
        if (m_tickPeriod > TimeDuration::zero() && elapsed >= m_tickPeriod) {
            // Tick has exceeded its period - overrun error.  If a specific
            // func was mid-execution across slices when the period expired,
            // it is the culprit -- name it.  (A boundary yield with no func
            // in flight means the tick's total work didn't fit; the host's
            // own cycle timing covers that case.)
            if (m_yieldState.funcWasExecuting && m_yieldState.yieldedFunc) {
                recordNodeOverrun(m_yieldState.yieldedFunc->name(),
                                  elapsed, elapsed - m_tickPeriod);
            }
            m_yieldState.active = false;
            return TickResult::Overrun;
        }
        return resumeTickEvaluation(deadline);
    }

    // Start a new tick
    if (m_networkModified)
        buildNetworkCacheData();

    if (m_tickPeriod == TimeDuration::zero())
        return TickResult::Complete;

    if (m_runStart == TimePoint::zero())
        m_runStart = nextPeriodOnPeriodBoundary(m_tickPeriod);

    m_tickStart = m_runStart + m_tickPeriod * m_tickNumber;

    // Tick all source signals that need it on this tick
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        for (const auto& signal : signals) {
            if (signal->isSourceSignal()) {
                if (signal->period() == TimeDuration::zero())
                    continue;

                // Background-island sources are ticked by
                // serviceBackgroundIslands() on the engine thread, on the
                // island's own schedule -- ticking them here too would
                // double-advance them.
                if (signal->inBackgroundIsland())
                    continue;

                // Should this source tick now?
                if ((m_tickStart.load() % signal->period()) == TimeDuration::zero()) {
                    signal->tick(m_tickStart);
                    updateSignalConsumerInputAvailability(signal, m_tickStart);
                }
            }
        }
    }

    // Evaluate islands with deadline support
    std::vector<NetworkIsland> islandsCopy;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        islandsCopy = m_networkIslands;
    }

    for (size_t i = 0; i < islandsCopy.size(); ++i) {
        // Event-driven islands (no periodic source, tickPeriod zero) are
        // evaluated when their inputs are set() -- inline on VM threads or
        // via the pending-event queue drained on the engine's actor thread.
        // They do NOT belong on the host's periodic schedule: their work is
        // unbounded relative to an RT budget (e.g. camera-frame tensor
        // transforms), and scheduling them here starves the host's
        // remaining budget via the yield/resume path.  Background-domain
        // islands are likewise off the host's schedule -- the engine thread
        // services them (serviceBackgroundIslands) with no tick budget.
        if (islandsCopy[i].tickPeriod == TimeDuration::zero()
            || islandsCopy[i].background)
            continue;

        TimePoint islandTime = resolveEvaluationTime(islandsCopy[i], m_tickStart);
        auto result = evaluateIsland(islandsCopy[i], islandTime, deadline);

        if (result == TickResult::Yielded) {
            m_yieldState.active = true;
            m_yieldState.islandIndex = i;
            m_yieldState.tickTime = m_tickStart;
            return TickResult::Yielded;
        }

        if (result == TickResult::Error)
            return TickResult::Error;
    }

    invokeTickCallbacks();
    m_tickNumber++;
    return TickResult::Complete;
}


DataflowEngine::TickResult DataflowEngine::resumeTickEvaluation(TimePoint deadline)
{
    if (!m_yieldState.active)
        return TickResult::Error;

    if (m_networkModified) {
        // Network changed - cannot safely resume
        m_yieldState.active = false;
        return TickResult::Error;
    }

    std::vector<NetworkIsland> islandsCopy;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        islandsCopy = m_networkIslands;
    }

    // If a func was mid-execution, resume it first
    if (m_yieldState.funcWasExecuting && m_yieldState.yieldedFunc) {
        const TimePoint resumeStart = TimePoint::currentTime();
        auto result = m_yieldState.yieldedFunc->resumeExecution(deadline);
        if (result == FuncExecResult::Yielded)
            return TickResult::Yielded;

        // Same attribution as the fresh-execution site: completing past the
        // deadline means this resume slice was blown by the node.
        {
            const TimePoint resumeEnd = TimePoint::currentTime();
            if (result == FuncExecResult::Completed && resumeEnd > deadline) {
                recordNodeOverrun(m_yieldState.yieldedFunc->name(),
                                  resumeEnd - resumeStart,
                                  resumeEnd - deadline);
            }
        }

        if (result == FuncExecResult::Error) {
            m_yieldState.active = false;
            return TickResult::Error;
        }

        // Func completed - update outputs and continue
        m_yieldState.funcWasExecuting = false;
        for (auto& output : m_yieldState.yieldedFunc->m_outputs)
            updateSignalConsumerInputAvailability(output.signal, m_yieldState.tickTime);
        m_yieldState.funcIndex++;
        m_yieldState.yieldedFunc = nullptr;
    }

    // Continue evaluating from saved position
    for (size_t i = m_yieldState.islandIndex; i < islandsCopy.size(); ++i) {
        // Same event-island + background-island skip as tickFor's fresh-tick
        // loop (indices into islandsCopy stay aligned -- we skip, not remove).
        if (islandsCopy[i].tickPeriod == TimeDuration::zero()
            || islandsCopy[i].background)
            continue;

        size_t startPeriodIndex = (i == m_yieldState.islandIndex) ? m_yieldState.periodIndex : 0;
        size_t startFuncIndex = (i == m_yieldState.islandIndex) ? m_yieldState.funcIndex : 0;

        TimePoint islandTime = resolveEvaluationTime(islandsCopy[i], m_yieldState.tickTime);
        auto result = evaluateIsland(
            islandsCopy[i], islandTime, deadline, startPeriodIndex, startFuncIndex);

        if (result == TickResult::Yielded) {
            m_yieldState.islandIndex = i;
            return TickResult::Yielded;
        }

        if (result == TickResult::Error) {
            m_yieldState.active = false;
            return TickResult::Error;
        }

        // Reset indices for next island
        m_yieldState.funcIndex = 0;
        m_yieldState.periodIndex = 0;
    }

    m_yieldState.active = false;
    invokeTickCallbacks();
    m_tickNumber++;
    return TickResult::Complete;
}


DataflowEngine::TickResult DataflowEngine::evaluateIsland(
    const NetworkIsland& island,
    TimePoint evaluationTime,
    TimePoint deadline,
    size_t startPeriodIndex,
    size_t startFuncIndex)
{
    if (island.funcs.empty())
        return TickResult::Complete;

    // Only refresh derived signals on first entry (not resume)
    if (startPeriodIndex == 0 && startFuncIndex == 0)
        refreshDerivedSignals(island, evaluationTime);

    // Convert executionOrders map to a vector for indexed access
    std::vector<std::pair<TimeDuration, std::vector<ptr<FuncNode>>>> orderedPeriods(
        island.executionOrders.begin(), island.executionOrders.end());

    uint64_t functionsExecuted;
    int32_t iterations = 0;

    do {
        functionsExecuted = 0;

        for (size_t periodIdx = startPeriodIndex; periodIdx < orderedPeriods.size(); ++periodIdx) {
            const auto& funcsForPeriod = orderedPeriods[periodIdx].second;
            size_t funcStart = (periodIdx == startPeriodIndex) ? startFuncIndex : 0;

            for (size_t funcIdx = funcStart; funcIdx < funcsForPeriod.size(); ++funcIdx) {
                const auto& func = funcsForPeriod[funcIdx];

                // RT GC yield: if the evaluating thread holds a GC yield
                // section (an RT host slice) and a collection is now pending,
                // suspend at this func boundary via the normal resumable
                // yield -- bounding the collector's wait on the RT slice to
                // ~one FuncNode instead of the rest of the tick.  Only
                // section holders trigger this; actor-thread event
                // evaluation is unaffected.
                if (roxal::SimpleMarkSweepGC::inGCYieldSectionOnThisThread() &&
                    roxal::SimpleMarkSweepGC::instance().isCollectionRequested()) {
                    m_yieldState.periodIndex = periodIdx;
                    m_yieldState.funcIndex = funcIdx;
                    m_yieldState.funcWasExecuting = false;
                    m_yieldState.yieldedFunc = nullptr;
                    return TickResult::Yielded;
                }

                if (!func->inputsAvailableAt(evaluationTime))
                    continue;

                // Budgeted evaluations time each node: a Completed result
                // that lands past the deadline means the node could not
                // yield (native func, or script between yield points) --
                // attribute the overrun to it.  A Yielded result is the
                // cooperative path and isn't charged.
                const bool budgeted = deadline != TimePoint::max();
                const TimePoint execStart =
                    budgeted ? TimePoint::currentTime() : TimePoint::zero();

                auto result = func->conditionallyExecute(evaluationTime, deadline);

                if (result == FuncExecResult::Completed) {
                    if (budgeted) {
                        const TimePoint execEnd = TimePoint::currentTime();
                        if (execEnd > deadline) {
                            recordNodeOverrun(func->name(),
                                              execEnd - execStart,
                                              execEnd - deadline);
                        }
                    }
                    functionsExecuted++;
                    for (auto& output : func->m_outputs)
                        updateSignalConsumerInputAvailability(output.signal, evaluationTime);
                }
                else if (result == FuncExecResult::Yielded) {
                    // Save position for resume
                    m_yieldState.periodIndex = periodIdx;
                    m_yieldState.funcIndex = funcIdx;
                    m_yieldState.funcWasExecuting = true;
                    m_yieldState.yieldedFunc = func;
                    return TickResult::Yielded;
                }
                else if (result == FuncExecResult::Error) {
                    return TickResult::Error;
                }
                // FuncExecResult::NotExecuted - continue to next func
            }
        }

        // After first full pass, reset start indices
        startPeriodIndex = 0;
        startFuncIndex = 0;

        iterations++;
        if (iterations > island.funcs.size() * 100)
            throw std::runtime_error("DataflowEngine: func execution didn't terminate - check for signal dependency cycles");

    } while (functionsExecuted > 0);

    return TickResult::Complete;
}


TimePoint DataflowEngine::resolveEvaluationTime(const NetworkIsland& island, TimePoint candidate) const
{
    bool haveSamples = false;
    TimePoint maxEarliest = TimePoint::microSecs(std::numeric_limits<int64_t>::min());

    for (const auto& signal : island.signals) {
        if (!signal)
            continue;

        std::lock_guard<std::recursive_mutex> lock(signal->m_valuesMutex);
        if (signal->values.empty())
            continue;

        haveSamples = true;

        TimePoint earliest = signal->values.begin()->first;

        if (earliest > maxEarliest)
            maxEarliest = earliest;
    }

    if (!haveSamples)
        return candidate;

    if (candidate < maxEarliest)
        return maxEarliest;

    return candidate;
}

void DataflowEngine::processEventDrivenSignalUpdate(ptr<Signal> signal, TimePoint timestamp)
{
    // Island evaluation executes FuncNode closures, which requires the
    // calling thread to be a VM thread (VM::thread set up). Producers on
    // foreign threads -- e.g. the DDS reader-signal thread -- hand the
    // update off to the engine's run loop, which drains the queue on its
    // own actor thread. (Evaluating in place there used to corrupt/crash:
    // invokeClosure on a thread with no VM Thread state.)
    if (roxal::VM::thread == nullptr) {
        // Wrap before taking the queue lock: ObjSignal construction touches
        // the engine mutex (wrapper registration).
        roxal::Value wrapper = roxal::Value::signalVal(std::move(signal));
        {
            std::lock_guard<std::mutex> lock(m_pendingEventMutex);
            m_pendingEventUpdates.emplace_back(std::move(wrapper), timestamp);
        }
        // Rouse the engine thread's idle drain sleep.  State was modified
        // under m_pendingEventMutex, so the waiter's predicate cannot miss it.
        m_pendingEventCv.notify_all();
        return;
    }

    // Bounded event pump: island evaluation runs closures, and a closure can
    // set() another event-driven signal -- which lands right back here on
    // the same thread.  Evaluating that nested update in place would recurse
    // one C++ frame per chain link (m_evalMutex is recursive), so a long
    // handler chain -- or a cycle -- becomes stack exhaustion.  Instead,
    // only the OUTERMOST call on a thread evaluates: nested calls append to
    // a per-thread queue that the outermost call drains iteratively after
    // the current island completes.  Depth stays O(1); a chained update now
    // evaluates after the in-flight island finishes instead of preempting
    // it mid-evaluation (the island's remaining funcs see consistent
    // pre-chain inputs).
    static thread_local bool tlEvaluatingEventChain = false;
    static thread_local std::vector<std::pair<ptr<Signal>, TimePoint>> tlChainedUpdates;

    if (tlEvaluatingEventChain) {
        tlChainedUpdates.emplace_back(std::move(signal), timestamp);
        return;
    }

    // Single-evaluator guard: serialize this event-driven evaluation with
    // tickFor()'s periodic evaluation so the two drivers never mutate engine
    // state concurrently.  Held only around the evaluation itself (and the
    // chain drain), NOT the foreign-thread queue path above (which merely
    // enqueues).  See m_evalMutex.
    //
    // Contended acquisition is covered as GC-safe blocking: the current
    // evaluator can park at a safepoint mid-island while holding the mutex,
    // and a waiter still counted as Running would stall the collection
    // barrier against it -- a circular wait.  RT yield-section holders never
    // take the covered path: a section's tickFor slice already holds
    // m_evalMutex recursively, so their try-lock succeeds immediately (and
    // they must not touch GC coordination state anyway).
    std::unique_lock<std::recursive_mutex> evalLock(m_evalMutex, std::try_to_lock);
    if (!evalLock.owns_lock()) {
        if (roxal::SimpleMarkSweepGC::inGCYieldSectionOnThisThread()) {
            evalLock.lock();
        } else {
            roxal::SimpleMarkSweepGC::GCSafeBlockScope blockCover;
            evalLock.lock();
        }
    }

    tlEvaluatingEventChain = true;
    struct ChainGuard {
        // Reset on ALL exits: an evaluation throw must not leave the thread
        // marked mid-chain (every later set() would defer with no drainer).
        // Pending chained updates are dropped with it -- same outcome as
        // the abandoned recursion.
        ~ChainGuard() { tlEvaluatingEventChain = false; tlChainedUpdates.clear(); }
    } chainGuard;

    ptr<Signal> nextSignal = std::move(signal);
    TimePoint nextTime = timestamp;
    uint64_t chained = 0;
    // A handler chain that never converges (cross-island update cycle) was
    // previously unbounded recursion (stack overflow); keep it a detectable
    // error rather than a silent spin.
    constexpr uint64_t kMaxChainedUpdates = 100000;

    for (;;) {
        if (m_networkModified)
            buildNetworkCacheData();

        NetworkIsland islandSnapshot;
        bool found = false;

        {
            std::lock_guard<std::recursive_mutex> lock(m_mutex);
            for (const auto& island : m_networkIslands) {
                if (std::find(island.signals.begin(), island.signals.end(), nextSignal) != island.signals.end()) {
                    islandSnapshot = island;
                    found = true;
                    break;
                }
            }
        }

        if (found)
            evaluateIsland(islandSnapshot, nextTime);

        if (tlChainedUpdates.empty())
            break;

        if (++chained > kMaxChainedUpdates)
            throw std::runtime_error(
                "DataflowEngine: event-driven update chain did not converge "
                "(signal handlers keep setting each other; check for an "
                "update cycle across islands)");

        nextSignal = std::move(tlChainedUpdates.front().first);
        nextTime = tlChainedUpdates.front().second;
        tlChainedUpdates.erase(tlChainedUpdates.begin());
    }
}

void DataflowEngine::traceAllSignals(roxal::ValueVisitor& visitor)
{
    // Runs on the collector with the GC mutex held.  Lock order:
    // GC mutex_ -> m_mutex -> (per-signal) m_valuesMutex.  Safe: no code
    // parks at a GC safepoint or allocates GC objects while holding m_mutex,
    // so no holder can block against the collector; Signal::trace already
    // takes m_valuesMutex from GC context (via ObjSignal::trace).
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::set<const Signal*> seen;
    auto traceOne = [&](const ptr<Signal>& s) {
        if (!s)
            return;
        if (!seen.insert(s.get()).second)
            return;
        s->trace(visitor);
    };
    for (const auto& s : signals)
        traceOne(s);
    for (const auto& island : m_networkIslands)
        for (const auto& s : island.signals)
            traceOne(s);

    // FuncNodes retain strong Values too (closure, const args, defaults,
    // previous inputs/outputs, yielded execution state) -- trace every node
    // the engine holds, incl. stale-island copies pending a rebuild.
    std::set<const FuncNode*> seenFuncs;
    auto traceFunc = [&](const ptr<FuncNode>& f) {
        if (!f)
            return;
        if (!seenFuncs.insert(f.get()).second)
            return;
        f->trace(visitor);
    };
    for (const auto& entry : funcs)
        traceFunc(entry.second);
    for (const auto& island : m_networkIslands) {
        for (const auto& f : island.funcs)
            traceFunc(f);
        for (const auto& order : island.executionOrders)
            for (const auto& f : order.second)
                traceFunc(f);
    }
}

void DataflowEngine::tracePendingEventUpdates(roxal::ValueVisitor& visitor)
{
    std::lock_guard<std::mutex> lock(m_pendingEventMutex);
    for (const auto& upd : m_pendingEventUpdates) {
        if (upd.first.isObj())
            visitor.visit(upd.first);
    }
    // Also root the batch currently being drained (see m_drainingEventUpdates):
    // the draining thread may be parked at a safepoint mid-evaluation.
    for (const auto& upd : m_drainingEventUpdates) {
        if (upd.first.isObj())
            visitor.visit(upd.first);
    }
}

bool DataflowEngine::processPendingEventUpdates()
{
    // Single-consumer enforcement: the always-on actor loop and a
    // host-driven runFor() can call this concurrently, and the batch below
    // lives in a MEMBER -- a second drainer swapping into
    // m_drainingEventUpdates while the first iterates it would clobber the
    // vector under it (lost updates, UAF).  Losing is benign: the winner
    // owns the whole batch, and producers notify m_pendingEventCv on
    // enqueue, so a backed-off drainer re-wakes.
    std::unique_lock<std::mutex> drainLock(m_eventDrainMutex, std::try_to_lock);
    if (!drainLock.owns_lock())
        return false;

    {
        std::lock_guard<std::mutex> lock(m_pendingEventMutex);
        if (m_pendingEventUpdates.empty())
            return false;
        // Swap into the MEMBER batch (not a local): the wrapper Values must
        // stay visible to tracePendingEventUpdates() while we evaluate below
        // -- this thread can park at a GC safepoint mid-drain (closure
        // FuncNode evaluation), and a drain-local batch would be unrooted
        // there.  m_drainingEventUpdates is empty here: only this function
        // fills it, m_eventDrainMutex enforces a single drainer, and it is
        // emptied before return.
        m_drainingEventUpdates.swap(m_pendingEventUpdates);
    }

    // Coalesce to the newest timestamp per underlying signal (distinct
    // wrapper Values can refer to the same signal): islands re-read current
    // signal values, so evaluating once per signal per drain is enough.
    // (Reading m_drainingEventUpdates without the lock is fine: only this
    // thread mutates it, and the tracer only READS it under the lock.)
    std::vector<std::pair<ptr<Signal>, TimePoint>> latest;
    for (auto& upd : m_drainingEventUpdates) {
        if (!roxal::isSignal(upd.first))
            continue;
        ptr<Signal> sig = roxal::asSignal(upd.first)->signal;
        if (!sig)
            continue;
        auto it = std::find_if(latest.begin(), latest.end(),
                               [&](const auto& e){ return e.first == sig; });
        if (it == latest.end())
            latest.emplace_back(std::move(sig), upd.second);
        else if (upd.second > it->second)
            it->second = upd.second;
    }
    for (auto& upd : latest)
        processEventDrivenSignalUpdate(upd.first, upd.second);

    // Release the batch.  Swap out under the lock, destruct OUTSIDE it: the
    // wrapper decRefs can hit unregisterAllocation (GC mutex), and the GC's
    // root scan takes m_pendingEventMutex while holding its own mutex --
    // never touch GC state while holding m_pendingEventMutex.
    std::vector<std::pair<roxal::Value, TimePoint>> done;
    {
        std::lock_guard<std::mutex> lock(m_pendingEventMutex);
        done.swap(m_drainingEventUpdates);
    }
    return true;
}


void DataflowEngine::initializeNode(const ptr<FuncNode>& node)
{
    if (!node)
        return;

    // Same serialization as evaluate(): runs on SCRIPT threads (func lifts /
    // signal operators) while the engine thread evaluates and the reclaimer
    // thread destroys dead signal wrappers.
    std::unique_lock<std::recursive_mutex> evalLock(m_evalMutex, std::try_to_lock);
    if (!evalLock.owns_lock()) {
        if (roxal::SimpleMarkSweepGC::inGCYieldSectionOnThisThread()) {
            evalLock.lock();
        } else {
            roxal::SimpleMarkSweepGC::GCSafeBlockScope blockCover;
            evalLock.lock();
        }
    }

    if (m_networkModified)
        buildNetworkCacheData();

    // Evaluate at the newest time the inputs actually carry information for.
    // NOT m_tickStart (while the engine sleeps toward the next boundary it is
    // in the FUTURE, and a future-stamped value would shadow every
    // event-driven set() for up to a tick) and NOT a fabricated zero (which
    // made a node lifted into a running network start out stale, and made the
    // result depend on creation order).  Inputs that last sampled earlier
    // zero-order-hold through valueAt(), exactly as on a tick.
    TimePoint evalTime = TimePoint::zero();
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        for (const auto& input : node->m_inputs) {
            if (!input.signal)
                continue;
            // Cold start: a clock that has never been evaluated has no
            // samples yet, so give it its t=0 entry (what evaluate() used to
            // do for every source signal, now only where it is needed).
            if (input.signal->isSourceSignal() && !input.signal->hasValues())
                input.signal->evaluate(TimePoint::zero());
            auto latest = input.signal->latestSampleTime();
            if (latest > evalTime)
                evalTime = latest;
            updateSignalConsumerInputAvailability(input.signal, latest);
        }
    }

    // A node whose inputs carry nothing yet has nothing to initialize from;
    // it simply computes on the island's next tick.  Same guard the tick path
    // applies in evaluateIsland().
    if (!node->inputsAvailableAt(evalTime))
        return;

    node->conditionallyExecute(evalTime);

    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        for (const auto& output : node->m_outputs)
            if (output.signal)
                updateSignalConsumerInputAvailability(output.signal, evalTime);
    }
}


void DataflowEngine::evaluate()
{
    // Runs on SCRIPT threads (func lifts / signal operators) while the
    // engine thread evaluates and the reclaimer thread destroys dead
    // signal wrappers (removeSignal mutates `signals`).  Serialize with
    // the other evaluators; a contended wait is covered as GC-safe
    // blocking (the in-flight evaluator can park at a safepoint while
    // holding the mutex).
    std::unique_lock<std::recursive_mutex> evalLock(m_evalMutex, std::try_to_lock);
    if (!evalLock.owns_lock()) {
        if (roxal::SimpleMarkSweepGC::inGCYieldSectionOnThisThread()) {
            evalLock.lock();
        } else {
            roxal::SimpleMarkSweepGC::GCSafeBlockScope blockCover;
            evalLock.lock();
        }
    }

    if (m_networkModified)
        buildNetworkCacheData();

    // Initialization is stamped at TIME ZERO -- the same convention as the
    // Signal constructor's initial entry.  m_tickStart must NOT be used
    // here: while the engine sleeps toward the next boundary, m_tickStart
    // is in the FUTURE, and sampling reads the latest-timestamped entry,
    // so a future-stamped initial value shadows every event-driven set()
    // for up to one tick period after a func lift (each lift re-runs
    // this).  Zero is also aligned to every signal's period grid and
    // always precedes tickStart, so the periodic-write diagnostics never
    // flag initialization writes.
    const TimePoint evalTime = TimePoint::zero();

    // Ensure all source signals have values at evaluation time.  m_mutex
    // guards the container iteration: `signals` is mutated concurrently
    // (removeSignal on the reclaimer thread destroying dead wrappers) --
    // iterating unlocked dereferences a dangling Signal*.  Held only over
    // this loop; evaluateNetwork below runs closures (can park at a GC
    // safepoint) and copies its islands under m_mutex internally.
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        for(const auto& signal : signals) {
            if (signal->isSourceSignal()) {
                signal->evaluate(evalTime);
            }
            updateSignalConsumerInputAvailability(signal, signal->latestSampleTime());
        }
    }

    evaluateNetwork(evalTime);
}


void DataflowEngine::buildSignalConsumers()
{
    signalConsumers.clear();
    for(auto& funcNamePtr : funcs) {
        ptr<FuncNode> func = funcNamePtr.second;
        // Build the signalConsumers mapping
        for (const auto& input : func->m_inputs) {
            TimeDuration latency = input.signal->period() * -input.index;
            FuncInputInfo info = {func, latency};
            signalConsumers[input.signal].push_back(info);

            // update signal's maximum required history
            input.signal->m_maxHistoryPeriods = std::max(input.signal->m_maxHistoryPeriods, (-input.index)+1);
        }
    }
}


void DataflowEngine::precomputeFuncPeriods()
{
    for (const auto& funcPair : funcs) {
        ptr<FuncNode> func = funcPair.second;

        // Collect input periods
        std::set<TimeDuration> inputPeriods {};
        for (const auto& input : func->m_inputs) {
            auto signalPeriod = input.signal->period();
            if (signalPeriod > TimeDuration::zero()) {
                inputPeriods.insert(signalPeriod);
            }
        }

        // Compute longest common multiple of input periods
        func->m_period = longestDividingPeriod(inputPeriods);
    }
}


void DataflowEngine::computeNetworkIslands()
{
    m_networkIslands.clear();

    std::map<ptr<Signal>, std::vector<ptr<FuncNode>>> signalProducers;
    for (const auto& funcPair : funcs) {
        const auto& func = funcPair.second;
        for (const auto& output : func->m_outputs) {
            signalProducers[output.signal].push_back(func);
        }
    }

    std::map<ptr<Signal>, bool> visitedSignals;
    std::map<ptr<FuncNode>, bool> visitedFuncs;

    auto enqueueSignal = [&](std::deque<ptr<Signal>>& queue, const ptr<Signal>& signal) {
        if (!signal)
            return;
        if (visitedSignals[signal])
            return;
        queue.push_back(signal);
    };

    auto enqueueFunc = [&](std::deque<ptr<FuncNode>>& queue, const ptr<FuncNode>& func) {
        if (!func)
            return;
        if (visitedFuncs[func])
            return;
        queue.push_back(func);
    };

    auto processQueues = [&](NetworkIsland& island,
                             std::deque<ptr<Signal>>& signalQueue,
                             std::deque<ptr<FuncNode>>& funcQueue) {
        while (!signalQueue.empty() || !funcQueue.empty()) {
            if (!signalQueue.empty()) {
                auto currentSignal = signalQueue.front();
                signalQueue.pop_front();
                if (!currentSignal || visitedSignals[currentSignal])
                    continue;

                visitedSignals[currentSignal] = true;
                island.signals.push_back(currentSignal);

                auto consumersIt = signalConsumers.find(currentSignal);
                if (consumersIt != signalConsumers.end()) {
                    for (const auto& consumer : consumersIt->second)
                        enqueueFunc(funcQueue, consumer.func);
                }

                auto producersIt = signalProducers.find(currentSignal);
                if (producersIt != signalProducers.end()) {
                    for (const auto& producer : producersIt->second)
                        enqueueFunc(funcQueue, producer);
                }

                if (currentSignal->isDerived) {
                    auto base = currentSignal->baseSignal.lock();
                    if (base)
                        enqueueSignal(signalQueue, base);
                }

                continue;
            }

            auto currentFunc = funcQueue.front();
            funcQueue.pop_front();
            if (!currentFunc || visitedFuncs[currentFunc])
                continue;

            visitedFuncs[currentFunc] = true;
            island.funcs.push_back(currentFunc);

            for (const auto& input : currentFunc->m_inputs)
                enqueueSignal(signalQueue, input.signal);

            for (const auto& output : currentFunc->m_outputs)
                enqueueSignal(signalQueue, output.signal);
        }
    };

    std::deque<ptr<Signal>> signalQueue;
    std::deque<ptr<FuncNode>> funcQueue;

    for (const auto& signal : signals) {
        if (visitedSignals[signal])
            continue;

        NetworkIsland island;
        signalQueue.clear();
        funcQueue.clear();
        enqueueSignal(signalQueue, signal);
        processQueues(island, signalQueue, funcQueue);

        if (!island.signals.empty() || !island.funcs.empty())
            m_networkIslands.push_back(std::move(island));
    }

    for (const auto& funcPair : funcs) {
        auto func = funcPair.second;
        if (visitedFuncs[func])
            continue;

        NetworkIsland island;
        signalQueue.clear();
        funcQueue.clear();
        enqueueFunc(funcQueue, func);
        processQueues(island, signalQueue, funcQueue);

        if (!island.signals.empty() || !island.funcs.empty())
            m_networkIslands.push_back(std::move(island));
    }
}

void DataflowEngine::precomputeExecutionOrders(NetworkIsland& island)
{
    island.executionOrders.clear();

    if (island.funcs.empty())
        return;

    // Map from execution intervals to functions
    std::map<TimeDuration, std::vector<ptr<FuncNode>>> executionGroups;

    // group orderings by execution interval
    for (const auto& func : island.funcs) {
        executionGroups[func->m_period].push_back(func);
    }

    // For each execution group, precompute the execution order
    for (const auto& groupPair : executionGroups) {
        TimeDuration interval = groupPair.first;
        const std::vector<ptr<FuncNode>>& funcsInGroup = groupPair.second;

        // Build the dependency graph for this group
        DependencyGraph depGraph;

        // Initialize the dependency graph nodes
        for (const auto& func : funcsInGroup) {
            depGraph[func] = {};
        }

        // Build edges in the dependency graph
        for (const auto& func : funcsInGroup) {
            for (const auto& input : func->m_inputs) {
                // Find if the input stream is produced by another func in this group
                for (const auto& potentialProducer : funcsInGroup) {
                    if (potentialProducer == func) continue; // Skip self

                    for (const auto& output : potentialProducer->m_outputs) {
                        if((output.signal == input.signal) && (input.index == 0)) {
                            // There is a dependency: func depends on potentialProducer
                            depGraph[func].insert(potentialProducer);
                        }
                    }
                }
            }
        }

        // Perform topological sort on the dependency graph
        std::vector<ptr<FuncNode>> sortedFuncs {};
        if (!topologicalSort(depGraph, sortedFuncs)) {
            throw std::runtime_error("Cyclic dependency detected among functions in execution group.");
        }

        // Store the precomputed execution order for this interval
        island.executionOrders[interval] = sortedFuncs;
    }

    #if 0 //defined(DEBUG_ENGINE)
    std::cout << "Execution Groups Dump:" << std::endl;
    std::cout << "=====================" << std::endl;

    for (const auto& groupPair : island.executionOrders) {
        TimeDuration interval = groupPair.first;
        const std::vector<ptr<FuncNode>>& funcsInGroup = groupPair.second;

        std::cout << "Interval: " << interval.microSecs() << " microseconds" << std::endl;
        std::cout << "Functions in this group:" << std::endl;

        for (const auto& func : funcsInGroup) {
            std::cout << "  - " << func->name() << std::endl;

            std::cout << "    Inputs:" << std::endl;
            for (const auto& input : func->m_inputs) {
                std::cout << "      " << input.name << " (Signal: " << input.signal->name()
                        << ", Latency: " << input.latency.microSecs() << " microseconds)" << std::endl;
            }

            std::cout << "    Outputs:" << std::endl;
            for (const auto& output : func->m_outputs) {
                std::cout << "      " << output.name << " (Signal: " << output.signal->name() << ")" << std::endl;
            }
        }

        std::cout << "Precomputed Execution Order:" << std::endl;
        for (const auto& func : island.executionOrders[interval]) {
            std::cout << "  " << func->name() << std::endl;
        }

        std::cout << std::endl;
    }
    #endif
}


bool DataflowEngine::topologicalSort(const DependencyGraph& depGraph,
                                   std::vector<ptr<FuncNode>>& sortedFuncs)
{
    std::map<ptr<FuncNode>, bool> tempMark {};
    std::map<ptr<FuncNode>, bool> permMark {};
    sortedFuncs.clear();

    std::function<bool(ptr<FuncNode>)> visit = [&](ptr<FuncNode> node) {
        if (permMark[node]) {
            return true; // Already visited
        }
        if (tempMark[node]) {
            return false; // Not a DAG, cyclic dependency
        }
        tempMark[node] = true;
        for (const auto& m : depGraph.at(node)) {
            if (!visit(m)) {
                return false;
            }
        }
        tempMark[node] = false;
        permMark[node] = true;
        sortedFuncs.push_back(node);
        return true;
    };

    for (const auto& kv : depGraph) {
        if (!permMark[kv.first]) {
            if (!visit(kv.first)) {
                return false; // Cyclic dependency detected
            }
        }
    }

    return true;
}


void DataflowEngine::updateSignalConsumerInputAvailability(ptr<Signal> signal, TimePoint signalTickedTime)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    // For each function that consumes this signal
    auto consumers = signalConsumers[signal];
    for (auto& inputInfo : consumers) {
        ptr<FuncNode> func = inputInfo.func;
        //auto latency = inputInfo.latency;

        // Update the input port's latest available time
        for (auto& inputPort : func->m_inputs) {
            if (inputPort.signal == signal) {
                inputPort.latestAvailableTime = signalTickedTime;
            }
        }
    }
}


void DataflowEngine::executeFunctionsInOrder(
    const std::vector<ptr<FuncNode>>& funcsToExecute,
    const std::map<TimeDuration, std::vector<ptr<FuncNode>>>& executionOrders) {
    if (funcsToExecute.empty()) return;

    // Determine the interval for these functions
    TimeDuration interval = computeExecutionInterval(funcsToExecute.front());

    #ifdef DEBUG_BUILD
    // ensure all the functions have the same interval
    for(const auto& func : funcsToExecute)
        assert(interval == computeExecutionInterval(func));
    #endif

    // Retrieve the precomputed execution order
    auto it = executionOrders.find(interval);
    if (it == executionOrders.end())
        return;

    const auto& precomputedOrder = it->second;
    #if 0
    std::cout << "DataflowEngine::executeFunctionsInOrder precomputedOrder.size=" << precomputedOrder.size() << "  funcsToExecute.size=" << funcsToExecute.size()<< std::endl;//!!!
    Names funcNamesToExecute;
    for (const auto& func : funcsToExecute)
        funcNamesToExecute.push_back(func->name());
    std::cout << "funcsToExecute:" << join(funcNamesToExecute, ", ") << std::endl;
    #endif

    // Execute functions in precomputed order
    for (const auto& func : precomputedOrder) {
        // Only execute if func is in funcsToExecute
        if (std::find(funcsToExecute.begin(), funcsToExecute.end(), func) != funcsToExecute.end()) {
            // Execute the function
//!!!            processFunctionExecuteEvent({Event::Type::FuncExecute, funcScheduledTimes[func], nullptr, func});
        }
    }
}


TimeDuration DataflowEngine::longestDividingPeriod(const std::set<TimeDuration>& periods) {
    if (periods.empty()) return TimeDuration::zero();

    auto gcd = periods.begin()->microSecs();
    for (const auto& period : periods) {
        gcd = std::gcd(gcd, period.microSecs());
    }

    return TimeDuration::microSecs(gcd);
}


TimeDuration DataflowEngine::computeExecutionInterval(ptr<FuncNode> func) {
    // Compute the function's execution interval based on input signal periods
    std::set<TimeDuration> inputPeriods {};
    for (const auto& input : func->m_inputs) {
        auto signalPeriod = input.signal->period();
        if (signalPeriod > TimeDuration::zero()) {
            inputPeriods.insert(signalPeriod);
        }
    }
    return longestDividingPeriod(inputPeriods);
}





TimePoint DataflowEngine::nextPeriodOnPeriodBoundary(TimeDuration period) const
{
    return nextPeriodOnPeriodBoundary(period.frequency());
}

TimePoint DataflowEngine::nextPeriodOnPeriodBoundary(double freq) const
{
    #ifdef DEBUG_BUILD
    assert(freq>0.0);
    #endif
    TimeDuration period = TimeDuration::microSecs(int64_t(1000000ULL/freq));
    TimePoint nextPlusHalf = TimePoint::currentTime()+TimeDuration::microSecs(1.5*period.microSecs());
    TimePoint nextOnBoundary = nextPlusHalf - TimeDuration::microSecs(int64_t(nextPlusHalf.microSecs() % period.microSecs()));
    return nextOnBoundary;
}


std::map<std::string, Value> DataflowEngine::signalValues() const
{
    // Same container-race guard as evaluate(): callable from script
    // threads while the reclaimer removes dead signals/funcs.
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::map<std::string, Value> signalValues;
    for (auto& signal : signals) {
        signalValues[signal->name()] = signal->lastValueBefore(m_tickStart + m_tickPeriod);

        // if signal is used as input to a Func with <0 index, also include that value
        for(const auto& func : funcs) {
            for(auto input : func.second->m_inputs) {
                if ((input.signal == signal) && (input.index < 0)) {
                    TimePoint inputPrevTime = m_tickStart + m_tickPeriod + input.signal->period()*input.index;
                    Value priorValue = input.signal->lastValueBefore(inputPrevTime);
                    if (priorValue.isNil() && input.defaultValue.has_value())
                        priorValue = input.defaultValue.value();
                    signalValues[signal->name()+"["+std::to_string(input.index)+"]"] = priorValue;
                }
            }
        }
    }
    return signalValues;
}


// return the list of consumers for a signal (and which input name they consume it on)
std::vector<std::pair<ptr<FuncNode>, std::string>> DataflowEngine::consumersOfSignal(ptr<Signal> signal) const
{
    std::vector<std::pair<ptr<FuncNode>, std::string>> consumers;

    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    for (const auto& kv : funcs) {
        const auto& func = kv.second;
        for (size_t i = 0; i < func->m_inputs.size(); ++i) {
            const auto& in = func->m_inputs[i];
            if (in.signal == signal)
                consumers.emplace_back(func, in.name);
        }
    }
    return consumers;
}

// return the list of producers for a signal (and which output name they produce it on)
std::vector<std::pair<ptr<FuncNode>, std::string>> DataflowEngine::producersOfSignal(ptr<Signal> signal) const
{
    std::vector<std::pair<ptr<FuncNode>, std::string>> producers;

    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    for (const auto& kv : funcs) {
        const auto& func = kv.second;
        for (size_t i = 0; i < func->m_outputs.size(); ++i) {
            const auto& out = func->m_outputs[i];
            if (out.signal == signal)
                producers.emplace_back(func, out.name);
        }
    }
    return producers;
}



void DataflowEngine::addTickCallback(std::function<void(ptr<DataflowEngine>, TimePoint)> callback)
{
    m_tickCallbacks.push_back(callback);
}


void DataflowEngine::buildNetworkCacheData()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    #if 0
    std::cout << "Rebuilding network cached data" << std::endl;
    #endif

    // Build consumer map and compute connected components for the current network
    buildSignalConsumers();
    computeNetworkIslands();

    // Ensure function periods are available before computing execution order
    precomputeFuncPeriods();

    std::set<TimeDuration> globalSourcePeriods {};
    bool haveBackground = false;

    for (auto& island : m_networkIslands) {
        // Resolve the island's execution domain: background if ANY member
        // signal declares it.  Kept off the shared periodic schedule --
        // its source periods must not contribute to the global tick period
        // (a slow/odd background period would otherwise drag the shared
        // grid finer for everyone), and the tick paths skip it by the
        // per-signal derived flag.
        island.background = false;
        for (const auto& signal : island.signals) {
            if (signal->domain() == Signal::Domain::Background) {
                island.background = true;
                break;
            }
        }
        island.bgNextDue = TimePoint::zero();
        haveBackground |= island.background;
        for (const auto& signal : island.signals)
            signal->setInBackgroundIsland(island.background);

        std::set<TimeDuration> islandSourcePeriods {};

        // EVERY periodic signal contributes its declared period, not just the
        // sources.  Two reasons:
        //
        //  * A derived signal's period is normally inherited from its inputs
        //    (a node's output takes the max frequency of its inputs), so
        //    including it is a no-op -- its period is already in the set.
        //  * But `<-` makes its left side derived (copyInto adopts the right
        //    side's isSource), which ORPHANS the signal that carried the
        //    island's declared rate.  An island of pure feedback loops then
        //    has no source at all, contributes nothing, and lands on whatever
        //    grid the rest of the program happens to impose -- while freq()
        //    still reports the rate that was asked for.
        //
        // Because the grid is the GCD of every declared period, each one
        // divides it exactly, so every node can be gated to run at precisely
        // the rate it declared.
        for (const auto& signal : island.signals) {
            if (signal->period() == TimeDuration::zero())
                continue;   // event-driven: no place on a periodic grid

            islandSourcePeriods.insert(signal->period());
            if (!island.background)
                globalSourcePeriods.insert(signal->period());
        }

        island.tickPeriod = longestDividingPeriod(islandSourcePeriods);
        precomputeExecutionOrders(island);
    }

    m_haveBackgroundIslands.store(haveBackground, std::memory_order_relaxed);

    m_tickPeriod = longestDividingPeriod(globalSourcePeriods);

    m_tickNumber = 0;
    m_runStart = TimePoint::zero();

    m_networkModified = false;

    // Rebuilds happen on the ticking thread too (tickFor's fresh-tick path):
    // defer the lint scan + printing to the engine thread.
    if (m_hostDriven.load(std::memory_order_relaxed))
        m_rtLintPending.store(true, std::memory_order_relaxed);
}


void DataflowEngine::invokeTickCallbacks()
{
    for (const auto& callback : m_tickCallbacks) {
        #ifdef DEBUG_BUILD
        try {
            callback(ptr_from_this(), m_tickStart);
        } catch(const std::exception& e) {
            std::cerr << "Exception in tick callback " << e.what() << std::endl;
        }
        #else
        try { callback(ptr_from_this(), m_tickStart); } catch(...) {}
        #endif
    }

}



std::string DataflowEngine::graph() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::stringstream ss;
    ss << "Funcs:\n";

    for (const auto& [funcName, func] : funcs) {
        ss << "  " << funcName << "\n";
        ss << "    Inputs:\n";
        for(auto i=0; i<func->paramNames.size(); ++i) {
            auto paramName = func->paramNames[i];
            auto sigIndex = func->paramSignalIndex[i];
            std::string inputExpr {};
            if (sigIndex != -1) {
                auto inputSignal = func->signalArgs[func->paramSignalIndex[i]];
                inputExpr = inputSignal->name();
            }
            else {
                inputExpr = toString(func->constArgs[paramName]);
            }
            ss << "      " << paramName << ": " << inputExpr << "\n";
        }

        ss << "    Outputs:\n";
        for (const auto& output : func->m_outputs) {
            ss << "      " << output.name << ": " << output.signal->name() << "\n";
        }
    }

    ss << "Signals:\n";

    for (const auto& signal : signals) {
        if (signal->isInternal())
            continue;
        ss << "  " << signal->name() << "\n";
    }

    return ss.str();
}


std::string DataflowEngine::graphDot(const std::string& title, std::map<std::string,Value> signalValues) const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::stringstream dot;
    dot << "digraph Dataflow {\n";
    if (!title.empty())
        dot << "  label=\"" << title << "\"\n";
    dot << "  graph [rankdir=LR, fontname=\"Helvetica\", fontsize=10];\n";
    dot << "  node [shape=box, style=filled, fillcolor=darkgrey, fontname=\"Helvetica\", fontsize=10];\n";
    dot << "  edge [fontname=\"Helvetica\", fontsize=8];\n\n";

    std::map<ptr<Signal>, std::vector<std::pair<std::string, int>>> signalToDestFuncs;
    std::map<ptr<Signal>, std::string> signalToSourceFunc;

    // First pass: collect all signal connections
    for (const auto& [funcName, func] : funcs) {
        dot << "  \"" << funcName << "\" [label=\"" << funcName << "\"];\n";

        for (size_t i = 0; i < func->m_inputs.size(); ++i) {
            const auto& input = func->m_inputs[i];
            if (input.signal && input.signal->isInternal())
                continue;
            signalToDestFuncs[input.signal].push_back({funcName, input.index});
        }

        for (const auto& output : func->m_outputs) {
            if (output.signal && output.signal->isInternal())
                continue;
            signalToSourceFunc[output.signal] = funcName;
        }
    }

    // Second pass: create edges
    for (const auto& [signal, destFuncs] : signalToDestFuncs) {
        if (!signal || signal->isInternal())
            continue;
        std::string signalName = signal->name();
        std::string sourceFunc = signalToSourceFunc[signal];

        if (sourceFunc.empty()) {
            // Source signal
            dot << "  \"" << signalName << "\" [shape=ellipse];\n";
            for (const auto& [destFunc, index] : destFuncs) {
                std::string label = signalName;
                if (index != 0) {
                    label += "[" + std::to_string(index) + "]";
                }
                auto signalValueIt = signalValues.find(label);
                if (signalValueIt != signalValues.end()) {
                    label += " = " + roxal::toString(signalValueIt->second);
                }
                dot << "  \"" << signalName << "\" -> \"" << destFunc << "\" [label=\"" << label << "\"];\n";
            }
        } else {
            // Internal signal
            for (const auto& [destFunc, index] : destFuncs) {
                std::string label = signalName;
                if (index != 0) {
                    label += "[" + std::to_string(index) + "]";
                }
                auto signalValueIt = signalValues.find(label);
                if (signalValueIt != signalValues.end()) {
                    label += " = " + roxal::toString(signalValueIt->second);
                }
                dot << "  \"" << sourceFunc << "\" -> \"" << destFunc << "\" [label=\"" << label << "\"];\n";
            }
        }
    }

    // Handle sink signals
    for (const auto& [signal, sourceFunc] : signalToSourceFunc) {
        if (!signal || signal->isInternal())
            continue;
        if (signalToDestFuncs.find(signal) == signalToDestFuncs.end()) {
            std::string signalName = signal->name();
            dot << "  \"" << signalName << "\" [shape=ellipse];\n";
            std::string label = signalName;
            auto signalValueIt = signalValues.find(label);
            if (signalValueIt != signalValues.end()) {
                label += " = " + roxal::toString(signalValueIt->second);
            }

            dot << "  \"" << sourceFunc << "\" -> \"" << signalName << "\" [label=\"" << label << "\"];\n";
        }
    }

    // Show derived signal relationships
    for (const auto& signal : signals) {
        if (!signal || !signal->isDerived || signal->isInternal())
            continue;
        auto base = signal->baseSignal.lock();
        if (base && !base->isInternal()) {
            dot << "  \"" << base->name() << "\" -> \"" << signal->name()
                << "\" [label=\"[" << signal->baseIndex << "]\"];\n";
        }
    }

    dot << "}\n";
    return dot.str();
}


namespace {

DataflowEngine::FuncSnapshot snapshotFunc(const ptr<FuncNode>& func)
{
    DataflowEngine::FuncSnapshot fs;
    fs.id = func->id();
    fs.name = func->name();
    fs.period = func->period();
    fs.srcName = func->srcName();
    fs.srcLine = func->srcLine();
    fs.srcCol = func->srcCol();
    for (const auto& ip : func->inputPorts())
        fs.inputs.push_back({ ip.name, ip.signal, ip.index, ip.defaultValue.has_value() });
    for (const auto& op : func->outputPorts())
        fs.outputs.push_back({ op.name, op.signal, 0, false });
    return fs;
}

} // namespace

// Locking recipe (same as islandDebugSnapshot, proven from script-thread
// NativeFns via sys._df_islands): rebuild the network cache if dirty, then
// copy under m_mutex — no m_evalMutex, no Value reads, no parking under lock.
std::optional<DataflowEngine::NetworkSnapshot> DataflowEngine::subnetworkContaining(const ptr<Signal>& signal)
{
    if (m_networkModified)
        buildNetworkCacheData();

    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    for (const auto& island : m_networkIslands) {
        bool found = false;
        for (const auto& s : island.signals)
            if (s == signal) { found = true; break; }
        if (!found)
            continue;

        NetworkSnapshot ns;
        ns.tickPeriod = island.tickPeriod;
        ns.background = island.background;
        ns.signals = island.signals;
        ns.funcs.reserve(island.funcs.size());
        for (const auto& f : island.funcs)
            if (f) ns.funcs.push_back(snapshotFunc(f));
        return ns;
    }
    return std::nullopt;
}

std::vector<DataflowEngine::NetworkSnapshot> DataflowEngine::allSubnetworks()
{
    if (m_networkModified)
        buildNetworkCacheData();

    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    std::vector<NetworkSnapshot> result;
    result.reserve(m_networkIslands.size());
    for (const auto& island : m_networkIslands) {
        NetworkSnapshot ns;
        ns.tickPeriod = island.tickPeriod;
        ns.background = island.background;
        ns.signals = island.signals;
        ns.funcs.reserve(island.funcs.size());
        for (const auto& f : island.funcs)
            if (f) ns.funcs.push_back(snapshotFunc(f));
        result.push_back(std::move(ns));
    }
    return result;
}

std::vector<ptr<Signal>> DataflowEngine::allSignals(bool includeInternal)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    std::vector<ptr<Signal>> result;
    std::set<const Signal*> seen;
    for (const auto& s : signals) {
        if (!s || (!includeInternal && s->isInternal()))
            continue;
        if (seen.insert(s.get()).second)
            result.push_back(s);
    }
    return result;
}

std::vector<DataflowEngine::IslandDebugInfo> DataflowEngine::islandDebugSnapshot()
{
    if (m_networkModified)
        buildNetworkCacheData();

    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    std::vector<IslandDebugInfo> result;
    result.reserve(m_networkIslands.size());

    for (const auto& island : m_networkIslands) {
        IslandDebugInfo info;
        info.tickPeriod = island.tickPeriod;
        info.eventDrivenOnly = true;
        bool sawSignal = false;

        for (const auto& signal : island.signals) {
            if (!signal)
                continue;

            sawSignal = true;

            if (!signal->isInternal())
                info.signals.push_back(signal->name());

            if (signal->period() > TimeDuration::zero())
                info.eventDrivenOnly = false;
        }

        if (!sawSignal)
            info.eventDrivenOnly = false;

        result.push_back(std::move(info));
    }

    return result;
}
