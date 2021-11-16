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

    enum class InterpretResult {
        OK,
        CompileError,
        RuntimeError
    };

    InterpretResult interpret(std::istream& source, const std::string& name);

    void push(const Value& value);
    Value pop();
    Value peek(int distance);

    bool call(ObjFunction* function, int argCount);
    bool callValue(const Value& callee, int argCount);

protected:
    InterpretResult execute();

    typedef std::vector<Value> ValueStack;


    const int MaxStack = 1024;

    ValueStack stack;
    ValueStack::iterator stackTop;

    std::vector<CallFrame> frames;


    bool inInnerScope; // i.e. scopeDepth > 0

    std::unordered_map<ObjString*, Value> globals;

    void resetStack();
    void freeObjects();

    void concatenate();

    void runtimeError(const std::string& format, ...);
};


}