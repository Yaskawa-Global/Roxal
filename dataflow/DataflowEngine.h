#pragma once

#include "Signal.h"
#include "Func.h"

#include <set>

namespace df {


//
// Singleton DataflowEngine keeps references to all active signals
//  and manages queue of events for updating them
class DataflowEngine
   : public std::enable_shared_from_this<DataflowEngine>
{
public:
    enum class ExecutionScheme {
        Strict,     // throws exception if Func execution can't be completed within the engine clock period (default)
        BestEffort  // warn, but continue executing if Func execution falls behind (may catch up if caused by transient longer func execution times)
    };

    static ptr<DataflowEngine> instance()
    {
        static ptr<DataflowEngine> engine = nullptr;
        if (engine == nullptr)
            engine = std::shared_ptr<DataflowEngine>(new DataflowEngine()); // Direct call to new

        return engine;
    }

    DataflowEngine(DataflowEngine const&)   = delete;
    void operator=(DataflowEngine const&) = delete;

    void setExecutionScheme(ExecutionScheme scheme) { m_executionScheme = scheme; }

    TimePoint currentTime() const;     // since DataflowEngine construction

    TimeDuration tickPeriod() const;

    // Run the engine
    void run(); // call tick() forever
    void runFor(TimeDuration duration); // call tick() for the duration

    // run for single engine tick (GCD of all clock signals)
    //  (if waitForTickStart==true and currentTime() is not yet tick-number*tick-period, wait until then)
    //  will rebuild network and restart tick count if network modified
    void tick(bool waitForTickStart = true);

    // clear everything ready for new network to be instantiated
    void clear();

    // last computed signal values
    std::map<std::string, Value> signalValues() const;


    void sleepUntil(TimePoint futureTime);


    // callback for each engine tick (who's period is the GCD of all clock signals). Called after all signal value for this tick have been computed.
    void addTickCallback(std::function<void(ptr<DataflowEngine>, TimePoint)> callback);

    std::string graph() const;

    // graphviz .dot file format string of network (optionally with signal values shown)
    std::string graphDot(const std::string& title, std::map<std::string,Value> signalValues = {}) const;

    virtual ~DataflowEngine();

protected:
    TimePoint tickStart() const { return m_tickStart; }

private:

    DataflowEngine();

    ExecutionScheme m_executionScheme;

    // engine ticks occur at GCD of all clock signals
    TimeDuration m_tickPeriod;

    TimePoint m_runStart;

    uint64_t m_tickNumber;

    // of start of current tick (ticks occur at GCD of all clock signals)
    TimePoint m_tickStart;
    std::vector<std::function<void(ptr<DataflowEngine>, TimePoint)>> m_tickCallbacks;

    // rebuild pre-computed network information (call after network changes - Func/Signal addition/removal/modification)
    void buildNetworkCacheData();

    void invokeTickCallbacks();

    bool m_networkModified;

    void addSignal(ptr<Signal> signal);
    void addFunc(ptr<Func> func);


    std::vector<ptr<Signal>> signals;
    std::map<std::string, ptr<Func>> funcs;

    // Mapping from signals to functions that consume them
    struct FuncInputInfo {
        ptr<Func> func;
        TimeDuration latency;
    };
    std::map<ptr<Signal>, std::vector<FuncInputInfo>> signalConsumers;

    void buildSignalConsumers();

    void precomputeFuncPeriods();

    // For precomputed execution orders
    typedef std::map<ptr<Func>, std::set<ptr<Func>>> DependencyGraph;
    std::map<TimeDuration, std::vector<ptr<Func>>> precomputedExecutionOrders;

    void precomputeExecutionOrders();

    bool topologicalSort(
        const DependencyGraph& depGraph,
        std::vector<ptr<Func>>& sortedFuncs
    );


    // set the input signal availability time for each func consuming this signal to its ticked time
    void updateSignalConsumerInputAvailability(ptr<Signal> signal, TimePoint signalTickedTime);


    // call processFunctionExecuteEvent() for each func,
    //  in the order they appear in precomputedExecutionOrders for their interval (LCM if input signal periods)
    void executeFunctionsInOrder(const std::vector<ptr<Func>>& funcsToExecute);

    // longest period which divides all periods (GCD)
    //  e.g. 4,6,12 -> 2
    TimeDuration longestDividingPeriod(const std::set<TimeDuration>& periods);

    TimeDuration computeExecutionInterval(ptr<Func> func);


    uint64_t microSecsSinceBoot() const; // microsecs since boot
    uint64_t usOffset; // microsecs since boot when engine instantiated


    TimePoint nextPeriodOnPeriodBoundary(TimeDuration periodMicrosecs) const;
    TimePoint nextPeriodOnPeriodBoundary(double freq) const;


    friend class Signal;
    friend class Func;
};


}
