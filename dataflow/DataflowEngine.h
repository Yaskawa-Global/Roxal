#pragma once

#include "Signal.h"
#include "compiler/VM.h"

#include <set>
#include <memory>
#include <mutex>
#include <condition_variable>

namespace df {

class FuncNode; // forward declaration

//
// Singleton DataflowEngine keeps references to all active signals
//  and manages queue of events for updating them
class DataflowEngine
   : public roxal::enable_ptr_from_this<DataflowEngine>
{
public:
    enum class ExecutionScheme {
        Strict,     // throws exception if FuncNode execution can't be completed within the engine clock period (default)
        BestEffort  // warn, but continue executing if FuncNode execution falls behind (may catch up if caused by transient longer func execution times)
    };

    // Result of time-limited tick execution
    enum class TickResult {
        Complete,      // All funcs evaluated for this tick
        Yielded,       // Time budget exhausted mid-evaluation, more work pending
        Overrun,       // Tick exceeded its period - error condition
        Error,         // Runtime error during execution
        Busy           // Evaluator lock held (event-island evaluation in
                       // flight on the engine thread); nothing was done --
                       // an RT host retries next cycle rather than blocking
    };

    // Access the singleton instance. If \p create is false and the engine has
    // not yet been instantiated, a null pointer is returned instead of
    // creating a new instance. This is useful during shutdown where creating a
    // new tracked pointer could access SGCL resources after they have been
    // destroyed.
    static ptr<DataflowEngine> instance(bool create = true)
    {
        static ptr<DataflowEngine> engine = nullptr;
        if (engine == nullptr && create) {
            engine = ptr<DataflowEngine>::from_raw(new DataflowEngine()); // Direct call to new (constructor is private)
        }

        return engine;
    }

    DataflowEngine(DataflowEngine const&)   = delete;
    void operator=(DataflowEngine const&) = delete;

    void setExecutionScheme(ExecutionScheme scheme) { m_executionScheme = scheme; }

    TimeDuration tickPeriod() const;
    uint64_t currentTickNumber() const;

    // Run the engine (the engine actor thread's main loop; queued once at
    // VM construction).  This thread is the ONLY periodic driver besides an
    // embedding RT host using tickFor() -- there is deliberately no other
    // way to pump the schedule.
    void run(); // call tick() forever

    // Internal single-step hook (script `_dataflow_tick()`): queue one tick
    // for the ENGINE thread to execute and wait (yieldably, parking for any
    // collection) until it completes.  Keeps test/dev single-stepping off
    // the calling thread so the sole-driver invariant holds.
    void requestTickAndWait();

    // Stop the engine (causes run() to exit)
    void stop();

    // Wake run()'s idle drain sleep (see m_pendingEventCv).  Called by
    // stop() and by VM::wakeAllThreadsForGC so a dormant engine thread
    // reaches its GC poll / shutdown check promptly.
    void wakeDrain();

    // run for single engine tick (GCD of all clock signals)
    //  (if waitForTickStart==true and TimePoint::currentTime() is not yet tick-number*tick-period, wait until then)
    //  will rebuild network and restart tick count if network modified
    void tick(bool waitForTickStart = true);

    // Time-limited tick execution for RT control loop integration.
    // Handles both starting new ticks and resuming yielded ones.
    // Returns: Complete when tick finished, Yielded if budget exhausted,
    //          Overrun if tick exceeded its period, Error on failure.
    TickResult tickFor(TimeDuration budget);

    // Check if there's pending work from a yielded tick
    bool hasYieldedWork() const { return m_yieldState.active; }

    // Per-node budget-overrun attribution.  A budgeted tick (tickFor) can be
    // blown two ways: a single FuncNode execution completes past the deadline
    // (the node couldn't yield -- a native func, or a script func between
    // yield points), or a yielded tick's resumable work still doesn't fit in
    // the tick period.  Both record the offending node here so the host can
    // name the culprit instead of just seeing Overrun/late ticks.  Records
    // aggregate per node until drained via consumeNodeOverruns(); the first
    // occurrence per node also emits one stderr warning so engines running
    // without a consuming host still surface it.
    struct NodeOverrun {
        std::string nodeName;         // FuncNode name (auto-generated names embed the script construction site)
        TimeDuration cost;            // most recent measured cost (node execution, or whole-tick elapsed for period overruns)
        TimeDuration overBudget;      // most recent overshoot past the deadline/period
        uint64_t occurrences { 0 };   // overruns attributed to this node since last drain
    };

    // Drain accumulated overrun records (host log/telemetry hook; the engine
    // analogue of VM::consumeNativeCallOverrun).
    std::vector<NodeOverrun> consumeNodeOverruns();

    // Give a newly added node an initial output value, computed from its
    // inputs as they are NOW.  Adding a node is a structural change: no other
    // node is evaluated and no existing signal is re-stamped, so wiring never
    // advances a feedback loop nor shifts an existing node's phase.
    void initializeNode(const ptr<FuncNode>& node);

    // evaluate the network without advancing time or ticking clocks
    // useful for initializing signal values when new nodes are added
    void evaluate();

    // Mark the network as modified so caches will be rebuilt on next tick/evaluate
    void markNetworkModified();

    // Change a registered signal's execution domain.  Serialized under the
    // engine mutex (which every network-cache rebuild holds while reading
    // domains) and followed by a rebuild request -- the only safe way to
    // change domain after registration (see Signal::setDomain).
    void setSignalDomain(const ptr<Signal>& signal, Signal::Domain domain);

    // clear everything ready for new network to be instantiated
    void clear();

    // last computed signal values
    std::map<std::string, Value> signalValues() const;

    // return the list of consumers for a signal (and which input name they consume it on)
    std::vector<std::pair<ptr<FuncNode>, std::string>> consumersOfSignal(ptr<Signal> signal) const;

    // return the list of producers for a signal (and which output name they produce it on)
    std::vector<std::pair<ptr<FuncNode>, std::string>> producersOfSignal(ptr<Signal> signal) const;


    // callback for each engine tick (who's period is the GCD of all clock signals). Called after all signal value for this tick have been computed.
    void addTickCallback(std::function<void(ptr<DataflowEngine>, TimePoint)> callback);

    std::string graph() const;

    // graphviz .dot file format string of network (optionally with signal values shown)
    std::string graphDot(const std::string& title, std::map<std::string,Value> signalValues = {}) const;

    struct IslandDebugInfo {
        TimeDuration tickPeriod { TimeDuration::zero() };
        std::vector<std::string> signals;
        bool eventDrivenOnly { false };
    };

    std::vector<IslandDebugInfo> islandDebugSnapshot();

    // ---- introspection snapshots (the inspect module's stable surface) ----
    // Plain-data copies of network structure taken under m_mutex; they hold
    // strong ptr<Signal>/ptr<FuncNode> references (keeping the C++ objects
    // alive) but deliberately no roxal::Value, so snapshots are GC-inert.

    struct PortSnapshot {
        std::string name;
        ptr<Signal> signal;
        int index { 0 };            // input latency (-1 = one period ago); 0 for outputs
        bool hasDefault { false };
    };

    struct FuncSnapshot {
        uint64_t id { 0 };
        std::string name;
        TimeDuration period { TimeDuration::zero() };
        std::string srcName;        // creation provenance (line 0 = unknown)
        size_t srcLine { 0 };
        size_t srcCol { 0 };
        std::vector<PortSnapshot> inputs;
        std::vector<PortSnapshot> outputs;
    };

    struct NetworkSnapshot {
        std::vector<ptr<Signal>> signals;
        std::vector<FuncSnapshot> funcs;
        TimeDuration tickPeriod { TimeDuration::zero() };
        bool background { false };
    };

    // The island (connected subnetwork) containing the given signal, or
    // nullopt if the signal is not part of any island.
    std::optional<NetworkSnapshot> subnetworkContaining(const ptr<Signal>& signal);

    // Every island of the current network.
    std::vector<NetworkSnapshot> allSubnetworks();

    // Every registered signal (deduplicated); internal (implicit
    // variable-monitor) signals excluded unless requested.
    std::vector<ptr<Signal>> allSignals(bool includeInternal = false);

    // remove a signal or func from the engine
    void removeSignal(const ptr<Signal>& signal, bool force = false);
    void removeFunc(ptr<FuncNode> func);

    // copy the attributes of the rhs signal into the lhs (lhs <- rhs)
    //  * lhs must be a source signal (not listed as the output of any producing funcs)
    //  * all funcs that have rhs as an input, will afterward have lhs as an input instead
    //  * lhs & rhs must have the same frequency
    void copyInto(const ptr<Signal>& lhs, const ptr<Signal>& rhs);

    // internal reference count for a signal held by the engine
    size_t signalRefCount(const ptr<Signal>& signal) const;

    // track how many ObjSignal wrappers reference a signal
    void registerSignalWrapper(const ptr<Signal>& signal);
    size_t unregisterSignalWrapper(const ptr<Signal>& signal); // returns remaining count
    size_t wrapperRefCount(const ptr<Signal>& signal) const;

    // how many functions consume this signal
    size_t consumerCount(const ptr<Signal>& signal) const;

    // Generate a unique function name based on the supplied base name
    static std::string uniqueFuncName(const std::string& base);

    virtual ~DataflowEngine();

protected:
    TimePoint tickStart() const { return m_tickStart; }

private:

    DataflowEngine();

    ExecutionScheme m_executionScheme;

public:
    // Diagnostics only: try-lock each engine mutex from the caller's thread
    // and report which are currently held elsewhere.  bit0 = m_mutex,
    // bit1 = m_evalMutex, bit2 = m_pendingEventMutex.  A wedge that holds a
    // bit forever names the contended resource.
    unsigned diagLocksHeld();
    // Why is the periodic driver not ticking? Reports the loop's own state:
    // tick number, milliseconds until the next scheduled tick (negative =
    // overdue), and the two flags that make run() bail to the outer loop.
    void diagTickState(long long& tickNumber, long long& msToNextTick,
                       bool& hostDriven, bool& shouldStop);
private:
    mutable std::recursive_mutex m_mutex; // guard network structures

    // Interim single-evaluator guard.  Held for the duration of island
    // evaluation by BOTH drivers -- tickFor() (the host/RT thread's periodic
    // schedule) and processEventDrivenSignalUpdate() (the actor thread draining
    // the pending-event queue) -- so the two never mutate engine state
    // (signalConsumers, FuncNode availability, island state) concurrently.
    // Deadlock-free w.r.t. the stop-the-world GC: both entry points are reached
    // from NATIVE code, and a thread is only counted by the collector while
    // inside execute(); a thread blocked here is off-execute(), so the GC never
    // waits on it.  Lock ORDER: m_evalMutex before m_mutex, never the reverse.
    // (Interim only: the RT thread can wait out an in-flight event evaluation --
    // sub-ms for camera tensor transforms; the real fix is the single-driver
    // rearchitecture.)
    std::recursive_mutex m_evalMutex;

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

    // Latched true on the first tickFor() call: an embedding host (e.g. an
    // RT control loop) has taken ownership of the PERIODIC schedule.  From
    // then on the engine's own run() loop stops ticking periodic
    // islands (they'd race the host's tickFor on the same islands) and
    // reduce to servicing the event-update queue -- the actor thread stays
    // the home of event-driven islands (vision streams etc.), keeping their
    // unbounded work off the host's RT budget.  Never reset: host-driven is
    // a mode commitment for the engine instance's lifetime.
    std::atomic<bool> m_hostDriven{false};

    // State for resuming a yielded tick execution
    struct YieldState {
        bool active { false };
        size_t islandIndex { 0 };
        size_t funcIndex { 0 };
        size_t periodIndex { 0 };
        TimePoint tickTime;
        bool funcWasExecuting { false };
        ptr<FuncNode> yieldedFunc;
    };
    YieldState m_yieldState;

    // Overrun-attribution store (see NodeOverrun above).  Guarded by its own
    // mutex: recording happens on the ticking thread, draining on whatever
    // thread the host logs from.  m_nodeOverrunWarned throttles the stderr
    // warning to once per node per engine lifetime (NOT per drain -- a
    // chronic offender shouldn't spam every consume cycle).
    void recordNodeOverrun(const std::string& nodeName, TimeDuration cost, TimeDuration overBudget);
    std::mutex m_nodeOverrunMutex;
    std::map<std::string, NodeOverrun> m_nodeOverruns;
    std::set<std::string> m_nodeOverrunWarned;

    // Advisory RT lint: when a host drives the periodic schedule (tickFor),
    // warn once per island composition about script-closure nodes on that
    // schedule -- script execution is budget-sliced, but any single
    // non-yieldable stretch can still blow the tick.  Advisory only (stderr),
    // never an error; silenced with ROXAL_RT_LINT=0.  The host-driven latch
    // and network rebuilds only SET m_rtLintPending -- the scan and printing
    // run on the engine thread's loop (rtLintIslands), never on the budgeted
    // tick path (stderr I/O alone can blow an RT slice).  m_rtLintAdvised
    // dedupes repeat compositions across rebuilds.
    void rtLintIslands();
    std::atomic<bool> m_rtLintPending { false };
    std::set<std::string> m_rtLintAdvised;

    // Evaluate any background-domain periodic islands that have come due,
    // on the calling (engine) thread with no tick budget.  Returns true if
    // any island was evaluated; soonestDue is set to the earliest pending
    // due time across background islands (zero if there are none) so idle
    // loops can size their sleeps.  Serialized against tickFor and event
    // evaluation by m_evalMutex.
    bool serviceBackgroundIslands(TimePoint& soonestDue);

    // Fast precheck for the run loops: true if the last network-cache build
    // produced at least one background periodic island.
    std::atomic<bool> m_haveBackgroundIslands { false };

    // Single-step tick requests (requestTickAndWait): monotonically counted
    // so a waiter observes exactly its own tick completing.  Serviced only
    // by the engine thread's run() loop.
    bool servicePendingTickRequests();
    std::atomic<uint64_t> m_tickRequests { 0 };
    std::atomic<uint64_t> m_tickRequestsDone { 0 };

    // Resume a previously yielded tick evaluation
    TickResult resumeTickEvaluation(TimePoint deadline);

    void addSignal(ptr<Signal> signal);
    void addFunc(ptr<FuncNode> func);


    std::vector<ptr<Signal>> signals;
    std::map<std::string, ptr<FuncNode>> funcs;

    // number of ObjSignal wrappers referencing each signal
    std::map<ptr<Signal>, size_t> signalWrapperRefs;

    // Mapping from signals to functions that consume them
    struct FuncInputInfo {
        ptr<FuncNode> func;
        TimeDuration latency;
    };
    std::map<ptr<Signal>, std::vector<FuncInputInfo>> signalConsumers;

    void buildSignalConsumers();

    void precomputeFuncPeriods();

    struct NetworkIsland {
        std::vector<ptr<Signal>> signals;
        std::vector<ptr<FuncNode>> funcs;
        std::map<TimeDuration, std::vector<ptr<FuncNode>>> executionOrders;
        TimeDuration tickPeriod { TimeDuration::zero() };

        // Background-domain island: any member signal declared
        // Signal::Domain::Background.  Kept off the shared periodic schedule
        // (tick/tickFor); serviced by serviceBackgroundIslands() on the
        // engine's own thread with no tick budget.
        bool background { false };

        // Next due evaluation time for a background periodic island.  Owned
        // by the servicing thread (engine thread, under m_evalMutex+m_mutex);
        // zero = not yet scheduled (first service aligns it to a period
        // boundary).  Reset by buildNetworkCacheData.
        TimePoint bgNextDue { TimePoint::zero() };
    };

    std::vector<NetworkIsland> m_networkIslands;

    void computeNetworkIslands();

    // For precomputed execution orders
    typedef std::map<ptr<FuncNode>, std::set<ptr<FuncNode>>> DependencyGraph;
    void precomputeExecutionOrders(NetworkIsland& island);

    bool topologicalSort(
        const DependencyGraph& depGraph,
        std::vector<ptr<FuncNode>>& sortedFuncs
    );


    // set the input signal availability time for each func consuming this signal to its ticked time
    void updateSignalConsumerInputAvailability(ptr<Signal> signal, TimePoint signalTickedTime);

    // execute functions whose inputs are available, without advancing time
    void evaluateNetwork(TimePoint evaluationTime);
    TickResult evaluateIsland(const NetworkIsland& island,
                              TimePoint evaluationTime,
                              TimePoint deadline = TimePoint::max(),
                              size_t startPeriodIndex = 0,
                              size_t startFuncIndex = 0);
    void refreshDerivedSignals(const NetworkIsland& island, TimePoint time);
    TimePoint resolveEvaluationTime(const NetworkIsland& island, TimePoint candidate) const;

    // call processFunctionExecuteEvent() for each func,
    //  in the order they appear in precomputedExecutionOrders for their interval (LCM if input signal periods)
    void executeFunctionsInOrder(const std::vector<ptr<FuncNode>>& funcsToExecute,
                                 const std::map<TimeDuration, std::vector<ptr<FuncNode>>>& executionOrders);

    // longest period which divides all periods (GCD)
    //  e.g. 4,6,12 -> 2
    TimeDuration longestDividingPeriod(const std::set<TimeDuration>& periods);

    TimeDuration computeExecutionInterval(ptr<FuncNode> func);




    TimePoint nextPeriodOnPeriodBoundary(TimeDuration periodMicrosecs) const;
    TimePoint nextPeriodOnPeriodBoundary(double freq) const;


    friend class Signal;
    friend class FuncNode;

    void processEventDrivenSignalUpdate(ptr<Signal> signal, TimePoint timestamp);

    // Event-driven updates arriving on non-VM threads (e.g. the DDS
    // reader-signal thread) cannot evaluate FuncNode closures in place; they
    // are queued here and drained by the engine's run loop on its own actor
    // thread. Guarded by m_pendingEventMutex (not m_mutex: producers must
    // never block on network evaluation).
    std::mutex m_pendingEventMutex;
    // Drain OWNERSHIP: exactly one drainer may run processPendingEventUpdates
    // at a time -- two engine-loop variants could historically race here and
    // both callers, and the drained batch lives in a member
    // (m_drainingEventUpdates, rooted for the GC tracer) that must have a
    // single owner.  try_lock'd; losers return "no work" (producers notify
    // m_pendingEventCv on enqueue, so a backed-off drainer re-wakes).
    std::mutex m_eventDrainMutex;
    // Each entry holds an ObjSignal wrapper Value (not a bare ptr<Signal>):
    // Values are the house convention for stored references, the wrapper's
    // trace() covers everything the signal owns, and holding it keeps the
    // signal registered (wrapper refcount) until the update is serviced.
    // The container itself is still invisible to the GC mark phase, so
    // tracePendingEventUpdates() below is called during root collection.
    std::vector<std::pair<roxal::Value, TimePoint>> m_pendingEventUpdates;
    // Woken by producers on enqueue, by stop(), and by the GC's
    // wakeAllThreadsForGC (via wakeDrain) so the idle drain sleep in run()
    // never stalls teardown or a collection barrier.
    std::condition_variable m_pendingEventCv;

public:
    // GC mark-phase hook: root the buffered time->Value history of EVERY
    // signal the engine still holds (registered signals + network-island
    // signals, including stale islands pending a rebuild).  The tracing GC
    // otherwise only reaches a signal's values through a live ObjSignal
    // wrapper -- but the engine keeps signals alive (and their maps
    // populated) after the last wrapper dies, and destroying such a map
    // later decRefs objects the tracer already swept (use-after-free).
    // Rooting them here matches the refcount reality: buffered Values live
    // exactly as long as some signal map holds them.
    void traceAllSignals(roxal::ValueVisitor& visitor);
private:
    // The batch currently being drained/evaluated.  Swapped out of
    // m_pendingEventUpdates under m_pendingEventMutex and kept HERE (a
    // member, not a drain-local) so the wrapper Values stay visible to
    // tracePendingEventUpdates() while the drain evaluates islands -- the
    // draining thread can park at a GC safepoint mid-drain (closure FuncNode
    // evaluation), and a drain-local batch would be unrooted at that moment.
    std::vector<std::pair<roxal::Value, TimePoint>> m_drainingEventUpdates;
    // Drain the queue (engine/VM thread only). Returns true if any were run.
    bool processPendingEventUpdates();

public:
    // GC mark-phase hook: a queued signal's embedded Values are otherwise
    // only traced through a reachable ObjSignal wrapper — if the last
    // wrapper dies while an update is queued, the mark phase would sweep
    // the values the drain is about to read. Called by SimpleMarkSweepGC
    // during root collection.
    void tracePendingEventUpdates(roxal::ValueVisitor& visitor);
};


}
