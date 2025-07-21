#include "ModuleSys.h"
#include "VM.h"
#include "Object.h"
#include "core/AST.h"
#include <core/json11.h>
#include <core/TimePoint.h>
#include "dataflow/Signal.h"
#include "dataflow/DataflowEngine.h"
#include "FFI.h"
#include <sstream>
#include <time.h>
#include <cmath>

using namespace roxal;

ModuleSys::ModuleSys()
{
    moduleTypeValue = objVal(moduleTypeVal(toUnicodeString("sys")));
}


void ModuleSys::registerBuiltins(VM& vm)
{
    auto addSys = [&](const std::string& name, NativeFn fn,
                      ptr<type::Type> funcType = nullptr,
                      std::vector<Value> defaults = {}){
        if (!vm.loadGlobal(toUnicodeString(name)).has_value())
            vm.defineNative(name, fn, funcType, defaults);
        link(name, fn, defaults);
    };

    if (!vm.loadGlobal(toUnicodeString("print")).has_value()) {
        addSys("print", [this](VM& vm, ArgsView a){ return print_builtin(vm,a); });
        addSys("len", [this](VM& vm, ArgsView a){ return len_builtin(vm,a); });
        addSys("help", [this](VM& vm, ArgsView a){ return help_builtin(vm,a); });
        addSys("clone", [this](VM& vm, ArgsView a){ return clone_builtin(vm,a); });
        {
            auto t = make_ptr<type::Type>(type::BuiltinType::Func);
            t->func = type::Type::FuncType();
            t->func->isProc = true;
            std::vector<Value> defaults { intVal(0), intVal(0), intVal(0), intVal(0) };
            auto params = BuiltinModule::constructParams({ {"s", type::BuiltinType::Int},
                                           {"ms", type::BuiltinType::Int},
                                           {"us", type::BuiltinType::Int},
                                           {"ns", type::BuiltinType::Int} },
                                         defaults);
            t->func->params.resize(params.size());
            for(size_t i=0;i<params.size();++i) t->func->params[i]=params[i];
            addSys("wait", [this](VM& vm, ArgsView a){ return wait_builtin(vm,a); }, t, defaults);
        }
        addSys("fork", [this](VM& vm, ArgsView a){ return fork_builtin(vm,a); });
        addSys("join", [this](VM& vm, ArgsView a){ return join_builtin(vm,a); });
        addSys("stacktrace", [this](VM& vm, ArgsView a){ return stacktrace_builtin(vm,a); });
        addSys("_threadid", [this](VM& vm, ArgsView a){ return threadid_builtin(vm,a); });
        addSys("_stackdepth", [this](VM& vm, ArgsView a){ return stackdepth_builtin(vm,a); });
        addSys("_wait", [this](VM& vm, ArgsView a){ return await_builtin(vm,a); });
        addSys("_runtests", [this](VM& vm, ArgsView a){ return runtests_builtin(vm,a); });
        addSys("_weakref", [this](VM& vm, ArgsView a){ return weakref_builtin(vm,a); });
        addSys("_weak_alive", [this](VM& vm, ArgsView a){ return weak_alive_builtin(vm,a); });
        addSys("_strongref", [this](VM& vm, ArgsView a){ return strongref_builtin(vm,a); });
        addSys("serialize", [this](VM& vm, ArgsView a){ return serialize_builtin(vm,a); });
        addSys("deserialize", [this](VM& vm, ArgsView a){ return deserialize_builtin(vm,a); });
        addSys("toJson", [this](VM& vm, ArgsView a){ return toJson_builtin(vm,a); });
        addSys("fromJson", [this](VM& vm, ArgsView a){ return fromJson_builtin(vm,a); });
    }

    if (!vm.loadGlobal(toUnicodeString("_clock")).has_value()) {
        addSys("_clock", [this](VM& vm, ArgsView a){ return clock_native(vm,a); });
        {
            auto t = make_ptr<type::Type>(type::BuiltinType::Func);
            t->func = type::Type::FuncType();
            auto params = BuiltinModule::constructParams({{"freq", type::BuiltinType::Int}}, {});
            t->func->params.resize(params.size());
            for(size_t i=0;i<params.size();++i) t->func->params[i]=params[i];
            addSys("clock", [this](VM& vm, ArgsView a){ return clock_signal_native(vm,a); }, t, {});
        }
        {
            auto t = make_ptr<type::Type>(type::BuiltinType::Func);
            t->func = type::Type::FuncType();
            type::Type::FuncType::ParamType p1(toUnicodeString("freq"));
            p1.type = make_ptr<type::Type>(type::BuiltinType::Int);
            type::Type::FuncType::ParamType p2(toUnicodeString("initial"));
            t->func->params.resize(2);
            t->func->params[0] = p1;
            t->func->params[1] = p2;
            addSys("signal", [this](VM& vm, ArgsView a){ return signal_source_native(vm,a); }, t, {});
        }
        addSys("_engine_stop", [this](VM& vm, ArgsView a){ return engine_stop_native(vm,a); });
        addSys("typeof", [this](VM& vm, ArgsView a){ return typeof_native(vm,a); });
        addSys("_df_graph", [this](VM& vm, ArgsView a){ return df_graph_native(vm,a); });
        addSys("_df_graphdot", [this](VM& vm, ArgsView a){ return df_graphdot_native(vm,a); });
        addSys("loadlib", [this](VM& vm, ArgsView a){ return loadlib_native(vm,a); });
    }
}

Value ModuleSys::print_builtin(VM& vm, ArgsView args)
{
    if (args.size() == 0) {
        std::cout << std::endl;
        return nilVal();
    }
    else if (args.size() != 1)
        throw std::invalid_argument("print expects either no arguments (to output a newline) or single argument convertable to a string");

    auto str = toString(args[0]);
    std::cout << str << std::endl;
    return nilVal();
}

Value ModuleSys::len_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 1)
        throw std::invalid_argument("len expects single argument");

    const Value& v (args[0]);
    int32_t len {1};

    switch (v.type()) {
        case ValueType::String: len = asString(v)->length(); break;
        case ValueType::List: len = asList(v)->length(); break;
        case ValueType::Dict: len = asDict(v)->length(); break;
        case ValueType::Vector: len = asVector(v)->length(); break;
        case ValueType::Range: {
            len = asRange(v)->length();
            if (len<0) return nilVal(); // has no defined length
        } break;
        default:
#ifdef DEBUG_BUILD
        std::cerr << "Unhandled type in len():" << v.typeName() << std::endl;
#endif
        ;
    }

    return intVal(len);
}

Value ModuleSys::help_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 1)
        throw std::invalid_argument("help expects single callable argument");

    Value target = args[0];
    ObjFunction* fn = nullptr;
    ptr<type::Type> fnType = nullptr;
    std::vector<ptr<ast::Annotation>> annots;

    if (isClosure(target)) {
        ObjClosure* c = asClosure(target);
        fn = c->function;
    } else if (isFunction(target)) {
        fn = asFunction(target);
    } else if (isBoundMethod(target)) {
        fn = asBoundMethod(target)->method->function;
    } else if (isBoundNative(target)) {
        fnType = asBoundNative(target)->funcType;
    } else if (isNative(target)) {
        fnType = asNative(target)->funcType;
    } else {
        throw std::invalid_argument("help expects a function or closure");
    }

    if (fn && fn->funcType.has_value())
        fnType = fn->funcType.value();

    if (fn)
        annots = fn->annotations;

    std::string sig;
    if (fnType)
        sig = fnType->toString();

    std::string doc;
    if (fn && !fn->doc.isEmpty())
        fn->doc.toUTF8String(doc);
    else
        for (const auto& a : annots) {
            if (toUTF8StdString(a->name) == "doc") {
                for (size_t i=0; i<a->args.size(); ++i) {
                    auto expr = a->args[i].second;
                    if (auto s = std::dynamic_pointer_cast<ast::Str>(expr)) {
                        if (!doc.empty()) doc += "\n";
                        std::string t; s->str.toUTF8String(t);
                        doc += t;
                    }
                }
            }
        }

    if (!doc.empty()) {
        sig += "\n";
        sig += doc;
    }

    return objVal(stringVal(toUnicodeString(sig)));
}

Value ModuleSys::clone_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 1)
        throw std::invalid_argument("clone takes a single argument (the value to deep-copy)");

    return args[0].clone();
}

Value ModuleSys::wait_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 4)
        throw std::invalid_argument("wait expects 4 arguments");

    int64_t s = toType(ValueType::Int, args[0], false).asInt();
    int64_t ms = toType(ValueType::Int, args[1], false).asInt();
    int64_t us = toType(ValueType::Int, args[2], false).asInt();
    int64_t ns = toType(ValueType::Int, args[3], false).asInt();

    int64_t totalus = s*1000000 + ms*1000 + us + ns/1000;
    auto microSecs { TimeDuration::microSecs(totalus) };

    VM::thread->threadSleep = true;
    VM::thread->threadSleepUntil = TimePoint::currentTime() + microSecs;

    return nilVal();
}

Value ModuleSys::fork_builtin(VM& vm, ArgsView args)
{
    if ((args.size() != 1) || !isClosure(args[0]))
        throw std::invalid_argument("fork expects single callable argument (e.g. func or proc)");

    ObjClosure* closure = asClosure(args[0]);

    // Check if closure captures any outer variables (has upvalues)
    if (!closure->upvalues.empty()) {
        throw std::runtime_error("fork cannot execute functions that capture variables from outer scopes. "
                                "The function must only use its parameters and global variables.");
    }

    auto newThread = std::make_shared<Thread>();
    vm.threads.store(newThread->id(), newThread);
    newThread->spawn(args[0]);

    int32_t id = int32_t(newThread->id());
    return intVal(id);
}

Value ModuleSys::join_builtin(VM& vm, ArgsView args)
{
    if ((args.size() != 1) || !args[0].isNumber())
        throw std::invalid_argument("join expects single numeric argument (thread id)");

    uint64_t id = args[0].asInt(); // FIXME: id is uint64

    auto count = vm.threads.erase_and_apply(id, [](ptr<Thread> t){
        t->join();
    });

    return count > 0 ? trueVal() : falseVal();
}

Value ModuleSys::threadid_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 0)
        throw std::invalid_argument("_threadid takes no arguments");

    int32_t id = int32_t(VM::thread->id()); // FIXME: id is uint64
    return intVal(id);
}

Value ModuleSys::stacktrace_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 0)
        throw std::invalid_argument("stacktrace takes no arguments");

    return vm.captureStacktrace();
}

Value ModuleSys::stackdepth_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 0)
        throw std::invalid_argument("_stackdepth takes no arguments");

    int32_t depth = int32_t(VM::thread->stackTop - VM::thread->stack.begin());
    return intVal(depth);
}

Value ModuleSys::await_builtin(VM& vm, ArgsView args)
{
    // Iterate over all the arguments and resolve each if it is a Future.
    // Resolving may block until the promise is fulfilled unless it already was.
    // As a special case, if a single argument is a list, iterate over the
    // elements of that list and resolve them.
    int32_t numFuturesResolved { 0 };
    if (args.size() == 1) {
        if (isFuture(args[0])) {
            args[0].resolve();
            numFuturesResolved++;
        }
        else if (isList(args[0])) {
            ObjList* l = asList(args[0]);
            for(auto& v : l->elts.get()) {
                v.resolve();
                numFuturesResolved++;
            }
        }
    }
    else {
        for(size_t i=0; i<args.size(); i++) {
            if (isFuture(args[i])) {
                args[i].resolve();
                numFuturesResolved++;
            }
        }
    }

    return intVal(numFuturesResolved);
}

Value ModuleSys::runtests_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 1 || !isString(args[0]))
        throw std::invalid_argument("_runtests expects single string argument");

    auto suite = toUTF8StdString(asString(args[0])->s);

    if (suite == "dataflow") {
        // TODO: Dataflow tests have been moved out - need to implement new roxal-based tests
        std::cout << "Dataflow tests temporarily disabled during Func class elimination" << std::endl;
        df::DataflowEngine::instance()->clear();
    }
    else if (suite == "conversions") {
        auto results = testConversions();

        int passes = 0;
        int fails = 0;
        for (const auto& result : results) {
            std::cout << "Test: " << std::get<0>(result) << " ";
            bool passed = std::get<1>(result);
            if (passed) {
                std::cout << "passed";
                passes++;
            }
            else {
                std::cout << "failed";
                fails++;
            }
            std::cout << " " << std::get<2>(result) << std::endl;
        }

        std::cout << "Passed " << passes << " failed " << fails << std::endl;
    }
    else if (suite == "serialize") {
        auto results = testValueSerialization();

        int passes = 0;
        int fails = 0;
        for (const auto& result : results) {
            std::cout << "Test: " << std::get<0>(result) << " ";
            bool passed = std::get<1>(result);
            if (passed) {
                std::cout << "passed";
                passes++;
            }
            else {
                std::cout << "failed";
                fails++;
            }
            std::cout << " " << std::get<2>(result) << std::endl;
        }

        std::cout << "Passed " << passes << " failed " << fails << std::endl;
    }

    return nilVal();
}

Value ModuleSys::weakref_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 1)
        throw std::invalid_argument("weakref expects single argument");

    return args[0].weakRef();
}

Value ModuleSys::weak_alive_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 1)
        throw std::invalid_argument("weak_alive expects single argument");

    return args[0].isAlive() ? trueVal() : falseVal();
}

Value ModuleSys::strongref_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 1)
        throw std::invalid_argument("strongref expects single argument");

    return args[0].strongRef();
}

Value ModuleSys::serialize_builtin(VM& vm, ArgsView args)
{
    if(args.size() < 1 || args.size() > 2)
        throw std::invalid_argument("serialize expects value and optional protocol string");
    std::string protocol = "default";
    if(args.size() == 2) {
        if(!isString(args[1]))
            throw std::invalid_argument("serialize protocol must be string");
        protocol = toUTF8StdString(asString(args[1])->s);
    }
    if(protocol != "default")
        throw std::invalid_argument("unknown serialization protocol");

    std::stringstream ss(std::ios::in|std::ios::out|std::ios::binary);
    auto ctx = make_ptr<SerializationContext>();
    writeValue(ss, args[0], ctx);
    std::string data = ss.str();
    std::vector<Value> bytes;
    bytes.reserve(data.size());
    for(unsigned char ch : data)
        bytes.push_back(byteVal(ch));
    return objVal(listVal(bytes));
}

Value ModuleSys::deserialize_builtin(VM& vm, ArgsView args)
{
    if(args.size() < 1 || args.size() > 2 || !isList(args[0]))
        throw std::invalid_argument("deserialize expects list of bytes and optional protocol string");

    std::string protocol = "default";
    if(args.size() == 2) {
        if(!isString(args[1]))
            throw std::invalid_argument("deserialize protocol must be string");
        protocol = toUTF8StdString(asString(args[1])->s);
    }
    if(protocol != "default")
        throw std::invalid_argument("unknown serialization protocol");

    ObjList* lst = asList(args[0]);
    std::string data;
    data.reserve(lst->length());
    for(int i=0;i<lst->length();i++) {
        Value v = lst->elts.at(i);
        uint8_t b;
        if(v.isByte()) {
            b = v.asByte();
        } else if(v.isInt()) {
            int iv = v.asInt();
            if(iv < 0 || iv > 255)
                throw std::runtime_error("deserialize int out of byte range");
            b = static_cast<uint8_t>(iv);
        } else {
            throw std::invalid_argument("deserialize expects list of bytes or ints");
        }
        data.push_back(static_cast<char>(b));
    }

    std::stringstream ss(std::ios::in|std::ios::out|std::ios::binary);
    ss.write(data.data(), data.size());
    ss.seekg(0);
    auto ctx = make_ptr<SerializationContext>();
    return readValue(ss, ctx);
}

static json11::Json valueToJson(const Value& v) {
    using json11::Json;
    switch(v.type()) {
        case ValueType::Nil:   return Json();
        case ValueType::Bool:  return Json(v.asBool());
        case ValueType::Byte:  return Json(int(v.asByte()));
        case ValueType::Int:   return Json(v.asInt());
        case ValueType::Real:  return Json(v.asReal());
        case ValueType::String: return Json(toUTF8StdString(asString(v)->s));
        case ValueType::List: {
            Json::array arr; arr.reserve(asList(v)->length());
            for(int i=0;i<asList(v)->length();++i)
                arr.push_back(valueToJson(asList(v)->elts.at(i)));
            return Json(arr);
        }
        case ValueType::Dict: {
            Json::object obj;
            for(const auto& kv : asDict(v)->items()) {
                if(!isString(kv.first))
                    throw std::runtime_error("dict key not string");
                obj[toUTF8StdString(asString(kv.first)->s)] = valueToJson(kv.second);
            }
            return Json(obj);
        }
        default:
            if(isObjectInstance(v) || isActorInstance(v))
                return valueToJson(toType(ValueType::Dict, v, false));
            throw std::runtime_error("unsupported type for toJson");
    }
}

static void dumpJsonPretty(const json11::Json& j, std::string& out, int indent=0) {
    using json11::Json;
    switch(j.type()) {
        case Json::ARRAY: {
            out += "[";
            auto arr = j.array_items();
            if(!arr.empty()) {
                out += "\n";
                int nIndent = indent+2;
                for(size_t i=0;i<arr.size();++i) {
                    out += std::string(nIndent,' ');
                    dumpJsonPretty(arr[i], out, nIndent);
                    if(i+1<arr.size()) out += ",\n";
                }
                out += "\n" + std::string(indent,' ');
            }
            out += "]";
            break;
        }
        case Json::OBJECT: {
            out += "{";
            auto obj = j.object_items();
            if(!obj.empty()) {
                out += "\n";
                int nIndent = indent+2;
                size_t count=0;
                for(auto it=obj.begin(); it!=obj.end(); ++it,++count) {
                    out += std::string(nIndent,' ');
                    dumpJsonPretty(Json(it->first), out, nIndent);
                    out += ": ";
                    dumpJsonPretty(it->second, out, nIndent);
                    if(count+1<obj.size()) out += ",\n";
                }
                out += "\n" + std::string(indent,' ');
            }
            out += "}";
            break;
        }
        default:
            out += j.dump();
    }
}

Value ModuleSys::toJson_builtin(VM& vm, ArgsView args)
{
    if(args.size() < 1 || args.size() > 2)
        throw std::invalid_argument("toJson expects value and optional indent bool");

    bool indent = true;
    if(args.size() == 2)
        indent = toType(ValueType::Bool, args[1], false).asBool();

    json11::Json j = valueToJson(args[0]);
    std::string out;
    if(indent)
        dumpJsonPretty(j, out, 0);
    else
        out = j.dump();

    return objVal(stringVal(toUnicodeString(out)));
}

static Value jsonToValue(const json11::Json& j) {
    using json11::Json;
    switch(j.type()) {
        case Json::NUL: return nilVal();
        case Json::BOOL: return boolVal(j.bool_value());
        case Json::NUMBER: {
            double n = j.number_value();
            if(std::floor(n) == n && n >= std::numeric_limits<int32_t>::min() && n <= std::numeric_limits<int32_t>::max())
                return intVal(static_cast<int32_t>(n));
            return realVal(n);
        }
        case Json::STRING: return objVal(stringVal(toUnicodeString(j.string_value())));
        case Json::ARRAY: {
            std::vector<Value> elts; elts.reserve(j.array_items().size());
            for(const auto& it : j.array_items()) elts.push_back(jsonToValue(it));
            return objVal(listVal(elts));
        }
        case Json::OBJECT: {
            ObjDict* d = dictVal();
            for(const auto& kv : j.object_items()) {
                d->store(objVal(stringVal(toUnicodeString(kv.first))), jsonToValue(kv.second));
            }
            return objVal(d);
        }
    }
    return nilVal();
}

Value ModuleSys::fromJson_builtin(VM& vm, ArgsView args)
{
    if(args.size() != 1 || !isString(args[0]))
        throw std::invalid_argument("fromJson expects json string");

    std::string s = toUTF8StdString(asString(args[0])->s);
    std::string err;
    json11::Json j = json11::Json::parse(s, err);
    if(!err.empty())
        throw std::invalid_argument(std::string("invalid json: ")+err);
    return jsonToValue(j);
}

Value ModuleSys::clock_native(VM& vm, ArgsView args)
{
    return realVal(double(clock())/CLOCKS_PER_SEC);
}

Value ModuleSys::clock_signal_native(VM& vm, ArgsView args)
{
    if ((args.size() != 1) || !args[0].isNumber())
        throw std::invalid_argument("clock expects single numeric argument");

    double freq = args[0].asReal();
    auto sig = df::Signal::newClockSignal(freq,df::DataflowEngine::uniqueFuncName("clock("+ std::to_string(int(freq)) + ")"));
    return objVal(signalVal(sig));
}

Value ModuleSys::signal_source_native(VM& vm, ArgsView args)
{
    if (args.size() < 1 || args.size() > 2 || !args[0].isNumber())
        throw std::invalid_argument("signal expects frequency and optional initial value");

    double freq = args[0].asReal();
    Value initial;
    if (args.size() >= 2)
        initial = args[1];
    auto sig = df::Signal::newSourceSignal(freq, initial);
    return objVal(signalVal(sig));
}

Value ModuleSys::engine_stop_native(VM& vm, ArgsView args)
{
    df::DataflowEngine::instance()->stop();
    return nilVal();
}

Value ModuleSys::typeof_native(VM& vm, ArgsView args)
{
    if (args.size() != 1)
        throw std::invalid_argument("typeof expects single argument");

    Value val = args[0];
    ValueType valueType;

    // Determine the ValueType of the argument

    if (val.isNil()) {
        valueType = ValueType::Nil;
    } else if (val.isBool()) {
        valueType = ValueType::Bool;
    } else if (val.isByte()) {
        valueType = ValueType::Byte;
    } else if (val.isInt()) {
        valueType = ValueType::Int;
    } else if (val.isReal()) {
        valueType = ValueType::Real;
    } else if (val.isEnum()) {
        valueType = ValueType::Enum;
    } else if (val.isType()) {
        valueType = ValueType::Type;
    } else if (isSignal(val)) {
        valueType = ValueType::Signal;
    } else if (isEvent(val)) {
        valueType = ValueType::Event;
    } else if (val.isObj()) {
        Obj* obj = val.asObj();
        if (obj->type == ObjType::Instance)
            return objVal(asObjectInstance(val)->instanceType);
        if (obj->type == ObjType::Actor)
            return objVal(asActorInstance(val)->instanceType);
        if (obj->type == ObjType::Exception) {
            ObjException* ex = asException(val);
            if (!ex->exType.isNil())
                return ex->exType;
            // fall back to builtin 'exception' type if somehow missing
            auto maybe = vm.globals.load(toUnicodeString("exception"));
            if (maybe.has_value())
                return maybe.value();
            return objVal(typeSpecVal(ValueType::Object));
        }

        // For primitive object wrappers like strings
        valueType = obj->valueType();
        ObjTypeSpec* typeObj = typeSpecVal(valueType);
        return objVal(typeObj);
    } else {
        // Fallback
        valueType = ValueType::Nil;
    }

    ObjTypeSpec* typeObj = typeSpecVal(valueType);
    return objVal(typeObj);
}

Value ModuleSys::df_graph_native(VM& vm, ArgsView args)
{
    if (args.size() != 0)
        throw std::invalid_argument("_df_graph has no arguments");

    auto engine = df::DataflowEngine::instance();
    auto dot = engine->graph();
    return objVal(stringVal(toUnicodeString(dot)));
}

Value ModuleSys::df_graphdot_native(VM& vm, ArgsView args)
{
    std::string title;
    if (args.size() > 1)
        throw std::invalid_argument("_df_graphdot expects zero or one title :string argument");
    if (args.size() == 1) {
        if (!isString(args[0]))
            throw std::invalid_argument("_df_graphdot expects string argument");
        title = toUTF8StdString(asString(args[0])->s);
    }

    auto engine = df::DataflowEngine::instance();
    auto dot = engine->graphDot(title, engine->signalValues());
    return objVal(stringVal(toUnicodeString(dot)));
}

Value ModuleSys::loadlib_native(VM& vm, ArgsView args)
{ 
    return roxal::loadlib_native(args); 
}
