#include "SimpleMarkSweepGC.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/stack.h>
#endif

#ifdef __EMSCRIPTEN__
#include "web/JsBridge.h"
#endif

#ifdef ROXAL_ENABLE_FILEIO
#include "AsyncIOManager.h"
#if defined(__EMSCRIPTEN__) && defined(ROXAL_ENABLE_AI_NN)
#include "web/NnBridge.h"
#endif
#ifdef ROXAL_ENABLE_AI_NN
#include "ModuleNN.h"
#endif
#endif
#include "GCRoots.h"
#include "ThreadManager.h"
#include "Object.h"
#include "Thread.h"
#include "Value.h"
#include "VM.h"
#include "dataflow/DataflowEngine.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <unordered_set>
#include <vector>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

// ---- Shadow-scan capture helpers --------------------------------------------
namespace {

// Shadow scan on/off (default ON; ROXAL_GC_SHADOW_SCAN=0 disables).
bool shadowScanEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("ROXAL_GC_SHADOW_SCAN");
        return !(v && *v == '0');
    }();
    return enabled;
}

// Verbose capture/scan tracing (ROXAL_GC_SHADOW_DEBUG=1); temporary A0
// bring-up diagnostics.
bool shadowScanDebug() {
    static const bool enabled = [] {
        const char* v = std::getenv("ROXAL_GC_SHADOW_DEBUG");
        return v && *v && *v != '0';
    }();
    return enabled;
}

// Low bound for a stack capture: the address of a local in a NOINLINE callee
// lies strictly below every byte of the caller's frame -- including the
// callee-saved register spill slots that __builtin_unwind_init() forces into
// the caller's prologue.  (A few bytes of this dead callee frame get scanned
// too; conservatively harmless.)
#if defined(__GNUC__)
__attribute__((noinline))
#endif
void* gcCaptureStackLowBound() {
#if defined(__GNUC__)
    // NOT the address of a local: GCC's return-local-addr analysis replaces
    // `return &local;` with NULL in optimized builds (found the hard way --
    // captures silently vanished in the Release-built FC binary).  This
    // noinline callee's frame address is strictly below every byte of the
    // caller's frame, including the __builtin_unwind_init() spill slots.
    return __builtin_frame_address(0);
#else
    volatile char anchor = 0;
    return const_cast<char*>(&anchor);
#endif
}

// Overwrite recently-dead stack frames (self-test aid): the scanner reads
// dead frames conservatively, so a test asserting NON-retention must first
// erase residue copies left by object-construction helpers.
#if defined(__GNUC__)
__attribute__((noinline))
#endif
void gcScrubDeadFrames() {
    volatile char pad[8192];
    for (size_t i = 0; i < sizeof(pad); ++i)
        pad[i] = 0;
#if defined(__GNUC__)
    asm volatile("" ::: "memory");
#endif
}

}  // namespace

// Capture must execute in a frame that PERSISTS while the thread is parked
// (the park function's own frame): __builtin_unwind_init() forces every
// callee-saved register -- which may hold the only live reference from an
// ancestor frame -- into THIS frame's spill slots, inside the scanned range.
// (NOT setjmp: glibc pointer-mangles SP/FP slots in jmp_buf.)
#if defined(__GNUC__)
#define ROXAL_GC_CAPTURE_PARKED_STACK(rec)                                     \
    do {                                                                       \
        __builtin_unwind_init();                                               \
        (rec)->capturedSP = gcCaptureStackLowBound();                          \
    } while (0)
#else
#define ROXAL_GC_CAPTURE_PARKED_STACK(rec)                                     \
    do {                                                                       \
        (rec)->capturedSP = gcCaptureStackLowBound();                          \
    } while (0)
#endif

namespace {
// Barrier poll cadence while an RT yield section is (or was just) held.
// RT section enter/exit is deliberately notify-free (condvar notify is not
// wait-free -- internal lock + FUTEX_WAKE syscall -- and must not run on the
// hard-RT path), so barrier waiters poll at this interval instead.  This
// bounds only the COLLECTOR's reaction to a section exit (collection-start
// latency), never the RT thread, whose enter/exit cost stays one atomic op.
constexpr std::chrono::microseconds kRTSectionPollInterval{25};
}

namespace {

using namespace roxal;

void visitStrongValue(ValueVisitor& visitor, const Value& value)
{
    if (!value.isObj()) {
        return;
    }
    if (value.isWeak()) {
        return;
    }
    visitor.visit(value);
}

template <typename Range>
void visitStrongValues(ValueVisitor& visitor, const Range& values)
{
    for (const auto& value : values) {
        visitStrongValue(visitor, value);
    }
}

void visitCallFrameRoots(const CallFrame& frame, ValueVisitor& visitor)
{
    visitStrongValue(visitor, frame.closure);
    visitStrongValues(visitor, frame.tailArgValues);
}

void visitThreadRoots(Thread& thread, ValueVisitor& visitor)
{
    for (auto it = thread.stack.begin(); it != thread.stackTop; ++it) {
        visitStrongValue(visitor, *it);
    }

    for (const auto& value : thread.openUpvalues) {
        visitStrongValue(visitor, value);
    }

    for (const auto& frame : thread.frames) {
        visitCallFrameRoots(frame, visitor);
    }

    for (const auto& pending : thread.pendingConversions) {
        visitStrongValue(visitor, pending.savedLHS);
        visitStrongValue(visitor, pending.convReceiver);
    }
    for (const auto& guard : thread.conversionInProgress) {
        visitStrongValue(visitor, guard.receiver);
    }

    for (const auto& entry : thread.eventHandlers) {
        visitStrongValue(visitor, entry.first);
        for (const auto& reg : entry.second) {
            visitStrongValue(visitor, reg.closure);
            if (reg.matchValue.has_value())
                visitStrongValue(visitor, reg.matchValue.value());
            if (reg.targetFilter.has_value())
                visitStrongValue(visitor, reg.targetFilter.value());
        }
    }

    for (const auto& entry : thread.eventToSignal) {
        visitStrongValue(visitor, entry.first);
        visitStrongValue(visitor, entry.second);
    }

    thread.pendingEvents.unsafeVisit([&visitor](const auto& pendingEvents) {
        for (const auto& pending : pendingEvents) {
            visitStrongValue(visitor, pending.eventType);
            visitStrongValue(visitor, pending.instance);
        }
    });

    // Trace event dispatch state UNCONDITIONALLY -- after a dispatch
    // completes, `active` flips false but currentEvent/handlerSnapshot still
    // HOLD their Values (they are only overwritten by the next dispatch).
    // Gating on `active` let a collection sweep the previously-dispatched
    // event instance; the next dispatch's PendingEvent::operator= then
    // decRef'd the freed object (ASan-confirmed use-after-free under
    // camera-signal `when` traffic).
    visitStrongValue(visitor, thread.eventDispatch.currentEvent.eventType);
    visitStrongValue(visitor, thread.eventDispatch.currentEvent.instance);
    for (const auto& reg : thread.eventDispatch.handlerSnapshot) {
        visitStrongValue(visitor, reg.closure);
        if (reg.matchValue.has_value())
            visitStrongValue(visitor, reg.matchValue.value());
        if (reg.targetFilter.has_value())
            visitStrongValue(visitor, reg.targetFilter.value());
    }

    visitStrongValue(visitor, thread.currentActorCall);
    for (const auto& arg : thread.currentActorArgs)
        visitStrongValue(visitor, arg);
    visitStrongValue(visitor, thread.currentActorFuture);
    visitStrongValue(visitor, thread.currentActorResult);
    visitStrongValue(visitor, thread.currentBoundCall);
#ifdef ROXAL_COMPUTE_SERVER
    if (thread.remoteComputeCallState.active) {
        for (const auto& arg : thread.remoteComputeCallState.args) {
            visitStrongValue(visitor, arg);
        }
        visitStrongValue(visitor, thread.remoteComputeCallState.completionFuture);
        visitStrongValue(visitor, thread.remoteComputeCallState.result);
    }
#endif
    visitStrongValue(visitor, thread.pendingWaitFor);
    visitStrongValue(visitor, thread.awaitedFuture);
    visitStrongValue(visitor, thread.pendingUncaughtException);
    for (const auto& session : thread.stmtActionStack) {
        visitStrongValue(visitor, session.lastReceiver);
    }
    visitStrongValue(visitor, thread.pendingConstructorInstance);
    if (thread.waitSuspension.active) {
        visitStrongValue(visitor, thread.waitSuspension.storedValue);
    }

    // Trace native continuation stack (e.g., filter/map/reduce iteration, param conversions)
    for (const auto& cont : thread.nativeContinuationStack) {
        visitStrongValue(visitor, cont.state);
    }

    // Trace native default param state stack
    for (const auto& state : thread.nativeDefaultParamStack) {
        visitStrongValue(visitor, state.receiver);
        visitStrongValue(visitor, state.declFunction);
        for (const auto& val : state.argsBuffer) {
            visitStrongValue(visitor, val);
        }
        // staticDefaults are currently double-anchored via declFunction's
        // builtinInfo, but the invariant is "every retained strong Value is
        // traced" -- do not rely on incidental anchoring.
        for (const auto& val : state.staticDefaults) {
            visitStrongValue(visitor, val);
        }
        for (const auto& entry : state.paramDefaultFuncs) {
            visitStrongValue(visitor, entry.second);
        }
    }

    // Trace native param conversion state stack
    for (const auto& state : thread.nativeParamConversionStack) {
        visitStrongValue(visitor, state.receiver);
        visitStrongValue(visitor, state.declFunction);
        for (const auto& val : state.argsBuffer) {
            visitStrongValue(visitor, val);
        }
    }

    // Trace closure param conversion stack (deferred closure call with async param conversions)
    for (const auto& state : thread.closureParamConversionStack) {
        visitStrongValue(visitor, state.moduleType);
    }
}

} // namespace

namespace roxal {

SimpleMarkSweepGC& SimpleMarkSweepGC::instance() {
    static SimpleMarkSweepGC gc;
    return gc;
}

// ---- Dedicated collector thread (ROXAL_GC_DEDICATED_THREAD) ----------------
// The struct is defined unconditionally (the unique_ptr member always exists,
// keeping the class layout flag-independent); only the thread code is gated.
struct SimpleMarkSweepGC::CollectorThreadState {
    std::thread thread;
    std::atomic<bool> stop{false};
};

SimpleMarkSweepGC::~SimpleMarkSweepGC() {
    stopCollectorThread();
}

bool SimpleMarkSweepGC::dedicatedCollectorEnabled() const noexcept {
#ifdef ROXAL_GC_DEDICATED_THREAD
    return true;
#else
    return false;
#endif
}

bool SimpleMarkSweepGC::dedicatedCollectorActive() const noexcept {
    return dedicatedCollectorActive_.load(std::memory_order_acquire);
}

void SimpleMarkSweepGC::startCollectorThread() {
    // The thread exists in EVERY build profile: it is the sole runtime
    // reclaimer of refcount-dead objects between collections (zero-crossings
    // only queue; someone must drain).  ROXAL_GC_DEDICATED_THREAD only
    // decides whether tracing collections ALSO run here (mutators then park
    // instead of self-electing).
    if (collectorState_) {
        return;
    }
    collectorState_ = std::make_unique<CollectorThreadState>();
#ifdef ROXAL_GC_DEDICATED_THREAD
    // Publish BEFORE the thread runs: a mutator hitting a safepoint after
    // this point parks-and-waits instead of self-electing, and must never
    // race a half-started collector into double election.
    dedicatedCollectorActive_.store(true, std::memory_order_release);
#endif
    collectorState_->thread = std::thread([this] { collectorThreadMain(); });
}

void SimpleMarkSweepGC::stopCollectorThread() {
    if (!collectorState_) {
        return;
    }
    collectorState_->stop.store(true, std::memory_order_release);
    {
        // Lock/unlock pairs the stop flag with the collector's cv wait so the
        // notify below cannot land between its predicate check and sleep.
        std::lock_guard<std::mutex> lock(mutex_);
        // Falling back to inline collection: mutators parked waiting for the
        // dedicated collector must not hang on a request nobody will serve.
        // Drop any pending request (allocation pressure re-requests, and
        // shutdown runs collectNowForShutdown anyway).
        dedicatedCollectorActive_.store(false, std::memory_order_release);
        collectionRequested_.store(false, std::memory_order_release);
    }
    safepointCv_.notify_all();
    if (collectorState_->thread.joinable()) {
        collectorState_->thread.join();
    }
    collectorState_.reset();
}

void SimpleMarkSweepGC::collectorThreadMain() {
#ifdef __linux__
    // A thread spawned from an RT-scheduled (SCHED_FIFO) parent inherits the
    // parent's policy -- the collector must never compete at RT priority.
    {
        struct sched_param param {};
        param.sched_priority = 0;
        pthread_setschedparam(pthread_self(), SCHED_OTHER, &param);
    }
    int appliedExcludeCore = -1;
#endif

    std::unique_lock<std::mutex> lock(mutex_);
    while (!collectorState_->stop.load(std::memory_order_acquire)) {
        // Timed wait: robust against any lost-wake path (e.g. an RT-section
        // requester whose deferred wake never fires because no other thread
        // reaches a safepoint) at the cost of a 10ms idle poll.
        safepointCv_.wait_for(lock, std::chrono::milliseconds(10), [&] {
            return collectorState_->stop.load(std::memory_order_relaxed) ||
#ifdef ROXAL_GC_DEDICATED_THREAD
                   // Collections run here only in dedicated-collection
                   // builds.  In inline builds this term MUST be absent: a
                   // pending request stays set until a mutator self-elects,
                   // and a permanently-true predicate makes wait_for spin
                   // WITHOUT EVER RELEASING mutex_ -- starving the very
                   // mutators that need it to park (barrier deadlock).
                   collectionRequested_.load(std::memory_order_acquire) ||
#endif
                   // Deferral-aware, and it MUST mirror the drain-side
                   // condition below exactly.  Retired objects are a wake
                   // reason only while the drain is allowed to run; waking on
                   // a condition the drain then refuses is the
                   // permanently-true predicate described above, and
                   // wait_for returns from it WITHOUT RELEASING mutex_ --
                   // the collector pins the GC mutex, every mutator blocks
                   // at pollContext entry, and the app freezes.  (That is
                   // not hypothetical: tests/gc_liveness.rox hangs
                   // deterministically in an inline-collection build if this
                   // term is a bare hasRetiredObjects().)
                   (hasRetiredObjects() &&
                    !collectionInProgress_.load(std::memory_order_acquire));
        });
        if (collectorState_->stop.load(std::memory_order_acquire)) {
            break;
        }
#ifdef ROXAL_GC_DEDICATED_THREAD
        const bool collectHere = collectionRequested_.load(std::memory_order_acquire);
#else
        // Inline-collection build: collections are performed by whichever
        // mutator self-elects at a safepoint; this thread ONLY reclaims.
        constexpr bool collectHere = false;
#endif
        if (!collectHere) {
            // Reclamation between collections -- and, in inline-collection
            // builds, the ONLY runtime drain of refcount-dead objects: the
            // collector role is the sole destructor-runner; retire-queue
            // producers woke us (or the idle poll caught an RT-section
            // retire).
            // Reclamation and tracing are MUTUALLY EXCLUSIVE.  freeObjects()
            // runs without mutex_ (destructors may allocate and take locks),
            // so in inline-collection builds a drain here would otherwise
            // overlap a self-electing mutator's mark/sweep -- which walks the
            // same control blocks and container members that dropReferences()
            // is resetting.  Defer the drain; the collection wakes us.
            // Defer only while a collection is ACTUALLY RUNNING.  A merely
            // REQUESTED collection must not suppress the drain: in inline
            // builds the request stands until some mutator self-elects at a
            // safepoint, which can be far away, and this thread is the only
            // reclaimer -- suppressing it there defers every refcount death
            // for the whole window and breaks deterministic reclamation
            // (tests/gc_liveness.rox catches exactly that: the transitive
            // drop of a dead parent had not run, so its child outlived its
            // last reference).  Nothing is traced before the collection
            // starts, and the start handshake waits out an in-flight drain
            // (see the reclaimDraining_ wait in pollContext), so excluding
            // only the running window is both sufficient and necessary.
            const bool collectionInFlight =
                collectionInProgress_.load(std::memory_order_acquire);
            if (hasRetiredObjects() && !collectionInFlight) {
                if (VM* vm = vm_.load(std::memory_order_acquire)) {
                    // Publish the drain window BEFORE releasing mutex_. The
                    // collection side tests reclaimDraining_ under this same
                    // lock, so admission is a single atomic decision. Setting
                    // it only inside freeObjects left a gap in which an
                    // inline collector saw "no drain", started marking, and
                    // then raced dropReferences() -- trace() reading the very
                    // COW members the drain resets, which is the shared_ptr
                    // control-block corruption this exclusion exists to stop.
                    reclaimBegin();
                    lock.unlock();
                    vm->freeObjects();
                    lock.lock();
                    reclaimEnd();
                }
            }
            continue;
        }

#ifdef __linux__
        // Keep the collector off the RT core (FC sets rtCoreExclusion when
        // pinned; it may be configured after this thread starts, so re-check
        // per collection).
        if (VM* vm = vm_.load(std::memory_order_acquire)) {
            const int excludeCore = vm->rtCoreExclusion();
            if (excludeCore != appliedExcludeCore && excludeCore >= 0) {
                cpu_set_t cpuset;
                CPU_ZERO(&cpuset);
                const unsigned int numCpus = std::thread::hardware_concurrency();
                for (unsigned int i = 0; i < numCpus; ++i) {
                    if (static_cast<int>(i) != excludeCore) {
                        CPU_SET(i, &cpuset);
                    }
                }
                pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
                appliedExcludeCore = excludeCore;
            }
        }
#endif

        // Barrier: same condition as the inline paths -- all registered
        // threads parked AND no RT yield-section in flight.  This thread is
        // never registered, so it does not count toward activeThreads_.
        // activeThreads_ == 0 (RT-only workload) passes trivially: RT-heavy
        // hosts collect without any mutator's cooperation.  Single-read
        // timed-wait discipline as in safepoint(): section enter/exit is
        // notify-free.
        while (collectionRequested_.load(std::memory_order_acquire) &&
               !collectorState_->stop.load(std::memory_order_acquire)) {
            const bool sectionsHeld =
                rtSectionCount_.load(std::memory_order_seq_cst) != 0;
            if (!anyRunningLocked() && !sectionsHeld) {
                break;
            }
            if (sectionsHeld) {
                safepointCv_.wait_for(lock, kRTSectionPollInterval);
            } else {
                safepointCv_.wait(lock);
            }
        }
        if (collectorState_->stop.load(std::memory_order_acquire) ||
            !collectionRequested_.load(std::memory_order_acquire)) {
            continue;
        }

        // The barrier is satisfied and mutex_ has not been released since:
        // publish the stop-the-world window NOW so no context can transition
        // into Running before performCollection sets collectionInProgress_.
        worldStopped_.store(true, std::memory_order_release);
        externalCollectorActive_ = true;
        externalCollectorThread_ = std::this_thread::get_id();
        CollectionResult result = performCollection(lock);
        worldStopped_.store(false, std::memory_order_release);
        statsCollections_.fetch_add(1, std::memory_order_relaxed);
        statsLastCollectorTid_.store(
            std::hash<std::thread::id>{}(std::this_thread::get_id()),
            std::memory_order_relaxed);
        // Reclaim fence up BEFORE the request flag drops: parked mutators
        // wake only after the retired batch is destroyed (deterministic
        // gc() = collect AND reclaim), while RT yield sections -- which
        // gate on the request/in-progress flags only -- resume immediately
        // and overlap the reclamation below (flat cycles intact).
        reclaimInProgress_.store(true, std::memory_order_release);
        collectionRequested_.store(false, std::memory_order_release);
        bytesAllocatedSinceLastCollect_.store(0, std::memory_order_relaxed);
        externalCollectorActive_ = false;
        externalCollectorThread_ = std::thread::id {};

        // Destruction runs outside mutex_ (same as the inline collector
        // paths: Obj destructors take mutex_ via unregisterAllocation).
        lock.unlock();
        // One atomic publish (see retireObjects): the dedicated collector is
        // less exposed than the inline path because it drains immediately
        // afterwards, but a mutator-side refcount death could still interleave.
        retireObjects(result.unreachable);
        if (VM* vm = vm_.load(std::memory_order_acquire)) {
            vm->freeObjects();
        }
        lock.lock();
        reclaimInProgress_.store(false, std::memory_order_release);
        safepointCv_.notify_all();
    }
}

thread_local std::uint32_t SimpleMarkSweepGC::tlExternalParticipantDepth_ = 0;
// Outermost participant on this thread (for pollCurrentThreadParticipant).
static thread_local SimpleMarkSweepGC::ExternalParticipant* tlThreadParticipant_ = nullptr;

SimpleMarkSweepGC::ExternalParticipant::ExternalParticipant(SimpleMarkSweepGC& gc)
    : gc_(&gc), id_(gc.nextExternalParticipantId_.fetch_add(1, std::memory_order_relaxed))
{
    gc_->externalParticipantEnter(id_);
    if (tlExternalParticipantDepth_++ == 0) {
        tlThreadParticipant_ = this;
    }
}

SimpleMarkSweepGC::ExternalParticipant::~ExternalParticipant()
{
    if (gc_ != nullptr) {
        if (tlExternalParticipantDepth_ > 0) {
            --tlExternalParticipantDepth_;
        }
        // Clear the thread-local pointer whenever THIS object was it --
        // regardless of depth -- so non-LIFO destruction can never leave a
        // dangling pointer (pollCurrentThreadParticipant then just no-ops).
        if (tlThreadParticipant_ == this) {
            tlThreadParticipant_ = nullptr;
        }
        gc_->externalParticipantExit(id_);
    }
}

bool SimpleMarkSweepGC::currentThreadIsExternalParticipant() noexcept
{
    return tlExternalParticipantDepth_ > 0;
}

void SimpleMarkSweepGC::pollCurrentThreadParticipant()
{
    if (tlThreadParticipant_ != nullptr) {
        tlThreadParticipant_->pollSafepointIfRequested();
    }
}

void SimpleMarkSweepGC::ExternalParticipant::pollSafepointIfRequested()
{
    if (gc_ != nullptr) {
        gc_->externalParticipantSafepoint(id_);
    }
}

void SimpleMarkSweepGC::registerAllocation(ObjControl* control) {
    if (!control) {
        return;
    }

    std::uint64_t allocationSize = control->allocationSize;
    currentAllocatedBytes_.fetch_add(allocationSize, std::memory_order_relaxed);

    if (allocationSize > 0) {
        std::uint64_t previous = bytesAllocatedSinceLastCollect_.fetch_add(allocationSize, std::memory_order_relaxed);
        std::uint64_t updated = previous + allocationSize;
        std::uint64_t threshold = autoTriggerThreshold_.load(std::memory_order_relaxed);
        if (threshold > 0 && updated >= threshold) {
            if (!VM::constructed()) {
                // Collections must not be requested while the VM singleton is
                // still constructing: requestCollect() re-enters VM::instance()
                // (recursive_init_error), and collecting roots from a
                // half-built VM would be unsound regardless. Remember the
                // crossing and fire on the first allocation afterwards.
                deferredAutoTrigger_.store(true, std::memory_order_relaxed);
            } else if (previous < threshold ||
                       deferredAutoTrigger_.load(std::memory_order_relaxed)) {
                deferredAutoTrigger_.store(false, std::memory_order_relaxed);
                // requestCollect() clears bytesAllocatedSinceLastCollect_ once it flips
                // collectionRequested_ from false to true so that the next GC cycle
                // starts counting from zero immediately while still reporting the
                // bytes that triggered the request (including manual gc() calls when
                // the threshold is disabled).
                requestCollect();
            }
        }
    }

    // New allocations start out "unmarked" by setting their mark epoch to 0.
    // The control is still thread-private here (caller hasn't published the
    // object), so plain initialization needs no lock.
    control->collecting.store(false, std::memory_order_relaxed);
    control->dropClaimed.store(false, std::memory_order_relaxed);
#ifdef ROXAL_GC_FORENSICS
    control->cloneInProgress.store(false, std::memory_order_relaxed);
    control->tracedEpoch.store(0, std::memory_order_relaxed);
#endif
    control->markEpoch.store(0uLL, std::memory_order_relaxed);

    // Lock-free registry append: slot store, then count release-publish (the
    // collector's acquire read never sees an unwritten slot).  Rollover
    // seals the full segment and CAS-pushes a fresh one onto the global
    // chain.  No lock anywhere on this path -- the last GC lock on the
    // allocation hot path is gone.
    AllocationSegment* seg = tlAllocSegment_;
    if (seg == nullptr ||
        seg->count.load(std::memory_order_relaxed) >= AllocationSegment::kCapacity) {
        if (seg != nullptr) {
            seg->sealed = true;   // owner abandons; collector may compact
        }
        seg = new AllocationSegment();
        AllocationSegment* head = segmentsHead_.load(std::memory_order_relaxed);
        do {
            seg->nextGlobal = head;
        } while (!segmentsHead_.compare_exchange_weak(
            head, seg, std::memory_order_release, std::memory_order_relaxed));
        tlAllocSegment_ = seg;
    }
    const std::uint32_t idx = seg->count.load(std::memory_order_relaxed);
    seg->slots[idx] = control;
    control->allocSeg = seg;
    control->allocSlot = idx;
    seg->count.store(idx + 1, std::memory_order_release);
    allocationCount_.fetch_add(1, std::memory_order_relaxed);
}

void SimpleMarkSweepGC::unregisterAllocation(ObjControl* control) {
    if (!control) {
        return;
    }

    std::uint64_t allocationSize = control->allocationSize;
    if (allocationSize > 0) {
        currentAllocatedBytes_.fetch_sub(allocationSize, std::memory_order_relaxed);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (auto* seg = static_cast<AllocationSegment*>(control->allocSeg)) {
        if (control->allocSlot < AllocationSegment::kCapacity &&
            seg->slots[control->allocSlot] == control) {
            seg->slots[control->allocSlot] = nullptr;   // tombstone
        }
        control->allocSeg = nullptr;
    }
    allocationCount_.fetch_sub(1, std::memory_order_relaxed);
}

void SimpleMarkSweepGC::compactSegmentsLocked()
{
    // Exclusive access: called inside performCollection (mutex_ held, world
    // stopped; no lock-free pushes can race -- allocators are parked or in
    // skipped RT slices).  Only SEALED segments are touched: the owner
    // thread has moved on, so shifting slots and rewriting counts is safe.
    AllocationSegment* prev = nullptr;
    AllocationSegment* seg = segmentsHead_.load(std::memory_order_relaxed);
    while (seg != nullptr) {
        AllocationSegment* next = seg->nextGlobal;
        if (seg->sealed) {
            const std::uint32_t n = seg->count.load(std::memory_order_relaxed);
            std::uint32_t out = 0;
            for (std::uint32_t i = 0; i < n; ++i) {
                if (ObjControl* ctrl = seg->slots[i]) {
                    if (out != i) {
                        seg->slots[out] = ctrl;
                        ctrl->allocSlot = out;
                    }
                    ++out;
                }
            }
            seg->count.store(out, std::memory_order_relaxed);
            if (out == 0) {
                // Unlink and free the empty segment.
                if (prev) {
                    prev->nextGlobal = next;
                } else {
                    // Head may have grown since we loaded it only if an
                    // allocator raced us -- impossible here (world stopped),
                    // so a plain store is sound.
                    segmentsHead_.store(next, std::memory_order_relaxed);
                }
                delete seg;
                seg = next;
                continue;
            }
        }
        prev = seg;
        seg = next;
    }
}

void SimpleMarkSweepGC::requestCollect() {
    if (!gcEnabled_.load(std::memory_order_acquire)) {
        return;
    }
    // Never wake/collect against a half-constructed VM (see registerAllocation).
    if (!VM::constructed()) {
        return;
    }
    std::uint64_t allocated = bytesAllocatedSinceLastCollect_.load(std::memory_order_relaxed);
    bool expected = false;
    // seq_cst: pairs with tryEnterGCSection()'s inc-then-check so an RT
    // yield-section and a collection request can never both proceed
    // unobserved by the other (see the Dekker note there).
    if (collectionRequested_.compare_exchange_strong(expected, true, std::memory_order_seq_cst)) {
        lastRequestedBytes_.store(allocated, std::memory_order_relaxed);
        bytesAllocatedSinceLastCollect_.store(0, std::memory_order_relaxed);
    }
    // RT path: an allocation inside an RT yield section may cross the
    // threshold and land here.  wakeAllThreadsForGC() takes per-thread sleep
    // mutexes (blocking work) -- never do that on the RT thread.  Defer the
    // wake: the next thread to reach a safepoint poll performs it (the
    // dataflow actor participant self-wakes within its ~10ms drain timeout
    // even without a notify, so the deferral is bounded).
    if (inGCYieldSectionOnThisThread()) {
        deferredGCWakePending_.store(true, std::memory_order_release);
        return;
    }
    VM::instance().wakeAllThreadsForGC();
}

void SimpleMarkSweepGC::setVM(VM* vm) {
    vm_.store(vm, std::memory_order_release);
}

void SimpleMarkSweepGC::retireObject(Obj* obj)
{
    if (!obj || !obj->control) {
        return;
    }
    ObjControl* c = obj->control;
#ifdef ROXAL_GC_FORENSICS
    // An object must be retired exactly once.  Two claims => two teardowns
    // => the same shared_ptr members released twice (the observed fault).
    // CONTAINMENT (diagnostic builds): a control block that fails the
    // liveness check here is FREED MEMORY -- pushing it corrupts the retire
    // queue and crashes within milliseconds, often before the violation
    // report can be read.  Report and REFUSE the push instead: the report
    // survives, the run continues, and repeated violations accumulate a
    // count.  This trades crash fidelity for attribution fidelity, which is
    // what a forensic build is for.
    if (c->forensicMagic.load(std::memory_order_relaxed) != ObjControl::kMagicAlive) {
        roxalForensicViolation(c, "retireObject(dead control)");
        return;
    }
    if (roxalForensicOn(ROXAL_FC_CHECKS) &&
        c->retireCount.load(std::memory_order_relaxed) > 1)
        roxalForensicViolation(c, "DOUBLE retire");
#endif
    c->retiredObj = obj;
    ObjControl* prev = retireQueueHead_.load(std::memory_order_relaxed);
    do {
        c->retireNext = prev;   // link BEFORE publish: take-all sees full chains
    } while (!retireQueueHead_.compare_exchange_weak(
        prev, c, std::memory_order_release, std::memory_order_relaxed));
    if (prev == nullptr && !inGCYieldSectionOnThisThread()) {
        // empty -> non-empty: wake the collector's idle wait.  RT sections
        // skip the notify (not syscall-free); the 10ms idle poll covers them.
        safepointCv_.notify_all();
    }
}

void SimpleMarkSweepGC::retireObjects(const std::vector<Obj*>& objs)
{
    // Build the chain WITHOUT publishing, then attach it with a single CAS so
    // a concurrent drain sees either none of this set or all of it.
    ObjControl* head = nullptr;
    ObjControl* tail = nullptr;
    for (Obj* obj : objs) {
        if (!obj || !obj->control)
            continue;
        ObjControl* c = obj->control;
#ifdef ROXAL_GC_FORENSICS
        if (c->forensicMagic.load(std::memory_order_relaxed) != ObjControl::kMagicAlive) {
            roxalForensicViolation(c, "retireObjects(dead control)");
            continue;
        }
        if (roxalForensicOn(ROXAL_FC_CHECKS) &&
            c->retireCount.load(std::memory_order_relaxed) > 1)
            roxalForensicViolation(c, "DOUBLE retire");
#endif
        c->retiredObj = obj;
        c->retireNext = head;
        head = c;
        if (!tail)
            tail = c;
    }
    if (!head)
        return;

    ObjControl* prev = retireQueueHead_.load(std::memory_order_relaxed);
    do {
        tail->retireNext = prev;
    } while (!retireQueueHead_.compare_exchange_weak(
        prev, head, std::memory_order_release, std::memory_order_relaxed));
    if (prev == nullptr && !inGCYieldSectionOnThisThread())
        safepointCv_.notify_all();
}

std::vector<Obj*> SimpleMarkSweepGC::drainRetiredObjects()
{
    ObjControl* chain = retireQueueHead_.exchange(nullptr, std::memory_order_acquire);
    std::vector<Obj*> out;
    for (ObjControl* c = chain; c != nullptr; c = c->retireNext) {
        if (c->retiredObj) {
            out.push_back(c->retiredObj);
        }
    }
    std::reverse(out.begin(), out.end());   // FIFO arrival order
    return out;
}

bool SimpleMarkSweepGC::hasRetiredObjects() const noexcept
{
    return retireQueueHead_.load(std::memory_order_acquire) != nullptr;
}

void SimpleMarkSweepGC::setAutoTriggerThreshold(std::uint64_t threshold) {
    autoTriggerThreshold_.store(threshold, std::memory_order_relaxed);
    bytesAllocatedSinceLastCollect_.store(0, std::memory_order_relaxed);
    lastRequestedBytes_.store(0, std::memory_order_relaxed);
}

std::uint64_t SimpleMarkSweepGC::autoTriggerThreshold() const noexcept {
    return autoTriggerThreshold_.load(std::memory_order_relaxed);
}

void SimpleMarkSweepGC::reclaimBegin() noexcept {
    reclaimDraining_.fetch_add(1, std::memory_order_release);
}

void SimpleMarkSweepGC::reclaimEnd() noexcept {
    if (reclaimDraining_.fetch_sub(1, std::memory_order_release) == 1)
        safepointCv_.notify_all();
}

bool SimpleMarkSweepGC::reclaimDraining() const noexcept {
    return reclaimDraining_.load(std::memory_order_acquire) != 0;
}

void SimpleMarkSweepGC::setEnabled(bool enabled) noexcept {
    gcEnabled_.store(enabled, std::memory_order_release);
    if (!enabled) {
        // Threads parked in pollContext wait on safepointCv_ for the request
        // to clear; store under the lock + notify, or they miss the wakeup
        // and stay parked until some unrelated notify.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            collectionRequested_.store(false, std::memory_order_release);
            bytesAllocatedSinceLastCollect_.store(0, std::memory_order_relaxed);
            lastRequestedBytes_.store(0, std::memory_order_relaxed);
        }
        safepointCv_.notify_all();
    }
}

bool SimpleMarkSweepGC::isEnabled() const noexcept {
    return gcEnabled_.load(std::memory_order_acquire);
}

std::uint64_t SimpleMarkSweepGC::currentEpoch() const noexcept {
    return epoch_.load(std::memory_order_relaxed);
}

size_t SimpleMarkSweepGC::lastCollectionFreed() const noexcept {
    return lastFreedCount_.load(std::memory_order_relaxed);
}

bool SimpleMarkSweepGC::isCollectionInProgress() const noexcept {
    return collectionInProgress_.load(std::memory_order_acquire);
}

// ---- RT GC-yield sections ---------------------------------------------

void SimpleMarkSweepGC::visitSingleThreadRoots(Thread& thread, ValueVisitor& visitor)
{
    // Delegate to the file-local complete implementation used by visitRoots.
    visitThreadRoots(thread, visitor);
}

thread_local std::uint32_t SimpleMarkSweepGC::tlGCYieldSectionDepth_ = 0;

bool SimpleMarkSweepGC::tryEnterGCSection() noexcept {
    // LIVENESS INVARIANT (any-thread mode): a request raised during an RT
    // slice is serviced by some non-RT mutator reaching a safepoint.  That
    // servicing context is guaranteed by construction: the dataflow engine's
    // actor thread is a persistent ExternalParticipant in every VM (created
    // at VM construction) and polls its safepoint at <=10ms cadence even when
    // idle -- it self-elects as external collector.  A build that gates the
    // engine thread off (future threadless targets) must not use RT sections
    // without the dedicated-collector mode.
    if (tlGCYieldSectionDepth_ > 0) {
        // Nested: this thread already gates the collector.
        ++tlGCYieldSectionDepth_;
        return true;
    }
    // Dekker-style pairing with requestCollect(): publish our count FIRST,
    // then check for a pending/running collection.  With seq_cst on both
    // sides, either we see the request (and back out) or the collector's
    // barrier sees our count (and waits for exitGCSection) -- never neither.
    rtSectionCount_.fetch_add(1, std::memory_order_seq_cst);
    if (collectionRequested_.load(std::memory_order_seq_cst) ||
        collectionInProgress_.load(std::memory_order_acquire)) {
        rtSectionCount_.fetch_sub(1, std::memory_order_seq_cst);
        statsRTSectionSkips_.fetch_add(1, std::memory_order_relaxed);
        // NO condvar notify here: notify_all is not wait-free (glibc takes
        // the condvar-internal lock and issues a FUTEX_WAKE syscall), which
        // would violate the hard-RT contract of this path.  A collector that
        // observed our transient +1 sleeps on a TIMED wait (every barrier
        // iteration that observes a nonzero section count does -- see the
        // single-read branch in the barrier loops) and self-corrects within
        // one poll interval (kRTSectionPollInterval).
        return false;
    }
    ++tlGCYieldSectionDepth_;
    statsRTSectionTid_.store(
        std::hash<std::thread::id>{}(std::this_thread::get_id()),
        std::memory_order_relaxed);
    return true;
}

void SimpleMarkSweepGC::exitGCSection() noexcept {
    if (tlGCYieldSectionDepth_ == 0) {
        return;  // unbalanced exit; ignore defensively
    }
    if (--tlGCYieldSectionDepth_ > 0) {
        return;
    }
    rtSectionCount_.fetch_sub(1, std::memory_order_seq_cst);
    // NO condvar notify (see tryEnterGCSection): a waiting collector that
    // observed sections held is on a timed wait and re-checks the count
    // within kRTSectionPollInterval.
}

SimpleMarkSweepGC::GCSafeBlockScope::GCSafeBlockScope(SimpleMarkSweepGC& gc)
    : gc_(gc)
{
    // BlockingRegion: the whole context becomes SafeBlocked (depth
    // preserved for the exit restore); capture bounded at this scope
    // object in the caller's frame.
    gc_.blockEnter(static_cast<const void*>(this));
}

SimpleMarkSweepGC::GCSafeBlockScope::~GCSafeBlockScope()
{
    gc_.blockExit();
}

bool SimpleMarkSweepGC::inGCYieldSectionOnThisThread() noexcept {
    return tlGCYieldSectionDepth_ > 0;
}

thread_local std::uint32_t SimpleMarkSweepGC::tlGCNoParkDepth_ = 0;

bool SimpleMarkSweepGC::inGCNoParkSectionOnThisThread() noexcept {
    return tlGCNoParkDepth_ > 0;
}

SimpleMarkSweepGC::CoordinationStats SimpleMarkSweepGC::coordinationStats() const noexcept {
    return CoordinationStats {
        statsCollections_.load(std::memory_order_relaxed),
        statsRTSectionSkips_.load(std::memory_order_relaxed),
        statsLastCollectorTid_.load(std::memory_order_relaxed),
        statsRTSectionTid_.load(std::memory_order_relaxed),
        statsSectionCollectorViolations_.load(std::memory_order_relaxed),
    };
}

// ---- Shadow-scan capture records --------------------------------------------

thread_local SimpleMarkSweepGC::StackCaptureRecord* SimpleMarkSweepGC::tlStackCapture_ = nullptr;

SimpleMarkSweepGC::StackCaptureRecord* SimpleMarkSweepGC::ensureStackCaptureLocked() {
    if (tlStackCapture_) {
        return tlStackCapture_;
    }
    auto rec = std::make_unique<StackCaptureRecord>();
    rec->owner = std::this_thread::get_id();
#if defined(__EMSCRIPTEN__)
    // Emscripten defines neither __linux__ nor pthread_getattr_np, and the
    // silent fallthrough left every record boundless -- the scanner then
    // skipped EVERY parked stack, so on wasm the conservative scan
    // contributed no roots at all and any object whose only reference sat
    // on a parked thread's stack was swept alive. Each pthread's shadow
    // stack lives in linear memory with its own bounds: base is the high
    // address, the stack grows down toward the end.
    rec->stackBase = reinterpret_cast<void*>(emscripten_stack_get_base());
    rec->stackLow  = reinterpret_cast<void*>(emscripten_stack_get_end());
#elif defined(__linux__)
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        void* addr = nullptr;
        size_t size = 0;
        if (pthread_attr_getstack(&attr, &addr, &size) == 0 && addr && size) {
            rec->stackLow = addr;
            rec->stackBase = static_cast<char*>(addr) + size;
        }
        pthread_attr_destroy(&attr);
    }
#endif
    // Without bounds (other platforms, or getattr failure) the record stays
    // unusable: the scanner skips records lacking a valid base.
    tlStackCapture_ = rec.get();
    stackCaptures_.push_back(std::move(rec));
    if (shadowScanDebug()) {
        std::cerr << "[shadow] new record " << (void*)tlStackCapture_
                  << " thread " << (void*)pthread_self()
                  << " low " << tlStackCapture_->stackLow
                  << " base " << tlStackCapture_->stackBase
                  << " (total " << stackCaptures_.size() << ")" << std::endl;
    }
    return tlStackCapture_;
}

void SimpleMarkSweepGC::onThreadEnter() {
    enterRunning();
}

void SimpleMarkSweepGC::onThreadExit() {
    exitRunning();
}

// ---- MutatorContext coordination --------------------------------------------



thread_local SimpleMarkSweepGC::MutatorContext* SimpleMarkSweepGC::tlContext_ = nullptr;
thread_local SimpleMarkSweepGC::AllocationSegment* SimpleMarkSweepGC::tlAllocSegment_ = nullptr;

SimpleMarkSweepGC::MutatorContext* SimpleMarkSweepGC::ensureContextLocked()
{
    if (tlContext_) {
        return tlContext_;
    }
    auto ctx = std::make_unique<MutatorContext>();
    ctx->owner = std::this_thread::get_id();
    tlContext_ = ctx.get();
    contexts_.push_back(std::move(ctx));
    return tlContext_;
}

bool SimpleMarkSweepGC::diagContexts(unsigned counts[4], unsigned& rt)
{
    counts[0] = counts[1] = counts[2] = counts[3] = 0;
    rt = static_cast<unsigned>(rtSectionCount_.load(std::memory_order_relaxed));
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock())
        return false;
    for (const auto& ctx : contexts_) {
        if (!ctx) continue;
        counts[static_cast<unsigned>(ctx->state)]++;
    }
    return true;
}

void SimpleMarkSweepGC::currentContextForDiag(int& state, int& runningDepth) const noexcept
{
    MutatorContext* ctx = tlContext_;
    if (!ctx) { state = -1; runningDepth = 0; return; }
    state = static_cast<int>(ctx->state);
    runningDepth = ctx->runningDepth;
}

bool SimpleMarkSweepGC::anyRunningLocked() const
{
    for (const auto& ctx : contexts_) {
        if (ctx && ctx->state == MutatorContext::State::Running) {
            return true;
        }
    }
    return false;
}

void SimpleMarkSweepGC::enterRunning()
{
    std::unique_lock<std::mutex> lock(mutex_);
    MutatorContext* ctx = ensureContextLocked();
    // A context may not BECOME Running while a collection is scanning -- the
    // same return protocol blockExit() uses, and for the same reason.  The
    // barrier only checks "nobody Running" when the collection STARTS; an
    // ungated 0->1 transition afterwards puts a mutator back on the heap
    // mid-mark, and any edge it stores into an already-traced object is never
    // marked and is swept while live.  (Observed exactly that: an actor call
    // enqueued during a collection left the callee/args/future unmarked at
    // strong=1, the sweep freed them, and the actor's next trace() incRef'd
    // freed memory.)  Only the 0->1 transition waits: a context already
    // Running cannot have a collection in progress to wait out, and making it
    // wait would deadlock the very thread the collection is waiting for.
    if (ctx->runningDepth == 0) {
        if (worldStopped_.load(std::memory_order_acquire) ||
            collectionInProgress_.load(std::memory_order_acquire)) {
            enterRunningBlocked_.fetch_add(1, std::memory_order_relaxed);
            while (worldStopped_.load(std::memory_order_acquire) ||
                   collectionInProgress_.load(std::memory_order_acquire)) {
                safepointCv_.wait(lock);
            }
            enterRunningBlocked_.fetch_sub(1, std::memory_order_relaxed);
        }
        ctx = ensureContextLocked();   // re-resolve: the wait dropped the lock
    }
    if (++ctx->runningDepth == 1) {
        ctx->state = MutatorContext::State::Running;
    }
    safepointCv_.notify_all();
}

void SimpleMarkSweepGC::exitRunning()
{
    std::lock_guard<std::mutex> lock(mutex_);
    MutatorContext* ctx = tlContext_;
    if (!ctx) {
        return;
    }
    if (ctx->runningDepth > 0 && --ctx->runningDepth == 0) {
        ctx->state = MutatorContext::State::Inactive;
    }
    // Leaving Running can complete a waiting barrier.
    safepointCv_.notify_all();
}

// Safepoint poll: park this context (capture set for the scanner), then
// either wait out the collection (dedicated/other collector) or self-elect
// (early boot / post-stop shutdown).
void SimpleMarkSweepGC::pollContext(Thread* currentThread)
{
    (void)currentThread;
    if (!gcEnabled_.load(std::memory_order_acquire) ||
        !collectionRequested_.load(std::memory_order_acquire)) {
        return;
    }
    // Sink-enforced rules: an RT yield-section holder
    // neither parks nor collects; a no-park section stays Running so the
    // barrier waits out the section.
    if (inGCYieldSectionOnThisThread()) {
        return;
    }
    if (inGCNoParkSectionOnThisThread()) {
        return;
    }
    if (deferredGCWakePending_.exchange(false, std::memory_order_acq_rel)) {
        VM::instance().wakeAllThreadsForGC();
    }

    std::vector<Obj*> toDestroy;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!collectionRequested_.load(std::memory_order_acquire)) {
            return;
        }
        MutatorContext* ctx = ensureContextLocked();
        if (ctx->state == MutatorContext::State::SafeBlocked) {
            return;  // paranoia: blocked threads do not poll
        }
        StackCaptureRecord* rec = ensureStackCaptureLocked();
        ROXAL_GC_CAPTURE_PARKED_STACK(rec);  // this frame persists through the waits
        const MutatorContext::State prior = ctx->state;
        ctx->state = MutatorContext::State::Parked;
        safepointCv_.notify_all();

        if (dedicatedCollectorActive_.load(std::memory_order_acquire) ||
            externalCollectorActive_) {
            // Someone else performs the collection; wait out the collection
            // AND its reclamation (the fence makes gc() deterministic).
            while (collectionRequested_.load(std::memory_order_acquire) ||
                   reclaimInProgress_.load(std::memory_order_acquire)) {
                safepointCv_.wait(lock);
            }
        } else {
            // Self-elect: this thread performs the collection inline.
            //
            // The collector role must be RE-CHECKED after every wake, not
            // just on entry.  Each wait below releases mutex_, and another
            // thread that entered this same branch can take the role in that
            // window; without the re-check BOTH threads pass the barrier and
            // run performCollection CONCURRENTLY -- two marks interleaved on
            // one heap, and the first to finish clears the stop-the-world
            // window while the other is still marking, releasing mutators
            // onto a heap mid-collection.  (That is what kept the
            // "Running context at collection start" invariant firing after
            // the admission gap was closed.)
            bool anotherCollectorTookOver = false;
            while (collectionRequested_.load(std::memory_order_acquire)) {
                if (dedicatedCollectorActive_.load(std::memory_order_acquire) ||
                    externalCollectorActive_) {
                    anotherCollectorTookOver = true;
                    break;
                }
                const bool sectionsHeld =
                    rtSectionCount_.load(std::memory_order_seq_cst) != 0;
                if (!anyRunningLocked() && !sectionsHeld) {
                    break;
                }
                if (sectionsHeld) {
                    safepointCv_.wait_for(lock, kRTSectionPollInterval);
                } else {
                    safepointCv_.wait(lock);
                }
            }
            if (anotherCollectorTookOver) {
                // Demoted to waiter: wait out their collection AND its
                // reclamation, exactly as the non-elector branch does.
                while (collectionRequested_.load(std::memory_order_acquire) ||
                       reclaimInProgress_.load(std::memory_order_acquire)) {
                    safepointCv_.wait(lock);
                }
            } else if (collectionRequested_.load(std::memory_order_acquire)) {
                // Both directions of the exclusion are required.  The
                // reclaimer-side deferral stops NEW drains during a
                // collection; this wait covers a drain already in flight
                // when the request lands.  A mid-drain dropReferences()
                // MUTATES an object's COW members (elts_.reset(),
                // properties_ = make_ptr...) while the mark phase TRAVERSES
                // them via trace() -- a data race on the shared_ptr control
                // block, which is precisely the corruption signature seen in
                // free().  (Children hitting zero inside freeObjects take
                // Obj::decRef's inline-drop path whenever a collection is in
                // progress, so the racing pair is reclaimer-drain vs marker.)
                // No cycle: the deferral is skip-and-repoll, not a held wait,
                // so the drain always completes and this wait terminates.
                // (An earlier version removed this wait after misattributing
                // an unrelated, GC-independent ~24s VM freeze to it -- see
                // wasm-gc-crash-repro.md section 2b.)
                //
                // Publish the stop-the-world window BEFORE this wait: the
                // barrier has already admitted the collection, but wait_for
                // RELEASES mutex_, and in that gap a context could transition
                // into Running (its gate keys on this flag) and then mutate
                // the heap for the whole collection -- storing edges into
                // objects the mark has already traced.  That gap is exactly
                // how the actor-queue mark-miss was getting in.
                //
                // Claim the collector ROLE together with the window, and
                // before the wait below releases mutex_.  Publishing the role
                // afterwards leaves a gap in which a second thread sitting in
                // this same branch still sees externalCollectorActive_ ==
                // false, elects itself, and runs a concurrent collection.
                worldStopped_.store(true, std::memory_order_release);
                externalCollectorActive_ = true;
                externalCollectorThread_ = std::this_thread::get_id();
                while (reclaimDraining_.load(std::memory_order_acquire) != 0)
                    safepointCv_.wait_for(lock, std::chrono::milliseconds(1));
                CollectionResult result = performCollection(lock);
                worldStopped_.store(false, std::memory_order_release);
                toDestroy = std::move(result.unreachable);
                statsCollections_.fetch_add(1, std::memory_order_relaxed);
                statsLastCollectorTid_.store(
                    std::hash<std::thread::id>{}(std::this_thread::get_id()),
                    std::memory_order_relaxed);
                // Fence up before the request drops (see collectorThreadMain).
                reclaimInProgress_.store(true, std::memory_order_release);
                collectionRequested_.store(false, std::memory_order_release);
                bytesAllocatedSinceLastCollect_.store(0, std::memory_order_relaxed);
                externalCollectorActive_ = false;
                externalCollectorThread_ = std::thread::id {};
            }
        }

        ctx->state = prior;
        rec->capturedSP = nullptr;
    }
    safepointCv_.notify_all();

    // ONE atomic publish for the whole sweep: see retireObjects().
    retireObjects(toDestroy);
    const bool pushed = !toDestroy.empty();
    if (pushed) {
        if (VM* vm = vm_.load(std::memory_order_acquire)) {
            vm->freeObjects();
        }
    }
    if (reclaimInProgress_.load(std::memory_order_acquire)) {
        // This thread self-elected: drop the fence now that its retired
        // batch is destroyed, and wake the mutators parked on it.
        reclaimInProgress_.store(false, std::memory_order_release);
        safepointCv_.notify_all();
    }
}

void SimpleMarkSweepGC::blockEnter(const void* scopeAnchor)
{
    // NOTE: refusing to block while a no-park section is held DEADLOCKS.
    // A blocking native holding a no-park cover could otherwise leave its
    // caller Running while it waits for a thread parked for the collection;
    // the collection barrier would then wait for the caller. The underlying
    // hazard is real (a safe-block nested inside a no-park cover makes the
    // thread quiescent while its native frame still holds unscannable Values),
    // but closing it needs a ROOTING-AWARE transition -- suspend the cover
    // only when the frame's Values are reachable from traced storage -- not
    // blanket precedence either way.
    std::lock_guard<std::mutex> lock(mutex_);
    MutatorContext* ctx = ensureContextLocked();
    // Capture bounded at the scope object in the caller's frame: locals
    // BELOW the scope object are not scanned while blocked (the scope
    // contract forbids GC-touching work inside it anyway).
    StackCaptureRecord* rec = ensureStackCaptureLocked();
    rec->capturedSP = const_cast<void*>(scopeAnchor);
    ctx->state = MutatorContext::State::SafeBlocked;
    // Becoming quiescent can complete a waiting barrier.
    safepointCv_.notify_all();
}

void SimpleMarkSweepGC::blockExit()
{
    std::unique_lock<std::mutex> lock(mutex_);
    // Return protocol: never leave the captured state while a collection is
    // scanning (acquiring mutex_ already waits out mark+sweep; the loop is
    // belt-and-braces).
    while (worldStopped_.load(std::memory_order_acquire) ||
           collectionInProgress_.load(std::memory_order_acquire)) {
        safepointCv_.wait(lock);
    }
    MutatorContext* ctx = tlContext_;
    if (ctx) {
        ctx->state = (ctx->runningDepth > 0) ? MutatorContext::State::Running
                                             : MutatorContext::State::Inactive;
    }
    if (tlStackCapture_) {
        tlStackCapture_->capturedSP = nullptr;
    }
    // A pending (not yet started) request re-observes us Running and we
    // park at our next poll.
}

// ---- Typed persistent roots (see GCRoots.h) ---------------------------------

void SimpleMarkSweepGC::registerPersistentRoot(GCRootBase* root)
{
    if (!root) {
        return;
    }
    debug_assert_msg(!inGCYieldSectionOnThisThread(),
                     "persistent roots may not be created inside an RT yield section");
    std::lock_guard<std::mutex> lock(persistentRootsMutex_);
    persistentRoots_.insert(root);
}

void SimpleMarkSweepGC::unregisterPersistentRoot(GCRootBase* root)
{
    if (!root) {
        return;
    }
    debug_assert_msg(!inGCYieldSectionOnThisThread(),
                     "persistent roots may not be destroyed inside an RT yield section");
    std::lock_guard<std::mutex> lock(persistentRootsMutex_);
    persistentRoots_.erase(root);
}

GCRootBase::GCRootBase()
{
    SimpleMarkSweepGC::instance().registerPersistentRoot(this);
}

GCRootBase::~GCRootBase()
{
    SimpleMarkSweepGC::instance().unregisterPersistentRoot(this);
}

void SimpleMarkSweepGC::externalParticipantEnter(std::uint64_t id)
{
    // A participant is a nested Running enter on this thread's one context
    // -- no map entry, no owner-marking; nesting is the depth counter.
    (void)id;
    enterRunning();
}





void SimpleMarkSweepGC::externalParticipantExit(std::uint64_t id)
{
    (void)id;
    exitRunning();
}

void SimpleMarkSweepGC::safepoint(Thread& currentThread) {
    pollContext(&currentThread);
}

void SimpleMarkSweepGC::externalParticipantSafepoint(std::uint64_t id)
{
    (void)id;
    pollContext(nullptr);
}

// ---- Conservative shadow scan (see header) ----------------------------------
// Runs inside performCollection with mutex_ held and the barrier satisfied:
// every registered thread is parked with a capture set, so their frames in
// [capturedSP, stackBase) are frozen (SafeBlocked has one documented noisy
// window -- see GCSafeBlockScope).  Reads raw stack memory via memcpy;
// sanitizers must not instrument those loads (dead-frame redzones are
// expected and conservatively harmless).
#if defined(__clang__) || defined(__GNUC__)
__attribute__((no_sanitize_address))
#if defined(__clang__)
__attribute__((no_sanitize("thread")))
#endif
#endif
void SimpleMarkSweepGC::shadowScanParkedStacks(std::uint64_t epoch) {
    // NaN-box encoding straight from Value.h (same namespace): an object
    // Value has QNAN|SignBit set; payload = low 48 bits minus weak/const
    // flag bits (mirrors Value::isObj()/asObjControl()).
    const std::uint64_t kObjPattern = QNAN | SignBit;
    const std::uint64_t kPayloadMask = ~(SignBit | QNAN | WeakMask | ConstMask);

    const auto oracleStart = std::chrono::steady_clock::now();

    // Address oracle v0: sorted [ctrl, ctrl + allocationSize) ranges built
    // from the registry snapshot (world stopped; rebuild each collection --
    // incremental ingestion needs collector-owned removals, phases D/E).
    struct Range {
        std::uintptr_t start;
        std::uintptr_t end;
        ObjControl* ctrl;
    };
    std::vector<Range> oracle;
    oracle.reserve(allocationCount_.load(std::memory_order_relaxed));
    forEachControlLocked([&](ObjControl* control) {
        if (!control->obj) {
            return;
        }
        if (control->collecting.load(std::memory_order_relaxed)) {
            return;  // already dying: not a retainable target
        }
        const auto start = reinterpret_cast<std::uintptr_t>(control);
        // allocationSize is uint64_t while uintptr_t is 32-bit on wasm32, so the
        // sum needs an explicit narrowing -- an allocation's end address is by
        // construction representable as a pointer, so this cannot truncate.
        const auto end = static_cast<std::uintptr_t>(start + control->allocationSize);
        oracle.push_back(Range { start, end, control });
    });
    std::sort(oracle.begin(), oracle.end(),
              [](const Range& a, const Range& b) { return a.start < b.start; });

    const auto scanStart = std::chrono::steady_clock::now();

    auto lookup = [&oracle](std::uintptr_t addr) -> ObjControl* {
        auto it = std::upper_bound(
            oracle.begin(), oracle.end(), addr,
            [](std::uintptr_t v, const Range& r) { return v < r.start; });
        if (it == oracle.begin()) {
            return nullptr;
        }
        --it;
        return addr < it->end ? it->ctrl : nullptr;
    };

    std::unordered_set<ObjControl*> hits;
    std::uint64_t words = 0, taggedHits = 0, rawHits = 0, stacks = 0;

    for (const auto& recPtr : stackCaptures_) {
        const StackCaptureRecord* rec = recPtr.get();
        if (shadowScanDebug()) {
            std::cerr << "[shadow] scan sees rec " << (void*)rec
                      << " sp " << rec->capturedSP
                      << " base " << rec->stackBase << std::endl;
        }
        if (!rec->capturedSP || !rec->stackBase) {
            continue;
        }
        auto sp = reinterpret_cast<std::uintptr_t>(rec->capturedSP);
        const auto low = reinterpret_cast<std::uintptr_t>(rec->stackLow);
        const auto base = reinterpret_cast<std::uintptr_t>(rec->stackBase);
        if (sp < low || sp >= base) {
            continue;  // defensive: capture outside registered bounds
        }
        sp &= ~std::uintptr_t(sizeof(std::uintptr_t) - 1);
        ++stacks;

        for (std::uintptr_t a = sp; a + sizeof(std::uintptr_t) <= base;
             a += sizeof(std::uintptr_t)) {
            // Rule (ii): raw pointer (incl. interior) at native word size.
            std::uintptr_t w;
            std::memcpy(&w, reinterpret_cast<const void*>(a), sizeof(w));
            ++words;
            if (ObjControl* c = lookup(w)) {
                ++rawHits;
                hits.insert(c);
                continue;
            }
            // Rule (i): NaN-tagged object Value.  On 32-bit targets the
            // 64-bit candidate is reconstructed at every word offset (a
            // single 32-bit word can never satisfy both tag and pointer
            // checks); on 64-bit this is the same read.
            if (a + sizeof(std::uint64_t) > base) {
                continue;
            }
            std::uint64_t w64;
            std::memcpy(&w64, reinterpret_cast<const void*>(a), sizeof(w64));
            if ((w64 & kObjPattern) == kObjPattern) {
                const auto payload = static_cast<std::uintptr_t>(w64 & kPayloadMask);
                if (ObjControl* c = lookup(payload)) {
                    ++taggedHits;
                    hits.insert(c);
                }
            }
        }
    }

    // Compare against the (final) precise mark -- the caller decides whether
    // scan-only hits then contribute (A1 conservative marking).
    lastScanOnly_.clear();
    std::uint64_t scanOnlyBytes = 0;
    for (ObjControl* c : hits) {
        if (c->markEpoch.load(std::memory_order_relaxed) != epoch) {
            lastScanOnly_.push_back(c);
            scanOnlyBytes += c->allocationSize;
        }
    }

    // Debug: attribute each scan-only retention to the stack word(s) that
    // caused it (second pass, debug-flag only) -- distinguishes dead-frame
    // residue from live references when a retention surprises us.
    if (shadowScanDebug() && !lastScanOnly_.empty()) {
        for (const auto& recPtr : stackCaptures_) {
            const StackCaptureRecord* rec = recPtr.get();
            if (!rec->capturedSP || !rec->stackBase) continue;
            auto sp = reinterpret_cast<std::uintptr_t>(rec->capturedSP)
                      & ~std::uintptr_t(sizeof(std::uintptr_t) - 1);
            const auto base = reinterpret_cast<std::uintptr_t>(rec->stackBase);
            for (std::uintptr_t a = sp; a + sizeof(std::uintptr_t) <= base;
                 a += sizeof(std::uintptr_t)) {
                std::uintptr_t w;
                std::memcpy(&w, reinterpret_cast<const void*>(a), sizeof(w));
                std::uintptr_t cand = w;
                if (!lookup(cand) && a + sizeof(std::uint64_t) <= base) {
                    std::uint64_t w64;
                    std::memcpy(&w64, reinterpret_cast<const void*>(a), sizeof(w64));
                    if ((w64 & kObjPattern) == kObjPattern)
                        cand = static_cast<std::uintptr_t>(w64 & kPayloadMask);
                }
                ObjControl* c = lookup(cand);
                if (!c) continue;
                for (ObjControl* so : lastScanOnly_) {
                    if (so == c) {
                        std::cerr << "[shadow] scan-only " << (void*)c
                                  << " retained by stack word @" << (void*)a
                                  << " = 0x" << std::hex << w << std::dec
                                  << " (rec " << (void*)rec
                                  << ", depth base-" << (base - a) << ")"
                                  << std::endl;
                        break;
                    }
                }
            }
        }
    }

    const auto scanEnd = std::chrono::steady_clock::now();
    const auto us = [](auto from, auto to) {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(to - from).count());
    };

    shadowCollectionsScanned_.fetch_add(1, std::memory_order_relaxed);
    shadowStacksScanned_.fetch_add(stacks, std::memory_order_relaxed);
    shadowWordsScanned_.fetch_add(words, std::memory_order_relaxed);
    shadowTaggedHits_.fetch_add(taggedHits, std::memory_order_relaxed);
    shadowRawHits_.fetch_add(rawHits, std::memory_order_relaxed);
    shadowUniqueObjects_.store(hits.size(), std::memory_order_relaxed);
    shadowScanOnlyObjects_.store(lastScanOnly_.size(), std::memory_order_relaxed);
    shadowScanOnlyBytes_.store(scanOnlyBytes, std::memory_order_relaxed);
    const std::uint64_t buildUs = us(oracleStart, scanStart);
    const std::uint64_t scanUs = us(scanStart, scanEnd);
    if (buildUs > shadowOracleBuildMaxUs_.load(std::memory_order_relaxed)) {
        shadowOracleBuildMaxUs_.store(buildUs, std::memory_order_relaxed);
    }
    if (scanUs > shadowScanMaxUs_.load(std::memory_order_relaxed)) {
        shadowScanMaxUs_.store(scanUs, std::memory_order_relaxed);
    }
}

SimpleMarkSweepGC::ShadowScanStats SimpleMarkSweepGC::shadowScanStats() const noexcept {
    return ShadowScanStats {
        shadowCollectionsScanned_.load(std::memory_order_relaxed),
        shadowStacksScanned_.load(std::memory_order_relaxed),
        shadowWordsScanned_.load(std::memory_order_relaxed),
        shadowTaggedHits_.load(std::memory_order_relaxed),
        shadowRawHits_.load(std::memory_order_relaxed),
        shadowUniqueObjects_.load(std::memory_order_relaxed),
        shadowScanOnlyObjects_.load(std::memory_order_relaxed),
        shadowScanOnlyBytes_.load(std::memory_order_relaxed),
        shadowOracleBuildMaxUs_.load(std::memory_order_relaxed),
        shadowScanMaxUs_.load(std::memory_order_relaxed),
    };
}

bool SimpleMarkSweepGC::shadowScanSawScanOnly(const void* control) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const ObjControl* c : lastScanOnly_) {
        if (static_cast<const void*>(c) == control) {
            return true;
        }
    }
    return false;
}

bool SimpleMarkSweepGC::conservativeMarkingEnabled() noexcept {
    // Default ON since the A1 gates passed (suite green in both modes,
    // stress + quarantine clean, FC loaded matrix clean, and the
    // previously-crashing FC low-threshold startup now completes --
    // conservative retention fixed a real unrooted-startup-local hole).
    // ROXAL_GC_CONSERVATIVE=0 is the kill switch (also used by the two
    // prompt-cycle-death tests via runtests.py).
    static const bool enabled = [] {
        const char* v = std::getenv("ROXAL_GC_CONSERVATIVE");
#ifdef __EMSCRIPTEN__
        // Default OFF on wasm: locals live outside scannable linear memory,
        // so the conservative contract ("a Value held only in a C++ local
        // survives") cannot be met -- the wasm test runner has always set
        // ROXAL_GC_CONSERVATIVE=0 for exactly this reason, while the browser
        // host silently ran the unsatisfiable default. Precise mode uses the
        // GCNoParkScope pathways (compiles wait out collections) instead.
        return v && *v == '1';
#else
        return !(v && *v == '0');
#endif
    }();
    return enabled;
}

SimpleMarkSweepGC::CollectionResult SimpleMarkSweepGC::performCollection(std::unique_lock<std::mutex>& lock) {
    (void)lock;

    // Invariant check (positive evidence, not last-writer diagnostics): a
    // collection must NEVER be performed by a thread holding an RT yield
    // section -- safepoint()/externalParticipantSafepoint() early-return for
    // section holders, so this counter staying 0 is the release-mode proof.
    if (inGCYieldSectionOnThisThread()) {
        statsSectionCollectorViolations_.fetch_add(1, std::memory_order_relaxed);
    }

#if defined(ROXAL_GC_FORENSICS) && !defined(ROXAL_GC_FORENSICS_FIELDS_ONLY)
    // INVARIANT (positive evidence): the barrier admitted this collection only
    // because no context was Running.  If any context is Running HERE, the
    // barrier is broken and every edge that thread stores from now on is
    // invisible to this mark.
    if (roxalForensicOn(ROXAL_FC_VERIFY)) {
        static std::atomic<unsigned> reported{0};
        for (const auto& c : contexts_) {
            if (!c || c->state != MutatorContext::State::Running) continue;
            if (reported.fetch_add(1, std::memory_order_relaxed) < 6) {
#ifndef __EMSCRIPTEN__
                std::fprintf(stderr,
                    "GC BARRIER-VIOLATION: Running context at collection start: "
                    "owner=%llu depth=%u | collector=%llu contexts=%zu\n",
                    (unsigned long long)std::hash<std::thread::id>{}(c->owner),
                    c->runningDepth,
                    (unsigned long long)std::hash<std::thread::id>{}(std::this_thread::get_id()),
                    contexts_.size());
#endif
            }
        }
    }
#endif
#if defined(ROXAL_GC_FORENSICS) && !defined(ROXAL_GC_FORENSICS_FIELDS_ONLY)
    // Is the heap already broken before we touch anything this cycle?
    roxalForensicHeapCheck("collection start");
#endif
    collectionInProgress_.store(true, std::memory_order_release);
    struct CollectionScope {
        explicit CollectionScope(SimpleMarkSweepGC& owner) : gc(owner) {}
        ~CollectionScope() {
            gc.collectionInProgress_.store(false, std::memory_order_release);
        }

        SimpleMarkSweepGC& gc;
    } scope(*this);

    const std::uint64_t previousEpoch = epoch_.fetch_add(1uLL, std::memory_order_relaxed);
    std::uint64_t epoch = previousEpoch + 1uLL;
    if (epoch == 0) {
        epoch = 1uLL;
        epoch_.store(epoch, std::memory_order_relaxed);
        forEachControlLocked([&](ObjControl* control) {
            control->markEpoch.store(0uLL, std::memory_order_relaxed);
        });
    }

    struct MarkWorklist : ValueVisitor {
        explicit MarkWorklist(std::uint64_t epoch) : epoch(epoch) {}

        void visit(const Value& value) override {
            if (!value.isObj()) {
                return;
            }
            if (value.isWeak()) {
                return;
            }

            Obj* obj = value.asObj();
            if (!obj) {
                return;
            }

            ObjControl* control = obj->control;
            if (!control) {
                return;
            }

            if (control->collecting.load(std::memory_order_relaxed)) {
                return;
            }

            std::uint64_t previous = control->markEpoch.load(std::memory_order_relaxed);
            if (previous == epoch) {
                return;
            }

            control->markEpoch.store(epoch, std::memory_order_relaxed);
            worklist.push_back(obj);
        }

        void drain() {
            while (!worklist.empty()) {
                Obj* current = worklist.back();
                worklist.pop_back();
                if (current) {
#ifdef ROXAL_GC_FORENSICS
                    if (current->control)
                        current->control->tracedEpoch.store(epoch, std::memory_order_relaxed);
#endif
                    current->trace(*this);
                }
            }
        }

        std::uint64_t epoch;
        std::vector<Obj*> worklist;
    } marker(epoch);

    visitRoots(marker);
    marker.drain();

    // MVCC: mark version chain snapshots.
    // Version chain entries hold raw Obj* snapshots that aren't reachable via
    // normal Value tracing.  We must mark them so the sweep doesn't collect
    // objects that an active snapshot may still need for version resolution.
    // (Lambda: runs again after conservative scan-marking, which can newly
    // mark objects whose version chains then need the same coverage.)
    auto markVersionChainSnapshots = [&]() {
        forEachControlLocked([&](ObjControl* control) {
            if (!control->obj)
                return;
            if (control->markEpoch.load(std::memory_order_relaxed) != epoch)
                return;
            ObjVersion* ver = control->versionChain.load(std::memory_order_relaxed);
            while (ver) {
                if (ver->snapshot && ver->snapshot->control) {
                    ObjControl* snapCtrl = ver->snapshot->control;
                    if (!snapCtrl->collecting.load(std::memory_order_relaxed) &&
                        snapCtrl->markEpoch.load(std::memory_order_relaxed) != epoch) {
                        snapCtrl->markEpoch.store(epoch, std::memory_order_relaxed);
                        // Trace the snapshot's children too (they may hold refs)
                        marker.worklist.push_back(ver->snapshot);
                    }
                }
                ver = ver->prev;
            }
        });
    };
    markVersionChainSnapshots();
    marker.drain();

    // MVCC: trim version chains on live objects.
    // All threads are at a safepoint, so no concurrent readers.
    {
        uint64_t minEpoch = snapshotEpochTracker.minEpoch();
        forEachControlLocked([&](ObjControl* control) {
            if (!control->obj)
                return;
            if (control->collecting.load(std::memory_order_relaxed))
                return;
            // Only trim marked (live) objects that have a version chain
            if (control->markEpoch.load(std::memory_order_relaxed) == epoch
                && control->versionChain.load(std::memory_order_relaxed) != nullptr) {
                control->obj.load(std::memory_order_relaxed)->trimVersionChain(minEpoch);
            }
        });
    }

    // Shadow scan (A0): mark state is final here (roots + MVCC snapshots all
    // drained) and the sweep has not started -- the scan compares first
    // (stats keep their "what conservatism adds" meaning either way).
    if (shadowScanEnabled()) {
        shadowScanParkedStacks(epoch);
        // A1: conservative marking -- objects referenced only from a parked
        // C++ stack become live.  Their reachable children and version
        // chains get the same coverage as precise roots.
        if (conservativeMarkingEnabled() && !lastScanOnly_.empty()) {
            for (ObjControl* control : lastScanOnly_) {
                if (!control || !control->obj)
                    continue;
                if (control->collecting.load(std::memory_order_relaxed))
                    continue;
                control->markEpoch.store(epoch, std::memory_order_relaxed);
                marker.worklist.push_back(control->obj);
            }
            marker.drain();
            markVersionChainSnapshots();
            marker.drain();
        }
    }

#ifdef ROXAL_GC_FORENSICS
    // MARK VERIFICATION (ROXAL_FC_VERIFY): mark state is final and nothing has
    // been claimed yet, so this is the last instant at which the question
    // "would this sweep free something still referenced?" can be answered
    // BEFORE the damage.  Walk every live (marked) object's strong edges; a
    // child that is unmarked will be swept out from under its live parent,
    // and a child already claimed dying is a refcount that reached zero while
    // a live parent still held it (the `strong=-1` signature).  Both are
    // impossible in a correct heap, and both name the parent type that owns
    // the bad edge -- which is the whole point: attribution at the cause
    // instead of at the eventual use-after-free.
    //
    // O(live objects x edges) per collection, hence its own flag bit.
    if (roxalForensicOn(ROXAL_FC_VERIFY)) {
        struct VerifyVisitor : ValueVisitor {
            std::uint64_t epoch { 0 };
            Obj* parent { nullptr };
            std::uint64_t unmarked { 0 };
            std::uint64_t dying { 0 };
            Obj* firstParent { nullptr };
            Obj* firstChild { nullptr };
            ObjControl* firstChildCtrl { nullptr };
            const char* firstKind { nullptr };
            std::uint64_t firstParentTraced { 0 };

            void visit(const Value& value) override {
                if (!value.isObj() || value.isWeak())
                    return;
                Obj* child = value.asObj();
                if (!child)
                    return;
                ObjControl* c = child->control;
                if (!c)
                    return;

                const bool claimed = c->collecting.load(std::memory_order_relaxed)
                                     || c->obj.load(std::memory_order_relaxed) == nullptr;
                const bool willSweep =
                    !claimed && c->markEpoch.load(std::memory_order_relaxed) != epoch;
                if (!claimed && !willSweep)
                    return;

                const char* kind = claimed ? "DYING-CHILD" : "MARK-MISS";
                if (claimed) ++dying; else ++unmarked;
                if (!firstChild) {
                    firstKind = kind;
                    firstParent = parent;
                    firstChild = child;
                    firstChildCtrl = c;
                    firstParentTraced = (parent && parent->control)
                        ? parent->control->tracedEpoch.load(std::memory_order_relaxed) : 0;
                }
            }
        } verifier;
        verifier.epoch = epoch;

        forEachControlLocked([&](ObjControl* control) {
            Obj* obj = control->obj;
            if (!obj)
                return;
            if (control->collecting.load(std::memory_order_relaxed))
                return;
            if (control->markEpoch.load(std::memory_order_relaxed) != epoch)
                return;   // this one is garbage; its edges are not our problem
            verifier.parent = obj;
            obj->trace(verifier);
        });

        const std::uint64_t bad = verifier.unmarked + verifier.dying;
        if (bad && verifier.firstChild) {
            roxalForensicMarkMiss(
                verifier.firstKind,
                (const void*)verifier.firstParent,
                verifier.firstParent ? unsigned(verifier.firstParent->type) : 0u,
                (const void*)verifier.firstChild,
                unsigned(verifier.firstChild->type),
                verifier.firstChildCtrl,
                (unsigned long long)epoch,
                (unsigned long long)bad,
                (unsigned long long)verifier.firstParentTraced);
        }
        statsVerifyBadEdges_.fetch_add(bad, std::memory_order_relaxed);
    }
#endif

    std::vector<Obj*> unreachable;
    unreachable.reserve(64);
    std::uint64_t totalFreedBytes = 0;
    forEachControlLocked([&](ObjControl* control) {
        Obj* obj = control->obj;
        if (!obj) {
            return;
        }

        if (control->collecting.load(std::memory_order_relaxed)) {
            return;
        }

        if (control->markEpoch.load(std::memory_order_relaxed) == epoch) {
            return;
        }

#if defined(ROXAL_GC_FORENSICS) && !defined(ROXAL_GC_FORENSICS_FIELDS_ONLY)
        // ROXAL_FC_LEAK: identify this object as garbage but do not reclaim
        // it.  Purely diagnostic -- see the flag's definition.
        if (roxalForensicOn(ROXAL_FC_LEAK))
            return;
#endif

        // Claim the death ATOMICALLY -- the same protocol Obj::decRef uses.
        // The earlier load() above is only a cheap early-out: reclamation
        // (VM::freeObjects) runs under a DIFFERENT mutex and its
        // dropReferences() decRefs children, so a refcount death can race
        // this sweep for the same control block.  A load-then-store pair
        // lets both sides win and retire the object twice: dropReferences
        // and delObj run twice, and the second pass releases an already-
        // freed shared_ptr (elts_/properties_) -- the observed corruption.
        if (control->collecting.exchange(true, std::memory_order_acq_rel)) {
            return;   // the refcount path claimed it first
        }
#ifdef ROXAL_GC_FORENSICS
        if (roxalForensicOn(ROXAL_FC_PROV)) {
        control->deathPath.store(2, std::memory_order_relaxed);
        control->deathThread.store(
            std::hash<std::thread::id>{}(std::this_thread::get_id()),
            std::memory_order_relaxed);
        control->deathEpoch.store(epoch, std::memory_order_relaxed);
        control->objTypeAtDeath.store(static_cast<std::uint8_t>(obj->type),
                                      std::memory_order_relaxed);
        control->retireCount.fetch_add(1, std::memory_order_relaxed);
        }
#endif
        // weak derefs observe death immediately; release pairs with the
        // acquire loads in strongRef/isAlive (RT yield sections and covered
        // native frames can run weak derefs while the sweep proceeds)
        control->obj.store(nullptr, std::memory_order_release);

        totalFreedBytes += control->allocationSize;

        // Edge-severing (dropReferences) deliberately does NOT happen here:
        // the reclaimer batch-drops the whole retired set AFTER mutators
        // resume, so the stop-the-world window pays no destructor-adjacent
        // cost and cycles still see consistent, fully dropped batches.
        unreachable.push_back(obj);
    });

    // Segment hygiene while the world is stopped: compact sealed segments'
    // tombstones and unlink empty ones.
    compactSegmentsLocked();

    lastFreedCount_.store(unreachable.size(), std::memory_order_relaxed);
    lastFreedBytes_.store(totalFreedBytes, std::memory_order_relaxed);
    VM::instance().cleanupWeakRegistries();

    CollectionResult result;
    result.unreachable = std::move(unreachable);
    result.freedBytes = totalFreedBytes;
    return result;
}

void SimpleMarkSweepGC::visitRoots(ValueVisitor& visitor) {
    VM& vm = VM::instance();

    // The ThreadManager index is the SOLE interpreter-root
    // source -- every Thread self-registers at construction (main, init,
    // REPL, dataflow engine, actor workers, yielded FuncNode execution
    // threads, compute workers), so the old vm.threads / replThread /
    // dataflowEngineThread / VM::thread special-case gathering (and its
    // blind spots) is gone.  World is stopped here; the manager's mutex
    // additionally excludes construction/destruction races.
    ThreadManager::instance().forEachThread([&](Thread& t) {
        visitThreadRoots(t, visitor);
    });

    // Unlocked iteration is REQUIRED here: the collector holds mutex_ for the
    // whole collection, while VariablesMap mutators can hold varsLock/
    // globalsLock across GC-touching work (ensureSignal allocates, assign
    // fires callbacks, Value destruction hits unregisterAllocation) -- taking
    // the map locks here would be a classic lock-order inversion (deadlock).
    // Safety comes from the coverage invariant instead: every thread that
    // mutates these maps is either inside execute() (parked at a safepoint
    // during collection) or holds an ExternalParticipant / GC yield section
    // (the barrier waits for it), so no mutation is concurrent with this scan.
    vm.globals.unsafeForEachModuleVar([&visitor](const auto& entry) {
        visitStrongValue(visitor, entry.second.value);
        if (entry.second.hasSignal())
            visitStrongValue(visitor, entry.second.signal);
    });
    vm.globals.unsafeForEachGlobal([&visitor](const auto& entry) {
        visitStrongValue(visitor, entry.second.value);
        if (entry.second.hasSignal())
            visitStrongValue(visitor, entry.second.signal);
    });

    visitStrongValue(visitor, vm.dataflowEngineActor);

    // Signals queued for event-driven island evaluation (handed off from
    // non-VM producer threads, e.g. the DDS reader-signal thread) may have
    // lost their last reachable ObjSignal wrapper while queued; trace their
    // embedded values so the drain doesn't read swept objects.
    if (auto dfEngine = df::DataflowEngine::instance(false)) {
        dfEngine->tracePendingEventUpdates(visitor);
        // Root every engine-held signal's buffered values: wrapper-less
        // signals (kept alive by the engine / stale islands) still hold
        // strong refcounted Values the tracer can't otherwise reach --
        // sweeping their targets leaves the maps holding dangling refs that
        // double-free when the signal is eventually destroyed.
        dfEngine->traceAllSignals(visitor);
    }

    // Async file-IO ops queued or executing hold their file handle as a
    // Value (plus possibly an already-resolved result) that may have no
    // other reachable reference — a fire-and-forget fileio.write whose
    // future was discarded must not have its ObjFile swept out from under
    // the worker thread.
    // AsyncIOManager.cpp is only compiled when the fileio module is enabled, so
    // this root has to be gated the same way -- otherwise a FILEIO=OFF build
    // fails to link on the two symbols below.
    #ifdef ROXAL_ENABLE_FILEIO
    if (AsyncIOManager* aio = AsyncIOManager::instanceIfCreated())
        aio->tracePending(visitor);
    #endif

    // ai.nn: input tensors held by queued/in-flight InferenceWorker jobs. The
    // caller may have dropped its references the moment predict() returned,
    // and the worker thread's stack is invisible to the collector. Locks
    // internally.
    #ifdef ROXAL_ENABLE_AI_NN
    roxal::nnTraceWorkers(visitor);
    #endif

    // ai.nn wasm provider bridge: in-flight requests pin their futures and
    // input tensors until the host's reply lands. Locks internally.
    #if defined(__EMSCRIPTEN__) && defined(ROXAL_ENABLE_AI_NN)
    roxal::web::nnTracePending(visitor);
    #endif

    // JS->Roxal callbacks (dom.on handlers, closures encoded into store
    // payloads): registered in a C++ heap map that nothing else traces.
    #ifdef __EMSCRIPTEN__
    roxal::web::traceCallbacks(visitor);
    #endif

    visitStrongValue(visitor, vm.conditionalInterruptClosure);
    visitStrongValue(visitor, vm.combinatorRelayFunction);
    visitStrongValue(visitor, vm.initString);

    // RT REPL pending closure (in-flight between setupLine and runFor)
    if (vm.pendingRTClosure_.isObj())
        visitStrongValue(visitor, vm.pendingRTClosure_);

    for (const auto& typeEntry : vm.builtinMethods) {
        for (const auto& methodEntry : typeEntry.second) {
            methodEntry.second.trace(visitor);
        }
    }

    ObjModuleType::allModules.unsafeApply([&visitor](const auto& modules) {
        for (const auto& moduleVal : modules) {
            visitStrongValue(visitor, moduleVal);
        }
    });

    // Cross-compiler user-module registry. In practice every Value here is
    // also reachable via ObjModuleType::allModules above, but visit
    // explicitly so the registry is self-sufficient as a root — if a future
    // change ever introduces a path that registers a module without also
    // pushing to allModules, the registry stays correct.
    {
        std::lock_guard<std::mutex> guard(vm.userModuleRegistryMutex);
        for (const auto& entry : vm.userModuleRegistry) {
            visitStrongValue(visitor, entry.second);
        }
    }

    // Note: ObjObjectType::enumTypes is NOT visited here. It holds raw pointers for
    // fast enum value -> type lookup, but enum types are already rooted through modules:
    // - User-defined enums are stored in module vars via DefineModuleVar opcode
    // - DDS/Proto enums are stored in module vars via registerGeneratedTypes()
    // - ObjModuleType::trace() traces all vars, so enum types are visited via allModules above
    // Typed persistent roots: iterated UNDER persistentRootsMutex_.  The
    // stopped-world barrier does not exclude every registrar -- a thread can
    // construct a self-registering root before it has any GC coverage at all
    // (e.g. a compute-server connection thread building its actor registry
    // before entering an ExternalParticipant) -- so lockless iteration here
    // would race that insert.  Cost: one uncontended lock per collection;
    // a registrar racing a collection briefly blocks, which is exactly the
    // ordering we want.
    {
        std::lock_guard<std::mutex> rootsLock(persistentRootsMutex_);
        for (const GCRootBase* root : persistentRoots_) {
            if (root != nullptr) {
                root->traceRoot(visitor);
            }
        }
    }
}

size_t SimpleMarkSweepGC::collectNowForShutdown() {
    std::vector<Obj*> toDestroy;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        debug_assert_msg(!anyRunningLocked(),
                         "collectNowForShutdown requires no Running contexts");
        debug_assert_msg(rtSectionCount_.load(std::memory_order_seq_cst) == 0,
                         "collectNowForShutdown requires no RT GC-yield sections");
        CollectionResult result = performCollection(lock);
        toDestroy = std::move(result.unreachable);
        collectionRequested_.store(false, std::memory_order_release);
        bytesAllocatedSinceLastCollect_.store(0, std::memory_order_relaxed);
    }

    retireObjects(toDestroy);   // one atomic publish (see retireObjects)

    return toDestroy.size();
}

// ---- Coordination self-test (sys._runtests('gc_coordination')) --------------

namespace {
// §6 gate 10 helper (see runScannerRecallSelfTest case 6): build an
// alive-but-unrooted long string in THIS dead-by-return frame.  Publish the
// expected control pointer outside the scanned stack and hand back only the
// ICU aux-buffer pointer.
//
// Keeping the expected identity in a side channel is essential: when the
// caller kept an XOR-hidden copy, GCC 10 hoisted the later XOR decode across
// forceCollection() and held the raw control pointer in a callee-saved
// register.  Stack capture then spilled that register and made this negative
// control report a false retention.
std::atomic<ObjControl*> borrowedAuxExpectedControl { nullptr };

#if defined(__GNUC__)
__attribute__((noinline))
#endif
const char16_t* makeBorrowedAuxCase()
{
    // Long content => ICU heap-allocates the buffer (short strings live
    // inline inside the GC block and WOULD legitimately be found).
    // Build via append (a fill, not a char_traits::copy) rather than
    // `const char* + std::string`: the operator+ temporary's 16-byte SSO
    // buffer confuses GCC's -Wstringop-overflow under IPA constprop.
    std::string content("scanner-borrowed-aux-");
    content.append(160, 'x');
    Value v = Value::stringVal(ustring::fromUTF8(content));
    ObjString* so = asStringObj(v);
    const char16_t* buf = so->s.getBuffer();
    borrowedAuxExpectedControl.store(so->control, std::memory_order_release);
    new Value(v);  // alive-but-unrooted, as in the recall cases
    return buf;
}
}  // namespace

// ---- Scanner recall self-test (sys._runtests('gc_scanner')) ----------------
// Each case leaves an object alive via a deliberately LEAKED heap Value
// (strong ref, but heap memory -- invisible to the precise roots), keeps
// only the probed reference form reachable from this thread's stack or a
// register, forces a collection (this registered script thread parks at
// safepoint(), so its stack is captured and shadow-scanned), and asserts the
// scanner flagged the object as scan-only.  The object IS still swept in
// this test (the assertion targets the scan, not retention), so the leaked
// Value dangles harmlessly (never touched again) and control pointers are
// compared by VALUE only.
// Frame-residue caveat: plain pointer temporaries from object setup can
// linger in this frame's dead slots and also hit via rule (ii); the
// assertions test RECALL (the scanner finds the object), not per-rule
// isolation -- a strict per-rule isolation harness is a possible follow-up
// gate, not part of A0.
bool SimpleMarkSweepGC::runScannerRecallSelfTest()
{
    VM::instance();
    if (!VM::thread) {
        std::cerr << "GC scanner self-test: no VM thread" << std::endl;
        return false;
    }
    if (!shadowScanEnabled()) {
        std::cerr << "GC scanner self-test summary: skipped (ROXAL_GC_SHADOW_SCAN=0)"
                  << std::endl;
        return true;
    }

    constexpr std::uintptr_t kEnc = 0x5a5a5a5a5a5a5a5aull;  // XOR-hide control ptrs
    int cases = 0;
    int failures = 0;

    auto forceCollection = [&]() {
        requestCollect();
        safepoint(*VM::thread);
    };
    auto makeLeakedUnrooted = [](const char* tag) -> std::pair<Obj*, const void*> {
        Value v = Value::stringVal(ustring::fromUTF8(
            std::string("scanner-recall-") + tag));
        Obj* obj = v.asObj();
        new Value(v);  // leaked strong ref: alive when v dies, yet unrooted
        return { obj, obj->control };
    };
    auto expectScanOnly = [&](const void* ctrl, const char* what) {
        ++cases;
        if (!shadowScanSawScanOnly(ctrl)) {
            ++failures;
            std::cerr << "GC scanner self-test: scanner MISSED " << what << std::endl;
        }
    };

    // Case 1: raw Obj* as the stack reference (rule ii).
    {
        auto [obj, ctrl] = makeLeakedUnrooted("raw");
        volatile std::uintptr_t rawKeep = reinterpret_cast<std::uintptr_t>(obj);
        forceCollection();
        (void)rawKeep;
        expectScanOnly(ctrl, "raw Obj* stack reference");
    }

    // Cases 2-4: NaN-tagged words built from the documented encoding
    // (Value.h: object => QNAN|SignBit|payload), incl. const- and weak-
    // flagged variants the scanner must normalize before payload extraction.
    {
        std::uintptr_t objEnc = 0, ctrlEnc = 0;
        {
            auto [obj, ctrl] = makeLeakedUnrooted("tagged");
            objEnc = reinterpret_cast<std::uintptr_t>(obj) ^ kEnc;
            ctrlEnc = reinterpret_cast<std::uintptr_t>(ctrl) ^ kEnc;
        }
        const std::uint64_t encoded =
            static_cast<std::uint64_t>(objEnc ^ kEnc) | QNAN | SignBit;
        volatile std::uint64_t taggedKeep = encoded;
        volatile std::uint64_t constKeep = encoded | ConstMask;
        volatile std::uint64_t weakKeep = encoded | WeakMask;
        forceCollection();
        (void)taggedKeep; (void)constKeep; (void)weakKeep;
        expectScanOnly(reinterpret_cast<const void*>(ctrlEnc ^ kEnc),
                       "NaN-tagged (plain/const/weak) stack words");
    }

    // Case 5: callee-saved-register-only reference.  Any function on the
    // park path that uses r13 must first save it into a scanned frame, and
    // safepoint()'s __builtin_unwind_init() force-spills it regardless.
#if defined(__GNUC__) && defined(__x86_64__)
    {
        std::uintptr_t objAddr = 0, ctrlEnc = 0;
        {
            auto [obj, ctrl] = makeLeakedUnrooted("register");
            objAddr = reinterpret_cast<std::uintptr_t>(obj);
            ctrlEnc = reinterpret_cast<std::uintptr_t>(ctrl) ^ kEnc;
        }
        // Explicit local register variable (the documented GCC/Clang form;
        // remains valid under C++17): the value LIVES in r13 across the
        // collection, so the compiler cannot reuse the register in between.
        {
            register std::uintptr_t keep asm("r13") = objAddr;
            asm volatile("" : "+r"(keep));   // materialize before parking
            objAddr = 0;                     // best-effort clear of the stack copy
            forceCollection();
            asm volatile("" : "+r"(keep));   // keep live across the collection
        }
        expectScanOnly(reinterpret_cast<const void*>(ctrlEnc ^ kEnc),
                       "register-only (r13) reference");
    }
#endif

    // Case 6: a pointer into
    // an object's AUXILIARY native storage -- here a long ObjString's
    // heap-allocated ICU buffer, a separate allocation the address oracle
    // deliberately does not map -- must NOT retain the object.  This is the
    // borrowed-pointer contract: the OWNER must stay rooted.
    // Construction happens in a noinline helper so the object/control
    // pointers only ever exist in a DEAD frame (then scrubbed) -- this
    // frame holds only the aux buffer pointer under test.  The expected
    // control identity lives in a test-only atomic side channel and is not
    // loaded until after the collection.
    {
        const char16_t* buf = makeBorrowedAuxCase();
        const char16_t* volatile bufKeep = buf;
        gcScrubDeadFrames();  // erase construction residue in the dead frame
        forceCollection();
        ObjControl* expected = borrowedAuxExpectedControl.exchange(
            nullptr, std::memory_order_acq_rel);
        ++cases;
        if (!expected) {
            ++failures;
            std::cerr << "GC scanner self-test: aux-storage case lost its "
                         "expected control identity" << std::endl;
        }
        else if (shadowScanSawScanOnly(expected)) {
            ++failures;
            std::cerr << "GC scanner self-test: aux-storage pointer RETAINED "
                         "its owner (borrowed-pointer contract violated -- "
                         "or stack residue survived the scrub)" << std::endl;
        }
        (void)bufKeep;
    }

    std::cerr << "GC scanner self-test summary: " << cases << " cases, "
              << failures << " failures" << std::endl;
    return failures == 0;
}

bool SimpleMarkSweepGC::runCoordinationSelfTest(std::chrono::milliseconds duration)
{
    using namespace std::chrono;
    using namespace std::chrono_literals;

    // Collections free through the VM; make sure it exists.
    VM::instance();

    const CoordinationStats before = coordinationStats();
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> rtEntries{0};
    std::atomic<std::uint64_t> rtSkips{0};
    std::atomic<std::uint64_t> polls{0};
    std::atomic<std::uint64_t> blockScopes{0};
    std::atomic<std::uint64_t> allocs{0};
    std::atomic<int> running{0};

    // "RT" workers: yield sections racing requestCollect.  The in-section
    // allocation exercises the deferred-wake path (a threshold crossing
    // inside a section must not do blocking wakes).
    auto rtWorker = [&](int idx) {
        running.fetch_add(1, std::memory_order_relaxed);
        std::uint64_t i = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            if (GCYieldScope gcs{}; gcs) {
                rtEntries.fetch_add(1, std::memory_order_relaxed);
                if ((i & 7) == 0) {
                    Value v = Value::stringVal(ustring::fromUTF8(
                        "st-rt-" + std::to_string(idx) + "-" + std::to_string(i)));
                    (void)v;
                    allocs.fetch_add(1, std::memory_order_relaxed);
                }
            } else {
                rtSkips.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(20us);  // let the collection run
            }
            ++i;
        }
        running.fetch_sub(1, std::memory_order_relaxed);
    };

    // Participant workers: persistent registration + polling, periodic
    // NESTED participants (owner-based whole-thread parking), periodic
    // GCSafeBlockScope (opaque blocking call), interned-string churn (the
    // locked reuse/publish/self-erase intern paths under concurrent drains).
    auto participantWorker = [&](int idx, bool nest) {
        running.fetch_add(1, std::memory_order_relaxed);
        ExternalParticipant participant(*this);
        std::uint64_t i = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            participant.pollSafepointIfRequested();
            polls.fetch_add(1, std::memory_order_relaxed);
            {
                Value v = Value::stringVal(ustring::fromUTF8(
                    "st-p" + std::to_string(idx) + "-" + std::to_string(i & 63)));
                (void)v;
                allocs.fetch_add(1, std::memory_order_relaxed);
            }
            if (nest && (i & 15) == 0) {
                ExternalParticipant nested(*this);
                nested.pollSafepointIfRequested();
            }
            if ((i & 63) == 0) {
                GCSafeBlockScope block;
                std::this_thread::sleep_for(50us);
                blockScopes.fetch_add(1, std::memory_order_relaxed);
            }
            ++i;
            std::this_thread::sleep_for(10us);
        }
        running.fetch_sub(1, std::memory_order_relaxed);
    };

    std::vector<std::thread> workers;
    {
        // The driving thread is typically a REGISTERED script thread (this
        // runs via sys._runtests inside execute()) that won't reach a
        // safepoint poll for the whole hammer -- cover it with a
        // GCSafeBlockScope so the barrier proceeds without it.  (Dogfoods
        // the primitive; also correct for an unregistered caller, where the
        // scope no-ops.)
        GCSafeBlockScope driverCover;

        workers.emplace_back(rtWorker, 0);
        workers.emplace_back(rtWorker, 1);
        workers.emplace_back(participantWorker, 0, false);
        workers.emplace_back(participantWorker, 1, true);
        workers.emplace_back(participantWorker, 2, true);

        // Driver: request collections as fast as they can complete.
        const auto deadline = steady_clock::now() + duration;
        while (steady_clock::now() < deadline) {
            requestCollect();
            std::this_thread::sleep_for(300us);
        }
        stop.store(true, std::memory_order_relaxed);

        // Watchdog join: a coordination bug shows up as workers stuck at the
        // barrier.  Don't hang the diagnostic -- report and hard-exit.
        const auto joinDeadline = steady_clock::now() + 5s;
        while (running.load(std::memory_order_relaxed) != 0) {
            if (steady_clock::now() > joinDeadline) {
                std::cerr << "GC self-test FAILED: " << running.load()
                          << " worker(s) failed to stop within 5s -- "
                          << "coordination deadlock (see stacks with a debugger)"
                          << std::endl;
                std::_Exit(2);
            }
            std::this_thread::sleep_for(1ms);
        }
        for (auto& w : workers) {
            w.join();
        }
    }

    const CoordinationStats after = coordinationStats();
    const std::uint64_t collections = after.collections - before.collections;
    const std::uint64_t violations =
        after.sectionCollectorViolations - before.sectionCollectorViolations;
    const std::uint32_t sectionsAtRest =
        rtSectionCount_.load(std::memory_order_seq_cst);

    bool pass = true;
    if (collections == 0) {
        std::cerr << "GC self-test FAILED: no collections were performed"
                  << std::endl;
        pass = false;
    }
    if (violations != 0) {
        std::cerr << "GC self-test FAILED: " << violations
                  << " collection(s) ran on a section-holding (RT) thread"
                  << std::endl;
        pass = false;
    }
    if (sectionsAtRest != 0) {
        std::cerr << "GC self-test FAILED: rtSectionCount_ = " << sectionsAtRest
                  << " at rest (leaked yield section)" << std::endl;
        pass = false;
    }

    // Counters go to stderr: the script-facing entry point
    // (sys._runtests('gc_coordination')) prints a deterministic verdict on
    // stdout for exact .out comparison, while the suite's .err regex asserts
    // the summary shape (>=1 collection, 0 violations).
    std::cerr << "GC self-test summary: "
              << collections << " collections, "
              << rtEntries.load() << " RT sections ("
              << rtSkips.load() << " skips), "
              << polls.load() << " participant polls, "
              << blockScopes.load() << " block scopes, "
              << allocs.load() << " allocations, "
              << violations << " violations" << std::endl;
    return pass;
}

} // namespace roxal
