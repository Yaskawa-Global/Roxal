#pragma once

#include "Signal.h"
#include "compiler/VM.h"

#include <set>
#include <memory>
#include <mutex>

namespace df {

class FuncNode; // forward declaration

//
// Singleton DataflowEngine keeps references to all active signals
//  and manages queue of events for updating them
class DataflowEngine
   : public std::enable_shared_from_this<DataflowEngine>
{
public:
    enum class ExecutionScheme {
        Strict,     // throws exception if FuncNode execution can't be completed within the engine clock period (default)
        BestEffort  // warn, but continue executing if FuncNode execution falls behind (may catch up if caused by transient longer func execution times)
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

    TimeDuration tickPeriod() const;

    // Run the engine
    void run(); // call tick() forever
    void runFor(TimeDuration duration); // call tick() for the duration

    // Stop the engine (causes run() to exit)
    void stop();

    // run for single engine tick (GCD of all clock signals)
    //  (if waitForTickStart==true and TimePoint::currentTime() is not yet tick-number*tick-period, wait until then)
    //  will rebuild network and restart tick count if network modified
    void tick(bool waitForTickStart = true);

    // evaluate the network without advancing time or ticking clocks
    // useful for initializing signal values when new nodes are added
    void evaluate();

    // Mark the network as modified so caches will be rebuilt on next tick/evaluate
    void markNetworkModified();

    // clear everything ready for new network to be instantiated
    void clear();

    // last computed signal values
    std::map<std::string, Value> signalValues() const;




    // callback for each engine tick (who's period is the GCD of all clock signals). Called after all signal value for this tick have been computed.
    void addTickCallback(std::function<void(ptr<DataflowEngine>, TimePoint)> callback);

    std::string graph() const;

    // graphviz .dot file format string of network (optionally with signal values shown)
    std::string graphDot(const std::string& title, std::map<std::string,Value> signalValues = {}) const;

    // Generate a unique function name based on the supplied base name
    static std::string uniqueFuncName(const std::string& base);

    virtual ~DataflowEngine();

protected:
    TimePoint tickStart() const { return m_tickStart; }

private:

    DataflowEngine();

    ExecutionScheme m_executionScheme;

    mutable std::recursive_mutex m_mutex; // guard network structures

    // engine ticks occur at GCD of all clock signals
    std::atomic<TimeDuration> m_tickPeriod;

    std::atomic<TimePoint> m_runStart;

    std::atomic<uint64_t> m_tickNumber;

    // of start of current tick (ticks occur at GCD of all clock signals)
    std::atomic<TimePoint> m_tickStart;
    std::vector<std::function<void(ptr<DataflowEngine>, TimePoint)>> m_tickCallbacks;

    // rebuild pre-computed network information (call after network changes - Func/Signal addition/removal/modification)
    void buildNetworkCacheData();

    void invokeTickCallbacks();

    std::atomic<bool> m_networkModified;
    std::atomic<bool> m_shouldStop{false};

    void addSignal(ptr<Signal> signal);
    void addFunc(ptr<FuncNode> func);


    std::vector<ptr<Signal>> signals;
    std::map<std::string, ptr<FuncNode>> funcs;

    // Mapping from signals to functions that consume them
    struct FuncInputInfo {
        ptr<FuncNode> func;
        TimeDuration latency;
    };
    std::map<ptr<Signal>, std::vector<FuncInputInfo>> signalConsumers;

    void buildSignalConsumers();

    void precomputeFuncPeriods();

    // For precomputed execution orders
    typedef std::map<ptr<FuncNode>, std::set<ptr<FuncNode>>> DependencyGraph;
    std::map<TimeDuration, std::vector<ptr<FuncNode>>> precomputedExecutionOrders;

    void precomputeExecutionOrders();

    bool topologicalSort(
        const DependencyGraph& depGraph,
        std::vector<ptr<FuncNode>>& sortedFuncs
    );


    // set the input signal availability time for each func consuming this signal to its ticked time
    void updateSignalConsumerInputAvailability(ptr<Signal> signal, TimePoint signalTickedTime);

    // execute functions whose inputs are available, without advancing time
    void evaluateNetwork(TimePoint evaluationTime);

    // call processFunctionExecuteEvent() for each func,
    //  in the order they appear in precomputedExecutionOrders for their interval (LCM if input signal periods)
    void executeFunctionsInOrder(const std::vector<ptr<FuncNode>>& funcsToExecute);

    // longest period which divides all periods (GCD)
    //  e.g. 4,6,12 -> 2
    TimeDuration longestDividingPeriod(const std::set<TimeDuration>& periods);

    TimeDuration computeExecutionInterval(ptr<FuncNode> func);




    TimePoint nextPeriodOnPeriodBoundary(TimeDuration periodMicrosecs) const;
    TimePoint nextPeriodOnPeriodBoundary(double freq) const;


    friend class Signal;
    friend class FuncNode;
};


}
