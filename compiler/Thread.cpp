#include "Thread.h"
#include "VM.h"
#include "Object.h"
#include "SimpleMarkSweepGC.h"

#include <algorithm>

using namespace roxal;

Thread::~Thread()
{
    openUpvalues.clear();

    // remove any event subscriptions for this thread
    for (auto& entry : eventHandlers) {
        if (!entry.first.isAlive()) continue;
        ObjEvent* ev = asEvent(entry.first);
        for (const auto& handler : entry.second) {
            for (auto it = ev->subscribers.begin(); it != ev->subscribers.end(); ) {
                if (!it->isAlive() || asClosure(*it) != asClosure(handler)) {
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

        ObjEvent* ev = nullptr;
        if (eventRef.isObj()) {
            ev = asEvent(eventRef);
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
                                      [](const Value& handler) {
                                          return handler.isWeak() && !handler.isAlive();
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

            result = InterpretResult::RuntimeError;
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

    ActorInstance* inst = actorInstOverride;
    if (actor) {
        if (inst == nullptr && actorInstance.isAlive())
            inst = asActorInstance(actorInstance);
        if (inst != nullptr) {
            std::lock_guard<std::mutex> lock { inst->queueMutex };
            quit = true;
            inst->queueConditionVar.notify_one();
        } else {
            quit = true;
        }
    }

    bool shouldDetachForCollector = false;
    if (VM::thread) {
        SimpleMarkSweepGC& gc = SimpleMarkSweepGC::instance();
        Thread* current = VM::thread.get();
        if (current && current != this && gc.isCollectorThread(current)) {
            shouldDetachForCollector = true;
        }
    }

    if (shouldDetachForCollector) {
        osthread->detach();
    }
    else if (osthread->get_id() == std::this_thread::get_id()) {
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
    actor = false;

    state = State::Completed;
}


void Thread::act(Value actorInstance)
{
    assert(isActorInstance(actorInstance));
    this->actorInstance = actorInstance.weakRef();

    actor = true;
    state = State::Spawned;

    osthread = make_ptr<std::thread>([this]() {
        try {
            auto& vm { VM::instance() };

            vm.thread = ptr_from_this(); // set thread local storage member

            Value actorVal = this->actorInstance;
            if (!actorVal.isAlive()) {
                state = State::Completed;
                return;
            }
            ActorInstance* actorInst = asActorInstance(actorVal);
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
                    if (quit)
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

                    if (isBoundMethod(callInfo.callee)) {
                        auto closure = asClosure(asBoundMethod(callInfo.callee)->method);

                        for(auto it = callInfo.args.rbegin(); it != callInfo.args.rend(); ++it)
                            push(*it);

                        vm.call(closure, callInfo.callSpec);

                        auto resultPair = vm.execute();
                        result = resultPair.first;

                        if (resultPair.first == InterpretResult::OK) {
                            if (callInfo.returnPromise != nullptr) {
                                Value ret = resultPair.second;
                                if (!ret.isPrimitive())
                                    ret = ret.clone();
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

                    } else if (isBoundNative(callInfo.callee)) {
                        ObjBoundNative* bn = asBoundNative(callInfo.callee);

                        for(auto it = callInfo.args.rbegin(); it != callInfo.args.rend(); ++it)
                            push(*it);

                        NativeFn native = bn->function;
                        ArgsView view{&(*vm.thread->stackTop) - callInfo.callSpec.argCount - 1,
                                    static_cast<size_t>(callInfo.callSpec.argCount + 1)};
                        native(vm, view);

                        popN(callInfo.callSpec.argCount);
                    }

                    // restore weak actor reference for next iteration
                    this->stack[0] = this->actorInstance;
                    currentActorCall = Value::nilVal();

                } else {
                    currentActorCall = Value::nilVal();
                }

            } while (true);

            // resolve any queued calls with nil so waiting futures complete
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
        }
        catch (std::exception& e) {
            std::cerr << "VM Runtime error: " << e.what() << std::endl;

            auto& vm { VM::instance() };
            vm.runtimeErrorFlag = true;
            vm.threads.apply([](const std::pair<const uint64_t, ptr<Thread>>& entry){
                if (entry.second)
                    entry.second->wake();
            });

            result = InterpretResult::RuntimeError;
            stack.clear();
            state = State::Completed;
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
    if (actor && actorInstance.isAlive()) {
        ActorInstance* inst = asActorInstance(actorInstance);
        inst->queueConditionVar.notify_one();
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
