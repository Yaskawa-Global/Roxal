#pragma once

#include <vector>

#include "Chunk.h"
#include "Value.h"

namespace roxal {


struct CallFrame {
    ObjFunction* function;
    Chunk::iterator ip;
    Value* slots;
};



class VM
{
public:
    VM();
    VM(std::istream& linestream); // single-line mode


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

    bool call(ObjFunction* function, int argCount);
    bool callValue(const Value& callee, int argCount);

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

    std::unordered_map<ObjString*, Value> globals;

    void resetStack();
    void freeObjects();

    void concatenate();

    void runtimeError(const std::string& format, ...);

    void defineNativeFunctions();

};


}