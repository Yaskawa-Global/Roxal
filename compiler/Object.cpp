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
        case ObjType::Range: return ValueType::Range;
        case ObjType::Type: return ValueType::Type; // TODO: need to return more specific ObjectType for type object & type actor?
        case ObjType::List: return ValueType::List;
        case ObjType::Dict: return ValueType::Dict;
        case ObjType::Stream: return ValueType::Stream;
        case ObjType::Instance: return static_cast<const ObjObjectType*>(this)->isActor ? ValueType::Actor : ValueType::Object;
        default: return ValueType::Nil;
    }
}


// deep copy
Obj* Obj::clone() const
{
    Obj* mutableThis = const_cast<Obj*>(this);

    if (type == ObjType::String)
        return mutableThis; // strings are immutable, safe to copy reference
        // FIXME!!!: collision for ObjType::Type - used by ObjTypeSpec and ObjPrimitive for builtin type!
    else if (type == ObjType::Bool || type == ObjType::Real || type == ObjType::Int /*|| type == ObjType::Type*/)
        return cloneObjPrimitive(static_cast<const ObjPrimitive*>(this));
    else if (type == ObjType::Range)
        return cloneRange(static_cast<const ObjRange*>(this));
    else if (type == ObjType::List)
        return cloneList(static_cast<const ObjList*>(this));
    else if (type == ObjType::Dict)
        return cloneDict(static_cast<const ObjDict*>(this));
    else if (type == ObjType::Function) // code is immutable, can copy reference
        return mutableThis;
    else if (type == ObjType::Upvalue)
        return cloneUpvalue(static_cast<const ObjUpvalue*>(this));
    else if (type == ObjType::Closure)
        return cloneClosure(static_cast<const ObjClosure*>(this));
    else if (type == ObjType::Future) {
        // wait for result, then turn it into an ObjPrimitive copy of the value
        Value value = static_cast<ObjFuture*>(mutableThis)->asValue();
        value.box();
        assert(value.isBoxed() && value.isObj() && isObjPrimitive(value));
        return asObjPrimitive(value);
    }
    else if (type == ObjType::Native)
        return mutableThis; // native functions are immutable
    else if (type == ObjType::Type) // complex types are immutable once declared
        return mutableThis;    
    else if (type == ObjType::Instance)
        return cloneObjectInstance(static_cast<const ObjectInstance*>(this));
    else if (type == ObjType::Actor)
        throw std::runtime_error("cannot be clone() type actor instances");
    else if (type == ObjType::BoundMethod)
        // NB: this clones the reciever object
        return cloneBoundMethod(static_cast<const ObjBoundMethod*>(this));
    else if (type == ObjType::RangeView)
        // NB: this clones the viewed target value
        return cloneRangeView(static_cast<const RangeView*>(this));

    throw std::runtime_error("clone() unimplemented for type "+std::to_string(int(this->type)));
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



// range

ObjRange::ObjRange()
    : start(intVal(0)), stop(intVal(0)), step(intVal(1)), closed(false)
{
    type = ObjType::Range;
}


ObjRange::ObjRange(const Value& rstart, const Value& rstop, const Value& rstep, bool rclosed)
    : start(rstart), stop(rstop), step(rstep), closed(rclosed)
{
    type = ObjType::Range;
}


ObjRange* roxal::rangeVal()
{
    return newObj<ObjRange>(__func__);
}


ObjRange* roxal::rangeVal(const Value& start, const Value& stop, const Value& step, bool closed)
{
    return newObj<ObjRange>(__func__,start,stop,step,closed);
}


std::string roxal::objRangeToString(const ObjRange* r)
{
    std::ostringstream oss {};

    //oss << "[";
    if (!r->start.isNil())
        oss << toString(r->start);
    oss << std::string(r->closed ? ".." : ":");
    if (!r->stop.isNil())
        oss << toString(r->stop);
    if (!r->step.isNil() && (r->step.asInt()!=1))
        oss << ":" << toString(r->step);
    //oss << "]";
    return oss.str();
}


ObjRange* roxal::cloneRange(const ObjRange* r)
{
    return newObj<ObjRange>(__func__,
                           r->start.clone(),
                           r->stop.clone(),
                           r->step.clone(),
                           r->closed);
}




// runtime types

ObjTypeSpec* roxal::typeSpecVal(ValueType t)
{
    #ifdef DEBUG_BUILD
    assert(t != ValueType::Object && t != ValueType::Actor);
    #endif
    auto ts = newObj<ObjTypeSpec>(__func__);
    ts->typeValue = t;
    return ts;
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


ObjList* roxal::cloneList(const ObjList* l)
{
    // TODO: optimize
    auto newl = newObj<ObjList>(__func__);
    auto lsize = l->elts.size();
    for(auto i=0; i<lsize; i++)
        newl->elts.push_back(l->elts.at(i).clone());
    return newl;
}



std::string roxal::objListToString(const ObjList* ol)
{
    throw std::runtime_error(std::string("unimplemented ")+__func__);
}


ObjDict* roxal::dictVal(const std::vector<std::pair<Value,Value>>& entries)
{
    auto d = newObj<ObjDict>(__func__);
    for(const auto& entry : entries)
        d->store(entry.first, entry.second);
    return d;
}

ObjDict* roxal::cloneDict(const ObjDict* d)
{
    auto newd = newObj<ObjDict>(__func__);
    const auto dkeys = d->keys();
    for(const auto& dkey : dkeys)
        newd->store(dkey.clone(), d->at(dkey).clone());
    return newd;
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
        case ObjType::Range: {
            return objRangeToString(asRange(v));
        }
        case ObjType::List: {
            auto l = asList(v);
            std::ostringstream os;
            os << "[";
            auto list { l->elts.get() };
            for(auto it = list.begin(); it != list.end(); ++it) {
                const auto& value { *it };
                auto valStr { toString(value) };
                if (isString(value)) valStr = "\""+valStr+"\"";
                os << valStr;
                if (it != list.end()-1)
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
            auto keys { d->keys() };
            for(const auto& key : keys) {
                const auto value { d->at(key) };
                auto keyStr { toString(key) };
                auto valStr { toString(value) };
                if (isString(key)) keyStr = "\""+keyStr+"\"";
                if (isString(value)) valStr = "\""+valStr+"\"";
                os << keyStr << ": " << valStr;
                if (i != keys.size()-1)
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
        case ObjType::Type: {
            ObjTypeSpec* ts = asTypeSpec(v);
            if ((ts->typeValue != ValueType::Object) && (ts->typeValue != ValueType::Actor)) {
                return "<type "+to_string(ts->typeValue)+">";
            }
            else {
                ObjObjectType* obj = asObjectType(v);
                return std::string("<type ")+(obj->isActor ? "actor" :"object")+" "+toUTF8StdString(obj->name)+">";
            }
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
        case ObjType::RangeView: {
            // TODO: convert view to underlying target value type (by taking view)
            //  and output that.  For now, output something that shows range and value
            RangeView* rv = asRangeView(v);
            return toString(rv->value)+"["+toString(rv->range)+"]";
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

    for(const auto& property : objectType->properties) {
        auto propNameTypeInitial { property.second };
        auto propName { std::get<0>(propNameTypeInitial) };
        //auto propType { std::get<1>(propNameTypeInitial) };
        auto propInitialvalue { std::get<2>(propNameTypeInitial) };
        properties[propName.hashCode()] = propInitialvalue;
    }
}

ObjectInstance::~ObjectInstance() 
{
    instanceType->decRef();
}


ObjectInstance* roxal::objectInstanceVal(ObjObjectType* objectType)
{
    return newObj<ObjectInstance>(__func__, objectType);
}


ObjectInstance* roxal::cloneObjectInstance(const ObjectInstance* obj)
{
    // clone (deep copy) object instance
    auto newobj = objectInstanceVal(obj->instanceType);

    for(const auto& index_value : obj->properties) {
        const auto index { index_value.first };
        const auto& value { index_value.second };

        if (value.isPrimitive())
            newobj->properties[index] = value;
        else if (isString(value)) {
            // strings are reference types, but immutable, so can copy reference
            newobj->properties[index] = value;
        } 
        else if (isObjectInstance(value)) {
            auto propcopy = cloneObjectInstance(asObjectInstance(value));  // recurse
            newobj->properties[index] = Value(propcopy);
        }
        else if (isActorInstance(value)) {
            throw std::runtime_error("clone of type actor unsuported");
        }
        else if (isList(value)) {

        }
        // TODO: add explicit handling of internal types like closure, future, function etc (just shallow copy)
        //       add explicit deep copying of builtin ref types like list and dict 
        else {
            #ifdef DEBUG_BUILD
            auto propName { std::get<0>(obj->instanceType->properties.at(index)) };
            std::cerr << "shallow copying property " << toUTF8StdString(propName) << " :" << value.typeName() <<  " from " << Value(obj->instanceType) << std::endl;
            std::cerr << " isPrimitive?" << (value.isPrimitive() ? "yes":"no") << std::endl;
            #endif
            newobj->properties[index] = value;
        }

    }

    return newobj;
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
    case ObjType::Type: return "type";
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
