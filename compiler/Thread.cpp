#include "Thread.h"
#include "VM.h"
#include "Object.h"
#include "SimpleMarkSweepGC.h"
#ifdef ROXAL_COMPUTE_SERVER
#include "ComputeConnection.h"
#endif
#include <algorithm>
#include <iostream>
#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

using namespace roxal;

namespace {
// Actor workers are ALWAYS non-RT: a
// worker spawned from an RT-scheduled parent (e.g. an actor constructed
// from a SCHED_FIFO control-loop slice) inherits the parent's policy and
// would compete with the control loop at RT priority.  Demote
// unconditionally -- same pattern as the dedicated GC collector thread --
// and additionally keep the worker off the host's reserved RT core when
// core exclusion is configured.
void demoteWorkerToNonRT()
{
#ifdef __linux__
    struct sched_param param {};
    param.sched_priority = 0;
    pthread_setschedparam(pthread_self(), SCHED_OTHER, &param);

    const int excludeCore = VM::instance().rtCoreExclusion();
    if (excludeCore >= 0) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        const unsigned int numCpus = std::thread::hardware_concurrency();
        for (unsigned int i = 0; i < numCpus; ++i) {
            if (static_cast<int>(i) != excludeCore)
                CPU_SET(i, &cpuset);
        }
        pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    }
#endif
}
} // namespace

#ifdef ROXAL_COMPUTE_SERVER
namespace {
ustring remoteMethodNameForCall(const ActorInstance::MethodCallInfo& callInfo)
{
    if (isBoundMethod(callInfo.callee)) {
        auto* boundMethod = asBoundMethod(callInfo.callee);
        auto* closure = asClosure(boundMethod->method);
        auto* function = asFunction(closure->function);
        return function->name;
    }

    if (isBoundNative(callInfo.callee)) {
        auto* boundNative = asBoundNative(callInfo.callee);
        if (isFunction(boundNative->declFunction))
            return asFunction(boundNative->declFunction)->name;
        throw std::runtime_error("remote actor call missing declared function metadata");
    }

    throw std::runtime_error("unsupported remote actor callee");
}
}
#endif

Thread::~Thread()
{
    // Unregister from the thread index FIRST: the collector must never see
    // a partially-destructed Thread (destruction runs on an unparked
    // mutator, so no scan is concurrent -- the mutex makes the set edit
    // safe regardless).
    ThreadManager::instance().unregisterThread(this);

    openUpvalues.clear();

    // remove any event subscriptions for this thread
    for (auto& entry : eventHandlers) {
        if (!entry.first.isAlive()) continue;
        ObjEventType* ev = asEventType(entry.first);
        for (const auto& handler : entry.second) {
            if (!handler.closure.isAlive())
                continue;
            for (auto it = ev->subscribers.begin(); it != ev->subscribers.end(); ) {
                if (!it->isAlive() || asClosure(*it) != asClosure(handler.closure)) {
                    ++it;
                    continue;
                }
                it = ev->subscribers.erase(it);
            }
        }
    }
}

void Thread::pruneEventRegistrations()
{
    // Walk the strong map of event -> handlers removing entries whose event
    // object was reclaimed or whose subscribers no longer point at a live
    // closure. This keeps the event registry from pinning dead closures and
    // ensures the Value GC can drop cycles that involve events.
    for (auto it = eventHandlers.begin(); it != eventHandlers.end();) {
        Value eventRef = it->first;
        if (!eventRef.isAlive()) {
            eventToSignal.erase(eventRef);
            it = eventHandlers.erase(it);
            continue;
        }

        ObjEventType* ev = nullptr;
        if (eventRef.isObj()) {
            ev = asEventType(eventRef);
        }

        if (!ev) {
            eventToSignal.erase(eventRef);
            it = eventHandlers.erase(it);
            continue;
        }

        // Clean up subscribers that point at dead closures. We only track
        // weak references here so losing the closure automatically unhooks
        // the handler.
        auto& subscribers = ev->subscribers;
        subscribers.erase(std::remove_if(subscribers.begin(), subscribers.end(),
                                         [](const Value& subscriber) {
                                             return !subscriber.isAlive();
                                         }),
                          subscribers.end());

        auto& handlers = it->second;
        handlers.erase(std::remove_if(handlers.begin(), handlers.end(),
                                      [&ev](const HandlerRegistration& handler) {
                                          if (!handler.closure.isAlive() ||
                                              (handler.closure.isWeak() && !handler.closure.isAlive()))
                                              return true;
                                          // Combinator relay registrations whose target combinator is
                                          // dead or already fulfilled are dead weight — prune them
                                          // (also drops the matching weak entry in ev->subscribers).
                                          if (handler.combinatorTarget.isWeak() &&
                                              handler.combinatorTarget.isNonNil()) {
                                              Value cbStrong = handler.combinatorTarget.strongRef();
                                              bool dead = cbStrong.isNil() || !isCombinator(cbStrong);
                                              bool fulfilled = !dead && asCombinator(cbStrong)->fulfilled;
                                              if (dead || fulfilled) {
                                                  if (ev && handler.closure.isNonNil()) {
                                                      Obj* relayObj = handler.closure.asObj();
                                                      auto& subs = ev->subscribers;
                                                      subs.erase(std::remove_if(subs.begin(), subs.end(),
                                                          [&](const Value& sub) {
                                                              return sub.isNonNil() && sub.asObj() == relayObj;
                                                          }), subs.end());
                                                  }
                                                  return true;
                                              }
                                          }
                                          return false;
                                      }),
                       handlers.end());

        if (handlers.empty()) {
            eventToSignal.erase(eventRef);
            it = eventHandlers.erase(it);
            continue;
        }

        ++it;
    }

    // Finally prune the auxiliary map that tracks which signal each event is
    // bound to. Both sides are stored as weak handles so we simply drop any
    // entries whose endpoints are gone.
    for (auto it = eventToSignal.begin(); it != eventToSignal.end();) {
        if (!it->first.isAlive()) {
            it = eventToSignal.erase(it);
            continue;
        }
        if (it->second.isWeak() && !it->second.isAlive()) {
            it = eventToSignal.erase(it);
            continue;
        }
        ++it;
    }
}

void Thread::join(ActorInstance* actorInstOverride)
{
    // When GC schedules this thread for shutdown it may only hold a raw pointer
    // to the actor instance.  Start with the override supplied by the finalizer
    // and fall back to the cached raw pointer if needed before attempting to
    // revive the weak handle below.
    ActorInstance* inst = actorInstOverride;
    if (inst == nullptr)
        inst = actorInstanceRaw.load(std::memory_order_acquire);

    {
        // GC-safe blocking region FIRST, then the join-once mutex: a second
        // joiner waiting on the mutex must count as quiescent, or the
        // collection barrier waits on it while the first joiner waits on a
        // worker that may itself need that collection to exit (four-way
        // deadlock).  The region also covers osthread->join(), as before:
        // the target thread may need a collection to complete before it
        // exits -- without this, actor finalization and shutdown joins can
        // deadlock the collection barrier.
        SimpleMarkSweepGC::GCSafeBlockScope blockScope;
        // Join-once: concurrent joiners (the actor lifecycle thread vs
        // script-end / shutdown joinAllThreads) serialize here; the loser
        // observes osthread == nullptr and returns.  Joining a std::thread
        // twice is UB (observed as a futex hang on the reaped tid).
        std::lock_guard<std::mutex> joinLock { joinMutex_ };
        if (state == State::Constructed || osthread == nullptr || !osthread->joinable())
            return;

        if (actor) {
            if (inst == nullptr && actorInstance.isAlive())
                inst = asActorInstance(actorInstance);
            if (inst != nullptr) {
                std::lock_guard<std::mutex> lock { inst->queueMutex };
                quit = true;
                inst->queueConditionVar.notify_one();
#ifdef ROXAL_COMPUTE_SERVER
                // Remote proxy workers can be blocked in ComputeConnection::future.get()
                // rather than waiting on the actor queue. Abort the transport first so
                // pending remote calls are rejected and the worker can unwind before the
                // blocking std::thread::join() below.
                if (inst->isRemote) {
                    auto conn = inst->remoteConn.lock();
                    if (conn)
                        conn->abort();
                }
#endif
            } else {
                quit = true;
            }
        }

        if (osthread->get_id() == std::this_thread::get_id()) {
            // An actor instance can be collected while the worker thread is in the
            // middle of running its own GC safepoint. Joining the same std::thread
            // would therefore self-deadlock. We already set quit=true and notified
            // the queue above, so detach the std::thread and allow this worker to
            // wind down naturally once it unwinds back out of Thread::act().
            osthread->detach();
        } else {
            osthread->join();
        }

        osthread = nullptr;
        state = State::Completed;
    }

    // Reference cleanup outside the blocked region (Value assignments touch
    // GC refcounts, which the region contract forbids).
    if (inst)
        inst->thread.reset();
    actorInstance = Value::nilVal();
    actorInstanceRaw.store(nullptr, std::memory_order_release);
    actor = false;
}


void Thread::act(Value actorInstance)
{
    assert(isActorInstance(actorInstance));
    this->actorInstance = actorInstance.weakRef();

    actor = true;
    state = State::Spawned;

    // Mark actor as alive before spawning the OS thread so that any queueCall()
    // arriving between now and the thread's first iteration doesn't see alive=false
    // and silently drop the call.
    asActorInstance(actorInstance)->alive.store(true, std::memory_order_release);

    osthread = make_ptr<std::thread>([this]() {
        // Actors always run on their own NON-RT thread (see
        // demoteWorkerToNonRT): never inherit an RT parent's policy.
        demoteWorkerToNonRT();

        // Hoisted above the try: the outer catch must be able to resolve the
        // in-flight call's promise (R2 below) -- destroying it unresolved
        // makes the awaiter crash with future_error(broken_promise) instead
        // of seeing an error.
        ActorInstance::MethodCallInfo callInfo {};
        try {
            auto& vm { VM::instance() };

            vm.thread = ptr_from_this(); // set thread local storage member

            // GC coverage for the WHOLE actor-thread lifetime.  The dispatch
            // loop handles Values (queued callees / args / results) outside
            // any vm.execute() frame -- invisible to the collector, they can
            // be swept while live on this C++ stack, and stores here can hide
            // objects from an in-flight mark.  As a persistent
            // ExternalParticipant the thread is either unparked (no
            // collection can start) or parked at a known-safe point: the
            // top-of-loop poll below, or a safepoint inside execute()
            // (VM::execute skips its own registration for participant
            // threads).  Post-loop teardown is covered too -- the participant
            // is destroyed only when this scope unwinds.
            SimpleMarkSweepGC::ExternalParticipant gcParticipant(
                SimpleMarkSweepGC::instance());

            Value actorVal = this->actorInstance;
            if (!actorVal.isAlive()) {
                state = State::Completed;
                actorInstanceRaw.store(nullptr, std::memory_order_release);
                return;
            }
            ActorInstance* actorInst = asActorInstance(actorVal);
            // Store a raw pointer while the actor is unquestionably alive so
            // the finalizer can still signal the worker after the weak handle
            // goes dead.  The join path clears the cache again once teardown is
            // complete.
            actorInstanceRaw.store(actorInst, std::memory_order_release);
            actorInst->thread_id = std::this_thread::get_id(); // store actor's thread in instance
            actorInst->thread = ptr_from_this();

            vm.resetStack();
            // frame local 0 is actor 'this' instance for actor method (as for object methods)
            push(actorVal);

            do {
                // Park here if a collection barrier is forming -- no queued-
                // call Values are held at this point (callInfo is reset FIRST
                // for exactly that reason: it is hoisted outside the loop for
                // the catch handler, so the previous call's Values must be
                // released before this thread parks).  (wakeAllThreadsForGC
                // notifies queueConditionVar via Thread::wake, and the wait
                // predicate below returns on a pending request, so an idle
                // actor reaches this poll promptly.)
                callInfo = ActorInstance::MethodCallInfo {};
                gcParticipant.pollSafepointIfRequested();

                {
                    std::unique_lock<std::mutex> lock { actorInst->queueMutex };
                    actorInst->queueConditionVar.wait(lock,[&]()
                    {
                        // wake when quitting, when a method is queued, when
                        // events are pending, or when a GC needs this thread
                        // to reach its poll (top of loop)
                        return quit || !actorInst->callQueue.empty() || !pendingEvents.empty()
                            || SimpleMarkSweepGC::instance().isCollectionRequested();
                    });
                    if (!actorInst->callQueue.empty()) {
                        callInfo = actorInst->callQueue.pop();
                    }
                    // Only break on quit if there is no call to process: if a call
                    // was already popped from the queue we must execute it (and
                    // fulfil its promise) before exiting, otherwise the caller
                    // blocks forever on a future that is never resolved.
                    if (quit && !callInfo.valid())
                        break;
                }

                if (!this->actorInstance.isAlive()) {
                    quit = true;
                    break;
                }

                // handle events even when no method was queued
                if (!vm.processPendingEvents()) {
                    quit = true;
                    break;
                }

                if (callInfo.valid()) {

                    // Root the WHOLE in-flight call, not just the callee:
                    // callInfo is a C++ local from here until completion.
                    currentActorCall = callInfo.callee;
                    currentActorArgs = callInfo.args;
                    currentActorFuture = callInfo.returnFuture;
                    Value strongActor = this->actorInstance.strongRef();
                    if (strongActor.isNil()) {
                        quit = true;
                        clearCurrentActorCall();
                        break;
                    }
                    // Ensure actor instance stays alive during call
                    this->stack[0] = strongActor;

#ifdef ROXAL_COMPUTE_SERVER
                    VM::ScopedPrintTarget printTargetScope(callInfo.printTarget);
                    if (actorInst->isRemote) {
                        remoteComputeCallState.active = true;
                        remoteComputeCallState.args.assign(callInfo.args.rbegin(), callInfo.args.rend());
                        remoteComputeCallState.completionFuture = callInfo.returnFuture;
                        remoteComputeCallState.result = Value::nilVal();
                        try {
                            auto conn = actorInst->remoteConn.lock();
                            if (!conn)
                                throw std::runtime_error("remote actor connection has been released");

                            Value ret = conn->callRemoteMethod(
                                actorInst->remoteActorId,
                                remoteMethodNameForCall(callInfo),
                                remoteComputeCallState.args,
                                callInfo.callSpec,
                                &remoteComputeCallState.result);
                            remoteComputeCallState.result = ret;

                            if (callInfo.returnPromise != nullptr) {
                                callInfo.returnPromise->set_value(remoteComputeCallState.result);
                                callInfo.returnPromise = nullptr;
                                if (!remoteComputeCallState.completionFuture.isNil()) {
                                    asFuture(remoteComputeCallState.completionFuture)->wakeWaiters();
                                    remoteComputeCallState.completionFuture = Value::nilVal();
                                }
                            }
                        } catch (const std::exception& e) {
                            std::cerr << "Remote actor call failed: " << e.what() << std::endl;
                            if (callInfo.returnPromise != nullptr) {
                                callInfo.returnPromise->set_value(Value::nilVal());
                                callInfo.returnPromise = nullptr;
                                if (!remoteComputeCallState.completionFuture.isNil()) {
                                    asFuture(remoteComputeCallState.completionFuture)->wakeWaiters();
                                    remoteComputeCallState.completionFuture = Value::nilVal();
                                }
                            }
                            quit = true;
                        }

                        remoteComputeCallState.clear();
                        this->stack[0] = this->actorInstance;
                        clearCurrentActorCall();
                        if (quit)
                            break;
                        continue;
                    }
#endif

                    if (isBoundMethod(callInfo.callee)) {
                        auto boundMethod = asBoundMethod(callInfo.callee);
                        auto closure = asClosure(boundMethod->method);
                        auto function = asFunction(closure->function);

                        // Determine if return type is const-qualified (-> const T)
                        bool returnIsConst = false;
                        if (function->funcType.has_value()) {
                            ptr<roxal::type::Type> ft { function->funcType.value() };
                            if (ft->func.has_value() && !ft->func->returnTypes.empty()) {
                                auto& rt = ft->func->returnTypes[0];
                                if (rt && rt->isConst)
                                    returnIsConst = true;
                            }
                        }

                        // Check if this is a native method wrapped in a BoundMethod
                        if (function->builtinInfo) {
                            // For native methods, we need to pass receiver as first arg
                            push(boundMethod->receiver);
                            for(auto it = callInfo.args.rbegin(); it != callInfo.args.rend(); ++it)
                                push(*it);

                            NativeFn native = function->builtinInfo->function;
                            ArgsView view{&(*vm.thread->stackTop) - callInfo.callSpec.argCount - 1,
                                          static_cast<size_t>(callInfo.callSpec.argCount + 1)};
                            Value ret{};
                            bool ok = true;
                            std::string nativeErr;
                            try {
                                ret = native(vm, view);
                            } catch (std::exception& e) {
                                ok = false;
                                nativeErr = e.what();
                            }
#ifdef __EMSCRIPTEN__
                            // Wasm cover for the POST-call region only: 'ret'
                            // and the temporaries below live in unscannable
                            // wasm frames across future resolution / cloning,
                            // which can safepoint.  The call itself must NOT
                            // be covered: a long-running native (the dataflow
                            // engine's run loop is the canonical case) polls
                            // safepoints internally at points where it holds
                            // no un-stored Value locals, and a cover would
                            // turn those polls into no-ops -- the thread then
                            // never parks and the collection barrier waits on
                            // it forever (silent app-wide freeze).
                            SimpleMarkSweepGC::GCNoParkScope nativeCover;
#endif

                            popN(callInfo.callSpec.argCount + 1);

                            if (callInfo.returnPromise != nullptr) {
                                // Resolve any futures before returning across actor boundary
                                currentActorResult = ret;
                                if (isFuture(ret))
                                    ret.resolveFuture();
                                currentActorResult = ret;
                                if (!ret.isPrimitive() && !isException(ret)) {
                                    if (returnIsConst) {
                                        ret = createFrozenSnapshot(ret);
                                        currentActorResult = ret;
                                    } else {
                                        Obj* obj = ret.asObj();
                                        bool soleOwner = obj && obj->control &&
                                            obj->control->strong.load(std::memory_order_acquire) <= 1;
                                        if (!soleOwner || !isIsolatedGraph(obj)) {
                                            ptr<CloneContext> cloneCtx = make_ptr<CloneContext>();
                                            ret = ret.clone(cloneCtx);
                                            currentActorResult = ret;
                                        }
                                    }
                                }
                                Value forward;
                                if (ok) {
                                    forward = ret;
                                } else {
                                    forward = pendingUncaughtException;
                                    pendingUncaughtException = Value::nilVal();
                                    if (!isException(forward))
                                        forward = Value::nilVal();
                                }
                                callInfo.returnPromise->set_value(forward);
                                callInfo.returnPromise = nullptr;
                                if (!callInfo.returnFuture.isNil()) {
                                    asFuture(callInfo.returnFuture)->wakeWaiters();
                                    callInfo.returnFuture = Value::nilVal();
                                }
                                // A plain C++ throw resolves the future with
                                // nil -- the awaiter sees a value, not an
                                // error -- so at least SAY what happened
                                // (this silence hid the dataflow engine's
                                // death for a whole session).
                                if (!ok && !isException(forward)) {
                                    std::fprintf(stderr, "actor native call '%s' failed: %s\n",
                                                 toUTF8StdString(function->name).c_str(), nativeErr.c_str());
                                    std::fflush(stderr);
                                }
                            } else if (!ok) {
                                // Fire-and-forget native call threw: nobody
                                // will ever observe it -- report, and drop any
                                // pending exception so it cannot leak into the
                                // NEXT call's failure path.
                                pendingUncaughtException = Value::nilVal();
                                std::fprintf(stderr, "actor native call '%s' failed: %s\n",
                                             toUTF8StdString(function->name).c_str(), nativeErr.c_str());
                                std::fflush(stderr);
                            }

                            {
                                auto diff = this->stackTop - (this->stack.begin()+1);
                                if (diff > 0) popN(size_t(diff));
                                if (this->stackTop == this->stack.begin())
                                    push(this->actorInstance);   // see comment at the twin below
                                else
                                    this->stack[0] = this->actorInstance;
                            }
                        } else {
                            // Regular closure method
                            for(auto it = callInfo.args.rbegin(); it != callInfo.args.rend(); ++it)
                                push(*it);

                            vm.call(closure, callInfo.callSpec);

                        auto resultPair = vm.execute();
                        result = resultPair.first;

                    if (resultPair.first == ExecutionStatus::OK) {
                        if (callInfo.returnPromise != nullptr) {
                            Value ret = resultPair.second;
                            // Root it for the whole hand-back window: the
                            // calls below allocate, so a collection can land
                            // while `ret` exists only as a C++ local.
                            currentActorResult = ret;
                            // Resolve any futures before returning across actor boundary
                            if (isFuture(ret))
                                ret.resolveFuture();
                            currentActorResult = ret;
                            if (!ret.isPrimitive() && !isException(ret)) {
                                if (returnIsConst) {
                                    ret = createFrozenSnapshot(ret);
                                    currentActorResult = ret;
                                } else {
                                    Obj* obj = ret.asObj();
                                    bool soleOwner = obj && obj->control &&
                                        obj->control->strong.load(std::memory_order_acquire) <= 1;
                                    if (!soleOwner || !isIsolatedGraph(obj)) {
                                        ptr<CloneContext> cloneCtx = make_ptr<CloneContext>();
                                        ret = ret.clone(cloneCtx);
                                    }
                                }
                            }
                            callInfo.returnPromise->set_value(ret);
                            callInfo.returnPromise = nullptr;
                            if (!callInfo.returnFuture.isNil()) {
                                asFuture(callInfo.returnFuture)->wakeWaiters();
                                callInfo.returnFuture = Value::nilVal();
                            }
                        }
                    } else {
                        // Forward an uncaught exception (if any) through the
                        // return future so awaiting code can observe and
                        // re-raise it.  A fire-and-forget call (a proc, or
                        // init -- no return promise) has no future to carry
                        // it, so REPORT it here instead: silently dropping it
                        // used to kill the actor with no trace anywhere.
                        // Either way the actor itself is still healthy — just
                        // this method invocation failed — so we keep serving
                        // subsequent calls (don't quit).
                        bool forwardedException = false;
                        Value pendingExc = pendingUncaughtException;
                        pendingUncaughtException = Value::nilVal();
                        const bool haveException = isException(pendingExc);
                        if (callInfo.returnPromise != nullptr) {
                            forwardedException = haveException;
                            callInfo.returnPromise->set_value(haveException ? pendingExc
                                                                            : Value::nilVal());
                            callInfo.returnPromise = nullptr;
                            if (!callInfo.returnFuture.isNil()) {
                                asFuture(callInfo.returnFuture)->wakeWaiters();
                                callInfo.returnFuture = Value::nilVal();
                            }
                        } else if (haveException) {
                            std::fprintf(stderr, "Uncaught exception in actor call '%s' (no awaiter): %s\n",
                                         toUTF8StdString(function->name).c_str(),
                                         objExceptionToString(asException(pendingExc)).c_str());
                            std::fflush(stderr);
                            forwardedException = true;   // reported: keep serving
                        }
                        // reset stack before breaking
                        {
                            auto diff = this->stackTop - (this->stack.begin()+1);
                            if (diff > 0) popN(size_t(diff));
                            // An exception unwind resetStack()s the whole
                            // thread stack (depth 0): re-PUSH the actor slot
                            // rather than writing below stackTop, or the next
                            // dispatch runs one slot off and corrupts the
                            // stack.
                            if (this->stackTop == this->stack.begin())
                                push(this->actorInstance);
                            else
                                this->stack[0] = this->actorInstance;
                        }
                        clearCurrentActorCall();
                        if (forwardedException) {
                            result = ExecutionStatus::OK;
                            // Continue serving subsequent calls.
                        } else {
                            quit = true;
                            break;
                        }
                    }

                        {
                            auto diff = this->stackTop - (this->stack.begin()+1);
                            if (diff > 0) popN(size_t(diff));
                            // An exception unwind resetStack()s the whole
                            // thread stack (depth 0): re-PUSH the actor slot
                            // rather than writing below stackTop, or the next
                            // dispatch runs one slot off and corrupts the
                            // stack.
                            if (this->stackTop == this->stack.begin())
                                push(this->actorInstance);
                            else
                                this->stack[0] = this->actorInstance;
                        }
                        } // end of regular closure else block

                    } else if (isBoundNative(callInfo.callee)) {
                        ObjBoundNative* bn = asBoundNative(callInfo.callee);

                        bool bnReturnIsConst = false;
                        if (bn->funcType) {
                            if (bn->funcType->func.has_value() && !bn->funcType->func->returnTypes.empty()) {
                                auto& rt = bn->funcType->func->returnTypes[0];
                                if (rt && rt->isConst)
                                    bnReturnIsConst = true;
                            }
                        }

                        for(auto it = callInfo.args.rbegin(); it != callInfo.args.rend(); ++it)
                            push(*it);

                        NativeFn native = bn->function;
                        ArgsView view{&(*vm.thread->stackTop) - callInfo.callSpec.argCount - 1,
                                      static_cast<size_t>(callInfo.callSpec.argCount + 1)};
                        Value ret{};
                        bool ok = true;
                        std::string nativeErr;
                        try {
                            ret = native(vm, view);
                        } catch (std::exception& e) {
                            ok = false;
                            nativeErr = e.what();
                        }
#ifdef __EMSCRIPTEN__
                        SimpleMarkSweepGC::GCNoParkScope nativeCover;  // see above
#endif

                        popN(callInfo.callSpec.argCount);

                        if (callInfo.returnPromise != nullptr) {
                            // Resolve any futures before returning across actor boundary
                            if (isFuture(ret))
                                ret.resolveFuture();
                            if (!ret.isPrimitive() && !isException(ret)) {
                                if (bnReturnIsConst) {
                                    ret = createFrozenSnapshot(ret);
                                } else {
                                    Obj* obj = ret.asObj();
                                    bool soleOwner = obj && obj->control &&
                                        obj->control->strong.load(std::memory_order_acquire) <= 1;
                                    if (!soleOwner || !isIsolatedGraph(obj)) {
                                        ptr<CloneContext> cloneCtx = make_ptr<CloneContext>();
                                        ret = ret.clone(cloneCtx);
                                    }
                                }
                            }
                            // On failure, forward the pending exception (if any)
                            // through the future so awaiters can re-raise.
                            Value forward;
                            if (ok) {
                                forward = ret;
                            } else {
                                forward = pendingUncaughtException;
                                pendingUncaughtException = Value::nilVal();
                                if (!isException(forward))
                                    forward = Value::nilVal();
                            }
                            callInfo.returnPromise->set_value(forward);
                            callInfo.returnPromise = nullptr;
                            if (!callInfo.returnFuture.isNil()) {
                                asFuture(callInfo.returnFuture)->wakeWaiters();
                                callInfo.returnFuture = Value::nilVal();
                            }
                            // Same reporting rule as the bound-method branch:
                            // a plain C++ throw otherwise reads as a nil
                            // result to the awaiter.
                            if (!ok && !isException(forward)) {
                                std::string bnName = isFunction(bn->declFunction)
                                    ? toUTF8StdString(asFunction(bn->declFunction)->name)
                                    : std::string("<native>");
                                std::fprintf(stderr, "actor native call '%s' failed: %s\n",
                                             bnName.c_str(), nativeErr.c_str());
                                std::fflush(stderr);
                            }
                        } else if (!ok) {
                            // Fire-and-forget native call threw: report and
                            // clear any pending exception so it cannot leak
                            // into the next call's failure path.
                            pendingUncaughtException = Value::nilVal();
                            std::string bnName = isFunction(bn->declFunction)
                                ? toUTF8StdString(asFunction(bn->declFunction)->name)
                                : std::string("<native>");
                            std::fprintf(stderr, "actor native call '%s' failed: %s\n",
                                         bnName.c_str(), nativeErr.c_str());
                            std::fflush(stderr);
                        }
                    }

                    // restore weak actor reference for next iteration
                    this->stack[0] = this->actorInstance;
                    clearCurrentActorCall();

                } else {
                    clearCurrentActorCall();
                }

            } while (true);

            // Set alive=false while holding queueMutex so any concurrent queueCall()
            // that acquires the lock after this point will see alive=false and
            // immediately resolve its promise, rather than pushing to an unserviced queue.
            {
                std::lock_guard<std::mutex> lock { actorInst->queueMutex };
                actorInst->alive.store(false, std::memory_order_release);
            }

            // Drain items that arrived before alive was cleared (or were already queued).
            while(!actorInst->callQueue.empty()) {
                auto pending = actorInst->callQueue.pop();
                if (pending.returnPromise) {
                    pending.returnPromise->set_value(Value::nilVal());
                    if (!pending.returnFuture.isNil()) {
                        asFuture(pending.returnFuture)->wakeWaiters();
                        pending.returnFuture = Value::nilVal();
                    }
                }
            }

            stack.clear();

            state = State::Completed;
            actorInstanceRaw.store(nullptr, std::memory_order_release);
        }
        catch (std::exception& e) {
            // What lands here is infrastructure failure (marshalling, cloning,
            // stack exhaustion in the dispatch preamble...) -- script
            // exceptions and native throws inside a call are handled and
            // forwarded above.  Fatal-class, so report and shut the VM down
            // in an orderly way -- but FIRST resolve the in-flight call's
            // promise: destroying it unresolved crashes the awaiter with
            // future_error(broken_promise) instead of an error it can see.
            if (callInfo.returnPromise != nullptr) {
                Value forward = pendingUncaughtException;
                pendingUncaughtException = Value::nilVal();
                if (!isException(forward))
                    forward = Value::nilVal();
                try {
                    callInfo.returnPromise->set_value(forward);
                    if (!callInfo.returnFuture.isNil())
                        asFuture(callInfo.returnFuture)->wakeWaiters();
                } catch (...) { /* already satisfied */ }
                callInfo.returnPromise = nullptr;
                callInfo.returnFuture = Value::nilVal();
            }

            auto& vm { VM::instance() };
            // runtimeError() = report + runtimeErrorFlag + wake every thread,
            // the same orderly-shutdown sequence this catch used to do by
            // hand (with consistent formatting and script location when one
            // is available).
            std::string msg = std::string("actor thread error: ") + e.what();
            vm.runtimeError("%s", msg.c_str());

            result = ExecutionStatus::RuntimeError;
            stack.clear();
            state = State::Completed;
            actorInstanceRaw.store(nullptr, std::memory_order_release);
        }
    });

}


void Thread::detach()
{
    assert(state != State::Constructed);

    if (osthread != nullptr)
        osthread->detach();
}

void Thread::wake()
{
    {
        std::unique_lock<std::mutex> lk(sleepMutex);
        sleepCondVar.notify_one();
    }
    if (actor) {
        ActorInstance* inst = nullptr;
        if (actorInstance.isAlive()) {
            inst = asActorInstance(actorInstance);
        } else {
            inst = actorInstanceRaw.load(std::memory_order_acquire);
        }
        if (inst) {
            inst->queueConditionVar.notify_one();
        }
    }
}









void Thread::pushFrame(CallFrame& frame)
{
    frame.parent = frames.end() - 1;
    frames.push_back(frame);
}

void Thread::popFrame()
{
    frames.pop_back();
}


std::atomic<uint64_t> Thread::nextId = 1;


void Thread::outputStack()
{
    // output stack
    if (stack.size() > 0) {

        std::cout << "          ";
        for(auto vi = stack.begin(); vi < stackTop; vi++) {
            bool aString = vi->isObj() && isString(*vi);
            std::cout << "[";
            if (!frames.empty() && (frames.end()-1)->slots == &(*vi) )
                std::cout << "F^"; // show frame pointer
            std::cout << " ";
            if (aString)
                std::cout << "'"; // quote strings
            std::cout << toString(*vi);
            if (aString)
                std::cout << "'";
            if (vi->isNumber()) // show numeric type
                std::cout << ":" << vi->typeName().at(0);

            std::cout << " ]";
        }
        std::cout << std::endl;
    }
}
