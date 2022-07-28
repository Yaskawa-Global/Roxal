#pragma once

#include <vector>

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



class VM
{
public:
    VM();
    VM(std::istream& linestream); // single-line mode
    ~VM();

    void setDissasemblyOutput(bool outputBytecodeDissasembly);


    enum class InterpretResult {
        OK,
        CompileError,
        RuntimeError
    };

    InterpretResult interpret(std::istream& source, const std::string& name);

    void interpretLine();

    void push(const Value& value);
    Value pop();
    void popN(size_t n); // call pop() n times
    Value peek(int distance);

    bool call(ObjClosure* closure, const CallSpec& callSpec);
    bool call(ValueType builtinType, const CallSpec& callSpec);
    bool callValue(const Value& callee, const CallSpec& callSpec);   
    bool invokeFromType(ObjObjectType* type, ObjString* name, const CallSpec& callSpec);
    bool invoke(ObjString* name, const CallSpec& callSpec);
    bool indexValue(const Value& indexable, int subscriptCount);
    bool bindMethod(ObjObjectType* instanceType, ObjString* name);
    ObjUpvalue* captureUpvalue(Value& local);
    void closeUpvalues(Value* last);
    Value opReturn();

    void defineMethod(ObjString* name);
    void defineNative(const std::string& name, NativeFn function);

protected:
    InterpretResult execute();

    bool outputBytecodeDissasembly;
    bool lineMode;
    std::istream* lineStream;

    typedef std::vector<Value> ValueStack;


    const int MaxStack = 1024;

    ValueStack stack;
    ValueStack::iterator stackTop;

    CallFrames frames;
    bool frameStart; // true for one iteration when ip is at initial ip for frame
    void pushFrame(CallFrame& frame) {
        frame.parent = frames.end()-1;
        frames.push_back(frame);
    }
    void popFrame() { frames.pop_back(); }

    // FIXME: use something other than UnicodeString (ObjString* or Value??)
    // map from name ObjString.hash to <name, value> pair
    std::unordered_map<int32_t, std::pair<icu::UnicodeString, Value>> globals;

    std::list<ObjUpvalue*> openUpvalues;

    ObjString* initString;

    bool threadSleep;
    uint64_t threadSleepUntil;

    void resetStack();
    void freeObjects();
    void outputAllocatedObjs();

    void concatenate();

    void runtimeError(const std::string& format, ...);


    // Builtin functions
    void defineBuiltinFunctions();

    Value print_builtin(int argCount, Value* args);
    Value sleep_builtin(int argCount, Value* args);



    // Native functions
    void defineNativeFunctions();

    Value clock_native(int argCount, Value* args);
    Value usSleep_native(int argCount, Value* args);
    Value msSleep_native(int argCount, Value* args);
    Value sleep_native(int argCount, Value* args);

};


}