#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ObjControl.h"

namespace roxal {

class Value;
class Thread;
class VM;

class ValueVisitor;

// Self-registering base for typed persistent GC roots (see GCRoots.h for
// the TracedMember/TracedRef templates built on it).  Lives here so types
// declared before Value's definition (e.g. SerializationContext) can be
// roots without an include cycle.  The collector calls traceRoot() during
// the mark phase (world stopped).
class GCRootBase {
public:
    GCRootBase(const GCRootBase&) = delete;
    GCRootBase& operator=(const GCRootBase&) = delete;

    virtual void traceRoot(ValueVisitor& visitor) const = 0;

protected:
    GCRootBase();           // registers with SimpleMarkSweepGC
    virtual ~GCRootBase();  // unregisters
};

// Root discovery: native-frame roots come from CONSERVATIVE scanning of
// parked C++ stacks rather than hand-enumerated visitors — portable to all
// native targets because parking is cooperative (each thread spills its own
// registers and records its own SP; no external thread suspension).  Exception: WebAssembly cannot expose wasm locals to any
// scanner, so a wasm embedding must either defer collections to host-loop
// turn boundaries (no native frames live => precise roots suffice) or
// adopt precise shadow-stack rooting (Root<Value> scopes) for that
// profile only.
class SimpleMarkSweepGC {
public:
    static constexpr std::uint64_t kDefaultAutoTriggerThreshold = 64ull * 1024ull * 1024ull; // 64 MiB

    // (Heap-resident roots are typed, self-registering members --
    // PersistentRoot/TracedMember/TracedRef, see GCRoots.h -- not
    // hand-written enumerations.)

    class ExternalParticipant {
    public:
        explicit ExternalParticipant(SimpleMarkSweepGC& gc);
        ~ExternalParticipant();

        ExternalParticipant(const ExternalParticipant&) = delete;
        ExternalParticipant& operator=(const ExternalParticipant&) = delete;

        void pollSafepointIfRequested();

    private:
        SimpleMarkSweepGC* gc_ { nullptr };
        std::uint64_t id_ { 0 };
    };

    static SimpleMarkSweepGC& instance();

    void registerAllocation(ObjControl* control);
    void unregisterAllocation(ObjControl* control);

    void requestCollect();
    void safepoint(Thread& currentThread);

    // ---- RT GC-yield sections -------------------------------------------
    // A real-time thread (e.g. a host's 1-2ms control-loop callback that
    // drives tickFor()/runFor()) must NEVER park at a safepoint or block on
    // the collector -- but it does touch GC state (allocates Values, mutates
    // traced containers).  It brackets that work in a *yield section*:
    //
    //   if (GCYieldScope gcs{}; gcs) { ...GC-touching RT work... }
    //   else { /* collection pending/in progress: skip this cycle */ }
    //
    // tryEnterGCSection() never blocks: it fails (returns false) while a
    // collection is requested or running, so the RT thread yields the cycle;
    // on success the collector's barrier waits (bounded -- the remainder of
    // one RT slice; execute()/tickFor() yield out early on request) for
    // exitGCSection() before starting.  Nestable per thread.  Identification
    // is dynamic: "the RT thread" is whichever thread currently holds a
    // section (thread-local depth), so no registration is needed.
    bool tryEnterGCSection() noexcept;
    void exitGCSection() noexcept;
    static bool inGCYieldSectionOnThisThread() noexcept;

    class GCYieldScope {
    public:
        explicit GCYieldScope(SimpleMarkSweepGC& gc = SimpleMarkSweepGC::instance())
            : gc_(gc), entered_(gc.tryEnterGCSection()) {}
        ~GCYieldScope() { if (entered_) gc_.exitGCSection(); }
        GCYieldScope(const GCYieldScope&) = delete;
        GCYieldScope& operator=(const GCYieldScope&) = delete;
        explicit operator bool() const noexcept { return entered_; }
    private:
        SimpleMarkSweepGC& gc_;
        bool entered_;
    };

    // Coordination observability (cheap atomics; logged at VM::shutdown).
    // Turns soak runs into positive evidence: collections happened, none ran
    // on a thread that ever held an RT section, skip counts sane.
    struct CoordinationStats {
        std::uint64_t collections;      // tracing collections performed
        std::uint64_t rtSectionSkips;   // tryEnterGCSection() denials (RT cycles yielded)
        std::uint64_t lastCollectorTid; // std::hash of last collector's thread id (diagnostic)
        std::uint64_t rtSectionTid;     // std::hash of last RT section holder's id (diagnostic)
        // MUST be 0: collections performed by a thread holding an RT yield
        // section.  Counted inside performCollection itself, so unlike the
        // last-writer tid hashes above this is sound positive evidence.
        std::uint64_t sectionCollectorViolations;
    };
    CoordinationStats coordinationStats() const noexcept;

    // ---- No-park sections -------------------------------------------------
    // A thread mid-COMPILE holds in-progress GC objects (function chains,
    // constant tables) reachable only through compiler C++ state -- invisible
    // to the mark phase.  If such a thread parks at a safepoint (e.g. a lazy
    // builtin-module load nested inside the compile runs a module script),
    // a collection sweeps those live objects.  A no-park section keeps the
    // thread's registration UNPARKED so the collection barrier simply waits
    // out the compile (bounded).  Unlike an RT yield section, the thread
    // still counts toward the barrier and never skips work.
    static bool inGCNoParkSectionOnThisThread() noexcept;
    class GCNoParkScope {
    public:
        GCNoParkScope() { ++tlGCNoParkDepth_; }
        ~GCNoParkScope() { if (tlGCNoParkDepth_ > 0) --tlGCNoParkDepth_; }
        GCNoParkScope(const GCNoParkScope&) = delete;
        GCNoParkScope& operator=(const GCNoParkScope&) = delete;
    };

    // ---- GC-safe blocking regions ------------------------------------------
    // A registered thread about to BLOCK in a call the GC can neither wake
    // nor poll (std::thread::join, an opaque native wait) brackets the call
    // in a GCSafeBlockScope: entry marks ALL of the thread's registrations
    // parked, so the collection barrier proceeds without it (its interpreter
    // stack stays rooted via the thread registry); exit waits out any
    // collection in flight, then unmarks -- the thread resumes mutating only
    // between collections.  Keep the scope tight around the blocking call;
    // code inside it must not touch GC state (allocate, decRef, mutate
    // traced containers).  Not nestable.
    class GCSafeBlockScope {
    public:
        explicit GCSafeBlockScope(SimpleMarkSweepGC& gc = instance());
        ~GCSafeBlockScope();
        GCSafeBlockScope(const GCSafeBlockScope&) = delete;
        GCSafeBlockScope& operator=(const GCSafeBlockScope&) = delete;
    private:
        SimpleMarkSweepGC& gc_;
    };

    // True when the calling thread holds an ExternalParticipant registration.
    // VM::execute consults this so a participant thread that evaluates a
    // Roxal closure (e.g. the dataflow actor thread running an event island)
    // does NOT register a second time via onThreadEnter -- a double-counted
    // thread can only park once, which would deadlock the collection barrier.
    static bool currentThreadIsExternalParticipant() noexcept;

    // Poll the calling thread's outermost ExternalParticipant (no-op when the
    // thread holds none).  Lets code deep inside a participant-covered thread
    // (e.g. DataflowEngine::run() under the actor thread's participant) park
    // at a safepoint without holding the participant object itself.
    static void pollCurrentThreadParticipant();

    void setVM(VM* vm);

    // ---- Retire queue (reclamation) ----------------------------------------
    // Every dead object -- RC zero-crossing or sweep-retired -- is pushed
    // here and destroyed by the COLLECTOR role only (the dedicated collector
    // thread between/after collections, or the shutdown path once the
    // collector is stopped).  Mutators never drain: destruction is promptly
    // QUEUED, not scope-tied.  Push is a lock-free CAS chain (link-then-publish, so the
    // consumer's take-all sees only fully linked chains); the consumer
    // reverses the chain for FIFO arrival order.  Producers wake the
    // collector on the empty->non-empty transition; RT yield-section
    // holders never notify (the collector's idle poll picks their retires
    // up), keeping the RT path syscall-free.
    void retireObject(Obj* obj);
    std::vector<Obj*> drainRetiredObjects();
    bool hasRetiredObjects() const noexcept;

    void setAutoTriggerThreshold(std::uint64_t threshold);
    std::uint64_t autoTriggerThreshold() const noexcept;

    void setEnabled(bool enabled) noexcept;
    bool isEnabled() const noexcept;

    void onThreadEnter();
    void onThreadExit();

    // Typed persistent roots (see GCRoots.h).  One mutex guards
    // register/unregister AND the collector's mark-phase iteration (never
    // the GC mutex_); creation/destruction inside an RT yield section is a
    // contract violation (debug assert).
    void registerPersistentRoot(class GCRootBase* root);
    void unregisterPersistentRoot(class GCRootBase* root);

    // inline: the interpreter polls this once per dispatched opcode
    bool isCollectionRequested() const noexcept {
        return collectionRequested_.load(std::memory_order_acquire);
    }
    std::uint64_t currentEpoch() const noexcept;
    size_t lastCollectionFreed() const noexcept;
    bool isCollectionInProgress() const noexcept;

    void visitRoots(ValueVisitor& visitor);

    // Complete single-thread root scan (stack, frames incl. tail args,
    // open upvalues, pending conversions, event-dispatch state, native
    // continuations, wait state, ...).  Exposed so holders of suspended
    // Threads OUTSIDE the VM registry (e.g. a dataflow FuncNode's yielded
    // execution thread) can trace them without duplicating the field list.
    static void visitSingleThreadRoots(Thread& thread, ValueVisitor& visitor);

    // Run a synchronous collection assuming all mutator threads have already
    // parked (e.g. during VM shutdown). Returns the number of objects queued
    // for destruction. The caller is responsible for draining
    // the retire queue afterwards via VM::freeObjects().
    size_t collectNowForShutdown();

    // ---- Collector/reclaimer thread ----------------------------------------
    // A dedicated non-RT thread exists in EVERY build profile: it is the
    // sole RUNTIME RECLAIMER -- refcount zero-crossings only queue objects,
    // and this thread drains the retire queue between collections (without
    // it, refcount-dead objects would sit undestroyed until an unrelated
    // tracing collection).  ROXAL_GC_DEDICATED_THREAD (CMake default ON on
    // Linux) additionally makes it perform every runtime COLLECTION:
    // mutators reaching a safepoint then only park and wait instead of
    // self-electing, and RT-only workloads collect without any mutator's
    // cooperation.  Without the flag, collections stay on the inline
    // any-thread election path.  While the thread is not alive (before VM
    // construction completes, or after stopCollectorThread() during
    // shutdown) inline election and the shutdown drain cover both roles, so
    // early-boot and shutdown keep working.  The state below is
    // UNCONDITIONAL so the class layout does not depend on the flag
    // (ABI/ODR safety -- embedders compile this header with their own flags).
    void startCollectorThread();
    void stopCollectorThread();
    bool dedicatedCollectorEnabled() const noexcept;   // compiled in?
    bool dedicatedCollectorActive() const noexcept;    // thread running?

    // ---- Coordination self-test (sys._runtests('gc_coordination')) --------
    // Hammers the coordination machinery from concurrent threads for
    // `duration`: RT yield sections racing requestCollect (incl. the
    // deferred-wake path via in-section allocation), persistent + nested
    // ExternalParticipants, GCSafeBlockScope, and interned-string churn --
    // then asserts collections actually ran, sectionCollectorViolations
    // stayed 0, and all section/park counters returned to rest.  Prints a
    // summary; returns true on pass.  Exits the process (code 2) if the
    // barrier deadlocks (workers unjoinable).
    bool runCoordinationSelfTest(
        std::chrono::milliseconds duration = std::chrono::milliseconds(2000));

    // ---- Scanner recall self-test (sys._runtests('gc_scanner')) -----------
    // Asserts the conservative shadow scanner finds a deliberately
    // unrooted-but-alive object through each stack-reference form: raw Obj*,
    // NaN-tagged words (plain + const/weak-flagged), and a callee-saved-
    // register-only reference (x86-64/GCC).  The assertion is that the
    // scanner FLAGGED the reference (scan-only recall) -- sweep behavior is
    // covered elsewhere.  Prints a summary to stderr; returns true on pass.
    bool runScannerRecallSelfTest();

    // Set once VM::shutdown() begins tearing the object graph down.  Object
    // destructors that must behave differently during bulk teardown (e.g.
    // ObjSignal clears its Signal's buffered Value history so those Object-
    // backed Values don't get decRef'd after their targets are freed) consult
    // this.  It is never set during normal incremental GC, so live shared
    // objects keep their state.
    void setShuttingDown(bool v) { m_shuttingDown.store(v, std::memory_order_release); }
    bool isShuttingDown() const { return m_shuttingDown.load(std::memory_order_acquire); }

    // ---- Conservative shadow scan -------------------------------------------
    // At each collection, conservatively scan every captured parked C++
    // stack -- dual rule: (i) NaN-tagged object Values (payload masked of
    // const/weak bits), (ii) raw words resolving into any registered
    // allocation ([ObjControl, ObjControl+allocationSize) range lookup,
    // interior pointers included) -- and compare hits against the precise
    // mark WITHOUT contributing to it.  Quantifies what a conservative-roots
    // collector would retain and surfaces objects a precise root missed
    // while a parked stack still references them (root-hole detector).
    // Default ON; disable with ROXAL_GC_SHADOW_SCAN=0.
    struct ShadowScanStats {
        std::uint64_t collectionsScanned;  // collections that ran the scan
        std::uint64_t stacksScanned;       // captured stacks scanned (total)
        std::uint64_t wordsScanned;        // words examined (total)
        std::uint64_t taggedHits;          // rule (i) hits (total)
        std::uint64_t rawHits;             // rule (ii) hits (total)
        std::uint64_t uniqueObjects;       // distinct blocks hit (last scan)
        std::uint64_t scanOnlyObjects;     // hit but NOT precisely marked (last)
        std::uint64_t scanOnlyBytes;       //   ...their bytes (pinning estimate)
        std::uint64_t oracleBuildMaxUs;    // max oracle build time
        std::uint64_t scanMaxUs;           // max stack-scan time
    };
    ShadowScanStats shadowScanStats() const noexcept;

    // Test hook: did the LAST collection's shadow scan hit this control block
    // when the precise mark did not?  (Pointer compared by value only -- the
    // block may already be freed.)  Used by the scanner recall self-tests.
    bool shadowScanSawScanOnly(const void* control) const;

    // Conservative marking (default ON; kill switch ROXAL_GC_CONSERVATIVE=0,
    // a DIAGNOSTIC mode -- not fully safe for native-stack-only refs):
    // scan-only hits
    // are MARKED -- objects referenced only from a parked C++ stack survive
    // the sweep, retiring the unrooted-local bug class for stack
    // references.  The shadow stats keep their compare-only meaning:
    // scanOnlyObjects/Bytes = what conservatism retained beyond the precise
    // roots.  Note prompt-cycle-death is no longer guaranteed: a stale
    // stack word may pin a dropped cycle until its slot is overwritten
    // (see the pinning stats + the runtests.py precise pin for the two
    // cycle tests).
    static bool conservativeMarkingEnabled() noexcept;

private:
    SimpleMarkSweepGC() = default;
    // Out-of-line: collectorState_ is a unique_ptr to an incomplete type.
    ~SimpleMarkSweepGC();

    std::atomic<bool> m_shuttingDown { false };

    struct CollectionResult {
        std::vector<Obj*> unreachable;
        std::uint64_t freedBytes = 0;
    };

    CollectionResult performCollection(std::unique_lock<std::mutex>& lock);
    void externalParticipantEnter(std::uint64_t id);
    void externalParticipantExit(std::uint64_t id);
    void externalParticipantSafepoint(std::uint64_t id);

    mutable std::mutex mutex_;

    // ---- Allocation registry: per-thread segments ---------------------------
    // registerAllocation is LOCK-FREE: each thread bump-appends into its
    // own current segment (slot store, then count release-publish); a full
    // segment is sealed and a fresh one is pushed onto the global chain
    // with a lock-free CAS.  The collector iterates the chain only inside
    // performCollection (mutex_ held, world stopped; syncs via the
    // acquire loads plus each mutator's park).  unregisterAllocation stays
    // UNDER mutex_ (reclaimer-side only, never the allocation hot path):
    // that lock is what makes freeing a control impossible mid-mark.
    // Tombstoned slots are compacted by the collector in sealed segments;
    // fully empty sealed segments are unlinked and freed.
    struct AllocationSegment {
        static constexpr std::uint32_t kCapacity = 4096;
        std::atomic<std::uint32_t> count { 0 };
        bool sealed { false };                 // owner moved on; compactable
        AllocationSegment* nextGlobal { nullptr };
        ObjControl* slots[kCapacity];
    };
    std::atomic<AllocationSegment*> segmentsHead_ { nullptr };
    static thread_local AllocationSegment* tlAllocSegment_;
    std::atomic<std::uint64_t> allocationCount_ { 0 };   // reserve() hints

    template<typename Fn>
    void forEachControlLocked(Fn&& fn) {
        for (AllocationSegment* seg = segmentsHead_.load(std::memory_order_acquire);
             seg != nullptr; seg = seg->nextGlobal) {
            const std::uint32_t n = seg->count.load(std::memory_order_acquire);
            for (std::uint32_t i = 0; i < n; ++i) {
                if (ObjControl* c = seg->slots[i]) {
                    fn(c);
                }
            }
        }
    }
    void compactSegmentsLocked();
    std::mutex persistentRootsMutex_;
    std::unordered_set<class GCRootBase*> persistentRoots_;
    std::condition_variable safepointCv_;
    std::atomic<std::uint64_t> epoch_{1};
    std::atomic<std::uint64_t> nextExternalParticipantId_{1};
    std::atomic<bool> collectionRequested_{false};
    // A requestCollect() from inside an RT yield section defers the (blocking)
    // wakeAllThreadsForGC to the next non-RT thread reaching a safepoint poll.
    std::atomic<bool> deferredGCWakePending_{false};
    // An auto-trigger threshold crossing happened while the VM singleton was
    // still constructing (requesting then would re-enter VM::instance()
    // mid-initialization); fire on the first allocation after construction.
    std::atomic<bool> deferredAutoTrigger_{false};
    std::atomic<bool> collectionInProgress_{false};
    // Reclaim fence: true from the moment a collection's request flag is
    // cleared until its retired batch has been destroyed.  Parked mutators
    // wait this out too, which makes a script-level gc() a deterministic
    // collect-AND-reclaim point (tests and teardown rely on it).  RT yield
    // sections do NOT consult it: tryEnterGCSection() gates only on the
    // request/in-progress flags, so RT cycles resume as soon as the world
    // restarts and reclamation overlaps them (flat-cycle goal intact).
    std::atomic<bool> reclaimInProgress_{false};
    std::atomic<size_t> lastFreedCount_{0};
    std::atomic<std::uint64_t> lastFreedBytes_{0};
    std::atomic<std::uint64_t> lastRequestedBytes_{0};
    std::atomic<std::uint64_t> bytesAllocatedSinceLastCollect_{0};
    std::atomic<std::uint64_t> currentAllocatedBytes_{0};
    std::atomic<std::uint64_t> autoTriggerThreshold_{kDefaultAutoTriggerThreshold};
    std::atomic<bool> gcEnabled_{true};
    std::atomic<ObjControl*> retireQueueHead_{nullptr};

    // Number of DISTINCT physical threads currently inside an RT GC-yield
    // section (see tryEnterGCSection).  Deliberately NOT part of
    // activeThreads_: section threads never park at the barrier; they gate
    // collection start via a separate barrier term (rtSectionCount_ == 0).
    // Atomic -- enter/exit must never take mutex_ (a collection holds mutex_
    // for the whole mark+sweep, and the RT path must never block on it).
    std::atomic<std::uint32_t> rtSectionCount_{0};
    // Per-thread section nesting depth; rtSectionCount_ moves only on 0<->1.
    static thread_local std::uint32_t tlGCYieldSectionDepth_;
    // Per-thread ExternalParticipant registration depth (see
    // currentThreadIsExternalParticipant).
    static thread_local std::uint32_t tlExternalParticipantDepth_;
    // Per-thread no-park section depth (see GCNoParkScope).
    static thread_local std::uint32_t tlGCNoParkDepth_;

    // Coordination stats (see CoordinationStats).
    std::atomic<std::uint64_t> statsCollections_{0};
    std::atomic<std::uint64_t> statsRTSectionSkips_{0};
    std::atomic<std::uint64_t> statsLastCollectorTid_{0};
    std::atomic<std::uint64_t> statsRTSectionTid_{0};
    std::atomic<std::uint64_t> statsSectionCollectorViolations_{0};

    bool externalCollectorActive_{false};
    std::thread::id externalCollectorThread_{};
    std::atomic<VM*> vm_{nullptr};

    // Dedicated collector thread state (see startCollectorThread).  Opaque
    // pimpl + atomic flag, both UNCONDITIONAL members: sizeof/layout is
    // identical whether or not ROXAL_GC_DEDICATED_THREAD is defined (only
    // code is flag-gated), so embedders compiling this header with different
    // flags cannot ODR-break the singleton (the exit-139 lesson).
    struct CollectorThreadState;
    std::unique_ptr<CollectorThreadState> collectorState_;
    std::atomic<bool> dedicatedCollectorActive_{false};
    void collectorThreadMain();

    // ---- Shadow-scan state (see the public section) -------------------------
    // One capture record per physical thread that has ever parked, created
    // lazily at its first park (mutex_ held at every park site, so plain
    // fields suffice -- the mutex provides the capture->scan ordering).
    // capturedSP is
    // non-null only while the thread is parked; a dead thread's record is
    // simply inert (SP null forever).
    struct StackCaptureRecord {
        void* stackLow {nullptr};    // lowest valid stack address
        void* stackBase {nullptr};   // highest valid stack address (scan bound)
        void* capturedSP {nullptr};  // scan lower bound; non-null = parked
        std::thread::id owner {};
    };
    static thread_local StackCaptureRecord* tlStackCapture_;
    std::vector<std::unique_ptr<StackCaptureRecord>> stackCaptures_;  // under mutex_
    StackCaptureRecord* ensureStackCaptureLocked();

    // ---- MutatorContext coordination state ----------------------------------
    // ONE context per physical thread (Inactive/Running/Parked/SafeBlocked;
    // nesting is a depth counter on the same context) -- the single source
    // of mutator-coordination truth.  Collection barrier: NO context is
    // Running, plus the RT-yield-section term (rtSectionCount_ == 0).
    // Created lazily on first GC contact.  All fields are protected by
    // mutex_ (the same lock the barrier evaluates under); a lock-free
    // transition protocol is possible future polish.  The capture record
    // above stays the scanner's interface: parking reuses
    // ensureStackCaptureLocked()/ROXAL_GC_CAPTURE_PARKED_STACK.
    struct MutatorContext {
        enum class State : std::uint8_t { Inactive, Running, Parked, SafeBlocked };
        State state { State::Inactive };
        std::uint32_t runningDepth { 0 };   // nested enters on this thread
        std::thread::id owner {};
    };
    static thread_local MutatorContext* tlContext_;
    std::vector<std::unique_ptr<MutatorContext>> contexts_;  // under mutex_
    MutatorContext* ensureContextLocked();
    bool anyRunningLocked() const;   // barrier term (excludes callers that parked first)
    void enterRunning();
    void exitRunning();
    void pollContext(Thread* currentThread);
    void blockEnter(const void* scopeAnchor);
    void blockExit();
    void shadowScanParkedStacks(std::uint64_t epoch);

    std::atomic<std::uint64_t> shadowCollectionsScanned_{0};
    std::atomic<std::uint64_t> shadowStacksScanned_{0};
    std::atomic<std::uint64_t> shadowWordsScanned_{0};
    std::atomic<std::uint64_t> shadowTaggedHits_{0};
    std::atomic<std::uint64_t> shadowRawHits_{0};
    std::atomic<std::uint64_t> shadowUniqueObjects_{0};
    std::atomic<std::uint64_t> shadowScanOnlyObjects_{0};
    std::atomic<std::uint64_t> shadowScanOnlyBytes_{0};
    std::atomic<std::uint64_t> shadowOracleBuildMaxUs_{0};
    std::atomic<std::uint64_t> shadowScanMaxUs_{0};
    std::vector<ObjControl*> lastScanOnly_;  // under mutex_; valid during the
                                             // collection that filled it,
                                             // stale ptr VALUES afterwards

    friend class ExternalParticipant;
};

} // namespace roxal
