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


Value ObjString::index(const Value& i) const
{
    if (!i.isNumber() && !isRange(i))
        throw std::invalid_argument("String index must be a number or a range.");

    UnicodeString substr {};
    if (i.isNumber()) {
        auto len = s.length();
        auto unit = i.asInt();
        // allow -ve numbers to index from the end of the string
        if (unit < 0)
            unit = len - (-unit);

        if (unit < 0 || unit >= len)
            throw std::invalid_argument("String index out-of-range.");

        s.extract(int32_t(unit),1,substr);
        return objVal(stringVal(substr));
    }
    else if (isRange(i)) {
        auto r = asRange(i);
        auto strLen = s.length();
        auto rangeLen = r->length(strLen);
        //std::cout << "::index " << i << " len:" << rangeLen << std::endl;
        for(auto i=0; i<rangeLen; i++) {
            auto targetIndex = r->targetIndex(i,strLen);
            //std::cout << " ti=" << targetIndex << std::endl;
            if ((targetIndex >= 0) && (targetIndex < strLen))
                substr += s.charAt(targetIndex);
        }
        if (substr.isBogus())
            throw std::invalid_argument("Resulting sub-string from index is not valid");

        return objVal(stringVal(substr));
    }
    throw std::runtime_error("String indexing subscript must be a number or a range.");
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



int32_t ObjRange::length(int32_t targetLen) const
{
    // handle common case of ints first
    if (   (start.isInt() || start.isNil())
         && (stop.isInt() || stop.isNil())
         && (step.isInt() || step.isNil())) {

        int32_t stepi = step.isNil() ? 1 : step.asInt();

        if (stepi > 0) { // normal order
            int32_t starti = start.isNil() ? 0 : start.asInt();
            int32_t stopi = stop.isNil() ? targetLen : stop.asInt();
            if (starti < 0) starti = targetLen + starti;
            if (stopi < 0) stopi = targetLen + stopi;

            if (!closed) {
                //return abs(stopi - starti)/stepi;
                if (starti >= stopi) return 0;
                return (stopi - starti - 1) / stepi + 1;
            }
            else {
                //return abs(stopi - starti + 1)/stepi;
                if (starti > stopi) return 0;
                return ((stopi+1) - starti - 1) / stepi + 1;
            }
        }
        else { // reverse order e.g. -2:1:-2
            int32_t starti = start.isNil() ? targetLen-1 : start.asInt();
            int32_t stopi = stop.isNil() ? (closed?0:-1) : stop.asInt();
            if (starti < 0) starti = targetLen + starti;
            if ((stopi < 0) && !stop.isNil()) stopi = targetLen + stopi;

            if (!closed)
                return (starti - stopi - 1)/-stepi + 1;
            else
                return (starti - (stopi-1) - 1)/-stepi + 1;
        }
    }
    else {
        throw std::runtime_error("non-int ranges unimplemented");
    }
}


int32_t ObjRange::length() const
{
    // handle common case of ints first
    if (   (start.isInt() || start.isNil())
         && (stop.isInt() || stop.isNil())
         && (step.isInt() || step.isNil())) {

        int32_t stepi = step.isNil() ? 1 : step.asInt();

        if (stepi > 0) { // normal order
            if (stop.isNil()) return -1;

            int32_t starti = start.isNil() ? 0 : start.asInt();
            int32_t stopi = stop.asInt();
            //if ((starti < 0) || (stopi < 0)) return -1;
            //std::cout << " length() starti:" << starti << " stopi:" << stopi << " stepi:" << stepi << " closed:" << closed << std::endl;
            if (!closed) {
                if (starti >= stopi) return 0;
                return (stopi - starti - 1) / stepi + 1;
            }
            else {
                if (starti > stopi) return 0;
                return ((stopi+1) - starti - 1) / stepi + 1;
            }

        } else { // reverse order e.g. -2:1:-2
            //std::cout << "length()" << objRangeToString(this) << std::endl;
            if (start.isNil()) return -1;

            int32_t starti = start.asInt();
            int32_t stopi = stop.isNil() ? (closed?0:-1) : stop.asInt();
            //if ((starti < 0) || ((stopi < 0) && !stop.isNil())) return -1;
            //std::cout << " length() starti:" << starti << " stopi:" << stopi << " stepi:" << stepi << (closed?" closed":" open") << std::endl;

            if (!closed)
                return (starti - stopi - 1)/-stepi + 1;
            else
                return (starti - (stopi-1) - 1)/-stepi + 1;
        }
    }
    else {
        throw std::runtime_error("non-int ranges unimplemented");
    }
}



int32_t ObjRange::targetIndex(int32_t index, int32_t targetLen) const
{
    // handle common case of ints first
    if (   (start.isInt() || start.isNil())
         && (stop.isInt() || stop.isNil())
         && (step.isInt() || step.isNil())) {

        bool rangeOverTarget = targetLen >= 0;

        auto stepi = step.isNil() ? 1 : step.asInt();
        int32_t starti, stopi;

        if (stepi > 0) { // normal order
            starti = start.isNil() ? 0 : start.asInt();
            stopi = stop.isNil() ? targetLen : stop.asInt();
            if (rangeOverTarget) {
                if (starti < 0) starti = targetLen + starti;
                if (stopi < 0) stopi = targetLen + stopi;
            }
        }
        else { // reverse order
            if (!rangeOverTarget && start.isNil())
                 throw std::invalid_argument("Indeterminate range start");

            starti = start.isNil() ? targetLen-1 : start.asInt();
            stopi = stop.isNil() ? -1 : stop.asInt();
            if (rangeOverTarget) {
                if (starti < 0) starti = targetLen + starti;
                if ((stopi < 0) && !stop.isNil()) stopi = targetLen + stopi;
            }
        }

        return starti + index*stepi;
    }
    else {
        throw std::runtime_error("non-int ranges unimplemented");
    }
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

    if (!r->start.isNil())
        oss << toString(r->start);
    oss << std::string(r->closed ? ".." : "..<");
    if (!r->stop.isNil())
        oss << toString(r->stop);
    if (!r->step.isNil() && (r->step.asInt()!=1))
        oss << " by " << toString(r->step);

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



ObjList::ObjList(const ObjRange* r)
{
    type = ObjType::List;
    int32_t rangeLen = r->length();
    for(int32_t i=0; i<rangeLen; i++)
        elts.push_back(intVal(r->targetIndex(i,-1)));
}


Value ObjList::index(const Value& i) const
{
    if (i.isNumber()) {
        auto index = i.asInt();
        if (index < 0 || index >= length())
            throw std::invalid_argument("List index out-of-range.");
        return elts.at(index);
    }
    else if (isRange(i)) {
        auto sublist = listVal();
        auto r = asRange(i);
        auto listLen = length();
        auto rangeLen = r->length(listLen);

        for(auto i=0; i<rangeLen; i++) {
            auto targetIndex = r->targetIndex(i,listLen);
            if ((targetIndex >= 0) && (targetIndex < listLen))
                sublist->elts.push_back(elts.at(targetIndex));
        }

        return objVal(sublist);
    }
    else
        throw std::invalid_argument("List indexing subscript must be a number or a range.");
    return nilVal();
}


void ObjList::setIndex(const Value& i, const Value& v)
{
    if (i.isNumber()) {
        auto index = i.asInt();
        if (index < 0 || index >= length())
            throw std::invalid_argument("List index out-of-range.");
        elts.store(index, v);
    }
    else if (isRange(i)) {

        if (!isList(v))
            throw std::invalid_argument("Assignment to list with range requires a list on the RHS.");

        const ObjList* rhsList = asList(v);
        auto rhsLen = rhsList->length();

        auto r = asRange(i);
        auto listLen = length();
        auto rangeLen = r->length(listLen);

        if (rhsLen != rangeLen)
            throw std::invalid_argument("Assignment to list with range requires a list on RHS of same length ("+std::to_string(rangeLen)+") as the range being assigned (len RHS is "+std::to_string(rhsLen)+" ).");

        for(auto i=0; i<rangeLen; i++) {
            auto targetIndex = r->targetIndex(i,listLen);
            if ((targetIndex >= 0) && (targetIndex < listLen)) {
                if (i < rhsLen)
                    elts.store(targetIndex,rhsList->elts.at(i));
            }
        }
    }
    else
        throw std::invalid_argument("List indexing subscript must be a number or a range (not "+to_string(i.type())+").");
}



ObjList* roxal::listVal()
{
    return newObj<ObjList>(__func__);
}


ObjList* roxal::listVal(const ObjRange* r)
{
    return newObj<ObjList>(__func__,r);
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
    std::ostringstream os;
    os << "[";
    auto list { ol->elts.get() };
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





ObjDict* roxal::dictVal()
{
    return newObj<ObjDict>(__func__);
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
    std::ostringstream os;
    os << "{";
    size_t i=0;
    auto keys { od->keys() };
    for(const auto& key : keys) {
        const auto value { od->at(key) };
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
            return objListToString(asList(v));
        }
        case ObjType::Dict: {
            return objDictToString(asDict(v));
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
                return std::string("<type ")+(obj->isActor ? "actor" :(obj->isInterface ? "interface" : (obj->isEnumeration ? "enum" : "object")))+" "+toUTF8StdString(obj->name)+">";
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
        default: ;
    }
    return "";
}



std::string roxal::toString(FunctionType ft)
{
    switch (ft) {
        case FunctionType::Function: return "Function";
        case FunctionType::Method: return "Method";
        case FunctionType::Initializer: return "Initializer";
        case FunctionType::Module: return "Module";
        default: return "?";
    }
}



ObjFunction::ObjFunction(const icu::UnicodeString& packageName, const icu::UnicodeString& moduleName)
    : arity(0), upvalueCount(0), name()
{
    type = ObjType::Function;
    chunk = std::make_shared<Chunk>(packageName, moduleName);
}


ObjFunction::~ObjFunction()
{
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




std::unordered_map<uint16_t, roxal::ObjObjectType*> ObjObjectType::enumTypes {};

ObjObjectType::ObjObjectType(const icu::UnicodeString& typeName, bool isactor, bool isinterface, bool isenumeration)
    : name(typeName), isActor(isactor), isInterface(isinterface), isEnumeration(isenumeration)
{
    typeValue = ValueType::Object;
    if (isActor)
        typeValue = ValueType::Actor;
    else if (isEnumeration) {
        typeValue = ValueType::Enum;

        // register ourselves in the global map of enum types, referenced in the enum values
        enumTypeId = randomUint16(1); // generate unique random id (1..max)
        while (ObjObjectType::enumTypes.find(enumTypeId) != ObjObjectType::enumTypes.end())
            enumTypeId = randomUint16(1);
        //std::cout << "registered new enum id " << enumTypeId << std::endl;
        enumTypes[enumTypeId] = this;
    }
}

ObjObjectType* roxal::objectTypeVal(const icu::UnicodeString& typeName, bool isActor, bool isInterface, bool isEnumeration)
{
    return newObj<ObjObjectType>(__func__, typeName, isActor, isInterface, isEnumeration);
}



ObjModuleType::ObjModuleType(const icu::UnicodeString& typeName)
    : name(typeName)
{
    typeValue = ValueType::Module;
}

ObjModuleType* roxal::moduleTypeVal(const icu::UnicodeString& typeName)
{
    return newObj<ObjModuleType>(__func__, typeName);
}




ObjectInstance::ObjectInstance(ObjObjectType* objectType)
{
    type = ObjType::Instance;
    instanceType = objectType;
    instanceType->incRef();

    for(const auto& property : objectType->properties) {
        const auto& prop { property.second };
        auto propInitialvalue { prop.initialValue };
        properties[prop.name.hashCode()] = propInitialvalue;
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
            const auto& prop { obj->instanceType->properties.at(index) };
            std::cerr << "shallow copying property " << toUTF8StdString(prop.name) << " :" << value.typeName() <<  " from " << Value(obj->instanceType) << std::endl;
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
