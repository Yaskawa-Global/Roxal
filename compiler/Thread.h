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

class Thread : public std::enable_shared_from_this<Thread> {
public:
    Thread()
      : threadSleep(false), osthread(nullptr), state(State::Constructed), execute_depth(0) {
        thisid = nextId.fetch_add(1);
        actor = false;
        quit = false;
        frames.reserve(256);
    }
    Thread(Thread&) = delete;
    virtual ~Thread() {
        for (auto* upvalue : openUpvalues) {
            upvalue->decRef();
        }
    }

    uint64_t id() { return thisid; }

    enum class State {
        Constructed,
        Spawned,
        Completed
    };

    std::atomic<State> state;

    void spawn(Value closure);
    void join();
    void act(Value actorInstance);
    void detach();
    void wake();

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

    std::list<ObjUpvalue*> openUpvalues;
    struct ValueHasher {
        size_t operator()(const Value& v) const noexcept { return v.hash(); }
    };
    struct ValueEqual {
        bool operator()(const Value& a, const Value& b) const noexcept { return a == b; }
    };
    std::unordered_map<Value, std::vector<Value>, ValueHasher, ValueEqual> eventHandlers;

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

private:
    ptr<std::thread> osthread;

    Value actorInstance;
    std::atomic<bool> actor;
    std::atomic<bool> quit;

    uint64_t thisid;
    static std::atomic_uint64_t nextId;
};

}
