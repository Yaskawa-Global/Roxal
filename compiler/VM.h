#pragma once

#include <vector>
#include <thread>
#include <atomic>
#include <list>

#include "core/atomic.h"
#include "Chunk.h"
#include "Value.h"

namespace roxal {

struct CallFrame; // forward


typedef std::vector<CallFrame> CallFrames;

struct CallFrame {
    #ifdef DEBUG_BUILD
    CallFrame() : closure(nullptr), slots(nullptr) {}
    #endif
    ObjClosure* closure;
    Chunk::iterator startIp;
    Chunk::iterator ip;
    Value* slots;

    CallFrames::iterator parent;

    // on frame start, move argument Value (second) to end of the frame's
    //  argument list (in existing stack arg placeholder slots)
    std::vector<Value> tailArgValues;

    // if not empty, used to reorder call arguments on the stack
    std::vector<int8_t> reorderArgs; // reordering

    // TODO: optimized this for common case where it is empty (ptr?)
    std::map<UnicodeString,Value> forwardStreamRefs;
};

class StreamEngine;


// The Virtual Machine (singleton)
class VM
{
public:

    static VM& instance()
    {
        static VM instance; // Guaranteed to be destroyed.
                            // Instantiated on first use.
        return instance;
    }

    VM(VM const&) = delete;
    void operator=(VM const&) = delete;

    void setDisassemblyOutput(bool outputBytecodeDisassembly);
    void appendModulePaths(const std::vector<std::string>& modulePaths);

    enum class InterpretResult {
        OK,
        CompileError,
        RuntimeError
    };

    InterpretResult interpret(std::istream& source, const std::string& name);
    InterpretResult interpretLine(std::istream& linestream);


    bool call(ObjClosure* closure, const CallSpec& callSpec);
    bool call(ValueType builtinType, const CallSpec& callSpec);
    bool callValue(const Value& callee, const CallSpec& callSpec);
    bool invokeFromType(ObjObjectType* type, ObjString* name, const CallSpec& callSpec);
    bool invoke(ObjString* name, const CallSpec& callSpec);
    bool indexValue(const Value& indexable, int subscriptCount);
    bool setIndexValue(const Value& indexable, int subscriptCount, Value& value);
    bool bindMethod(ObjObjectType* instanceType, ObjString* name);
    ObjUpvalue* captureUpvalue(Value& local);
    void closeUpvalues(Value* last);
    Value opReturn();

    void defineProperty(ObjString* name);
    void defineMethod(ObjString* name);
    void defineEnumLabel(ObjString* name);
    void defineNative(ObjModuleType* moduleType, const std::string& name, NativeFn function);


    const int MaxStack = 1024;
    typedef std::vector<Value> ValueStack;

    inline void push(const Value& value) { thread->push(value); }
    inline Value pop() { return thread->pop(); }
    inline void popN(size_t n) { thread->popN(n); } // call pop() n times
    inline Value& peek(int distance) { return thread->peek(distance); }


    class Thread : public std::enable_shared_from_this<Thread> {
    public:
        Thread()
          : threadSleep(false), osthread(nullptr), state(State::Constructed) {
            thisid = nextId.fetch_add(1);
            actor=false;
            quit=false;
          }
        Thread(Thread&) = delete;
        virtual ~Thread() {}

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

        void push(const Value& value);
        Value pop();
        void popN(size_t n); // call pop() n times
        Value& peek(int distance);

        ValueStack stack;
        ValueStack::iterator stackTop;

        CallFrames frames;
        bool frameStart; // true for one iteration when ip is at initial ip for frame
        void pushFrame(CallFrame& frame) {
            frame.parent = frames.end()-1;
            frames.push_back(frame);
        }
        void popFrame() { frames.pop_back(); }

        // dump stack to std::cout
        void outputStack();


        bool threadSleep;
        uint64_t threadSleepUntil;

        InterpretResult result;

    private:
        ptr<std::thread> osthread;

        Value actorInstance;
        std::atomic<bool> actor;
        std::atomic<bool> quit;

        uint64_t thisid;
        static std::atomic_uint64_t nextId;
    };

    // the current thread
    static thread_local ptr<Thread> thread;

protected:
    VM();
    ~VM();

    std::pair<InterpretResult,Value> execute();

    bool outputBytecodeDisassembly;
    bool lineMode;
    std::istream* lineStream;

    std::vector<std::string> modulePaths {};

    atomic_unordered_map<uint64_t, ptr<Thread>> threads;


    ObjModuleType::VariablesMap& moduleVars() {
        #ifdef DEBUG_BUILD
        assert(thread != nullptr);
        assert(!thread->frames.empty());
        assert(thread->frames.back().closure != nullptr);
        assert(thread->frames.back().closure->function != nullptr);
        assert(isModuleType(thread->frames.back().closure->function->moduleType));
        #endif
        auto currentFrame { thread->frames.back() };

        return asModuleType(currentFrame.closure->function->moduleType)->vars;
    }

    std::list<ObjUpvalue*> openUpvalues; // FIXME: move to Thread, figure out if cross-thread closures are an issue

    ObjString* initString;

    // TODO: perhaps implement inheritance first, then pre-define
    //  object type as root of class heirarchy and add clone() and other
    //  builtins to that
    // Builtin methods
    //  type name -> method-name,method-closure
    // Since methods are added to types as they're defined at runtime,
    //  we need to
    //std::map<icu::UnicodeString, std::pair<icu::UnicodeString, Value>> builtinMethods;

    void resetStack();
    void freeObjects();
    void outputAllocatedObjs();

    void concatenate();

    void runtimeError(const std::string& format, ...);


    // Builtin functions
    void defineBuiltinFunctions(ObjModuleType* moduleType);

    Value print_builtin(int argCount, Value* args);
    Value len_builtin(int argCount, Value* args);
    Value clone_builtin(int argCount, Value* args);
    Value sleep_builtin(int argCount, Value* args);
    Value fork_builtin(int argCount, Value* args);
    Value join_builtin(int argCount, Value* args);
    Value threadid_builtin(int argCount, Value* args);
    Value wait_builtin(int argCount, Value* args);



    // Native functions
    void defineNativeFunctions(ObjModuleType* moduleType);

    Value clock_native(int argCount, Value* args);
    Value usSleep_native(int argCount, Value* args);
    Value msSleep_native(int argCount, Value* args);
    Value sleep_native(int argCount, Value* args);
    Value sin_native(int argCount, Value* args);
    Value cos_native(int argCount, Value* args);

};


}
