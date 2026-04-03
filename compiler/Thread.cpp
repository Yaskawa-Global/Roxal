#include "Thread.h"
#include "VM.h"
#include "Object.h"
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

#ifdef ROXAL_COMPUTE_SERVER
namespace {
icu::UnicodeString remoteMethodNameForCall(const ActorInstance::MethodCallInfo& callInfo)
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
                                      [](const HandlerRegistration& handler) {
                                          return !handler.closure.isAlive() ||
                                                 (handler.closure.isWeak() && !handler.closure.isAlive());
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

void Thread::spawn(Value closure)
{
    assert(isClosure(closure));

    state = State::Spawned;
    osthread = make_ptr<std::thread>([this,closure]() {
        // When running in an RT context, ensure actor threads don't run on the
        // RT core and use normal (non-RT) scheduling policy.
        #ifdef __linux__
        {
            int excludeCore = VM::instance().rtCoreExclusion();
            if (excludeCore >= 0) {
                cpu_set_t cpuset;
                CPU_ZERO(&cpuset);
                unsigned int numCpus = std::thread::hardware_concurrency();
                for (unsigned int i = 0; i < numCpus; ++i) {
                    if (static_cast<int>(i) != excludeCore)
                        CPU_SET(i, &cpuset);
                }
                pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

                struct sched_param param {};
                param.sched_priority = 0;
                pthread_setschedparam(pthread_self(), SCHED_OTHER, &param);
            }
        }
        #endif

        try {
            auto& vm { VM::instance() };

            vm.thread = ptr_from_this(); // set thread local storage member

            vm.resetStack();
            push(closure);
            vm.call(asClosure(closure),CallSpec(0));

            auto execResult = vm.execute();
            result = execResult.first;

            stack.clear();

            state = State::Completed;
            actor = false;
        }
        catch (std::exception& e) {
            std::cerr << "VM Runtime error: " << e.what() << std::endl;

            auto& vm { VM::instance() };
            vm.runtimeErrorFlag = true;
            vm.threads.apply([](const std::pair<const uint64_t, ptr<Thread>>& entry){
                if (entry.second)
                    entry.second->wake();
            });

            result = ExecutionStatus::RuntimeError;
            stack.clear();
            state = State::Completed;
            actor = false;
        }
    });
}

void Thread::join(ActorInstance* actorInstOverride)
{
    if (state == State::Constructed || osthread == nullptr || !osthread->joinable())
        return;

    // When GC schedules this thread for shutdown it may only hold a raw pointer
    // to the actor instance.  Start with the override supplied by the finalizer
    // and fall back to the cached raw pointer if needed before attempting to
    // revive the weak handle below.
    ActorInstance* inst = actorInstOverride;
    if (inst == nullptr)
        inst = actorInstanceRaw.load(std::memory_order_acquire);
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
    if (inst)
        inst->thread.reset();
    actorInstance = Value::nilVal();
    actorInstanceRaw.store(nullptr, std::memory_order_release);
    actor = false;

    state = State::Completed;
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
        try {
            auto& vm { VM::instance() };

            vm.thread = ptr_from_this(); // set thread local storage member

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

                ActorInstance::MethodCallInfo callInfo {};
                {
                    std::unique_lock<std::mutex> lock { actorInst->queueMutex };
                    actorInst->queueConditionVar.wait(lock,[&]()
                    {
                        // wake when quitting, when a method is queued, or when events are pending
                        return quit || !actorInst->callQueue.empty() || !pendingEvents.empty();
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

                    currentActorCall = callInfo.callee;
                    Value strongActor = this->actorInstance.strongRef();
                    if (strongActor.isNil()) {
                        quit = true;
                        currentActorCall = Value::nilVal();
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
                                if (!remoteComputeCallState.completionFuture.isNil()) {
                                    asFuture(remoteComputeCallState.completionFuture)->wakeWaiters();
                                    remoteComputeCallState.completionFuture = Value::nilVal();
                                }
                            }
                        } catch (const std::exception& e) {
                            std::cerr << "Remote actor call failed: " << e.what() << std::endl;
                            if (callInfo.returnPromise != nullptr) {
                                callInfo.returnPromise->set_value(Value::nilVal());
                                if (!remoteComputeCallState.completionFuture.isNil()) {
                                    asFuture(remoteComputeCallState.completionFuture)->wakeWaiters();
                                    remoteComputeCallState.completionFuture = Value::nilVal();
                                }
                            }
                            quit = true;
                        }

                        remoteComputeCallState.clear();
                        this->stack[0] = this->actorInstance;
                        currentActorCall = Value::nilVal();
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
                            try {
                                ret = native(vm, view);
                            } catch (std::exception& e) {
                                ok = false;
                            }

                            popN(callInfo.callSpec.argCount + 1);

                            if (callInfo.returnPromise != nullptr) {
                                // Resolve any futures before returning across actor boundary
                                if (isFuture(ret))
                                    ret.resolveFuture();
                                if (!ret.isPrimitive() && !isException(ret)) {
                                    if (returnIsConst) {
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
                                callInfo.returnPromise->set_value(ok ? ret : Value::nilVal());
                                if (!callInfo.returnFuture.isNil()) {
                                    asFuture(callInfo.returnFuture)->wakeWaiters();
                                    callInfo.returnFuture = Value::nilVal();
                                }
                            }

                            {
                                auto diff = this->stackTop - (this->stack.begin()+1);
                                if (diff > 0) popN(size_t(diff));
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
                            // Resolve any futures before returning across actor boundary
                            if (isFuture(ret))
                                ret.resolveFuture();
                            if (!ret.isPrimitive() && !isException(ret)) {
                                if (returnIsConst) {
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
                            callInfo.returnPromise->set_value(ret);
                            if (!callInfo.returnFuture.isNil()) {
                                asFuture(callInfo.returnFuture)->wakeWaiters();
                                callInfo.returnFuture = Value::nilVal();
                            }
                        }
                    } else {
                        if (callInfo.returnPromise != nullptr) {
                            callInfo.returnPromise->set_value(Value::nilVal());
                            if (!callInfo.returnFuture.isNil()) {
                                asFuture(callInfo.returnFuture)->wakeWaiters();
                                callInfo.returnFuture = Value::nilVal();
                            }
                        }
                        quit = true;
                        // reset stack before breaking
                        {
                            auto diff = this->stackTop - (this->stack.begin()+1);
                            if (diff > 0) popN(size_t(diff));
                            this->stack[0] = this->actorInstance;
                        }
                        currentActorCall = Value::nilVal();
                        break;
                    }

                        {
                            auto diff = this->stackTop - (this->stack.begin()+1);
                            if (diff > 0) popN(size_t(diff));
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
                        try {
                            ret = native(vm, view);
                        } catch (std::exception& e) {
                            ok = false;
                        }

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
                            // On failure, resolve to nil to avoid broken promises.
                            callInfo.returnPromise->set_value(ok ? ret : Value::nilVal());
                            if (!callInfo.returnFuture.isNil()) {
                                asFuture(callInfo.returnFuture)->wakeWaiters();
                                callInfo.returnFuture = Value::nilVal();
                            }
                        }
                    }

                    // restore weak actor reference for next iteration
                    this->stack[0] = this->actorInstance;
                    currentActorCall = Value::nilVal();

                } else {
                    currentActorCall = Value::nilVal();
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
            std::cerr << "VM Runtime error: " << e.what() << std::endl;

            auto& vm { VM::instance() };
            vm.runtimeErrorFlag = true;
            vm.threads.apply([](const std::pair<const uint64_t, ptr<Thread>>& entry){
                if (entry.second)
                    entry.second->wake();
            });

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


void Thread::push(const Value& value)
{
    *stackTop = value;
    stackTop++;

    #ifdef DEBUG_BUILD
    if (stackTop == stack.end())
        throw std::runtime_error("Stack overflow");
    #endif
}


Value Thread::pop()
{
    #ifdef DEBUG_BUILD
    if (stackTop == stack.begin())
        throw std::runtime_error("Stack underflow");
    #endif

    stackTop--;
    auto retValue = *stackTop; // copy (hold ref)

    if (stackTop->isObj())
        *stackTop = Value(); // ensure to call decRef on objects

    return retValue;
}

void Thread::popN(size_t n)
{
    for(auto i=0; i<n; i++) pop();
}



Value& Thread::peek(int distance)
{
    #ifdef DEBUG_BUILD
    if (stackTop - stack.begin() <= distance)
        throw std::runtime_error("Stack underflow access ("+std::to_string(distance)+" stacksize:"+std::to_string(stackTop - stack.begin())+")");
    #endif
    return *(stackTop - 1 - distance);
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
