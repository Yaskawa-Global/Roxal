#include <stdexcept>
#include <cassert>
#include <unordered_map>
#include <sstream>
#include <algorithm>
#include <iomanip>
#include <dlfcn.h>
#include <future>

#include <core/types.h>
#include "Object.h"
#include "VM.h"
#include "FFI.h"
#include "Value.h"
#include "Thread.h"
#include "dataflow/Signal.h"

using namespace roxal;


atomic_vector<Obj*> Obj::unrefedObjs {};

#include <core/AST.h>

namespace {

using namespace roxal;

void writeTypeInfo(std::ostream& out, const type::Type& t);
ptr<type::Type> readTypeInfo(std::istream& in);
void writeAnnotation(std::ostream& out, const ast::Annotation& a);
ptr<ast::Annotation> readAnnotation(std::istream& in);

void writeString(std::ostream& out, const icu::UnicodeString& s) {
    std::string u8; s.toUTF8String(u8);
    uint32_t len = u8.size();
    out.write(reinterpret_cast<char*>(&len),4);
    out.write(u8.data(), len);
}

icu::UnicodeString readString(std::istream& in) {
    uint32_t len; in.read(reinterpret_cast<char*>(&len),4);
    std::string u8(len,'\0'); if(len) in.read(u8.data(), len);
    return icu::UnicodeString::fromUTF8(u8);
}

void writeTypeInfo(std::ostream& out, const type::Type& t) {
    uint8_t b = static_cast<uint8_t>(t.builtin);
    out.write(reinterpret_cast<char*>(&b),1);
    if (t.builtin == type::BuiltinType::Func && t.func.has_value()) {
        uint8_t hasFunc = 1; out.write(reinterpret_cast<char*>(&hasFunc),1);
        const auto& ft = t.func.value();
        uint8_t proc = ft.isProc ? 1 : 0; out.write(reinterpret_cast<char*>(&proc),1);
        uint32_t pc = ft.params.size();
        out.write(reinterpret_cast<char*>(&pc),4);
        for(const auto& p : ft.params){
            uint8_t present = p.has_value()?1:0; out.write(reinterpret_cast<char*>(&present),1);
            if(present){
                writeString(out, p->name);
                uint8_t ht = p->type.has_value()?1:0; out.write(reinterpret_cast<char*>(&ht),1);
                if(ht) writeTypeInfo(out, *p->type.value());
                uint8_t hd = p->hasDefault?1:0; out.write(reinterpret_cast<char*>(&hd),1);
            }
        }
        uint32_t rc = ft.returnTypes.size();
        out.write(reinterpret_cast<char*>(&rc),4);
        for(const auto& rt : ft.returnTypes)
            writeTypeInfo(out, *rt);
    } else if (t.builtin == type::BuiltinType::Func) {
        uint8_t hasFunc = 0; out.write(reinterpret_cast<char*>(&hasFunc),1);
    }
}

ptr<type::Type> readTypeInfo(std::istream& in) {
    uint8_t b; in.read(reinterpret_cast<char*>(&b),1);
    auto t = std::make_shared<type::Type>(static_cast<type::BuiltinType>(b));
    if (t->builtin == type::BuiltinType::Func) {
        uint8_t hasFunc; in.read(reinterpret_cast<char*>(&hasFunc),1);
        if(hasFunc){
            t->func = type::Type::FuncType();
            auto& ft = t->func.value();
            uint8_t proc; in.read(reinterpret_cast<char*>(&proc),1); ft.isProc = proc!=0;
            uint32_t pc; in.read(reinterpret_cast<char*>(&pc),4); ft.params.resize(pc);
            for(uint32_t i=0;i<pc;i++){
                uint8_t present; in.read(reinterpret_cast<char*>(&present),1);
                if(present){
                    type::Type::FuncType::ParamType param;
                    param.name = readString(in);
                    param.nameHashCode = param.name.hashCode();
                    uint8_t ht; in.read(reinterpret_cast<char*>(&ht),1);
                    if(ht) param.type = readTypeInfo(in);
                    uint8_t hd; in.read(reinterpret_cast<char*>(&hd),1); param.hasDefault = hd!=0;
                    ft.params[i] = param;
                }
            }
            uint32_t rc; in.read(reinterpret_cast<char*>(&rc),4);
            for(uint32_t i=0;i<rc;i++)
                ft.returnTypes.push_back(readTypeInfo(in));
        }
    }
    return t;
}

void writeExpr(std::ostream& out, const ptr<ast::Expression>& expr){
    using namespace ast;
    if(auto s = std::dynamic_pointer_cast<Str>(expr)){
        uint8_t tag=0; out.write(reinterpret_cast<char*>(&tag),1);
        writeString(out, s->str);
    } else if(auto n = std::dynamic_pointer_cast<Num>(expr)){
        uint8_t tag=1; out.write(reinterpret_cast<char*>(&tag),1);
        if(std::holds_alternative<int32_t>(n->num)){
            uint8_t ty=0; out.write(reinterpret_cast<char*>(&ty),1);
            int32_t v=std::get<int32_t>(n->num); out.write(reinterpret_cast<char*>(&v),4);
        } else {
            uint8_t ty=1; out.write(reinterpret_cast<char*>(&ty),1);
            double v=std::get<double>(n->num); out.write(reinterpret_cast<char*>(&v),8);
        }
    } else if(auto b = std::dynamic_pointer_cast<Bool>(expr)){
        uint8_t tag=2; out.write(reinterpret_cast<char*>(&tag),1);
        uint8_t v=b->value?1:0; out.write(reinterpret_cast<char*>(&v),1);
    } else if(auto v = std::dynamic_pointer_cast<Variable>(expr)){
        uint8_t tag=3; out.write(reinterpret_cast<char*>(&tag),1);
        writeString(out, v->name);
    } else {
        throw std::runtime_error("unsupported annotation expr serialization");
    }
}

ptr<ast::Expression> readExpr(std::istream& in){
    using namespace ast;
    uint8_t tag; in.read(reinterpret_cast<char*>(&tag),1);
    switch(tag){
        case 0: {
            auto s = std::make_shared<Str>();
            s->str = readString(in);
            return s;
        }
        case 1: {
            auto n = std::make_shared<Num>();
            uint8_t ty; in.read(reinterpret_cast<char*>(&ty),1);
            if(ty==0){
                int32_t v; in.read(reinterpret_cast<char*>(&v),4);
                n->num = v;
            } else {
                double v; in.read(reinterpret_cast<char*>(&v),8);
                n->num = v;
            }
            return n;
        }
        case 2: {
            auto b = std::make_shared<Bool>();
            uint8_t v; in.read(reinterpret_cast<char*>(&v),1);
            b->value = v!=0; return b;
        }
        case 3: {
            auto v = std::make_shared<Variable>();
            v->name = readString(in);
            return v;
        }
    }
    throw std::runtime_error("unsupported annotation expr tag");
}

void writeAnnotation(std::ostream& out, const ast::Annotation& a){
    writeString(out, a.name);
    uint32_t count = a.args.size(); out.write(reinterpret_cast<char*>(&count),4);
    for(const auto& arg : a.args){
        writeString(out, arg.first);
        writeExpr(out, arg.second);
    }
}

ptr<ast::Annotation> readAnnotation(std::istream& in){
    auto a = std::make_shared<ast::Annotation>();
    a->name = readString(in);
    uint32_t count; in.read(reinterpret_cast<char*>(&count),4);
    for(uint32_t i=0;i<count;i++){
        icu::UnicodeString name = readString(in);
        ptr<ast::Expression> expr = readExpr(in);
        a->args.emplace_back(name, expr);
    }
    return a;
}

}

#ifdef DEBUG_TRACE_MEMORY
atomic_map<Obj*, const char*> Obj::allocatedObjs {};
#endif

atomic_vector<ObjModuleType*> ObjModuleType::allModules {};


ValueType Obj::valueType() const
{
    // FIXME: For efficiency, the enum values between ValueType and ObjType should be synchronized
    // and the Value::type() made efficient by returning a cast from ObjType -> ValueType for all
    // object cases to avoid a cascade of is*() checks for every single type.
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
        case ObjType::Vector: return ValueType::Vector;
        case ObjType::Matrix: return ValueType::Matrix;
        case ObjType::Signal: return ValueType::Signal;
        case ObjType::Event: return ValueType::Event;
        case ObjType::Function: return ValueType::Function;
        case ObjType::Closure: return ValueType::Closure;
        case ObjType::Upvalue: return ValueType::Upvalue;
        case ObjType::Exception: return ValueType::Object;
        case ObjType::Instance: return static_cast<const ObjObjectType*>(this)->isActor ? ValueType::Actor : ValueType::Object;
        case ObjType::Actor: return ValueType::Actor;
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
    else if (type == ObjType::Vector)
        return cloneVector(static_cast<const ObjVector*>(this));
    else if (type == ObjType::Matrix)
        return cloneMatrix(static_cast<const ObjMatrix*>(this));
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
        throw std::runtime_error("cannot clone() type actor instances");
    else if (type == ObjType::BoundMethod)
        // NB: this clones the receiver object
        return cloneBoundMethod(static_cast<const ObjBoundMethod*>(this));
    else if (type == ObjType::BoundNative)
        return cloneBoundNative(static_cast<const ObjBoundNative*>(this));
    else if (type == ObjType::Library)
        return mutableThis;
    else if (type == ObjType::ForeignPtr)
        return mutableThis;
    else if (type == ObjType::Event)
        // Note: currently ObjEvents have no user-mutable state, so we can just share the reference
        return mutableThis;

    throw std::runtime_error("clone() unimplemented for type "+std::to_string(int(this->type)));
}






// interned strings table
static atomic_unordered_map<int32_t, ObjString*> strings {};

ObjString::ObjString()
    : s()
{
    type = ObjType::String;
    hash = 0;
}

ObjString::ObjString(const UnicodeString& us)
    :  s(us)
{
    type = ObjType::String;
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

void roxal::updateInternedString(ObjString* obj, const UnicodeString& newVal)
{
    if (!obj) return;
    strings.erase(obj->hash);
    obj->s = newVal;
    obj->hash = obj->s.hashCode();
    strings.store(obj->hash, obj);
}

void ObjString::write(std::ostream& out, roxal::ptr<SerializationContext> ctx) const
{
    std::string ss;
    s.toUTF8String(ss);
    uint32_t len = ss.size();
    out.write(reinterpret_cast<char*>(&len), 4);
    out.write(ss.data(), len);
}

void ObjString::read(std::istream& in, roxal::ptr<SerializationContext> ctx)
{
    uint32_t len;
    in.read(reinterpret_cast<char*>(&len), 4);
    std::string ss(len, '\0');
    if (len > 0) in.read(ss.data(), len);
    UnicodeString us = UnicodeString::fromUTF8(ss);
    updateInternedString(this, us);
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

void ObjRange::write(std::ostream& out, roxal::ptr<SerializationContext> ctx) const
{
    writeValue(out, start, ctx);
    writeValue(out, stop, ctx);
    writeValue(out, step, ctx);
    uint8_t c = closed ? 1 : 0;
    out.write(reinterpret_cast<char*>(&c), 1);
}

void ObjRange::read(std::istream& in, roxal::ptr<SerializationContext> ctx)
{
    start = readValue(in, ctx);
    stop  = readValue(in, ctx);
    step  = readValue(in, ctx);
    uint8_t c; in.read(reinterpret_cast<char*>(&c), 1);
    closed = c != 0;
}

ObjVector::ObjVector(const Eigen::VectorXd& values)
    : vec(values)
{
    type = ObjType::Vector;
}

ObjVector::ObjVector(int32_t size)
    : vec(size)
{
    type = ObjType::Vector;
    vec.setZero();
}

Value ObjVector::index(const Value& i) const
{
    if (i.isNumber()) {
        auto index = i.asInt();
        if (index < 0 || index >= length())
            throw std::invalid_argument("Vector index out-of-range.");
        return realVal(vec[index]);
    }
    else if (isRange(i)) {
        auto r = asRange(i);
        auto vecLen = length();
        auto rangeLen = r->length(vecLen);
        std::vector<double> elts;
        elts.reserve(rangeLen);
        for(int32_t j=0; j<rangeLen; ++j) {
            auto targetIndex = r->targetIndex(j, vecLen);
            if ((targetIndex >= 0) && (targetIndex < vecLen))
                elts.push_back(vec[targetIndex]);
        }
        Eigen::VectorXd vals(elts.size());
        for(size_t k=0; k<elts.size(); ++k)
            vals[k] = elts[k];
        return objVal(vectorVal(vals));
    }
    else
        throw std::invalid_argument("Vector indexing subscript must be a number or a range.");
    return nilVal();
}

void ObjVector::setIndex(const Value& i, const Value& v)
{
    if (i.isNumber()) {
        auto index = i.asInt();
        if (index < 0 || index >= length())
            throw std::invalid_argument("Vector index out-of-range.");
        Value rv = toType(ValueType::Real, v, /*strict=*/false);
        vec[index] = rv.asReal();
    }
    else if (isRange(i)) {
        if (!isVector(v))
            throw std::invalid_argument("Assignment to vector with range requires a vector on the RHS.");
        const ObjVector* rhsVec = asVector(v);
        auto r = asRange(i);
        auto vecLen = length();
        auto rangeLen = r->length(vecLen);
        if (rhsVec->length() != rangeLen)
            throw std::invalid_argument("Assignment to vector with range requires a vector on RHS of same length ("+std::to_string(rangeLen)+") as the range being assigned (len RHS is "+std::to_string(rhsVec->length())+" ).");
        for(int32_t j=0; j<rangeLen; ++j) {
            auto targetIndex = r->targetIndex(j, vecLen);
            if ((targetIndex >= 0) && (targetIndex < vecLen)) {
                if (j < rhsVec->length())
                    vec[targetIndex] = rhsVec->vec[j];
            }
        }
    }
    else {
        throw std::invalid_argument("Vector indexing subscript must be a number or a range (not "+to_string(i.type())+").");
    }
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

void ObjList::concatenate(const ObjList* other)
{
    // Efficiently append all elements from other list in one lock
    // (atomic_vector::append handles the reserve internally)
    elts.append(other->elts);
}

void ObjList::append(const Value& value)
{
    elts.push_back(value);
}

void ObjList::write(std::ostream& out, roxal::ptr<SerializationContext> ctx) const
{
    uint32_t len = length();
    out.write(reinterpret_cast<char*>(&len), 4);
    auto list = elts.get();
    for(const auto& v : list)
        writeValue(out, v, ctx);
}

void ObjList::read(std::istream& in, roxal::ptr<SerializationContext> ctx)
{
    uint32_t len;
    in.read(reinterpret_cast<char*>(&len), 4);
    elts.clear();
    for(uint32_t i=0;i<len;i++)
        elts.push_back(readValue(in, ctx));
}

void ObjList::set(const ObjList* other)
{
    elts = other->elts; // atomic_vector assignment performs copy
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

void ObjDict::set(const ObjDict* other)
{
    std::lock_guard<std::mutex> lockThis(m);
    std::lock_guard<std::mutex> lockOther(other->m);
    m_keys = other->m_keys;
    entries = other->entries;
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

void ObjDict::write(std::ostream& out, roxal::ptr<SerializationContext> ctx) const
{
    auto ents = items();
    uint32_t len = ents.size();
    out.write(reinterpret_cast<char*>(&len), 4);
    for(const auto& p : ents) {
        writeValue(out, p.first, ctx);
        writeValue(out, p.second, ctx);
    }
}

void ObjDict::read(std::istream& in, roxal::ptr<SerializationContext> ctx)
{
    uint32_t len;
    in.read(reinterpret_cast<char*>(&len), 4);
    m_keys.clear();
    entries.clear();
    for(uint32_t i=0;i<len;i++) {
        Value k = readValue(in, ctx);
        Value v = readValue(in, ctx);
        m_keys.push_back(k);
        entries[k] = v;
    }
}


ObjVector* roxal::vectorVal()
{
    return newObj<ObjVector>(__func__);
}

ObjVector* roxal::vectorVal(int32_t size)
{
    auto v = newObj<ObjVector>(__func__, size);
    return v;
}

ObjVector* roxal::vectorVal(const Eigen::VectorXd& values)
{
    auto v = newObj<ObjVector>(__func__, values);
    return v;
}

ObjVector* roxal::cloneVector(const ObjVector* v)
{
    auto newv = newObj<ObjVector>(__func__, v->vec.size());
    newv->vec = v->vec;
    return newv;
}

void ObjVector::set(const ObjVector* other)
{
    vec = other->vec;
}


std::string roxal::objVectorToString(const ObjVector* ov)
{
    std::ostringstream os;
    os << "[";
    for(int i=0; i<ov->vec.size(); ++i) {
        os << ov->vec[i];
        if (i != ov->vec.size()-1)
            os << ' ';
    }
    os << "]";
    return os.str();
}

bool ObjVector::equals(const ObjVector* other, double eps) const
{
    if (other == nullptr)
        return false;

    // Check if dimensions match
    if (vec.size() != other->vec.size())
        return false;

    // Use Eigen's isApprox for element-wise comparison with tolerance
    return vec.isApprox(other->vec, eps);
}

void ObjVector::write(std::ostream& out, roxal::ptr<SerializationContext> ctx) const
{
    uint32_t len = vec.size();
    out.write(reinterpret_cast<char*>(&len), 4);
    for(uint32_t i=0;i<len;i++) {
        double d = vec[i];
        out.write(reinterpret_cast<char*>(&d), 8);
    }
}

void ObjVector::read(std::istream& in, roxal::ptr<SerializationContext> ctx)
{
    uint32_t len;
    in.read(reinterpret_cast<char*>(&len), 4);
    vec.resize(len);
    for(uint32_t i=0;i<len;i++) {
        double d; in.read(reinterpret_cast<char*>(&d), 8);
        vec[i] = d;
    }
}


ObjMatrix::ObjMatrix(const Eigen::MatrixXd& values)
    : mat(values)
{
    type = ObjType::Matrix;
}

ObjMatrix::ObjMatrix(int32_t rows, int32_t cols)
    : mat(rows, cols)
{
    type = ObjType::Matrix;
    mat.setZero();
}

ObjMatrix* roxal::matrixVal()
{
    return newObj<ObjMatrix>(__func__);
}

ObjMatrix* roxal::matrixVal(int32_t rows, int32_t cols)
{
    auto m = newObj<ObjMatrix>(__func__, rows, cols);
    return m;
}

ObjMatrix* roxal::matrixVal(const Eigen::MatrixXd& values)
{
    auto m = newObj<ObjMatrix>(__func__, values);
    return m;
}

static ObjVector* valueToVector(const Value& v)
{
    if (isVector(v))
        return asVector(v);
    std::vector<Value> args{v};
    Value conv = construct(ValueType::Vector, args.begin(), args.end());
    return asVector(conv);
}

static ObjMatrix* valueToMatrix(const Value& v)
{
    if (isMatrix(v))
        return asMatrix(v);
    std::vector<Value> args{v};
    Value conv = construct(ValueType::Matrix, args.begin(), args.end());
    return asMatrix(conv);
}

ObjMatrix* roxal::cloneMatrix(const ObjMatrix* m)
{
    auto newm = newObj<ObjMatrix>(__func__, m->mat.rows(), m->mat.cols());
    newm->mat = m->mat;
    return newm;
}

void ObjMatrix::set(const ObjMatrix* other)
{
    mat = other->mat;
}
void ObjPrimitive::write(std::ostream& out, roxal::ptr<SerializationContext> ctx) const
{
    ValueType vt = valueType();
    uint8_t t = static_cast<uint8_t>(vt);
    out.write(reinterpret_cast<char*>(&t), 1);
    switch(vt) {
        case ValueType::Bool: {
            uint8_t b = as.boolean ? 1 : 0;
            out.write(reinterpret_cast<char*>(&b), 1);
            break; }
        case ValueType::Int: {
            int32_t i = as.integer;
            out.write(reinterpret_cast<char*>(&i), 4);
            break; }
        case ValueType::Real: {
            double d = as.real;
            out.write(reinterpret_cast<char*>(&d), 8);
            break; }
        case ValueType::Type: {
            uint8_t bt = static_cast<uint8_t>(as.btype);
            out.write(reinterpret_cast<char*>(&bt),1);
            break; }
        default:
            throw std::runtime_error("ObjPrimitive serialization unsupported type" );
    }
}

void ObjPrimitive::read(std::istream& in, roxal::ptr<SerializationContext> ctx)
{
    uint8_t t; in.read(reinterpret_cast<char*>(&t),1);
    ValueType vt = static_cast<ValueType>(t);

    switch(vt) {
        case ValueType::Bool: {
            type = ObjType::Bool;
            uint8_t b; in.read(reinterpret_cast<char*>(&b),1);
            as.boolean = b!=0;
            break; }
        case ValueType::Int: {
            type = ObjType::Int;
            int32_t i; in.read(reinterpret_cast<char*>(&i),4);
            as.integer = i;
            break; }
        case ValueType::Real: {
            type = ObjType::Real;
            double d; in.read(reinterpret_cast<char*>(&d),8);
            as.real = d;
            break; }
        case ValueType::Type: {
            type = ObjType::Type;
            uint8_t bt; in.read(reinterpret_cast<char*>(&bt),1);
            as.btype = static_cast<ValueType>(bt);
            break; }
        default:
            throw std::runtime_error("ObjPrimitive deserialization unsupported type" );
    }
}

// Default serialization stubs for unsupported object types
void ObjSignal::write(std::ostream& out, roxal::ptr<SerializationContext> ctx) const
{
    uint8_t tag = static_cast<uint8_t>(ObjType::Signal);
    out.write(reinterpret_cast<char*>(&tag),1);
    std::string n = signal ? signal->name() : "";
    uint32_t len = n.size();
    out.write(reinterpret_cast<char*>(&len),4);
    out.write(n.data(), len);
    double freq = signal ? signal->frequency() : 0.0;
    out.write(reinterpret_cast<char*>(&freq),8);
    Value val = signal ? signal->lastValue() : nilVal();
    writeValue(out, val, ctx);
}

void ObjSignal::read(std::istream& in, roxal::ptr<SerializationContext> ctx)
{
    uint8_t tag; in.read(reinterpret_cast<char*>(&tag),1);
    if(tag != static_cast<uint8_t>(ObjType::Signal))
        throw std::runtime_error("ObjSignal::read mismatched tag");
    uint32_t len; in.read(reinterpret_cast<char*>(&len),4);
    std::string n(len,'\0'); if(len) in.read(n.data(), len);
    double freq; in.read(reinterpret_cast<char*>(&freq),8);
    Value v = readValue(in, ctx);
    signal = df::Signal::newSignal(freq, v, n);
    changeEvent = nilVal();
    type = ObjType::Signal;
}

void ObjEvent::write(std::ostream& out, roxal::ptr<SerializationContext> ctx) const
{
    uint8_t tag = static_cast<uint8_t>(ObjType::Event);
    out.write(reinterpret_cast<char*>(&tag),1);
    uint32_t count = subscribers.size();
    out.write(reinterpret_cast<char*>(&count),4);
    for(const auto& s : subscribers)
        writeValue(out, s, ctx);
}

void ObjEvent::read(std::istream& in, roxal::ptr<SerializationContext> ctx)
{
    uint8_t tag; in.read(reinterpret_cast<char*>(&tag),1);
    if(tag != static_cast<uint8_t>(ObjType::Event))
        throw std::runtime_error("ObjEvent::read mismatched tag");
    uint32_t count; in.read(reinterpret_cast<char*>(&count),4);
    subscribers.clear();
    for(uint32_t i=0;i<count;i++)
        subscribers.push_back(readValue(in, ctx));
    type = ObjType::Event;
}

void ObjLibrary::write(std::ostream& out, roxal::ptr<SerializationContext> ctx) const
{
    uint8_t tag = static_cast<uint8_t>(ObjType::Library);
    out.write(reinterpret_cast<char*>(&tag),1);
    uint8_t h = 0; out.write(reinterpret_cast<char*>(&h),1);
}

void ObjLibrary::read(std::istream& in, roxal::ptr<SerializationContext> ctx)
{
    uint8_t tag; in.read(reinterpret_cast<char*>(&tag),1);
    if(tag != static_cast<uint8_t>(ObjType::Library))
        throw std::runtime_error("ObjLibrary::read mismatched tag");
    uint8_t h; in.read(reinterpret_cast<char*>(&h),1);
    handle = nullptr;
    type = ObjType::Library;
}
void ObjForeignPtr::write(std::ostream&, roxal::ptr<SerializationContext>) const { throw std::runtime_error("Cannot serialize foreign pointers"); }
void ObjForeignPtr::read(std::istream&, roxal::ptr<SerializationContext>) { throw std::runtime_error("Cannot deserialize foreign pointers"); }
void ObjException::write(std::ostream& out, roxal::ptr<SerializationContext> ctx) const
{
    uint8_t tag = static_cast<uint8_t>(ObjType::Exception);
    out.write(reinterpret_cast<char*>(&tag),1);
    writeValue(out, message, ctx);
    writeValue(out, exType, ctx);
    writeValue(out, stackTrace, ctx);
}

void ObjException::read(std::istream& in, roxal::ptr<SerializationContext> ctx)
{
    uint8_t tag; in.read(reinterpret_cast<char*>(&tag),1);
    if(tag != static_cast<uint8_t>(ObjType::Exception))
        throw std::runtime_error("ObjException::read mismatched tag");
    message = readValue(in, ctx);
    exType = readValue(in, ctx);
    stackTrace = readValue(in, ctx);
    type = ObjType::Exception;
}
void ObjFunction::write(std::ostream& out, roxal::ptr<SerializationContext> ctx) const
{
    uint8_t tag = static_cast<uint8_t>(ObjType::Function);
    out.write(reinterpret_cast<char*>(&tag), 1);

    std::string n; name.toUTF8String(n);
    uint32_t len = n.size();
    out.write(reinterpret_cast<char*>(&len), 4);
    out.write(n.data(), len);

    uint8_t hasType = funcType.has_value() ? 1 : 0;
    out.write(reinterpret_cast<char*>(&hasType), 1);
    if(hasType)
        writeTypeInfo(out, *funcType.value());

    out.write(reinterpret_cast<const char*>(&arity), 4);
    out.write(reinterpret_cast<const char*>(&upvalueCount), 4);

    chunk->serialize(out, ctx);

    uint32_t annCount = annotations.size();
    out.write(reinterpret_cast<char*>(&annCount), 4);
    for(const auto& a : annotations)
        writeAnnotation(out, *a);

    uint8_t s = strict ? 1 : 0; out.write(reinterpret_cast<char*>(&s),1);

    uint8_t ft = static_cast<uint8_t>(fnType); out.write(reinterpret_cast<char*>(&ft),1);

    writeValue(out, ownerType, ctx);

    uint8_t acc = static_cast<uint8_t>(access); out.write(reinterpret_cast<char*>(&acc),1);

    uint32_t defCount = paramDefaultFunc.size();
    out.write(reinterpret_cast<char*>(&defCount),4);
    if(defCount) {
        for(const auto& kv : paramDefaultFunc) {
            int32_t key = kv.first;
            out.write(reinterpret_cast<char*>(&key),4);
            kv.second->write(out, ctx);
        }
    }

    writeValue(out, moduleType, ctx);
}

void ObjFunction::read(std::istream& in, roxal::ptr<SerializationContext> ctx)
{
    uint8_t tag; in.read(reinterpret_cast<char*>(&tag),1);
    if(tag != static_cast<uint8_t>(ObjType::Function))
        throw std::runtime_error("ObjFunction::read mismatched tag");
    type = ObjType::Function;

    uint32_t len; in.read(reinterpret_cast<char*>(&len),4);
    std::string ns(len,'\0'); if(len) in.read(ns.data(),len);
    name = icu::UnicodeString::fromUTF8(ns);

    uint8_t hasType; in.read(reinterpret_cast<char*>(&hasType),1);
    if(hasType)
        funcType = readTypeInfo(in);
    else
        funcType.reset();

    in.read(reinterpret_cast<char*>(&arity),4);
    in.read(reinterpret_cast<char*>(&upvalueCount),4);

    chunk = std::make_shared<Chunk>(icu::UnicodeString(), icu::UnicodeString(), icu::UnicodeString());
    chunk->deserialize(in, ctx);

    uint32_t annCount; in.read(reinterpret_cast<char*>(&annCount),4);
    annotations.clear();
    for(uint32_t i=0;i<annCount;i++)
        annotations.push_back(readAnnotation(in));

    uint8_t s; in.read(reinterpret_cast<char*>(&s),1); strict = s!=0;

    uint8_t ft; in.read(reinterpret_cast<char*>(&ft),1); fnType = static_cast<FunctionType>(ft);

    ownerType = readValue(in, ctx);

    uint8_t acc; in.read(reinterpret_cast<char*>(&acc),1); access = static_cast<ast::Access>(acc);

    uint32_t defCount; in.read(reinterpret_cast<char*>(&defCount),4);
    paramDefaultFunc.clear();
    for(uint32_t i=0;i<defCount;i++) {
        int32_t key; in.read(reinterpret_cast<char*>(&key),4);
        auto func = newObj<ObjFunction>(__func__, icu::UnicodeString(), icu::UnicodeString(), icu::UnicodeString());
        func->read(in, ctx);
        paramDefaultFunc[key] = func;
    }

    moduleType = readValue(in, ctx);
}
void ObjUpvalue::write(std::ostream& out, roxal::ptr<SerializationContext> ctx) const
{
    ObjUpvalue* self = const_cast<ObjUpvalue*>(this);
    if (self->location != &self->closed) {
        self->closed = *self->location;
        self->location = &self->closed;
    }

    uint8_t tag = static_cast<uint8_t>(ObjType::Upvalue);
    out.write(reinterpret_cast<char*>(&tag),1);
    writeValue(out, self->closed, ctx);
}

void ObjUpvalue::read(std::istream& in, roxal::ptr<SerializationContext> ctx)
{
    uint8_t tag; in.read(reinterpret_cast<char*>(&tag),1);
    if(tag != static_cast<uint8_t>(ObjType::Upvalue))
        throw std::runtime_error("ObjUpvalue::read mismatched tag");
    type = ObjType::Upvalue;
    closed = readValue(in, ctx);
    location = &closed;
}

void ObjClosure::write(std::ostream& out, roxal::ptr<SerializationContext> ctx) const
{
    uint8_t tag = static_cast<uint8_t>(ObjType::Closure);
    out.write(reinterpret_cast<char*>(&tag),1);
    // Preserve object identity of the referenced function by using the same
    // serialization context.  Without this we end up creating a new
    // SerializationContext inside ObjFunction::write which breaks reference
    // tracking and can lead to infinite recursion when a closure references its
    // owning type.
    function->write(out, ctx);
    uint32_t count = upvalues.size();
    out.write(reinterpret_cast<char*>(&count),4);
    for(auto* uv : upvalues) {
        uint8_t present = uv ? 1 : 0;
        out.write(reinterpret_cast<char*>(&present),1);
        if(present)
            uv->write(out, ctx);
    }
}

void ObjClosure::read(std::istream& in, roxal::ptr<SerializationContext> ctx)
{
    uint8_t tag; in.read(reinterpret_cast<char*>(&tag),1);
    if(tag != static_cast<uint8_t>(ObjType::Closure))
        throw std::runtime_error("ObjClosure::read mismatched tag");
    type = ObjType::Closure;

    auto fn = newObj<ObjFunction>(__func__, icu::UnicodeString(), icu::UnicodeString(), icu::UnicodeString());
    // Use the same serialization context so that references from the function
    // back to this closure's owning structures are properly resolved.
    fn->read(in, ctx);
    function = fn;
    function->incRef();

    uint32_t count; in.read(reinterpret_cast<char*>(&count),4);
    upvalues.resize(count,nullptr);
    for(uint32_t i=0;i<count;i++) {
        uint8_t present; in.read(reinterpret_cast<char*>(&present),1);
        if(present) {
            auto uv = newObj<ObjUpvalue>(__func__, nullptr);
            uv->read(in, ctx);
            upvalues[i] = uv;
            uv->incRef();
        }
    }
}
void ObjFuture::write(std::ostream& out, roxal::ptr<SerializationContext> ctx) const
{
    Value resolved = future.valid() ? future.get() : nilVal();
    writeValue(out, resolved, ctx);
}

void ObjFuture::read(std::istream& in, roxal::ptr<SerializationContext> ctx)
{
    // Deserialize as resolved value and wrap back into a fulfilled future
    Value val = readValue(in, ctx);
    std::promise<Value> p;
    p.set_value(val);
    future = p.get_future().share();
}
void ObjNative::write(std::ostream&, roxal::ptr<SerializationContext>) const { throw std::runtime_error("ObjNative serialization not implemented"); }
void ObjNative::read(std::istream&, roxal::ptr<SerializationContext>) { throw std::runtime_error("ObjNative deserialization not implemented"); }
void ObjTypeSpec::write(std::ostream& out, roxal::ptr<SerializationContext> ctx) const
{
    uint8_t tv = static_cast<uint8_t>(typeValue);
    out.write(reinterpret_cast<char*>(&tv), 1);
}

void ObjTypeSpec::read(std::istream& in, roxal::ptr<SerializationContext> ctx)
{
    uint8_t tv;
    in.read(reinterpret_cast<char*>(&tv), 1);
    typeValue = static_cast<ValueType>(tv);
}
void ObjObjectType::write(std::ostream& out, roxal::ptr<SerializationContext> ctx) const
{
    ObjTypeSpec::write(out);

    std::string n; name.toUTF8String(n);
    uint32_t len = n.size();
    out.write(reinterpret_cast<char*>(&len), 4);
    out.write(n.data(), len);

    uint8_t b = isActor ? 1 : 0; out.write(reinterpret_cast<char*>(&b),1);
    b = isInterface ? 1 : 0; out.write(reinterpret_cast<char*>(&b),1);
    b = isEnumeration ? 1 : 0; out.write(reinterpret_cast<char*>(&b),1);

    writeValue(out, superType, ctx);

    b = isCStruct ? 1 : 0; out.write(reinterpret_cast<char*>(&b),1);
    out.write(reinterpret_cast<const char*>(&cstructArch),4);
    out.write(reinterpret_cast<const char*>(&enumTypeId),2);

    uint32_t pcount = propertyOrder.size();
    out.write(reinterpret_cast<char*>(&pcount),4);
    for(int32_t h : propertyOrder) {
        const auto& prop = properties.at(h);
        std::string pn; prop.name.toUTF8String(pn);
        uint32_t plen = pn.size();
        out.write(reinterpret_cast<char*>(&plen),4);
        out.write(pn.data(), plen);
        writeValue(out, prop.type, ctx);
        writeValue(out, prop.initialValue, ctx);
        uint8_t acc = static_cast<uint8_t>(prop.access);
        out.write(reinterpret_cast<char*>(&acc),1);
        uint8_t hasC = prop.ctype.has_value() ? 1 : 0;
        out.write(reinterpret_cast<char*>(&hasC),1);
        if(hasC) {
            std::string ct; prop.ctype->toUTF8String(ct);
            uint32_t ctlen = ct.size();
            out.write(reinterpret_cast<char*>(&ctlen),4);
            out.write(ct.data(), ctlen);
        }
    }

    uint32_t mcount = methods.size();
    out.write(reinterpret_cast<char*>(&mcount),4);
    for(const auto& kv : methods) {
        const auto& method = kv.second;
        std::string mn; method.name.toUTF8String(mn);
        uint32_t mlen = mn.size();
        out.write(reinterpret_cast<char*>(&mlen),4);
        out.write(mn.data(), mlen);
        writeValue(out, method.closure, ctx);
        uint8_t acc = static_cast<uint8_t>(method.access);
        out.write(reinterpret_cast<char*>(&acc),1);
    }

    uint32_t lcount = enumLabelValues.size();
    out.write(reinterpret_cast<char*>(&lcount),4);
    for(const auto& kv : enumLabelValues) {
        const auto& label = kv.second;
        std::string ln; label.first.toUTF8String(ln);
        uint32_t llen = ln.size();
        out.write(reinterpret_cast<char*>(&llen),4);
        out.write(ln.data(), llen);
        writeValue(out, label.second, ctx);
    }
}

void ObjObjectType::read(std::istream& in, roxal::ptr<SerializationContext> ctx)
{
    ObjTypeSpec::read(in);

    uint32_t len; in.read(reinterpret_cast<char*>(&len),4);
    std::string ns(len, '\0');
    if(len>0) in.read(ns.data(), len);
    name = icu::UnicodeString::fromUTF8(ns);

    uint8_t b;
    in.read(reinterpret_cast<char*>(&b),1); isActor = b!=0;
    in.read(reinterpret_cast<char*>(&b),1); isInterface = b!=0;
    in.read(reinterpret_cast<char*>(&b),1); isEnumeration = b!=0;

    superType = readValue(in, ctx);

    in.read(reinterpret_cast<char*>(&b),1); isCStruct = b!=0;
    in.read(reinterpret_cast<char*>(&cstructArch),4);
    in.read(reinterpret_cast<char*>(&enumTypeId),2);

    uint32_t pcount; in.read(reinterpret_cast<char*>(&pcount),4);
    properties.clear(); propertyOrder.clear();
    for(uint32_t i=0;i<pcount;i++) {
        uint32_t plen; in.read(reinterpret_cast<char*>(&plen),4);
        std::string pn(plen,'\0'); if(plen>0) in.read(pn.data(), plen);
        icu::UnicodeString uname = icu::UnicodeString::fromUTF8(pn);
        Value ptype = readValue(in, ctx);
        Value init  = readValue(in, ctx);
        uint8_t acc; in.read(reinterpret_cast<char*>(&acc),1);
        uint8_t hasC; in.read(reinterpret_cast<char*>(&hasC),1);
        std::optional<icu::UnicodeString> ct;
        if(hasC) {
            uint32_t ctlen; in.read(reinterpret_cast<char*>(&ctlen),4);
            std::string cts(ctlen,'\0'); if(ctlen>0) in.read(cts.data(), ctlen);
            ct = icu::UnicodeString::fromUTF8(cts);
        }
        int32_t hash = uname.hashCode();
        Property prop{uname, ptype, init, static_cast<ast::Access>(acc), nilVal(), ct};
        properties[hash] = prop;
        propertyOrder.push_back(hash);
    }

    uint32_t mcount; in.read(reinterpret_cast<char*>(&mcount),4);
    methods.clear();
    for(uint32_t i=0;i<mcount;i++) {
        uint32_t mlen; in.read(reinterpret_cast<char*>(&mlen),4);
        std::string mn(mlen,'\0'); if(mlen>0) in.read(mn.data(), mlen);
        icu::UnicodeString uname = icu::UnicodeString::fromUTF8(mn);
        Value clos = readValue(in, ctx);
        uint8_t acc; in.read(reinterpret_cast<char*>(&acc),1);
        int32_t hash = uname.hashCode();
        Method m{uname, clos, static_cast<ast::Access>(acc), nilVal()};
        methods[hash] = m;
    }

    uint32_t lcount; in.read(reinterpret_cast<char*>(&lcount),4);
    enumLabelValues.clear();
    for(uint32_t i=0;i<lcount;i++) {
        uint32_t llen; in.read(reinterpret_cast<char*>(&llen),4);
        std::string ln(llen,'\0'); if(llen>0) in.read(ln.data(), llen);
        icu::UnicodeString uname = icu::UnicodeString::fromUTF8(ln);
        Value val = readValue(in, ctx);
        int32_t hash = uname.hashCode();
        enumLabelValues[hash] = {uname, val};
    }

    if(isEnumeration) {
        enumTypes[enumTypeId] = this;
    }
}
void ObjPackageType::write(std::ostream& out, roxal::ptr<SerializationContext> ctx) const
{
    uint8_t tag = static_cast<uint8_t>(ObjType::Type);
    out.write(reinterpret_cast<char*>(&tag),1);
}

void ObjPackageType::read(std::istream& in, roxal::ptr<SerializationContext> ctx)
{
    uint8_t tag; in.read(reinterpret_cast<char*>(&tag),1);
    if(tag != static_cast<uint8_t>(ObjType::Type))
        throw std::runtime_error("ObjPackageType::read mismatched tag");
    typeValue = ValueType::Type;
}
void ObjModuleType::write(std::ostream& out, roxal::ptr<SerializationContext> ctx) const
{
    ObjTypeSpec::write(out);

    std::string n; name.toUTF8String(n);
    uint32_t len = n.size();
    out.write(reinterpret_cast<char*>(&len), 4);
    out.write(n.data(), len);
}

void ObjModuleType::read(std::istream& in, roxal::ptr<SerializationContext> ctx)
{
    ObjTypeSpec::read(in);

    uint32_t len; in.read(reinterpret_cast<char*>(&len),4);
    std::string ns(len, '\0');
    if(len>0) in.read(ns.data(), len);
    name = icu::UnicodeString::fromUTF8(ns);

    allModules.push_back(this);
}
void ObjectInstance::write(std::ostream& out, roxal::ptr<SerializationContext> ctx) const
{
    // Only serialize the object contents here.  Reference tracking is handled
    // by the calling writeValue() helper.
    writeValue(out, Value(instanceType), ctx);
    uint32_t count = properties.size();
    out.write(reinterpret_cast<char*>(&count),4);
    for(const auto& kv : properties) {
        int32_t h = kv.first;
        out.write(reinterpret_cast<char*>(&h),4);
        writeValue(out, kv.second, ctx);
    }
}

void ObjectInstance::read(std::istream& in, roxal::ptr<SerializationContext> ctx)
{
    // Reference tracking is handled by readValue().  Just read the object
    // contents here.
    Value typeVal = readValue(in, ctx);
    instanceType = asObjectType(typeVal);
    instanceType->incRef();
    uint32_t count; in.read(reinterpret_cast<char*>(&count),4);
    properties.clear();
    for(uint32_t i=0;i<count;i++) {
        int32_t h; in.read(reinterpret_cast<char*>(&h),4);
        Value v = readValue(in, ctx);
        properties[h] = v;
    }
}
void ObjBoundMethod::write(std::ostream& out, roxal::ptr<SerializationContext> ctx) const
{
    uint8_t tag = static_cast<uint8_t>(ObjType::BoundMethod);
    out.write(reinterpret_cast<char*>(&tag),1);
    writeValue(out, receiver, ctx);
    writeValue(out, objVal(method));
}

void ObjBoundMethod::read(std::istream& in, roxal::ptr<SerializationContext> ctx)
{
    uint8_t tag; in.read(reinterpret_cast<char*>(&tag),1);
    if(tag != static_cast<uint8_t>(ObjType::BoundMethod))
        throw std::runtime_error("ObjBoundMethod::read mismatched tag");
    receiver = readValue(in, ctx);
    Value mval = readValue(in, ctx);
    method = asClosure(mval);
    method->incRef();
    type = ObjType::BoundMethod;
}

void ObjBoundNative::write(std::ostream& out, roxal::ptr<SerializationContext> ctx) const
{
    uint8_t tag = static_cast<uint8_t>(ObjType::BoundNative);
    out.write(reinterpret_cast<char*>(&tag),1);
    writeValue(out, receiver, ctx);
    uint8_t p = isProc ? 1 : 0; out.write(reinterpret_cast<char*>(&p),1);
    uint32_t defc = defaultValues.size();
    out.write(reinterpret_cast<char*>(&defc),4);
    for(const auto& v : defaultValues)
        writeValue(out, v, ctx);
}

void ObjBoundNative::read(std::istream& in, roxal::ptr<SerializationContext> ctx)
{
    uint8_t tag; in.read(reinterpret_cast<char*>(&tag),1);
    if(tag != static_cast<uint8_t>(ObjType::BoundNative))
        throw std::runtime_error("ObjBoundNative::read mismatched tag");
    receiver = readValue(in, ctx);
    uint8_t p; in.read(reinterpret_cast<char*>(&p),1); isProc = p!=0;
    uint32_t defc; in.read(reinterpret_cast<char*>(&defc),4);
    defaultValues.clear();
    for(uint32_t i=0;i<defc;i++)
        defaultValues.push_back(readValue(in, ctx));
    function = nullptr;
    funcType = nullptr;
    type = ObjType::BoundNative;
}

void ActorInstance::write(std::ostream& out, roxal::ptr<SerializationContext> ctx) const
{
    // Only serialize the contents. Reference tracking is handled by writeValue().
    writeValue(out, Value(instanceType), ctx);
    uint32_t count = properties.size();
    out.write(reinterpret_cast<char*>(&count),4);
    for(const auto& kv : properties) {
        int32_t h = kv.first;
        out.write(reinterpret_cast<char*>(&h),4);
        writeValue(out, kv.second, ctx);
    }
}

void ActorInstance::read(std::istream& in, roxal::ptr<SerializationContext> ctx)
{
    // Only read the contents. Reference tracking is handled by readValue().
    Value typeVal = readValue(in, ctx);
    instanceType = asObjectType(typeVal);
    instanceType->incRef();
    uint32_t count; in.read(reinterpret_cast<char*>(&count),4);
    properties.clear();
    for(uint32_t i=0;i<count;i++) {
        int32_t h; in.read(reinterpret_cast<char*>(&h),4);
        Value v = readValue(in, ctx);
        properties[h] = v;
    }
    auto newThread = std::make_shared<Thread>();
    // Keep the thread alive by registering it with the VM. Without this the
    // Thread object would be destroyed immediately after deserialization,
    // causing std::terminate since the underlying std::thread is still
    // joinable.
    VM::instance().registerThread(newThread);
    thread = newThread;
    newThread->act(objVal(this));
}

void ObjMatrix::write(std::ostream& out, roxal::ptr<SerializationContext> ctx) const
{
    uint32_t rows = mat.rows();
    uint32_t cols = mat.cols();
    out.write(reinterpret_cast<char*>(&rows), 4);
    out.write(reinterpret_cast<char*>(&cols), 4);
    for(uint32_t r=0;r<rows;r++)
        for(uint32_t c=0;c<cols;c++) {
            double d = mat(r,c);
            out.write(reinterpret_cast<char*>(&d), 8);
        }
}

void ObjMatrix::read(std::istream& in, roxal::ptr<SerializationContext> ctx)
{
    uint32_t rows, cols;
    in.read(reinterpret_cast<char*>(&rows), 4);
    in.read(reinterpret_cast<char*>(&cols), 4);
    mat.resize(rows, cols);
    for(uint32_t r=0;r<rows;r++)
        for(uint32_t c=0;c<cols;c++) {
            double d; in.read(reinterpret_cast<char*>(&d), 8);
            mat(r,c) = d;
        }
}

ObjSignal::ObjSignal(ptr<df::Signal> s)
    : signal(s), changeEvent(nilVal())
{
    type = ObjType::Signal;
}

ObjEvent* ObjSignal::ensureChangeEvent()
{
    if (!changeEvent.isNil())
        return asEvent(changeEvent);

    changeEvent = objVal(eventVal());
    Value eventWeak = changeEvent.weakRef();
    signal->addValueChangedCallback([eventWeak](TimePoint t, ptr<df::Signal>, const Value&){
        if (!eventWeak.isAlive())
            return;
        ObjEvent* ev = asEvent(eventWeak);
        scheduleEventHandlers(eventWeak, ev, t);
    });
    return asEvent(changeEvent);
}

ObjSignal* roxal::signalVal(ptr<df::Signal> s)
{
    return newObj<ObjSignal>(__func__, s);
}

std::string roxal::objSignalToString(const ObjSignal* os)
{
    if (!os || !os->signal)
        return "<signal nil>";
    try {
        return toString(os->signal->lastValue());
    } catch (...) {
        return "<signal>";
    }
}

ObjEvent* roxal::eventVal()
{
    return newObj<ObjEvent>(__func__);
}

std::string roxal::objEventToString(const ObjEvent* ev)
{
    (void)ev;
    return std::string("<event>");
}



ObjLibrary::~ObjLibrary()
{
    if (handle)
        dlclose(handle);
}

ObjLibrary* roxal::libraryVal(void* handle)
{
    return newObj<ObjLibrary>(__func__, handle);
}

std::string roxal::objLibraryToString(const ObjLibrary* lib)
{
    std::ostringstream oss;
    oss << "<library " << lib->handle << ">";
    return oss.str();
}

ObjForeignPtr* roxal::foreignPtrVal(void* ptr)
{
    return newObj<ObjForeignPtr>(__func__, ptr);
}

std::string roxal::objForeignPtrToString(const ObjForeignPtr* fp)
{
    std::ostringstream oss;
    oss << "<ptr " << fp->ptr << ">";
    return oss.str();
}

ObjException* roxal::exceptionVal(Value message, Value exType, Value stackTrace)
{
    return newObj<ObjException>(__func__, message, exType, stackTrace);
}

std::string roxal::objExceptionToString(const ObjException* ex)
{
    if (!ex) return "<exception>";
    if (!ex->message.isNil())
        return std::string("<exception ") + toString(ex->message) + ">";
    return std::string("<exception>");
}

std::string roxal::stackTraceToString(Value frames)
{
    if (frames.isNil() || !isList(frames)) return "";
    ObjList* listObj = asList(frames);
    std::ostringstream oss;
    auto list = listObj->elts.get();
    for(const auto& v : list) {
        if (!isDict(v)) continue;
        ObjDict* d = asDict(v);
        Value funcVal = d->at(objVal(stringVal(UnicodeString("function"))));
        Value lineVal = d->at(objVal(stringVal(UnicodeString("line"))));
        Value colVal  = d->at(objVal(stringVal(UnicodeString("col"))));
        Value fileVal = d->at(objVal(stringVal(UnicodeString("filename"))));

        UnicodeString funcName = isString(funcVal) ? asString(funcVal)->s : UnicodeString("<script>");
        int line = lineVal.isNumber() ? lineVal.asInt() : -1;
        int col  = colVal.isNumber() ? colVal.asInt() : -1;
        std::string fname = isString(fileVal) ? toUTF8StdString(asString(fileVal)->s) : "";

        if (!fname.empty())
            oss << fname << ":" << line << ":" << col << ": in " << toUTF8StdString(funcName) << "\n";
        else
            oss << "[line " << line << ":" << col << "]: in " << toUTF8StdString(funcName) << "\n";
    }
    return oss.str();
}

std::string roxal::objExceptionStackTraceToString(const ObjException* ex)
{
    if (!ex)
        return "";
    return stackTraceToString(ex->stackTrace);
}

Value ObjMatrix::index(const Value& row) const
{
    if (row.isNumber()) {
        int r = row.asInt();
        if (r < 0 || r >= rows())
            throw std::invalid_argument("Matrix row index out-of-range.");
        Eigen::VectorXd vals = mat.row(r);
        return objVal(vectorVal(vals));
    } else if (isRange(row)) {
        ObjRange* rr = asRange(row);
        int rowCount = rr->length(rows());
        Eigen::MatrixXd m(rowCount, cols());
        for(int i=0;i<rowCount;++i) {
            int target = rr->targetIndex(i, rows());
            if (target >=0 && target < rows())
                m.row(i) = mat.row(target);
        }
        return objVal(matrixVal(m));
    }
    throw std::invalid_argument("Matrix indexing subscript must be a number or a range.");
    return nilVal();
}

Value ObjMatrix::index(const Value& row, const Value& col) const
{
    bool rowRange = isRange(row);
    bool colRange = isRange(col);

    if (row.isNumber() && col.isNumber()) {
        int r = row.asInt();
        int c = col.asInt();
        if (r < 0 || r >= rows() || c < 0 || c >= cols())
            throw std::invalid_argument("Matrix index out-of-range.");
        return realVal(mat(r,c));
    }

    std::vector<int> rowIdx;
    if (row.isNumber()) {
        int r = row.asInt();
        if (r < 0 || r >= rows())
            throw std::invalid_argument("Matrix row index out-of-range.");
        rowIdx.push_back(r);
    } else if (rowRange) {
        ObjRange* rr = asRange(row);
        int rowCount = rr->length(rows());
        rowIdx.reserve(rowCount);
        for(int i=0;i<rowCount;++i) {
            int target = rr->targetIndex(i, rows());
            if (target >=0 && target < rows())
                rowIdx.push_back(target);
        }
    } else {
        throw std::invalid_argument("Matrix row index must be a number or a range.");
    }

    std::vector<int> colIdx;
    if (col.isNumber()) {
        int c = col.asInt();
        if (c < 0 || c >= cols())
            throw std::invalid_argument("Matrix column index out-of-range.");
        colIdx.push_back(c);
    } else if (colRange) {
        ObjRange* cr = asRange(col);
        int colCount = cr->length(cols());
        colIdx.reserve(colCount);
        for(int i=0;i<colCount;++i) {
            int target = cr->targetIndex(i, cols());
            if (target >=0 && target < cols())
                colIdx.push_back(target);
        }
    } else {
        throw std::invalid_argument("Matrix column index must be a number or a range.");
    }

    if (rowIdx.size()==1 && colIdx.size()==1) {
        return realVal(mat(rowIdx[0], colIdx[0]));
    } else if (rowIdx.size()==1 && colIdx.size()>1) {
        Eigen::VectorXd vals(colIdx.size());
        for(size_t j=0;j<colIdx.size();++j)
            vals[j] = mat(rowIdx[0], colIdx[j]);
        return objVal(vectorVal(vals));
    } else if (rowIdx.size()>1 && colIdx.size()==1) {
        Eigen::VectorXd vals(rowIdx.size());
        for(size_t i=0;i<rowIdx.size();++i)
            vals[i] = mat(rowIdx[i], colIdx[0]);
        return objVal(vectorVal(vals));
    } else {
        Eigen::MatrixXd sub(rowIdx.size(), colIdx.size());
        for(size_t i=0;i<rowIdx.size();++i)
            for(size_t j=0;j<colIdx.size();++j)
                sub(i,j) = mat(rowIdx[i], colIdx[j]);
        return objVal(matrixVal(sub));
    }
    return nilVal();
}

void ObjMatrix::setIndex(const Value& row, const Value& value)
{
    std::vector<int> rowIdx;
    if (row.isNumber()) {
        int r = row.asInt();
        if (r < 0 || r >= rows())
            throw std::invalid_argument("Matrix row index out-of-range.");
        rowIdx.push_back(r);
    } else if (isRange(row)) {
        ObjRange* rr = asRange(row);
        int rowCount = rr->length(rows());
        rowIdx.reserve(rowCount);
        for(int i=0;i<rowCount;++i) {
            int target = rr->targetIndex(i, rows());
            if (target >=0 && target < rows())
                rowIdx.push_back(target);
        }
    } else {
        throw std::invalid_argument("Matrix row index must be a number or a range.");
    }

    if (rowIdx.size()==1) {
        ObjVector* vec = valueToVector(value);
        if (vec->length() != cols())
            throw std::invalid_argument("Assignment to matrix row requires vector length " + std::to_string(cols()));
        for(int c=0;c<cols();++c)
            mat(rowIdx[0], c) = vec->vec[c];
        return;
    }

    ObjMatrix* rhs = valueToMatrix(value);
    if (rhs->cols() != cols() || rhs->rows() != (int)rowIdx.size())
        throw std::invalid_argument("Assignment to matrix rows requires a matrix of size ("+std::to_string(rowIdx.size())+","+std::to_string(cols())+")");

    for(size_t i=0;i<rowIdx.size();++i)
        mat.row(rowIdx[i]) = rhs->mat.row(i);
}

void ObjMatrix::setIndex(const Value& row, const Value& col, const Value& value)
{
    bool rowRange = isRange(row);
    bool colRange = isRange(col);

    std::vector<int> rowIdx;
    if (row.isNumber()) {
        int r = row.asInt();
        if (r < 0 || r >= rows())
            throw std::invalid_argument("Matrix row index out-of-range.");
        rowIdx.push_back(r);
    } else if (rowRange) {
        ObjRange* rr = asRange(row);
        int rowCount = rr->length(rows());
        rowIdx.reserve(rowCount);
        for(int i=0;i<rowCount;++i) {
            int target = rr->targetIndex(i, rows());
            if (target >=0 && target < rows())
                rowIdx.push_back(target);
        }
    } else {
        throw std::invalid_argument("Matrix row index must be a number or a range.");
    }

    std::vector<int> colIdx;
    if (col.isNumber()) {
        int c = col.asInt();
        if (c < 0 || c >= cols())
            throw std::invalid_argument("Matrix column index out-of-range.");
        colIdx.push_back(c);
    } else if (colRange) {
        ObjRange* cr = asRange(col);
        int colCount = cr->length(cols());
        colIdx.reserve(colCount);
        for(int i=0;i<colCount;++i) {
            int target = cr->targetIndex(i, cols());
            if (target >=0 && target < cols())
                colIdx.push_back(target);
        }
    } else {
        throw std::invalid_argument("Matrix column index must be a number or a range.");
    }

    if (rowIdx.size()==1 && colIdx.size()==1) {
        double scalar = toType(ValueType::Real, value, false).asReal();
        mat(rowIdx[0], colIdx[0]) = scalar;
        return;
    }

    if (rowIdx.size()==1) {
        ObjVector* vec = valueToVector(value);
        if (vec->length() != (int)colIdx.size())
            throw std::invalid_argument("Assignment to matrix subrow requires vector length " + std::to_string(colIdx.size()));
        for(size_t j=0;j<colIdx.size();++j)
            mat(rowIdx[0], colIdx[j]) = vec->vec[j];
        return;
    } else if (colIdx.size()==1) {
        ObjVector* vec = valueToVector(value);
        if (vec->length() != (int)rowIdx.size())
            throw std::invalid_argument("Assignment to matrix subcolumn requires vector length " + std::to_string(rowIdx.size()));
        for(size_t i=0;i<rowIdx.size();++i)
            mat(rowIdx[i], colIdx[0]) = vec->vec[i];
        return;
    } else {
        ObjMatrix* rhs = valueToMatrix(value);
        if (rhs->rows()!= (int)rowIdx.size() || rhs->cols()!= (int)colIdx.size())
            throw std::invalid_argument("Assignment to matrix submatrix requires matrix of size ("+std::to_string(rowIdx.size())+","+std::to_string(colIdx.size())+")");
        for(size_t i=0;i<rowIdx.size();++i)
            for(size_t j=0;j<colIdx.size();++j)
                mat(rowIdx[i], colIdx[j]) = rhs->mat(i,j);
    }
}

bool ObjMatrix::equals(const ObjMatrix* other, double eps) const
{
    if (other == nullptr)
        return false;

    // Check if dimensions match
    if (mat.rows() != other->mat.rows() || mat.cols() != other->mat.cols())
        return false;

    // Use Eigen's isApprox for element-wise comparison with tolerance
    return mat.isApprox(other->mat, eps);
}

std::string roxal::objMatrixToString(const ObjMatrix* om)
{
    using std::min;

    const int rows = om->mat.rows();
    const int cols = om->mat.cols();

    const int firstRows = min(rows, 16);
    const int lastRows  = rows > 32 ? 16 : (rows - firstRows);
    const int firstCols = min(cols, 16);
    const int lastCols  = cols > 32 ? 16 : (cols - firstCols);

    std::vector<size_t> colWidthFirst(firstCols, 0);
    std::vector<size_t> colWidthLast(lastCols, 0);

    auto updateWidths = [&](int r) {
        for(int c=0; c<firstCols; ++c) {
            std::string s = format("%g", om->mat(r,c));
            colWidthFirst[c] = std::max(colWidthFirst[c], s.size());
        }
        for(int c=0; c<lastCols; ++c) {
            std::string s = format("%g", om->mat(r, cols-lastCols+c));
            colWidthLast[c] = std::max(colWidthLast[c], s.size());
        }
    };

    for(int r=0; r<rows; ++r)
        updateWidths(r);

    std::ostringstream os;
    os << "[";

    auto outputRow = [&](int r) {
        if(r > 0)
            os << "\n ";
        for(int c=0; c<firstCols; ++c) {
            std::string s = format("%g", om->mat(r,c));
            os << std::left << std::setw(colWidthFirst[c]) << s;
            if(c != firstCols-1 || cols > firstCols)
                os << ' ';
        }
        if(cols > 32)
            os << "... ";
        for(int c=0; c<lastCols; ++c) {
            std::string s = format("%g", om->mat(r, cols-lastCols+c));
            os << std::left << std::setw(colWidthLast[c]) << s;
            if(c != lastCols-1)
                os << ' ';
        }
    };

    for(int r=0; r<firstRows; ++r)
        outputRow(r);

    if(rows > 32) {
        os << "\n ...\n ";
        for(int r=rows-lastRows; r<rows; ++r)
            outputRow(r);
    }

    os << "]";
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
        case ObjType::Vector: {
            return objVectorToString(asVector(v));
        }
        case ObjType::Matrix: {
            return objMatrixToString(asMatrix(v));
        }
        case ObjType::Signal: {
            return objSignalToString(asSignal(v));
        }
        case ObjType::Event: {
            return objEventToString(asEvent(v));
        }
        case ObjType::Library: {
            return objLibraryToString(asLibrary(v));
        }
        case ObjType::ForeignPtr: {
            return objForeignPtrToString(asForeignPtr(v));
        }
        case ObjType::Exception: {
            return objExceptionToString(asException(v));
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
        case ObjType::BoundNative: {
            return std::string("<native method>");
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



ObjFunction::ObjFunction(const icu::UnicodeString& packageName, const icu::UnicodeString& moduleName,
                         const icu::UnicodeString& sourceName)
    : arity(0), upvalueCount(0), name(), strict(false), ownerType(nilVal())
{
    type = ObjType::Function;
    chunk = std::make_shared<Chunk>(packageName, moduleName, sourceName);
}


ObjFunction::~ObjFunction()
{
    for (auto& entry : paramDefaultFunc) {
        if (entry.second != nullptr)
            entry.second->decRef();
    }
    paramDefaultFunc.clear();
    if (nativeSpec) {
        delete static_cast<FFIWrapper*>(nativeSpec);
        nativeSpec = nullptr;
    }
}






ObjNative::ObjNative(NativeFn _function, void* _data,
                     ptr<roxal::type::Type> _funcType,
                     std::vector<Value> defaults)
    : function(_function), data(_data), funcType(_funcType), defaultValues(std::move(defaults))
{
    type = ObjType::Native;
}


ObjNative* roxal::nativeVal(NativeFn function, void* data,
                           ptr<roxal::type::Type> funcType,
                           std::vector<Value> defaults)
{
    return newObj<ObjNative>(__func__,function,data,funcType,std::move(defaults));
}




std::unordered_map<uint16_t, roxal::ObjObjectType*> ObjObjectType::enumTypes {};

ObjObjectType::ObjObjectType(const icu::UnicodeString& typeName, bool isactor, bool isinterface, bool isenumeration)
    : name(typeName), isActor(isactor), isInterface(isinterface), isEnumeration(isenumeration), superType(nilVal())
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
    allModules.push_back(this);
}

ObjModuleType* roxal::moduleTypeVal(const icu::UnicodeString& typeName)
{
    return newObj<ObjModuleType>(__func__, typeName);
}

ObjModuleType::~ObjModuleType()
{
    allModules.erase(this);
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

    // initialize instance properties from actor type definition
    for(const auto& property : objectType->properties) {
        const auto& prop { property.second };
        auto propInitialvalue { prop.initialValue };
        properties[prop.name.hashCode()] = propInitialvalue;
    }
}

ActorInstance::~ActorInstance()
{
    instanceType->decRef();
    if (auto t = thread.lock()) {
        t->join(this);
    }
}


Value ActorInstance::queueCall(const Value& callee, const CallSpec& callSpec, Value* argsStackTop)
{
    // queue producer for consumer Thread::act()
    #ifdef DEBUG_BUILD
    assert(isBoundMethod(callee) || isBoundNative(callee));
    Value recv = isBoundMethod(callee) ? asBoundMethod(callee)->receiver : asBoundNative(callee)->receiver;
    assert(isActorInstance(recv));
    #endif

    std::lock_guard<std::mutex> lock { queueMutex };

    // TODO: arrange for push to move to queue to avoid copy
    MethodCallInfo callInfo {};
    callInfo.callee = callee;
    callInfo.callSpec = callSpec;
    for(auto i=0; i<callSpec.argCount; i++) {
        Value arg = *(argsStackTop - i - 1);
        if (!arg.isPrimitive())
            arg = arg.clone();
        callInfo.args.push_back(arg);
    }
    callInfo.returnPromise = nullptr;

    if (isBoundMethod(callee)) {
        auto funcObj = asBoundMethod(callee)->method->function;
        if (funcObj->funcType.has_value()) {
            ptr<roxal::type::Type> funcType { funcObj->funcType.value() };
            assert(funcType->func.has_value());
            if (!funcType->func.value().isProc) {
                callInfo.returnPromise = std::make_shared<std::promise<Value>>();
            }
        }
    }
    else if (isBoundNative(callee)) {
        // For builtin methods, check if it's a proc or func
        auto bound = asBoundNative(callee);

        // Only create a return promise if it's NOT a proc (i.e., it's a func)
        if (!bound->isProc) {
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
    case ObjType::BoundNative: return "function";
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
    case ObjType::Dict: return "dict";
    case ObjType::Vector: return "vector";
    case ObjType::Matrix: return "matrix";
    case ObjType::Signal: return "signal";
    case ObjType::Event: return "event";
    case ObjType::Library: return "library";
    case ObjType::ForeignPtr: return "foreignptr";
    case ObjType::Exception: return "exception";
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
