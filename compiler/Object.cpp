#include <stdexcept>
#include <cassert>
#include <unordered_map>

#include <core/types.h>
#include "Object.h"
#include "Stream.h"

using namespace roxal;


atomic_vector<Obj*> Obj::unrefedObjs {};

#ifdef DEBUG_TRACE_MEMORY
atomic_map<Obj*, const char*> Obj::allocatedObjs {};
#endif


ValueType Obj::valueType() const
{
    switch (type) {
        case ObjType::Bool: return ValueType::Bool;
        // TODO: Byte, Decimal
        case ObjType::Int: return ValueType::Int;
        case ObjType::Real: return ValueType::Real;
        case ObjType::String: return ValueType::String;
        case ObjType::Type: return ValueType::Type;
        case ObjType::List: return ValueType::List;
        case ObjType::Dict: return ValueType::Dict;
        case ObjType::Stream: return ValueType::Stream;
        case ObjType::Instance: return static_cast<const ObjObjectType*>(this)->isActor ? ValueType::Actor : ValueType::Object;
        case ObjType::ObjectType: return ValueType::Type;
        default: return ValueType::Nil;
    }
}


void Obj::registerStream()
{
    if (type==ObjType::Stream)
        StreamEngine::instance()->registerStream(static_cast<Stream*>(this));
    else
        throw std::runtime_error("can't call registerStream on non-stream object "+objTypeName(this));
}

void Obj::unregisterStream()
{
    if (type==ObjType::Stream)
        StreamEngine::instance()->unregisterStream(static_cast<Stream*>(this));
    else
        throw std::runtime_error("can't call unregisterStream on non-stream object "+objTypeName(this));
}




// interned strings table
static atomic_unordered_map<int32_t, ObjString*> strings {};


ObjString::ObjString(const UnicodeString& us)
    :  s(us)
{
    type = ObjType::String;
    refCount = 0;
    hash = s.hashCode();

    // add ourself to the string intern table
    #ifdef DEBUG_BUILD
    if (strings.containsKey(hash))
        throw std::runtime_error("Duplicate ObjString creation");
    #endif
    strings.store(hash, this);
}


ObjString::~ObjString()
{
    // remove ourself from the strings intern table
    strings.erase(hash);
}



static uint32_t fnv1a32(const UnicodeString& s)
{
    // FNV-1a 32bit hash
    //  see https://en.wikipedia.org/wiki/Fowler%E2%80%93Noll%E2%80%93Vo_hash_function
    const char16_t* buf = s.getBuffer();
    uint32_t h = 2166136261u;
    for(int i=0; i<s.length();i++) {
        char16_t c = buf[i];
        h ^= uint8_t(buf[i] & 0xff);
        h *= 16777619;
        h ^= uint8_t((buf[i] >> 8) & 0xff);
        h *= 16777619;
    }
    return h;
}


ObjString* roxal::stringVal(const UnicodeString& s) 
{ 
    int32_t hash = s.hashCode();
    auto objStr = strings.lookup(hash);
    if (!objStr.has_value()) { // not found
        // create new
        return newObj<ObjString>(__func__,s); 
    }
    else { // found existing string
        return objStr.value();
    }
} 



std::string roxal::objFunctionToString(const ObjFunction* of)
{
    std::string s;
    of->name.toUTF8String(s);
    return "<func "+s+">";
}


std::string roxal::objStringToString(const ObjString* os)
{
    std::string s;
    static_cast<const ObjString*>(os)->s.toUTF8String(s);
    return s;
}



ObjList* roxal::listVal(const std::vector<Value>& elts)
{
    auto l = newObj<ObjList>(__func__);
    for(const auto& elt : elts)
        l->elts.push_back(elt);
    return l;
}


std::string roxal::objListToString(const ObjList* ol)
{
    throw std::runtime_error(std::string("unimplemented ")+__func__);
}


ObjDict* roxal::dictVal(const std::vector<std::pair<Value,Value>>& entries)
{
    auto d = newObj<ObjDict>(__func__);
    for(const auto& entry : entries)
        d->entries[entry.first] = entry.second;
    return d;
}

std::string roxal::objDictToString(const ObjDict* od)
{
    throw std::runtime_error(std::string("unimplemented ")+__func__);
}







std::string roxal::objToString(const Value& v)
{
    switch(objType(v)) {
        case ObjType::Closure: {
            return objFunctionToString(asClosure(v)->function);
        }
        case ObjType::Upvalue: {
            return "upvalue";
        }
        case ObjType::Function: {
            return objFunctionToString(asFunction(v));
        }
        case ObjType::Native: {
            return "<native fn>";
        }
        case ObjType::String: {
            return toUTF8StdString(asUString(v));
        }
        case ObjType::List: {
            auto l = asList(v);
            std::ostringstream os;
            os << "[";
            for(auto it = l->elts.begin(); it != l->elts.end(); ++it) {
                const auto& value { *it };
                auto valStr { toString(value) };
                if (isString(value)) valStr = "\""+valStr+"\"";
                os << valStr;
                if (it != l->elts.end()-1)
                    os << ", ";                
            }
            os << "]";
            return os.str();
        }
        case ObjType::Dict: {
            auto d = asDict(v);
            std::ostringstream os;
            os << "{";
            size_t i=0;
            for(auto it = d->entries.begin(); it != d->entries.end(); ++it) {
                const auto& key { it->first };
                const auto& value { it->second };
                auto keyStr { toString(key) };
                auto valStr { toString(value) };
                if (isString(key)) keyStr = "\""+keyStr+"\"";
                if (isString(value)) valStr = "\""+valStr+"\"";
                os << keyStr << ": " << valStr;
                if (i != d->entries.size()-1)
                    os << ", ";
                i++;
            }
            os << "}";
            return os.str();
        }
        case ObjType::Stream: {
            // converting a stream to a string actually converts its current value to a string
            return toString(asStream(v)->currentValue());
        }
        case ObjType::ObjectType: {
            ObjObjectType* obj = asObjectType(v);
            return std::string("<type ")+(obj->isActor ? "actor" :"object")+" "+toUTF8StdString(obj->name)+">";
        }
        case ObjType::Instance: {
            ObjectInstance* inst = asObjectInstance(v);
            return std::string("object "+toUTF8StdString(inst->instanceType->name));
        }
        case ObjType::Actor: {
            ActorInstance* inst = asActorInstance(v);
            return std::string("actor "+toUTF8StdString(inst->instanceType->name));
        }
        case ObjType::BoundMethod: {
            return objFunctionToString(asBoundMethod(v)->method->function);
        }
        case ObjType::Future: {
            ObjFuture* fut = asFuture(v);
            Value v = fut->future.get(); // will block if promise not fulfilled
            return toString(v);
        }
        default: ;
    }
    return "";
}




ObjFunction::ObjFunction()
    : arity(0), upvalueCount(0), name()
{
    type = ObjType::Function;
    chunk = std::make_shared<Chunk>();
}






ObjNative::ObjNative(NativeFn _function)
    : function(_function)
{
    type = ObjType::Native;
}


ObjNative* roxal::nativeVal(NativeFn function)
{
    return newObj<ObjNative>(__func__,function);
}



ObjObjectType* roxal::objectTypeVal(const icu::UnicodeString& typeName, bool isActor)
{
    return newObj<ObjObjectType>(__func__, typeName, isActor);
}


ObjectInstance::ObjectInstance(ObjObjectType* objectType) 
{ 
    type = ObjType::Instance; 
    instanceType = objectType;
    instanceType->incRef();
}

ObjectInstance::~ObjectInstance() 
{
    instanceType->decRef();
}


ObjectInstance* roxal::objectInstanceVal(ObjObjectType* objectType)
{
    return newObj<ObjectInstance>(__func__, objectType);
}



ActorInstance::ActorInstance(ObjObjectType* objectType) 
{ 
    type = ObjType::Actor; 
    instanceType = objectType;
    instanceType->incRef();
}

ActorInstance::~ActorInstance() 
{
    instanceType->decRef();
}


Value ActorInstance::queueCall(const Value& callee, const CallSpec& callSpec, Value* argsStackTop)
{
    // queue producer for consumer Thread::act()
    #ifdef DEBUG_BUILD
    assert(isBoundMethod(callee));
    assert(isActorInstance(asBoundMethod(callee)->receiver));
    #endif
    
    std::lock_guard<std::mutex> lock { queueMutex };

    // TODO: arrange for push to move to queue to avoid copy
    MethodCallInfo callInfo {};
    callInfo.callee = callee;
    callInfo.callSpec = callSpec;
    for(auto i=0; i<callSpec.argCount; i++)
        callInfo.args.push_back( *(argsStackTop - i - 1) );
    callInfo.returnPromise = nullptr;

    // if method is a func, create promise for return value
    auto funcObj = asBoundMethod(callee)->method->function;
    if (funcObj->funcType.has_value()) {
        ptr<roxal::type::Type> funcType { funcObj->funcType.value() };
        assert(funcType->func.has_value());
        if (!funcType->func.value().isProc) {
            callInfo.returnPromise = std::make_shared<std::promise<Value>>();
        }
    }

    callQueue.push(callInfo);

    queueConditionVar.notify_one();

    Value futureReturn {}; // nil by default
    if (callInfo.returnPromise != nullptr)
        futureReturn = Value(futureVal(callInfo.returnPromise->get_future()));

    return futureReturn;
}


ActorInstance* roxal::actorInstanceVal(ObjObjectType* objectType)
{
    return newObj<ActorInstance>(__func__, objectType);
}



ObjBoundMethod::ObjBoundMethod(const Value& instance, ObjClosure* closure)
    : receiver(instance), method(closure)
{
    type = ObjType::BoundMethod;
    method->incRef();
}

ObjBoundMethod::~ObjBoundMethod() 
{
    method->decRef();
}






bool roxal::objsEqual(const Value& l, const Value& r)
{
    if (!l.isObj() || !r.isObj())
        return false;

    if (objType(l) != objType(r))
        return false;

    switch (objType(l)) {
        case ObjType::String: {
            auto ls = asString(l);
            auto rs = asString(r);
            if (ls == rs) // identical object
                return true;            

            // Trust hash.  Possible different strings with has collision will
            //  compare as equal (low probability)
            // TODO: consider doing full char comparison for equality if hashes match
            return asString(l)->hash == asString(l)->hash;
            // if (ls->s.length() != rs->s.length())
            //     return false;
            // return ls->s == rs->s;
        } break;
        default: 
            throw std::runtime_error("Unimplemented object type for objEqual:"+std::to_string(int(objType(l))));

    }
    return false;
}


std::string roxal::objTypeName(Obj* obj)
{
    if (obj == nullptr) return "null";

    switch (obj->type) {
    case ObjType::None: return "none";
    case ObjType::ObjectType: return static_cast<ObjObjectType*>(obj)->isActor ? "type actor" : "type object";
    case ObjType::Instance: return "object";
    case ObjType::Actor: return "actor";
    case ObjType::BoundMethod: return "function";
    case ObjType::Closure: return "closure";
    case ObjType::Function: return "function";
    case ObjType::Native: return "native";
    case ObjType::Upvalue: return "upvalue";
    case ObjType::Future: return "future";
    case ObjType::Bool: return "bool";
    case ObjType::Int: return "int";
    case ObjType::Real: return "real";
    case ObjType::String: return "string";
    case ObjType::List: return "list";
    }
    return "unknown";
}



#ifdef DEBUG_BUILD
void roxal::testObjectValues()
{
    Value i0 { 4 };
    Value i1 { 6 };
    Value i2 { 8 };

    Value l1 { listVal(std::vector<Value>{i0,i1,i2}) };

    assert(isList(l1));
    assert(!l1.isNil());
    ObjList* lp = static_cast<ObjList*>(l1.asObj());
    assert(lp->elts.size() == 3);

    Value l2 = l1;
    assert(isList(l2));
    assert(!l2.isNil());

    assert(l1 == l2);
    assert(!(l2 == nilVal()));
}
#endif
