#pragma once

#include <vector>

#include "Chunk.h"
#include "Value.h"

namespace roxal {


struct CallFrame {
    ObjClosure* closure;
    Chunk::iterator ip;
    Value* slots;
};



class VM
{
public:
    VM();
    VM(std::istream& linestream); // single-line mode
    ~VM();


    enum class InterpretResult {
        OK,
        CompileError,
        RuntimeError
    };

    InterpretResult interpret(std::istream& source, const std::string& name);

    void interpretLine();

    void push(const Value& value);
    Value pop();
    Value peek(int distance);

    bool call(ObjClosure* closure, int argCount);
    bool callValue(const Value& callee, int argCount);    
    ObjUpvalue* captureUpvalue(Value& local);
    void closeUpvalues(Value* last);

    void defineNative(const std::string& name, NativeFn function);

protected:
    InterpretResult execute();

    bool lineMode;
    std::istream* lineStream;

    typedef std::vector<Value> ValueStack;


    const int MaxStack = 1024;

    ValueStack stack;
    ValueStack::iterator stackTop;

    std::vector<CallFrame> frames;

    // FIXME: use something other than UnicodeString (ObjString* or Value??)
    // map from name ObjString.hash to <name, value> pair
    std::unordered_map<int32_t, std::pair<icu::UnicodeString, Value>> globals;

    std::list<ObjUpvalue*> openUpvalues;

    void resetStack();
    void freeObjects();
    void outputAllocatedObjs();

    void concatenate();

    void runtimeError(const std::string& format, ...);

    void defineNativeFunctions();

};


}