#include "DataflowEngine.h"
#include "FuncNode.h"
#include "compiler/VM.h"
#include "compiler/Object.h"
#include "core/common.h"

#include <stdexcept>
#include <numeric>
#include <thread>
#include <iostream>
#include <string.h>

using namespace df;

#include <atomic>

static std::atomic<uint64_t> gFuncCounter{0};

std::string DataflowEngine::uniqueFuncName(const std::string& base)
{
    gFuncCounter++;
    if (gFuncCounter == 1)
        return base;
    return base + "#" + std::to_string(gFuncCounter);
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

void DataflowEngine::registerSignalWrapper(ptr<Signal> signal)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    signalWrapperRefs[signal]++;
}

size_t DataflowEngine::unregisterSignalWrapper(ptr<Signal> signal)
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

size_t DataflowEngine::wrapperRefCount(ptr<Signal> signal) const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto it = signalWrapperRefs.find(signal);
    if (it == signalWrapperRefs.end())
        return 0;
    return it->second;
}

size_t DataflowEngine::consumerCount(ptr<Signal> signal) const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto it = signalConsumers.find(signal);
    if (it == signalConsumers.end())
        return 0;
    return it->second.size();
}

size_t DataflowEngine::signalRefCount(ptr<Signal> signal) const
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

void DataflowEngine::removeSignal(ptr<Signal> signal, bool force)
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


void DataflowEngine::copyInto(ptr<Signal> lhs, ptr<Signal> rhs)
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

    lhs->values = rhs->values;
    for(const auto& rhsCallback : rhs->valueChangedCallbacks)
        lhs->valueChangedCallbacks.push_back(rhsCallback);

    lhs->isDerived = rhs->isDerived;
    lhs->baseSignal = rhs->baseSignal;
    lhs->baseIndex = rhs->baseIndex;

    // Update any functions that use rhs as an input to use lhs instead
    for (const auto& kv : funcs) {
        auto& func = kv.second;
        for (auto& inputPort : func->m_inputs) {
            if (inputPort.signal == rhs)
                func->reassignInput(inputPort.name, lhs);
        }
    }

    // Copy the values from rhs to lhs
    for (const auto& kv : rhs->values) {
        lhs->setValueAt(kv.first, kv.second);
    }

    m_networkModified = true;
}

void DataflowEngine::clear()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    signalConsumers.clear();
    precomputedExecutionOrders.clear();
    signalWrapperRefs.clear();
    signals.clear();
    funcs.clear();
    m_networkModified = false;
    m_tickCallbacks.clear();
    m_tickStart = TimePoint::zero();
    m_tickPeriod = TimeDuration::zero();
    m_runStart = TimePoint::zero();
    m_tickNumber = 0;
}

void DataflowEngine::stop()
{
    m_shouldStop = true;
}


TimeDuration DataflowEngine::tickPeriod() const
{
    if (m_networkModified)
        const_cast<DataflowEngine*>(this)->buildNetworkCacheData();

    return m_tickPeriod;
}



void DataflowEngine::run() {
    m_shouldStop = false;

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
        if (m_networkModified) {
            buildNetworkCacheData();

            if (m_tickPeriod > TimeDuration::zero()) {
                m_runStart = nextPeriodOnPeriodBoundary(m_tickPeriod);
                m_tickNumber = 0;
            }
            continue;
        }

        if (m_tickPeriod == TimeDuration::zero()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        m_tickStart = m_runStart + m_tickPeriod*m_tickNumber;
        if (m_tickStart < TimePoint::currentTime()) {
            m_runStart = nextPeriodOnPeriodBoundary(m_tickPeriod);
            m_tickNumber = 0;
            continue;
        }

        sleepUntil(m_tickStart);
        tick(false);
    }
}

void DataflowEngine::runFor(TimeDuration duration)
{
    if (m_networkModified)
        buildNetworkCacheData();


    auto runUntil = TimePoint::currentTime() + duration;

    if (m_tickPeriod == TimeDuration::zero())
        m_runStart = TimePoint::currentTime();
    else
        m_runStart = nextPeriodOnPeriodBoundary(m_tickPeriod);
    //std::cout << "currentTime=" << TimePoint::currentTime().humanString() << " start=" << start.humanString() << std::endl;//!!!
    #if TRACE_EXECUTION
    trace(TraceEntry{TimePoint::currentTime(), "runFor() schedule initial signal ticks", signalsStart, std::nullopt, std::nullopt});
    #endif


    // keep running until we are out of time or stop is requested
    while (TimePoint::currentTime() < runUntil && !m_shouldStop) {

        if (m_networkModified) {
            buildNetworkCacheData();
            if (m_tickPeriod > TimeDuration::zero()) {
                m_runStart = nextPeriodOnPeriodBoundary(m_tickPeriod);
                m_tickNumber = 0;
            }
            continue; // start loop again with updated timing
        }

        if (m_tickPeriod == TimeDuration::zero()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        m_tickStart = m_runStart + m_tickPeriod*m_tickNumber;

        if (m_tickStart < TimePoint::currentTime()) {
            m_runStart = nextPeriodOnPeriodBoundary(m_tickPeriod);
            m_tickNumber = 0;
            continue;
        }

        // wait until the next tick should start
        sleepUntil(m_tickStart);

        tick(/*waitForTickStart=*/false);

    }


    #if TRACE_EXECUTION
    printTrace();
    #endif
}


void DataflowEngine::tick(bool waitForTickStart)
{
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


    for(const auto& signal : signals) {
        if (signal->isSourceSignal()) {
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

    for(const auto& signal : signals) {
        if (signal->isDerived) {
            auto src = signal->baseSignal.lock();
            if (src) {
                try {
                    Value val = src->valueAtIndex(signal->baseIndex);
                    signal->setValueAt(m_tickStart, val);
                } catch(...) {
                    signal->setValueAt(m_tickStart, Value());
                }
                updateSignalConsumerInputAvailability(signal, m_tickStart);
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


void DataflowEngine::evaluateNetwork(TimePoint evaluationTime)
{
    // pass over the network executing funcs whose inputs are available, until
    //  there are no more functions that can be executed
    uint64_t functionsExecuted;
    int32_t iterations = 0;
    bool funcsEmpty = true;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        funcsEmpty = funcs.empty();
    }

    if (!funcs.empty())
        do {
            functionsExecuted = 0;

            // although we can iterate over the funcs in any order, it is more likely they'll
            //  be ready to execute if they're executed in their dependency order, since each that is executed
            //  will produce the inputs of the next one tested for readiness
            // So, loop over the funcs grouped from shortest to longest period and in dependency order for each group

            auto executeFuncIfinputsAvailable = [&](const ptr<FuncNode>& func) {

                if (func->inputsAvailableAt(evaluationTime)) {
                    if (func->conditionallyExecute(evaluationTime)) {
                        functionsExecuted++;

                        // update the consumer input availability
                        for(auto& output : func->m_outputs)
                            updateSignalConsumerInputAvailability(output.signal, evaluationTime);
                    }
                }
            };

            // take a copy to avoid holding the lock while executing functions (expensive!)
            std::map<TimeDuration, std::vector<ptr<FuncNode>>> precomputedExecutionOrdersCopy;
            {
                std::lock_guard<std::recursive_mutex> lock(m_mutex);
                precomputedExecutionOrdersCopy = precomputedExecutionOrders;
            }
            for(const auto& periodFuncs : precomputedExecutionOrdersCopy) {
                auto& funcs { periodFuncs.second };
                for(auto func : funcs) {
                    executeFuncIfinputsAvailable(func);
                }
            }

            iterations++;
            if (iterations > funcs.size()*100) // FIXME: need to account for fact that loopPeriod may be much shorter than longest func exec period, fudge for now
                throw std::runtime_error("DataflowEngine: func execution didn't terminate - check for signal dependency cycles");

        } while (functionsExecuted > 0);
}


void DataflowEngine::evaluate()
{
    if (m_networkModified)
        buildNetworkCacheData();

    // For evaluation, use time zero and ensure all source signals have initial values
    TimePoint evalTime = TimePoint::zero();

    // Ensure all source signals have values at evaluation time
    for(const auto& signal : signals) {
        if (signal->isSourceSignal()) {
            signal->evaluate(evalTime);
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


void DataflowEngine::precomputeExecutionOrders()
{
    precomputeFuncPeriods();

    precomputedExecutionOrders.clear();

    // Map from execution intervals to functions
    std::map<TimeDuration, std::vector<ptr<FuncNode>>> executionGroups;

    // group orderings by execution interval
    for (const auto& funcPair : funcs) {
        ptr<FuncNode> func = funcPair.second;

        // Add the function to the execution group corresponding to its interval
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
        precomputedExecutionOrders[interval] = sortedFuncs;
    }

    #if 0 //defined(DEBUG_ENGINE)
    std::cout << "Execution Groups Dump:" << std::endl;
    std::cout << "=====================" << std::endl;

    for (const auto& groupPair : executionGroups) {
        TimeDuration interval = groupPair.first;
        const std::vector<ptr<FuncNode>>& funcsInGroup = groupPair.second;

        std::cout << "Interval: " << interval.microSecs() << " microseconds" << std::endl;
        std::cout << "Functions in this group:" << std::endl;

        for (const auto& func : funcsInGroup) {
            std::cout << "  - " << func->name() << std::endl;

            std::cout << "    Inputs:" << std::endl;
            for (const auto& input : func->inputs) {
                std::cout << "      " << input.name << " (Signal: " << input.signal->name()
                        << ", Latency: " << input.latency.microSecs() << " microseconds)" << std::endl;
            }

            std::cout << "    Outputs:" << std::endl;
            for (const auto& output : func->outputs) {
                std::cout << "      " << output.name << " (Signal: " << output.signal->name() << ")" << std::endl;
            }
        }

        std::cout << "Precomputed Execution Order:" << std::endl;
        for (const auto& func : precomputedExecutionOrders[interval]) {
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


void DataflowEngine::executeFunctionsInOrder(const std::vector<ptr<FuncNode>>& funcsToExecute) {
    if (funcsToExecute.empty()) return;

    // Determine the interval for these functions
    TimeDuration interval = computeExecutionInterval(funcsToExecute.front());

    #ifdef DEBUG_BUILD
    // ensure all the functions have the same interval
    for(const auto& func : funcsToExecute)
        assert(interval == computeExecutionInterval(func));
    #endif

    // Retrieve the precomputed execution order
    const auto& precomputedOrder = precomputedExecutionOrders[interval];
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

    // first, find the LCM of all source signal periods - we have to run the network at this rate
    std::set<TimeDuration> sourcePeriods {};
    for(const auto& signal : signals) {
        if (signal->isSourceSignal()) {
            if (signal->period() == TimeDuration::zero())
                throw std::runtime_error("Signal " + signal->name() + " has zero period");
            sourcePeriods.insert(signal->period());
        }
    }
    m_tickPeriod = longestDividingPeriod(sourcePeriods);
    #if 0
    std::cout << "Loop period: " << m_tickPeriod.humanString() << std::endl;
    #endif

    buildSignalConsumers();

    // Precompute execution orders before starting the loop
    precomputeExecutionOrders();

    m_tickNumber = 0;
    m_runStart = TimePoint::zero();

    m_networkModified = false;
}


void DataflowEngine::invokeTickCallbacks()
{
    for (const auto& callback : m_tickCallbacks) {
        #ifdef DEBUG_BUILD
        try {
            callback(shared_from_this(), m_tickStart);
        } catch(const std::exception& e) {
            std::cerr << "Exception in tick callback " << e.what() << std::endl;
        }
        #else
        try { callback(shared_from_this(), m_tickStart); } catch(...) {}
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
            signalToDestFuncs[input.signal].push_back({funcName, input.index});
        }

        for (const auto& output : func->m_outputs) {
            signalToSourceFunc[output.signal] = funcName;
        }
    }

    // Second pass: create edges
    for (const auto& [signal, destFuncs] : signalToDestFuncs) {
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

    dot << "}\n";
    return dot.str();
}
