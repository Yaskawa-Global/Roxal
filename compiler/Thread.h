#pragma once

#include <thread>
#include <atomic>
#include <list>
#include <unordered_map>
#include <mutex>
#include <condition_variable>

#include "core/atomic.h"
#include "core/TimePoint.h"
#include "Value.h"
#include "Object.h"
#include "InterpretResult.h"

namespace roxal {

struct CallFrame;
using CallFrames = std::vector<CallFrame>;

class Thread
  : public enable_ptr_from_this<Thread> {
public:
    Thread()
      : threadSleep(false), osthread(nullptr), state(State::Constructed), execute_depth(0) {
        thisid = nextId.fetch_add(1);
        actor = false;
        quit = false;
        result = InterpretResult::OK;
        frames.reserve(256);
    }
    Thread(Thread&) = delete;
    virtual ~Thread();

    uint64_t id() { return thisid; }

    enum class State {
        Constructed,
        Spawned,
        Completed
    };

    std::atomic<State> state;

    void spawn(Value closure);
    void join(ActorInstance* actorInstOverride = nullptr);
    void act(Value actorInstance);
    void detach();
    void wake();

    // is this thread associated with an actor instance?
    bool isActorThread() const { return actor; }

    static void resetIdCounter(uint64_t value=1) { nextId.store(value); }

    void push(const Value& value);
    Value pop();
    void popN(size_t n);
    Value& peek(int distance);

    using ValueStack = std::vector<Value>;
    ValueStack stack;
    ValueStack::iterator stackTop;

    CallFrames frames;
    bool frameStart;
    void pushFrame(CallFrame& frame);
    void popFrame();

    void outputStack();

    std::mutex sleepMutex;
    std::condition_variable sleepCondVar;
    std::atomic_bool threadSleep;
    std::atomic<TimePoint> threadSleepUntil;

    InterpretResult result;

    // list of open UpValue (Value* pointers into the stack) in stack address order
    std::list<Value> openUpvalues; // ObjUpvalue

    struct ValueHasher {
        size_t operator()(const Value& v) const noexcept { return v.hash(); }
    };
    struct ValueEqual {
        bool operator()(const Value& a, const Value& b) const noexcept { return a == b; }
    };

    // Per-thread mapping of event to subscribed handler closures. The key is a
    // weak reference to the event object and each value is the list of handler
    // closures registered by this thread.
    std::unordered_map<Value, std::vector<Value>, ValueHasher, ValueEqual> eventHandlers;

    // When a handler is registered on a signal's change event, the signal is
    // recorded here so the handler can access the latest signal value.  Keys
    // are weak references to the signal's change event and values are the
    // corresponding signal objects.
    std::unordered_map<Value, Value, ValueHasher, ValueEqual> eventToSignal;

    struct PendingEvent {
        TimePoint when;
        Value event;
    };

    struct PendingEventCompare {
        bool operator()(const PendingEvent& a, const PendingEvent& b) const {
            return a.when > b.when;
        }
    };

    atomic_priority_queue<PendingEvent, PendingEventCompare> pendingEvents;

    int execute_depth;

    void pruneEventRegistrations();

    // Keeps the currently executing actor call target alive while the
    // interpreter is inside Thread::act().
    Value currentActorCall { Value::nilVal() };

private:
    ptr<std::thread> osthread;

    // weak reference to associated actor instance
    Value actorInstance;
    std::atomic<bool> actor;
    std::atomic<bool> quit;

    uint64_t thisid;
    static std::atomic_uint64_t nextId;
};

}
