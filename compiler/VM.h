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

class StreamEngine;



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
    void popN(size_t n); // call pop() n times
    Value peek(int distance);

    bool call(ObjClosure* closure, int argCount);
    bool call(ValueType builtinType, int argCount);
    bool callValue(const Value& callee, int argCount);   
    bool invokeFromType(ObjObjectType* type, ObjString* name, int argCount);
    bool invoke(ObjString* name, int argCount);
    bool indexValue(const Value& indexable, int subscriptCount);
    bool bindMethod(ObjObjectType* instanceType, ObjString* name);
    ObjUpvalue* captureUpvalue(Value& local);
    void closeUpvalues(Value* last);

    void defineMethod(ObjString* name);
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

    ptr<StreamEngine> sengine;

    ObjString* initString;

    void resetStack();
    void freeObjects();
    void outputAllocatedObjs();

    void concatenate();

    void runtimeError(const std::string& format, ...);

    void defineNativeFunctions();

    // Native functions
    Value clock_native(int argCount, Value* args);
    Value usSleep_native(int argCount, Value* args);
    Value msSleep_native(int argCount, Value* args);
    Value sleep_native(int argCount, Value* args);

};


}