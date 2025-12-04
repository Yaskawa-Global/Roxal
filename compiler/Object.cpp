#include <stdexcept>
#include <cassert>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <algorithm>
#include <limits>
#include <iomanip>
#include <iostream>
#include <dlfcn.h>
#include <future>
#include <vector>
#include <utility>
#include <core/AST.h>

#include <core/types.h>
#include "VM.h"
#include "FFI.h"
#include "Value.h"
#include "Thread.h"
#include "dataflow/Signal.h"
#include "dataflow/DataflowEngine.h"
#include "Object.h"
#include "ModuleSys.h"

using namespace roxal;


atomic_vector<Obj*> Obj::unrefedObjs {};

void Obj::decRef()
{
    auto prevCount = control->strong.fetch_sub(1, std::memory_order_relaxed);
    if (prevCount <= 1) {
        if (!control->collecting.exchange(true, std::memory_order_relaxed)) {
            if (SimpleMarkSweepGC::instance().isCollectionInProgress()) {
                dropReferences();
            }
            control->obj = nullptr;
            unrefedObjs.push_back(this);
            SimpleMarkSweepGC::instance().notifyCleanupPending();
        }
    }
}



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
    auto t = make_ptr<type::Type>(static_cast<type::BuiltinType>(b));
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
    if(auto s = dynamic_ptr_cast<Str>(expr)){
        uint8_t tag=0; out.write(reinterpret_cast<char*>(&tag),1);
        writeString(out, s->str);
    } else if(auto n = dynamic_ptr_cast<Num>(expr)){
        uint8_t tag=1; out.write(reinterpret_cast<char*>(&tag),1);
        if(std::holds_alternative<int32_t>(n->num)){
            uint8_t ty=0; out.write(reinterpret_cast<char*>(&ty),1);
            int32_t v=std::get<int32_t>(n->num); out.write(reinterpret_cast<char*>(&v),4);
        } else {
            uint8_t ty=1; out.write(reinterpret_cast<char*>(&ty),1);
            double v=std::get<double>(n->num); out.write(reinterpret_cast<char*>(&v),8);
        }
    } else if(auto b = dynamic_ptr_cast<Bool>(expr)){
        uint8_t tag=2; out.write(reinterpret_cast<char*>(&tag),1);
        uint8_t v=b->value?1:0; out.write(reinterpret_cast<char*>(&v),1);
    } else if(auto v = dynamic_ptr_cast<Variable>(expr)){
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
            auto s = make_ptr<Str>();
            s->str = readString(in);
            return s;
        }
        case 1: {
            auto n = make_ptr<Num>();
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
            auto b = make_ptr<Bool>();
            uint8_t v; in.read(reinterpret_cast<char*>(&v),1);
            b->value = v!=0; return b;
        }
        case 3: {
            auto v = make_ptr<Variable>();
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
    auto a = make_ptr<ast::Annotation>();
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
atomic_map<Obj*, std::string> Obj::allocatedObjs {};
#endif

atomic_vector<Value> ObjModuleType::allModules {};


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
        case ObjType::File: return ValueType::Object;
        case ObjType::EventType: return ValueType::Event;
        case ObjType::EventInstance: return ValueType::Object;
        case ObjType::Function: return ValueType::Function;
        case ObjType::Closure: return ValueType::Closure;
        case ObjType::Upvalue: return ValueType::Upvalue;
        case ObjType::Exception: return ValueType::Object;
        case ObjType::Instance: {
            debug_assert_msg(dynamic_cast<const ObjectInstance*>(this) != nullptr || dynamic_cast<const ActorInstance*>(this) != nullptr,
                             "Obj::valueType() Instance called on non-instance object");
            auto actorInst = dynamic_cast<const ActorInstance*>(this);
            if (actorInst) return ValueType::Actor;
            return ValueType::Object;
        }
        case ObjType::Actor: return ValueType::Actor;
        default: return ValueType::Nil;
    }
}

void Obj::dropReferences()
{
    // Default implementation does nothing.
}







// interned strings table
static atomic_unordered_map<int32_t, ObjString*> strings {};

void roxal::visitInternedStrings(const std::function<void(ObjString*)>& fn)
{
    strings.unsafeApply([&fn](const auto& interned) {
        for (const auto& entry : interned) {
            if (entry.second) {
                fn(entry.second);
            }
        }
    });
}

void roxal::purgeDeadInternedStrings()
{
    std::vector<int32_t> deadHashes;
    strings.unsafeApply([&deadHashes](const auto& interned) {
        deadHashes.reserve(interned.size());
        for (const auto& entry : interned) {
            ObjString* str = entry.second;
            if (!str) {
                deadHashes.push_back(entry.first);
                continue;
            }
            ObjControl* control = str->control;
            if (!control || control->obj == nullptr) {
                deadHashes.push_back(entry.first);
            }
        }
    });

    for (int32_t hash : deadHashes) {
        strings.erase(hash);
    }
}

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
        return Value::stringVal(substr);
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

        return Value::stringVal(substr);
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


unique_ptr<ObjString, UnreleasedObj> roxal::newObjString(const UnicodeString& s)
{
    int32_t hash = s.hashCode();
    auto objStr = strings.lookup(hash);
    if (!objStr.has_value()) { // not found

        // create new
        #ifdef DEBUG_BUILD
        return newObj<ObjString>(std::string(__func__)+" '" + toUTF8StdString(s) + "'",__FILE__,__LINE__,s);
        #else
        return newObj<ObjString>(s);
        #endif
    }
    else { // found existing string
        return unique_ptr<ObjString, UnreleasedObj>(objStr.value());
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
    : start(Value::intVal(0)), stop(Value::intVal(0)), step(Value::intVal(1)), closed(false)
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

void ObjRange::trace(ValueVisitor& visitor) const
{
    visitor.visit(start);
    visitor.visit(stop);
    visitor.visit(step);
}

void ObjRange::dropReferences()
{
    start = Value::nilVal();
    stop = Value::nilVal();
    step = Value::nilVal();
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
        return Value::realVal(vec[index]);
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
        return Value::vectorVal(vals);
    }
    else
        throw std::invalid_argument("Vector indexing subscript must be a number or a range.");
    return Value::nilVal();
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


unique_ptr<ObjRange, UnreleasedObj> roxal::newRangeObj()
{
    #ifdef DEBUG_BUILD
    return newObj<ObjRange>(__func__,__FILE__,__LINE__);
    #else
    return newObj<ObjRange>();
    #endif
}


unique_ptr<ObjRange, UnreleasedObj> roxal::newRangeObj(const Value& start, const Value& stop, const Value& step, bool closed)
{
    #ifdef DEBUG_BUILD
    return newObj<ObjRange>(__func__,__FILE__,__LINE__,start,stop,step,closed);
    #else
    return newObj<ObjRange>(start,stop,step,closed);
    #endif
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

unique_ptr<Obj, UnreleasedObj> ObjRange::clone() const
{
    return newRangeObj(start.clone(), stop.clone(), step.clone(), closed);
}




// runtime types

unique_ptr<ObjTypeSpec, UnreleasedObj> roxal::newTypeSpecObj(ValueType t)
{
    #ifdef DEBUG_BUILD
    assert(t != ValueType::Object && t != ValueType::Actor);
    auto ts = newObj<ObjTypeSpec>(__func__, __FILE__, __LINE__);
    #else
    auto ts = newObj<ObjTypeSpec>();
    #endif
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
        elts.push_back(Value::intVal(r->targetIndex(i,-1)));
}


Value ObjList::index(const Value& i) const
{
    if (i.isNumber()) {
        auto index = i.asInt();
        auto len = length();
        if (index < 0)
            index = len - (-index);
        if (index < 0 || index >= len)
            throw std::invalid_argument("List index out-of-range.");
        return elts.at(index);
    }
    else if (isRange(i)) {
        auto sublist = newListObj();
        auto r = asRange(i);
        auto listLen = length();
        auto rangeLen = r->length(listLen);

        for(auto i=0; i<rangeLen; i++) {
            auto targetIndex = r->targetIndex(i,listLen);
            if ((targetIndex >= 0) && (targetIndex < listLen))
                sublist->elts.push_back(elts.at(targetIndex));
        }

        return Value::objVal(std::move(sublist));
    }
    else
        throw std::invalid_argument("List indexing subscript must be a number or a range.");
    return Value::nilVal();
}


void ObjList::setIndex(const Value& i, const Value& v)
{
    if (i.isNumber()) {
        auto index = i.asInt();
        auto len = length();
        if (index < 0)
            index = len - (-index);
        if (index < 0 || index >= len)
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

void ObjList::trace(ValueVisitor& visitor) const
{
    elts.unsafeApply([&visitor](const auto& values) {
        for (const auto& value : values) {
            visitor.visit(value);
        }
    });
}

void ObjList::dropReferences()
{
    elts.clear();
}

void ObjList::set(const ObjList* other)
{
    elts = other->elts; // atomic_vector assignment performs copy
}


unique_ptr<ObjList, UnreleasedObj> roxal::newListObj()
{
    #ifdef DEBUG_BUILD
    return newObj<ObjList>(__func__,__FILE__,__LINE__);
    #else
    return newObj<ObjList>();
    #endif
}


unique_ptr<ObjList, UnreleasedObj> roxal::newListObj(const ObjRange* r)
{
    #ifdef DEBUG_BUILD
    return newObj<ObjList>(__func__,__FILE__,__LINE__,r);
    #else
    return newObj<ObjList>(r);
    #endif
}

unique_ptr<ObjList, UnreleasedObj> roxal::newListObj(const std::vector<Value>& elts)
{
    #ifdef DEBUG_BUILD
    auto l = newObj<ObjList>(__func__, __FILE__, __LINE__);
    #else
    auto l = newObj<ObjList>();
    #endif
    for(const auto& elt : elts)
        l->elts.push_back(elt);
    return l;
}

unique_ptr<Obj, UnreleasedObj> ObjList::clone() const
{
    auto newl = newListObj();
    auto lsize = elts.size();
    for(auto i=0; i<lsize; i++)
        newl->elts.push_back(elts.at(i).clone());
    return newl;
}

bool ObjList::equals(const ObjList* other) const
{
    if (other == nullptr)
        return false;

    auto lst1 = elts.get();
    auto lst2 = other->elts.get();

    if (lst1.size() != lst2.size())
        return false;

    for(size_t i=0;i<lst1.size();++i)
        if (!lst1[i].equals(lst2[i], false))
            return false;

    return true;
}



namespace {

struct ContainerPrintContext {
    std::unordered_set<const ObjList*> activeLists;
    std::unordered_set<const ObjDict*> activeDicts;
};

std::string objListToStringInternal(const ObjList* ol,
                                    ContainerPrintContext& context);

std::string objDictToStringInternal(const ObjDict* od,
                                    ContainerPrintContext& context);

std::string valueToPrintableString(const Value& value,
                                   ContainerPrintContext& context)
{
    if (isList(value))
        return objListToStringInternal(asList(value), context);

    if (isDict(value))
        return objDictToStringInternal(asDict(value), context);

    auto result { toString(value) };
    if (isString(value))
        result = "\"" + result + "\"";
    return result;
}

std::string objListToStringInternal(const ObjList* ol,
                                    ContainerPrintContext& context)
{
    if (!context.activeLists.insert(ol).second)
        return "[...]";

    std::ostringstream os;
    os << "[";
    auto list { ol->elts.get() };
    for (auto it = list.begin(); it != list.end(); ++it) {
        os << valueToPrintableString(*it, context);
        if (it != list.end() - 1)
            os << ", ";
    }
    os << "]";

    context.activeLists.erase(ol);
    return os.str();
}

std::string objDictToStringInternal(const ObjDict* od,
                                    ContainerPrintContext& context)
{
    if (!context.activeDicts.insert(od).second)
        return "{...}";

    std::ostringstream os;
    os << "{";
    auto keys { od->keys() };
    for (auto it = keys.begin(); it != keys.end(); ++it) {
        const auto& key { *it };
        const auto value { od->at(key) };

        os << valueToPrintableString(key, context)
           << ": "
           << valueToPrintableString(value, context);

        if (it != keys.end() - 1)
            os << ", ";
    }
    os << "}";

    context.activeDicts.erase(od);
    return os.str();
}

} // namespace

std::string roxal::objListToString(const ObjList* ol)
{
    ContainerPrintContext context;
    return objListToStringInternal(ol, context);
}





unique_ptr<ObjDict, UnreleasedObj> roxal::newDictObj()
{
    #ifdef DEBUG_BUILD
    return newObj<ObjDict>(__func__, __FILE__, __LINE__);
    #else
    return newObj<ObjDict>();
    #endif
}

unique_ptr<ObjDict, UnreleasedObj> roxal::newDictObj(const std::vector<std::pair<Value,Value>>& entries)
{
    #ifdef DEBUG_BUILD
    auto d = newObj<ObjDict>(__func__, __FILE__, __LINE__);
    #else
    auto d = newObj<ObjDict>();
    #endif
    for(const auto& entry : entries)
        d->store(entry.first, entry.second);
    return d;
}

unique_ptr<Obj, UnreleasedObj> ObjDict::clone() const
{
    auto newd = newDictObj();
    const auto dkeys = keys();
    for(const auto& dkey : dkeys)
        newd->store(dkey.clone(), at(dkey).clone());
    return newd;
}

void ObjDict::set(const ObjDict* other)
{
    std::lock_guard<std::mutex> lockThis(m);
    std::lock_guard<std::mutex> lockOther(other->m);
    m_keys = other->m_keys;
    entries = other->entries;
}

bool ObjDict::equals(const ObjDict* other) const
{
    if (other == nullptr)
        return false;
    if (other == this)
        return true;

    // lock both dictionaries while comparing
    std::lock_guard<std::mutex> lockThis(m);
    std::lock_guard<std::mutex> lockOther(other->m);

    if (entries.size() != other->entries.size())
        return false;

    // entries is a std::map keyed by Value, which provides ordering
    // irrespective of insertion order.  Compare by keys and values
    for(const auto& [key, val] : entries) {
        auto it = other->entries.find(key);
        if (it == other->entries.end())
            return false;
        if (!val.equals(it->second, false))
            return false;
    }

    return true;
}


std::string roxal::objDictToString(const ObjDict* od)
{
    ContainerPrintContext context;
    return objDictToStringInternal(od, context);
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

void ObjDict::trace(ValueVisitor& visitor) const
{
    for (const auto& entry : entries) {
        visitor.visit(entry.first);
        visitor.visit(entry.second);
    }
}

void ObjDict::dropReferences()
{
    std::lock_guard<std::mutex> lock(m);
    m_keys.clear();
    entries.clear();
}


unique_ptr<ObjVector, UnreleasedObj> roxal::newVectorObj()
{
    #ifdef DEBUG_BUILD
    return newObj<ObjVector>(__func__, __FILE__, __LINE__);
    #else
    return newObj<ObjVector>();
    #endif
}

unique_ptr<ObjVector, UnreleasedObj> roxal::newVectorObj(int32_t size)
{
    #ifdef DEBUG_BUILD
    auto v = newObj<ObjVector>(__func__, __FILE__, __LINE__, size);
    #else
    auto v = newObj<ObjVector>(size);
    #endif
    return v;
}

unique_ptr<ObjVector, UnreleasedObj> roxal::newVectorObj(const Eigen::VectorXd& values)
{
    #ifdef DEBUG_BUILD
    auto v = newObj<ObjVector>(__func__, __FILE__, __LINE__, values);
    #else
    auto v = newObj<ObjVector>(values);
    #endif
    return v;
}

unique_ptr<Obj, UnreleasedObj> ObjVector::clone() const
{
    auto newv = newVectorObj(vec.size());
    newv->vec = vec;
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

unique_ptr<ObjMatrix, UnreleasedObj> roxal::newMatrixObj()
{
    #ifdef DEBUG_BUILD
    return newObj<ObjMatrix>(__func__, __FILE__, __LINE__);
    #else
    return newObj<ObjMatrix>();
    #endif
}

unique_ptr<ObjMatrix, UnreleasedObj> roxal::newMatrixObj(int32_t rows, int32_t cols)
{
    #ifdef DEBUG_BUILD
    auto m = newObj<ObjMatrix>(__func__, __FILE__, __LINE__, rows, cols);
    #else
    auto m = newObj<ObjMatrix>(rows, cols);
    #endif
    return m;
}

unique_ptr<ObjMatrix, UnreleasedObj> roxal::newMatrixObj(const Eigen::MatrixXd& values)
{
    #ifdef DEBUG_BUILD
    auto m = newObj<ObjMatrix>(__func__, __FILE__, __LINE__, values);
    #else
    auto m = newObj<ObjMatrix>(values);
    #endif
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

unique_ptr<Obj, UnreleasedObj> ObjMatrix::clone() const
{
    auto newm = newMatrixObj(mat);
    return newm;
}

void ObjMatrix::set(const ObjMatrix* other)
{
    mat = other->mat;
}

unique_ptr<Obj, UnreleasedObj> ObjPrimitive::clone() const
{
    if (type == ObjType::Bool)
        return newBoolObj(as.boolean);
    else if (type == ObjType::Int)
        return newIntObj(as.integer);
    else if (type == ObjType::Real)
        return newRealObj(as.real);
    else if (type == ObjType::Type)
        return newTypeObj(as.btype);
#ifdef DEBUG_BUILD
    throw std::runtime_error("Unsupported ObjPrimitive Type "+std::to_string(int(type)));
#else
    return newBoolObj(false);
#endif
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
            int64_t i = as.integer;
            out.write(reinterpret_cast<char*>(&i), 8);
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
            int64_t i; in.read(reinterpret_cast<char*>(&i),8);
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
unique_ptr<Obj, UnreleasedObj> ObjSignal::clone() const { throw std::runtime_error("cannot clone signals"); }
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
    Value val = signal ? signal->lastValue() : Value::nilVal();
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
    changeEventType = Value::nilVal();
    type = ObjType::Signal;
}

ObjEventType::ObjEventType(const icu::UnicodeString& typeName)
    : name(typeName)
    , superType(Value::nilVal())
{
    type = ObjType::EventType;
}

unique_ptr<Obj, UnreleasedObj> ObjEventType::clone() const {
    // event type definitions are immutable once created; share reference
    return unique_ptr<Obj, UnreleasedObj>(const_cast<ObjEventType*>(this));
}

void ObjEventType::write(std::ostream& out, roxal::ptr<SerializationContext> ctx) const
{
    uint8_t tag = static_cast<uint8_t>(ObjType::EventType);
    out.write(reinterpret_cast<char*>(&tag), 1);

    std::string n; name.toUTF8String(n);
    uint32_t len = n.size();
    out.write(reinterpret_cast<char*>(&len), 4);
    if (len > 0) out.write(n.data(), len);

    writeValue(out, superType, ctx);

    uint32_t propCount = payloadProperties.size();
    out.write(reinterpret_cast<char*>(&propCount), 4);
    for (const auto& prop : payloadProperties) {
        std::string pn; prop.name.toUTF8String(pn);
        uint32_t plen = pn.size();
        out.write(reinterpret_cast<char*>(&plen), 4);
        if (plen > 0) out.write(pn.data(), plen);
        writeValue(out, prop.type, ctx);
        writeValue(out, prop.initialValue, ctx);
    }

    uint32_t subCount = subscribers.size();
    out.write(reinterpret_cast<char*>(&subCount), 4);
    for (const auto& subscriber : subscribers) {
        writeValue(out, subscriber, ctx);
    }
}

void ObjEventType::read(std::istream& in, roxal::ptr<SerializationContext> ctx)
{
    uint8_t tag; in.read(reinterpret_cast<char*>(&tag), 1);
    if (tag != static_cast<uint8_t>(ObjType::EventType))
        throw std::runtime_error("ObjEventType::read mismatched tag");

    uint32_t len; in.read(reinterpret_cast<char*>(&len), 4);
    std::string ns(len, '\0');
    if (len > 0) in.read(ns.data(), len);
    name = icu::UnicodeString::fromUTF8(ns);

    superType = readValue(in, ctx);

    payloadProperties.clear();
    propertyLookup.clear();

    uint32_t propCount; in.read(reinterpret_cast<char*>(&propCount), 4);
    for (uint32_t i = 0; i < propCount; ++i) {
        uint32_t plen; in.read(reinterpret_cast<char*>(&plen), 4);
        std::string pn(plen, '\0');
        if (plen > 0) in.read(pn.data(), plen);
        icu::UnicodeString propName = icu::UnicodeString::fromUTF8(pn);

        Value typeValue = readValue(in, ctx);
        Value initial = readValue(in, ctx);

        payloadProperties.push_back({ propName, typeValue, initial });
        propertyLookup[propName.hashCode()] = payloadProperties.size() - 1;
    }

    uint32_t subCount; in.read(reinterpret_cast<char*>(&subCount), 4);
    subscribers.clear();
    subscribers.reserve(subCount);
    for (uint32_t i = 0; i < subCount; ++i) {
        subscribers.push_back(readValue(in, ctx));
    }

    type = ObjType::EventType;
}

void ObjEventType::trace(ValueVisitor& visitor) const
{
    visitor.visit(superType);
    for (const auto& prop : payloadProperties) {
        visitor.visit(prop.type);
        visitor.visit(prop.initialValue);
    }
    for (const auto& subscriber : subscribers) {
        visitor.visit(subscriber);
    }
}

void ObjEventType::dropReferences()
{
    superType = Value::nilVal();
    for (auto& prop : payloadProperties) {
        prop.type = Value::nilVal();
        prop.initialValue = Value::nilVal();
    }
    subscribers.clear();
}

std::vector<ObjEventType::PayloadPropertyView> ObjEventType::orderedPayloadProperties() const
{
    std::vector<PayloadPropertyView> result;
    result.reserve(payloadProperties.size());
    for (size_t i = 0; i < payloadProperties.size(); ++i) {
        const auto& prop = payloadProperties[i];
        int32_t hash = prop.name.hashCode();
        result.push_back({i, &prop, static_cast<uint16_t>(hash & 0x7fff)});
    }
    return result;
}

std::optional<ObjEventType::PayloadPropertyView>
ObjEventType::findPayloadPropertyByHash15(uint16_t hash15, bool& ambiguous) const
{
    ambiguous = false;
    const PayloadProperty* match = nullptr;
    size_t matchIndex = 0;
    int32_t matchHash = 0;
    for (size_t i = 0; i < payloadProperties.size(); ++i) {
        const auto& prop = payloadProperties[i];
        int32_t propHash = prop.name.hashCode();
        if (static_cast<uint16_t>(propHash & 0x7fff) != hash15)
            continue;
        if (match != nullptr) {
            ambiguous = true;
            return std::nullopt;
        }
        match = &prop;
        matchIndex = i;
        matchHash = propHash;
    }
    if (match == nullptr)
        return std::nullopt;
    return PayloadPropertyView{matchIndex, match, static_cast<uint16_t>(matchHash & 0x7fff)};
}

ObjEventInstance::ObjEventInstance(const Value& eventType)
    : typeHandle(eventType)
{
    type = ObjType::EventInstance;
    if (isEventType(eventType)) {
        ObjEventType* typeObj = asEventType(eventType);
        payload.reserve(typeObj->payloadProperties.size());
        for (const auto& prop : typeObj->payloadProperties) {
            auto propInitialValue = prop.initialValue;
            // Clone reference types to avoid sharing between instances
            if (!propInitialValue.isPrimitive()) {
                // Events should not have signal members
                if (isSignal(propInitialValue)) {
                    throw std::runtime_error("events cannot have signal members");
                }
                propInitialValue = propInitialValue.clone();
            }
            payload.push_back(propInitialValue);
        }
    }
}

unique_ptr<Obj, UnreleasedObj> ObjEventInstance::clone() const {
    // event instances are immutable snapshots; share reference
    return unique_ptr<Obj, UnreleasedObj>(const_cast<ObjEventInstance*>(this));
}

void ObjEventInstance::write(std::ostream& out, roxal::ptr<SerializationContext> ctx) const
{
    uint8_t tag = static_cast<uint8_t>(ObjType::EventInstance);
    out.write(reinterpret_cast<char*>(&tag), 1);
    writeValue(out, typeHandle, ctx);

    uint32_t count = payload.size();
    out.write(reinterpret_cast<char*>(&count), 4);
    for (const auto& value : payload) {
        writeValue(out, value, ctx);
    }
}

void ObjEventInstance::read(std::istream& in, roxal::ptr<SerializationContext> ctx)
{
    uint8_t tag; in.read(reinterpret_cast<char*>(&tag), 1);
    if (tag != static_cast<uint8_t>(ObjType::EventInstance))
        throw std::runtime_error("ObjEventInstance::read mismatched tag");

    typeHandle = readValue(in, ctx);

    uint32_t count; in.read(reinterpret_cast<char*>(&count), 4);
    payload.clear();
    payload.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        payload.push_back(readValue(in, ctx));
    }

    type = ObjType::EventInstance;
}

void ObjEventInstance::trace(ValueVisitor& visitor) const
{
    visitor.visit(typeHandle);
    for (const auto& value : payload) {
        visitor.visit(value);
    }
}

void ObjEventInstance::dropReferences()
{
    typeHandle = Value::nilVal();
    payload.clear();
}

unique_ptr<Obj, UnreleasedObj> ObjLibrary::clone() const {
    // dynamic libraries are represented by handles; share the handle
    return unique_ptr<Obj, UnreleasedObj>(const_cast<ObjLibrary*>(this));
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
unique_ptr<Obj, UnreleasedObj> ObjForeignPtr::clone() const {
    // foreign pointers are opaque handles; cloning would be unsafe
    return unique_ptr<Obj, UnreleasedObj>(const_cast<ObjForeignPtr*>(this));
}
void ObjForeignPtr::write(std::ostream&, roxal::ptr<SerializationContext>) const { throw std::runtime_error("Cannot serialize foreign pointers"); }
void ObjForeignPtr::read(std::istream&, roxal::ptr<SerializationContext>) { throw std::runtime_error("Cannot deserialize foreign pointers"); }
unique_ptr<Obj, UnreleasedObj> ObjFile::clone() const {
    // files cannot be duplicated; share the underlying handle
    return unique_ptr<Obj, UnreleasedObj>(const_cast<ObjFile*>(this));
}
void ObjFile::write(std::ostream&, roxal::ptr<SerializationContext>) const { throw std::runtime_error("Cannot serialize file handles"); }
void ObjFile::read(std::istream&, roxal::ptr<SerializationContext>) { throw std::runtime_error("Cannot deserialize file handles"); }
unique_ptr<Obj, UnreleasedObj> ObjException::clone() const { throw std::runtime_error("cannot clone exceptions"); }
void ObjException::write(std::ostream& out, roxal::ptr<SerializationContext> ctx) const
{
    uint8_t tag = static_cast<uint8_t>(ObjType::Exception);
    out.write(reinterpret_cast<char*>(&tag),1);
    writeValue(out, message, ctx);
    writeValue(out, exType, ctx);
    writeValue(out, stackTrace, ctx);
    writeValue(out, detail, ctx);
}

void ObjException::read(std::istream& in, roxal::ptr<SerializationContext> ctx)
{
    uint8_t tag; in.read(reinterpret_cast<char*>(&tag),1);
    if(tag != static_cast<uint8_t>(ObjType::Exception))
        throw std::runtime_error("ObjException::read mismatched tag");
    message = readValue(in, ctx);
    exType = readValue(in, ctx);
    stackTrace = readValue(in, ctx);
    detail = readValue(in, ctx);
    type = ObjType::Exception;
}

void ObjException::trace(ValueVisitor& visitor) const
{
    visitor.visit(message);
    visitor.visit(exType);
    visitor.visit(stackTrace);
    visitor.visit(detail);
}

void ObjException::dropReferences()
{
    message = Value::nilVal();
    exType = Value::nilVal();
    stackTrace = Value::nilVal();
    detail = Value::nilVal();
}
unique_ptr<Obj, UnreleasedObj> ObjFunction::clone() const {
    // function objects are immutable; share the reference
    return unique_ptr<Obj, UnreleasedObj>(const_cast<ObjFunction*>(this));
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

    {
        std::string ds; doc.toUTF8String(ds);
        uint32_t dlen = ds.size();
        out.write(reinterpret_cast<char*>(&dlen),4);
        if(dlen) out.write(ds.data(), dlen);
    }

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
            asFunction(kv.second)->write(out, ctx);
        }
    }

    writeValue(out, moduleType, ctx);

    // Serialize whether this function has a native implementation
    // The pointer itself can't be serialized, but we need to know to re-link it
    uint8_t hasNativeImpl = (nativeImpl != nullptr) ? 1 : 0;
    out.write(reinterpret_cast<char*>(&hasNativeImpl), 1);
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

    chunk = make_ptr<Chunk>(icu::UnicodeString(), icu::UnicodeString(), icu::UnicodeString());
    chunk->deserialize(in, ctx);

    uint32_t annCount; in.read(reinterpret_cast<char*>(&annCount),4);
    annotations.clear();
    for(uint32_t i=0;i<annCount;i++)
        annotations.push_back(readAnnotation(in));

    {
        uint32_t dlen; in.read(reinterpret_cast<char*>(&dlen),4);
        if(dlen) {
            std::string ds(dlen,'\0'); in.read(ds.data(),dlen);
            doc = icu::UnicodeString::fromUTF8(ds);
        } else {
            doc = icu::UnicodeString();
        }
    }

    uint8_t s; in.read(reinterpret_cast<char*>(&s),1); strict = s!=0;

    uint8_t ft; in.read(reinterpret_cast<char*>(&ft),1); fnType = static_cast<FunctionType>(ft);

    ownerType = readValue(in, ctx);
    if(!ownerType.isNil())
        ownerType = ownerType.weakRef();

    uint8_t acc; in.read(reinterpret_cast<char*>(&acc),1); access = static_cast<ast::Access>(acc);

    uint32_t defCount; in.read(reinterpret_cast<char*>(&defCount),4);
    paramDefaultFunc.clear();
    for(uint32_t i=0;i<defCount;i++) {
        int32_t key; in.read(reinterpret_cast<char*>(&key),4);
        Value func = Value::functionVal(icu::UnicodeString(), icu::UnicodeString(), icu::UnicodeString(), icu::UnicodeString());
        asFunction(func)->read(in, ctx);
        paramDefaultFunc[key] = func;
    }

    moduleType = readValue(in, ctx);
    if(!moduleType.isNil())
        moduleType = moduleType.weakRef();

    // Read the native implementation flag
    uint8_t hasNativeImpl = 0;
    in.read(reinterpret_cast<char*>(&hasNativeImpl), 1);

    // If this function had a native implementation, re-link it from the live builtin module
    if (hasNativeImpl && chunk && !chunk->moduleName.isEmpty()) {
        Value builtinModuleVal = VM::instance().getBuiltinModuleType(chunk->moduleName);
        if (builtinModuleVal.isNonNil()) {
            ObjModuleType* builtinModule = asModuleType(builtinModuleVal);
            auto liveVarOpt = builtinModule->vars.load(name);
            if (liveVarOpt.has_value() && isClosure(liveVarOpt.value())) {
                ObjClosure* liveClosure = asClosure(liveVarOpt.value());
                if (isFunction(liveClosure->function)) {
                    ObjFunction* liveFunc = asFunction(liveClosure->function);
                    if (liveFunc->nativeImpl) {
                        nativeImpl = liveFunc->nativeImpl;
                        nativeDefaults = liveFunc->nativeDefaults;
                    }
                }
            }
        }
        if (nativeImpl == nullptr) {
            throw std::runtime_error("Unable to relink native function '" +
                                     toUTF8StdString(name) + "' for module '" +
                                     toUTF8StdString(chunk->moduleName) + "'");
        }
    }
}

void ObjFunction::trace(ValueVisitor& visitor) const
{
    visitor.visit(ownerType);
    visitor.visit(moduleType);
    for (const auto& def : nativeDefaults) {
        visitor.visit(def);
    }
    for (const auto& entry : paramDefaultFunc) {
        visitor.visit(entry.second);
    }
    if (chunk) {
        for (const auto& constant : chunk->constants) {
            visitor.visit(constant);
        }
    }
}

void ObjFunction::dropReferences()
{
    ownerType = Value::nilVal();
    moduleType = Value::nilVal();

    if (chunk) {
        // Constants can hold strong refs to other objects. Clear them here so
        // they decRef while the chunk is still intact instead of during the
        // destructor after the pointed-to objects may already have been freed.
        chunk->code.clear();
        chunk->constants.clear();
        chunk.reset();
    }

    nativeDefaults.clear();
    paramDefaultFunc.clear();
}

unique_ptr<Obj, UnreleasedObj> ObjUpvalue::clone() const
{
    auto newup = newUpvalueObj(location);
    newup->closed = newup->location->clone();
    newup->location = &newup->closed;
    return newup;
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

void ObjUpvalue::trace(ValueVisitor& visitor) const
{
    if (location && location != &closed) {
        visitor.visit(*location);
    }
    visitor.visit(closed);
}

void ObjUpvalue::dropReferences()
{
    closed = Value::nilVal();
    location = &closed;
}

unique_ptr<Obj, UnreleasedObj> ObjClosure::clone() const
{
    auto newc = newClosureObj(function);
    newc->upvalues.resize(upvalues.size());
    for(size_t i=0; i<upvalues.size(); i++)
        newc->upvalues[i] = upvalues.at(i).clone();
    return newc;
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
    writeValue(out, function, ctx);
    uint32_t count = upvalues.size();
    out.write(reinterpret_cast<char*>(&count),4);
    for(auto uv : upvalues) {
        uint8_t present = uv.isNil() ? 0 : 1;
        out.write(reinterpret_cast<char*>(&present),1);
        if(present)
            writeValue(out, uv, ctx);
    }
}

void ObjClosure::read(std::istream& in, roxal::ptr<SerializationContext> ctx)
{
    uint8_t tag; in.read(reinterpret_cast<char*>(&tag),1);
    if(tag != static_cast<uint8_t>(ObjType::Closure))
        throw std::runtime_error("ObjClosure::read mismatched tag");
    type = ObjType::Closure;

    // Use the same serialization context so that references from the function
    // back to this closure's owning structures are properly resolved.
    function = readValue(in, ctx);

    uint32_t count; in.read(reinterpret_cast<char*>(&count),4);
    upvalues.resize(count);
    for(uint32_t i=0;i<count;i++) {
        uint8_t present; in.read(reinterpret_cast<char*>(&present),1);
        if(present) {
            Value uv = readValue(in, ctx);
            upvalues[i] = uv;
        }
    }
}

void ObjClosure::trace(ValueVisitor& visitor) const
{
    visitor.visit(function);
    for (const auto& upvalue : upvalues) {
        visitor.visit(upvalue);
    }
}

void ObjClosure::dropReferences()
{
    function = Value::nilVal();
    upvalues.clear();
    handlerThread.reset();
}

unique_ptr<Obj, UnreleasedObj> ObjFuture::clone() const
{
    Value value = const_cast<ObjFuture*>(this)->asValue();
    value.box();
    assert(value.isBoxed() && value.isObj() && isObjPrimitive(value));
    Obj* obj = value.asObj();
    obj->incRef();
    return unique_ptr<Obj, UnreleasedObj>(obj);
}
void ObjFuture::write(std::ostream& out, roxal::ptr<SerializationContext> ctx) const
{
    Value resolved = future.valid() ? future.get() : Value::nilVal();
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

void ObjFuture::trace(ValueVisitor& visitor) const
{
    if (future.valid()) {
        if (future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            visitor.visit(future.get());
        }
    }
}

void ObjFuture::dropReferences()
{
    future = std::shared_future<Value>();
    std::lock_guard<std::mutex> lk(waitMutex);
    waiters.clear();
}

void ObjFuture::addWaiter(const ptr<Thread>& t)
{
    std::lock_guard<std::mutex> lk(waitMutex);
    for (auto it = waiters.begin(); it != waiters.end(); ++it) {
        if (auto sp = it->lock()) {
            if (sp == t)
                return;
        } else {
            it = waiters.erase(it);
            if (it == waiters.end()) break;
        }
    }
    waiters.push_back(t);
}

void ObjFuture::wakeWaiters()
{
    std::vector<ptr<Thread>> toWake;
    {
        std::lock_guard<std::mutex> lk(waitMutex);
        for (auto it = waiters.begin(); it != waiters.end(); ) {
            if (auto sp = it->lock()) {
                toWake.push_back(sp);
                ++it;
            } else {
                it = waiters.erase(it);
            }
        }
        waiters.clear();
    }
    for (auto& t : toWake) t->wake();
}
unique_ptr<Obj, UnreleasedObj> ObjNative::clone() const {
    // native functions are immutable; share the reference
    return unique_ptr<Obj, UnreleasedObj>(const_cast<ObjNative*>(this));
}
void ObjNative::write(std::ostream&, roxal::ptr<SerializationContext>) const { throw std::runtime_error("ObjNative serialization not implemented"); }
void ObjNative::read(std::istream&, roxal::ptr<SerializationContext>) { throw std::runtime_error("ObjNative deserialization not implemented"); }

void ObjNative::trace(ValueVisitor& visitor) const
{
    for (const auto& value : defaultValues) {
        visitor.visit(value);
    }
}

void ObjNative::dropReferences()
{
    defaultValues.clear();
}
unique_ptr<Obj, UnreleasedObj> ObjTypeSpec::clone() const {
    // type metadata is immutable; share the reference
    return unique_ptr<Obj, UnreleasedObj>(const_cast<ObjTypeSpec*>(this));
}
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
unique_ptr<Obj, UnreleasedObj> ObjObjectType::clone() const {
    // object type definitions are immutable once created; share reference
    return unique_ptr<Obj, UnreleasedObj>(const_cast<ObjObjectType*>(this));
}
void ObjObjectType::write(std::ostream& out, roxal::ptr<SerializationContext> ctx) const
{
    ObjTypeSpec::write(out);

    std::string n; name.toUTF8String(n);
    uint32_t len = n.size();
    out.write(reinterpret_cast<char*>(&len), 4);
    if (len>0) out.write(n.data(), len);

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
        uint8_t isConst = prop.isConst ? 1 : 0;
        out.write(reinterpret_cast<char*>(&isConst),1);
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
        uint8_t isConst; in.read(reinterpret_cast<char*>(&isConst),1);
        uint8_t hasC; in.read(reinterpret_cast<char*>(&hasC),1);
        std::optional<icu::UnicodeString> ct;
        if(hasC) {
            uint32_t ctlen; in.read(reinterpret_cast<char*>(&ctlen),4);
            std::string cts(ctlen,'\0'); if(ctlen>0) in.read(cts.data(), ctlen);
            ct = icu::UnicodeString::fromUTF8(cts);
        }
        int32_t hash = uname.hashCode();
        Property prop{uname, ptype, init, static_cast<ast::Access>(acc), isConst != 0, Value::nilVal(), ct};
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
        Method m{uname, clos, static_cast<ast::Access>(acc), Value::nilVal()};
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

void ObjObjectType::trace(ValueVisitor& visitor) const
{
    visitor.visit(superType);
    for (const auto& entry : properties) {
        const auto& prop = entry.second;
        visitor.visit(prop.type);
        visitor.visit(prop.initialValue);
        visitor.visit(prop.ownerType);
    }
    for (const auto& entry : methods) {
        const auto& method = entry.second;
        visitor.visit(method.closure);
        visitor.visit(method.ownerType);
    }
    for (const auto& entry : enumLabelValues) {
        visitor.visit(entry.second.second);
    }
}

void ObjObjectType::dropReferences()
{
    superType = Value::nilVal();

    for (auto& entry : properties) {
        auto& prop = entry.second;
        prop.type = Value::nilVal();
        prop.initialValue = Value::nilVal();
        prop.ownerType = Value::nilVal();
    }

    for (auto& entry : methods) {
        auto& method = entry.second;
        method.closure = Value::nilVal();
        method.ownerType = Value::nilVal();
    }

    for (auto& entry : enumLabelValues) {
        entry.second.second = Value::nilVal();
    }
}

std::vector<ObjObjectType::PublicPropertyView> ObjObjectType::orderedPublicProperties() const
{
    std::vector<PublicPropertyView> result;
    result.reserve(propertyOrder.size());
    for (int32_t hash : propertyOrder) {
        auto it = properties.find(hash);
        if (it == properties.end())
            continue;
        const Property& prop = it->second;
        if (prop.access != ast::Access::Public)
            continue;
        int32_t key = prop.name.hashCode();
        result.push_back({key, &prop, static_cast<uint16_t>(key & 0x7fff)});
    }
    return result;
}

std::optional<ObjObjectType::PublicPropertyView>
ObjObjectType::findPublicPropertyByHash15(uint16_t hash15, bool& ambiguous) const
{
    ambiguous = false;
    const Property* match = nullptr;
    int32_t key = 0;
    for (int32_t hash : propertyOrder) {
        auto it = properties.find(hash);
        if (it == properties.end())
            continue;
        const Property& prop = it->second;
        if (prop.access != ast::Access::Public)
            continue;
        int32_t propKey = prop.name.hashCode();
        if (static_cast<uint16_t>(propKey & 0x7fff) != hash15)
            continue;
        if (match != nullptr) {
            ambiguous = true;
            return std::nullopt;
        }
        match = &prop;
        key = propKey;
    }
    if (match == nullptr)
        return std::nullopt;
    return PublicPropertyView{key, match, static_cast<uint16_t>(key & 0x7fff)};
}
unique_ptr<Obj, UnreleasedObj> ObjPackageType::clone() const {
    // package types contain no mutable state; share the reference
    return unique_ptr<Obj, UnreleasedObj>(const_cast<ObjPackageType*>(this));
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
unique_ptr<Obj, UnreleasedObj> ObjModuleType::clone() const {
    // module types are immutable; share the reference
    return unique_ptr<Obj, UnreleasedObj>(const_cast<ObjModuleType*>(this));
}
void ObjModuleType::write(std::ostream& out, roxal::ptr<SerializationContext> ctx) const
{
    ObjTypeSpec::write(out);

    std::string n; name.toUTF8String(n);
    uint32_t len = n.size();
    out.write(reinterpret_cast<char*>(&len), 4);
    out.write(n.data(), len);

    std::string full;
    fullName.toUTF8String(full);
    uint32_t fullLen = static_cast<uint32_t>(full.size());
    out.write(reinterpret_cast<char*>(&fullLen), 4);
    if (fullLen)
        out.write(full.data(), fullLen);

    std::string source;
    sourcePath.toUTF8String(source);
    uint32_t sourceLen = static_cast<uint32_t>(source.size());
    out.write(reinterpret_cast<char*>(&sourceLen), 4);
    if (sourceLen)
        out.write(source.data(), sourceLen);

    auto varsSnapshot = vars.snapshot();
    std::unordered_map<int32_t, icu::UnicodeString> aliasLookup;
    for (const auto& alias : moduleAliasSnapshot())
        aliasLookup.emplace(alias.first.hashCode(), alias.second);

    uint32_t varCount = varsSnapshot.size();
    out.write(reinterpret_cast<char*>(&varCount), 4);
    for (const auto& entry : varsSnapshot) {
        std::string varName;
        entry.first.toUTF8String(varName);
        uint32_t nameLen = static_cast<uint32_t>(varName.size());
        out.write(reinterpret_cast<char*>(&nameLen), 4);
        if (nameLen)
            out.write(varName.data(), nameLen);

        int32_t nameHash = entry.first.hashCode();
        uint8_t flags = 0;
        if (constVars.find(nameHash) != constVars.end())
            flags |= 0x2;
        icu::UnicodeString aliasFullName;
        auto aliasIt = aliasLookup.find(nameHash);
        if (aliasIt != aliasLookup.end()) {
            flags |= 0x1;
            aliasFullName = aliasIt->second;
        } else if (isModuleType(entry.second)) {
            // When no explicit alias metadata was recorded fall back to the
            // module's full name.  This ensures older cache files still record
            // enough information to rebuild the module graph when reloaded.
            flags |= 0x1;
            ObjModuleType* module = asModuleType(entry.second);
            aliasFullName = module->fullName.isEmpty() ? module->name : module->fullName;
        }

        out.write(reinterpret_cast<char*>(&flags), 1);

        if (flags & 0x1) {
            if (aliasFullName.isEmpty())
                aliasFullName = entry.first;
            std::string aliasFullUtf8;
            aliasFullName.toUTF8String(aliasFullUtf8);
            uint32_t aliasLen = static_cast<uint32_t>(aliasFullUtf8.size());
            out.write(reinterpret_cast<char*>(&aliasLen), 4);
            if (aliasLen)
                out.write(aliasFullUtf8.data(), aliasFullUtf8.size());
        }

        writeValue(out, entry.second, ctx);
    }

    // Persist the cstruct annotation map so cached modules know which type
    // declarations should recreate their FFI metadata when reloaded.
    uint32_t cstructCount = static_cast<uint32_t>(cstructArch.size());
    out.write(reinterpret_cast<char*>(&cstructCount), 4);
    for (const auto& entry : cstructArch) {
        int32_t nameHash = entry.first;
        int32_t arch = static_cast<int32_t>(entry.second);
        out.write(reinterpret_cast<char*>(&nameHash), 4);
        out.write(reinterpret_cast<char*>(&arch), 4);
    }

    // Serialize property-level ctype annotations keyed by the declaring type
    // hash and the property name hash so @ctype metadata survives cache loads.
    uint32_t typeCount = static_cast<uint32_t>(propertyCTypes.size());
    out.write(reinterpret_cast<char*>(&typeCount), 4);
    for (const auto& typeEntry : propertyCTypes) {
        int32_t typeHash = typeEntry.first;
        out.write(reinterpret_cast<char*>(&typeHash), 4);

        const auto& props = typeEntry.second;
        uint32_t propCount = static_cast<uint32_t>(props.size());
        out.write(reinterpret_cast<char*>(&propCount), 4);

        for (const auto& propEntry : props) {
            int32_t propHash = propEntry.first;
            out.write(reinterpret_cast<char*>(&propHash), 4);

            std::string ctypeUtf8;
            propEntry.second.toUTF8String(ctypeUtf8);
            uint32_t ctypeLen = static_cast<uint32_t>(ctypeUtf8.size());
            out.write(reinterpret_cast<char*>(&ctypeLen), 4);
            if (ctypeLen)
                out.write(ctypeUtf8.data(), ctypeLen);
        }
    }
}

void ObjModuleType::read(std::istream& in, roxal::ptr<SerializationContext> ctx)
{
    ObjTypeSpec::read(in);

    uint32_t len; in.read(reinterpret_cast<char*>(&len),4);
    std::string ns(len, '\0');
    if(len>0) in.read(ns.data(), len);
    name = icu::UnicodeString::fromUTF8(ns);

    uint32_t fullLen = 0;
    in.read(reinterpret_cast<char*>(&fullLen), 4);
    std::string full(fullLen, '\0');
    if (fullLen)
        in.read(full.data(), fullLen);
    if (fullLen > 0)
        fullName = icu::UnicodeString::fromUTF8(full);
    else
        fullName = name;

    uint32_t sourceLen = 0;
    in.read(reinterpret_cast<char*>(&sourceLen), 4);
    std::string source(sourceLen, '\0');
    if (sourceLen)
        in.read(source.data(), sourceLen);
    if (sourceLen > 0)
        sourcePath = icu::UnicodeString::fromUTF8(source);
    else
        sourcePath = icu::UnicodeString();

    allModules.push_back(Value::objRef(this));

    vars.clear();
    clearModuleAliases();
    constVars.clear();
    uint32_t varCount = 0;
    in.read(reinterpret_cast<char*>(&varCount), 4);
    for (uint32_t i = 0; i < varCount; ++i) {
        uint32_t nameLen = 0;
        in.read(reinterpret_cast<char*>(&nameLen), 4);
        std::string name(nameLen, '\0');
        if (nameLen)
            in.read(name.data(), nameLen);
        icu::UnicodeString varName = icu::UnicodeString::fromUTF8(name);

        uint8_t flags = 0;
        in.read(reinterpret_cast<char*>(&flags), 1);
        if (flags & 0x2)
            constVars.insert(varName.hashCode());
        if (flags & 0x1) {
            uint32_t aliasLen = 0;
            in.read(reinterpret_cast<char*>(&aliasLen), 4);
            std::string alias(aliasLen, '\0');
            if (aliasLen)
                in.read(alias.data(), aliasLen);
            icu::UnicodeString aliasFullName = icu::UnicodeString::fromUTF8(alias);
            if (aliasFullName.isEmpty())
                aliasFullName = varName;
            // Store the alias so reconcileModuleReferences() knows which fully
            // qualified module should be rebound to this slot.
            registerModuleAlias(varName, aliasFullName);
        }

        Value stored = readValue(in, ctx);
        vars.store(varName, stored, true);
    }

    // Rebuild the cstruct metadata so the VM can mark cached object types as
    // FFI-compatible when they are constructed.
    cstructArch.clear();
    uint32_t cstructCount = 0;
    in.read(reinterpret_cast<char*>(&cstructCount), 4);
    for (uint32_t i = 0; i < cstructCount; ++i) {
        int32_t nameHash = 0;
        int32_t arch = 0;
        in.read(reinterpret_cast<char*>(&nameHash), 4);
        in.read(reinterpret_cast<char*>(&arch), 4);
        cstructArch[nameHash] = arch;
    }

    propertyCTypes.clear();
    uint32_t typeCount = 0;
    in.read(reinterpret_cast<char*>(&typeCount), 4);
    for (uint32_t i = 0; i < typeCount; ++i) {
        int32_t typeHash = 0;
        in.read(reinterpret_cast<char*>(&typeHash), 4);

        uint32_t propCount = 0;
        in.read(reinterpret_cast<char*>(&propCount), 4);
        auto& props = propertyCTypes[typeHash];
        for (uint32_t p = 0; p < propCount; ++p) {
            int32_t propHash = 0;
            in.read(reinterpret_cast<char*>(&propHash), 4);

            uint32_t ctypeLen = 0;
            in.read(reinterpret_cast<char*>(&ctypeLen), 4);
            std::string ctypeUtf8(ctypeLen, '\0');
            if (ctypeLen)
                in.read(ctypeUtf8.data(), ctypeLen);
            props[propHash] = icu::UnicodeString::fromUTF8(ctypeUtf8);
        }
    }
}

void ObjModuleType::trace(ValueVisitor& visitor) const
{
    vars.unsafeForEachModuleVar([&visitor](const auto& nameValue) {
        visitor.visit(nameValue.second.value);
        if (nameValue.second.hasSignal())
            visitor.visit(nameValue.second.signal);
    });
}

void ObjModuleType::dropReferences()
{
    vars.unsafeForEachModuleVar([](auto& nameValue) {
        nameValue.second.value = Value::nilVal();
        if (nameValue.second.hasSignal())
            nameValue.second.clearSignal();
    });
    vars.clear();
    clearModuleAliases();
    cstructArch.clear();
    sourcePath = icu::UnicodeString();
    propertyCTypes.clear();
}

void ObjModuleType::registerModuleAlias(const icu::UnicodeString& alias,
                                        const icu::UnicodeString& moduleFullName)
{
    // Track aliases by hash so they survive cache round-trips without
    // depending on the underlying table layout.
    moduleAliases.insert_or_assign(alias.hashCode(), std::make_pair(alias, moduleFullName));
}

std::vector<std::pair<icu::UnicodeString, icu::UnicodeString>> ObjModuleType::moduleAliasSnapshot() const
{
    std::vector<std::pair<icu::UnicodeString, icu::UnicodeString>> result;
    result.reserve(moduleAliases.size());
    for (const auto& entry : moduleAliases)
        result.push_back(entry.second);
    return result;
}

icu::UnicodeString ObjModuleType::moduleAliasFullName(const icu::UnicodeString& alias) const
{
    auto it = moduleAliases.find(alias.hashCode());
    if (it != moduleAliases.end())
        return it->second.second;
    return icu::UnicodeString();
}

void ObjModuleType::clearModuleAliases()
{
    moduleAliases.clear();
}
void ObjectInstance::write(std::ostream& out, roxal::ptr<SerializationContext> ctx) const
{
    // Only serialize the object contents here.  Reference tracking is handled
    // by the calling writeValue() helper.
    writeValue(out, instanceType, ctx);
    uint32_t count = properties.size();
    out.write(reinterpret_cast<char*>(&count),4);
    for(const auto& kv : properties) {
        int32_t h = kv.first;
        out.write(reinterpret_cast<char*>(&h),4);
        writeValue(out, kv.second.value, ctx);
    }
}

void ObjectInstance::read(std::istream& in, roxal::ptr<SerializationContext> ctx)
{
    // Reference tracking is handled by readValue().  Just read the object
    // contents here.
    instanceType = readValue(in, ctx);
    uint32_t count; in.read(reinterpret_cast<char*>(&count),4);
    properties.clear();
    for(uint32_t i=0;i<count;i++) {
        int32_t h; in.read(reinterpret_cast<char*>(&h),4);
        Value v = readValue(in, ctx);
        auto& slot = properties[h];
        slot.clearSignal();
        slot.value = v;
    }
}

void ObjectInstance::trace(ValueVisitor& visitor) const
{
    visitor.visit(instanceType);
    for (const auto& entry : properties) {
        visitor.visit(entry.second.value);
        visitor.visit(entry.second.signal);
    }
}

void ObjectInstance::dropReferences()
{
    instanceType = Value::nilVal();
    for (auto& entry : properties) {
        entry.second.value = Value::nilVal();
        entry.second.signal = Value::nilVal();
    }
    properties.clear();
}

unique_ptr<Obj, UnreleasedObj> ObjBoundMethod::clone() const
{
    auto newmb = newBoundMethodObj(receiver, method);
    newmb->receiver = newmb->receiver.clone();
    return newmb;
}
void ObjBoundMethod::write(std::ostream& out, roxal::ptr<SerializationContext> ctx) const
{
    uint8_t tag = static_cast<uint8_t>(ObjType::BoundMethod);
    out.write(reinterpret_cast<char*>(&tag),1);
    writeValue(out, receiver, ctx);
    writeValue(out, method, ctx);
}

void ObjBoundMethod::read(std::istream& in, roxal::ptr<SerializationContext> ctx)
{
    uint8_t tag; in.read(reinterpret_cast<char*>(&tag),1);
    if(tag != static_cast<uint8_t>(ObjType::BoundMethod))
        throw std::runtime_error("ObjBoundMethod::read mismatched tag");
    receiver = readValue(in, ctx);
    Value mval = readValue(in, ctx);
    method = mval.weakRef();
    type = ObjType::BoundMethod;
}

void ObjBoundMethod::trace(ValueVisitor& visitor) const
{
    visitor.visit(receiver);
    visitor.visit(method);
}

void ObjBoundMethod::dropReferences()
{
    receiver = Value::nilVal();
    method = Value::nilVal();
}

unique_ptr<Obj, UnreleasedObj> ObjBoundNative::clone() const
{
    auto newbm = newBoundNativeObj(receiver, function, isProc, funcType, defaultValues, declFunction);
    newbm->receiver = newbm->receiver.clone();
    return newbm;
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
    declFunction = Value::nilVal();
    type = ObjType::BoundNative;
}

void ObjBoundNative::trace(ValueVisitor& visitor) const
{
    visitor.visit(receiver);
    for (const auto& value : defaultValues) {
        visitor.visit(value);
    }
    visitor.visit(declFunction);
}

void ObjBoundNative::dropReferences()
{
    receiver = Value::nilVal();
    defaultValues.clear();
    declFunction = Value::nilVal();
}

void ActorInstance::write(std::ostream& out, roxal::ptr<SerializationContext> ctx) const
{
    // Only serialize the contents. Reference tracking is handled by writeValue().
    writeValue(out, instanceType, ctx);
    uint32_t count = properties.size();
    out.write(reinterpret_cast<char*>(&count),4);
    for(const auto& kv : properties) {
        int32_t h = kv.first;
        out.write(reinterpret_cast<char*>(&h),4);
        writeValue(out, kv.second.value, ctx);
    }
}

void ActorInstance::read(std::istream& in, roxal::ptr<SerializationContext> ctx)
{
    // Only read the contents. Reference tracking is handled by readValue().
    instanceType = readValue(in, ctx);
    uint32_t count; in.read(reinterpret_cast<char*>(&count),4);
    properties.clear();
    for(uint32_t i=0;i<count;i++) {
        int32_t h; in.read(reinterpret_cast<char*>(&h),4);
        Value v = readValue(in, ctx);
        auto& slot = properties[h];
        slot.clearSignal();
        slot.value = v;
    }
    ptr<Thread> newThread = make_ptr<Thread>();
    // Keep the thread alive by registering it with the VM. Without this the
    // Thread object would be destroyed immediately after deserialization,
    // causing std::terminate since the underlying std::thread is still
    // joinable.
    VM::instance().registerThread(newThread);
    thread = newThread;
    newThread->act(Value::objRef(this));
}

Value ActorInstance::ensurePropertySignal(int32_t nameHash, const std::string& signalName)
{
    auto it = properties.find(nameHash);
    if (it == properties.end())
        return Value::nilVal();
    return it->second.ensureSignal(signalName);
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
    : signal(s), engine(nullptr), changeEventType(Value::nilVal())
{
    type = ObjType::Signal;
    if (signal) {
        auto eng = df::DataflowEngine::instance();
        engine = eng.get();
        engine->registerSignalWrapper(signal);
    }
}

ObjSignal::~ObjSignal()
{
    if (signal && engine) {
        size_t remaining = engine->unregisterSignalWrapper(signal);
        if (remaining == 0 && engine->consumerCount(signal) == 0)
            engine->removeSignal(signal, true);
    }
}

void ObjSignal::trace(ValueVisitor& visitor) const
{
    visitor.visit(changeEventType);
    if (signal)
        signal->trace(visitor);
}

void ObjSignal::dropReferences()
{
    changeEventType = Value::nilVal();
    changeEventSignal.reset();
    changeEventUsesTimeSpan = false;
}

// Lazily create the shared SignalChanged event type and register the callback
// responsible for emitting instances when the signal's value changes.
ObjEventType* ObjSignal::ensureChangeEventType()
{
    auto currentSignal = signal;
    auto subscribeCallbacks = [&](const ptr<df::Signal>& target) {
        if (!target)
            return;
        Value eventWeak = changeEventType.weakRef();
        bool useSpan = changeEventUsesTimeSpan;
        target->addValueChangedCallback([eventWeak, useSpan](TimePoint t, ptr<df::Signal> sig, const Value& sample){
            if (!eventWeak.isAlive())
                return;
            Value eventTypeStrong = eventWeak.strongRef();
            if (eventTypeStrong.isNil())
                return;
            ObjEventType* ev = asEventType(eventTypeStrong);
            std::vector<Value> payload;
            payload.reserve(ev->payloadProperties.size());
            // First payload slot carries the current signal sample.
            payload.push_back(sample);
            if (useSpan) {
                // Emit the elapsed steady-clock time since engine start as a
                // TimeSpan instance when the sys module is available.
                payload.push_back(sysNewTimeSpan(t.microSecs()));
            } else {
                payload.push_back(Value::intVal(static_cast<int32_t>(t.microSecs())));
            }
            // Track the discrete tick associated with this change. Prefer the
            // signal's own period when available, otherwise fall back to the
            // engine's global tick counter.
            int64_t tickIndex = 0;
            if (sig && sig->period() != TimeDuration::zero()) {
                auto periodMicros = sig->period().microSecs();
                if (periodMicros > 0)
                    tickIndex = t.microSecs() / periodMicros;
            } else if (auto engine = df::DataflowEngine::instance(false)) {
                tickIndex = static_cast<int64_t>(engine->currentTickNumber());
            }
            tickIndex = std::clamp(tickIndex,
                                   static_cast<int64_t>(std::numeric_limits<int32_t>::min()),
                                   static_cast<int64_t>(std::numeric_limits<int32_t>::max()));
            payload.push_back(Value::intVal(static_cast<int32_t>(tickIndex)));
            payload.push_back(Value::signalVal(sig));

            // Package the change information into a new event instance and invoke
            // every subscribed handler for this occurrence.
            Value instance = Value::eventInstanceVal(eventTypeStrong, std::move(payload));
            // Deliver change notifications immediately while preserving the
            // original logical timestamp inside the payload. This keeps callbacks
            // responsive even when the dataflow engine's tick time is ahead of the
            // VM thread's wall clock (for example when ticks are advanced manually).
            scheduleEventHandlers(eventWeak, ev, instance, TimePoint::currentTime());
        });
        changeEventSignal = target;
    };

    if (!changeEventType.isNil()) {
        ObjEventType* existing = asEventType(changeEventType);
        auto subscribed = changeEventSignal.lock();
        if (currentSignal && currentSignal != subscribed)
            subscribeCallbacks(currentSignal);
        return existing;
    }

    auto eventType = newEventTypeObj(toUnicodeString("SignalChanged"));
    ObjEventType* typeObj = eventType.get();

    ObjEventType::PayloadProperty valueProp { toUnicodeString("value"), Value::nilVal(), Value::nilVal() };
    ObjObjectType* spanType = sysTimeSpanType();
    changeEventUsesTimeSpan = spanType != nullptr;
    ObjEventType::PayloadProperty timeProp { toUnicodeString("timestamp"), Value::nilVal(), Value::nilVal() };
    if (changeEventUsesTimeSpan) {
        timeProp.type = Value::objRef(spanType);
        timeProp.initialValue = sysNewTimeSpan(0);
    } else {
        timeProp.type = Value::typeSpecVal(ValueType::Int);
        timeProp.initialValue = Value::intVal(0);
    }
    ObjEventType::PayloadProperty tickProp { toUnicodeString("tick"),
                                             Value::typeSpecVal(ValueType::Int),
                                             Value::intVal(0) };
    auto appendProperty = [&](const ObjEventType::PayloadProperty& property) {
        typeObj->payloadProperties.push_back(property);
        typeObj->propertyLookup[property.name.hashCode()] = typeObj->payloadProperties.size() - 1;
    };

    appendProperty(valueProp);
    appendProperty(timeProp);
    appendProperty(tickProp);

    ObjEventType::PayloadProperty signalProp { toUnicodeString("source_signal"),
                                               Value::typeSpecVal(ValueType::Signal),
                                               Value::nilVal() };
    appendProperty(signalProp);

    changeEventType = Value::objVal(std::move(eventType));
    subscribeCallbacks(currentSignal);
    return asEventType(changeEventType);
}

unique_ptr<ObjSignal, UnreleasedObj> roxal::newSignalObj(ptr<df::Signal> s)
{
    #ifdef DEBUG_BUILD
    return newObj<ObjSignal>(__func__, __FILE__, __LINE__, s);
    #else
    return newObj<ObjSignal>(s);
    #endif
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

unique_ptr<ObjEventType, UnreleasedObj> roxal::newEventTypeObj(const icu::UnicodeString& name,
                                                               Value superType)
{
    #ifdef DEBUG_BUILD
    auto eventType = newObj<ObjEventType>(__func__, __FILE__, __LINE__, name);
    #else
    auto eventType = newObj<ObjEventType>(name);
    #endif
    eventType->superType = superType;
    return eventType;
}

unique_ptr<ObjEventInstance, UnreleasedObj> roxal::newEventInstanceObj(const Value& eventType,
                                                                       std::vector<Value> payload)
{
    #ifdef DEBUG_BUILD
    auto instance = newObj<ObjEventInstance>(__func__, __FILE__, __LINE__, eventType);
    #else
    auto instance = newObj<ObjEventInstance>(eventType);
    #endif
    if (!payload.empty())
        instance->payload = std::move(payload);
    return instance;
}

std::string roxal::objEventTypeToString(const ObjEventType* ev)
{
    if (!ev)
        return std::string("<event>");
    std::string name;
    ev->name.toUTF8String(name);
    if (name.empty() || name == "event")
        return std::string("<event>");
    return std::string("<event ") + name + ">";
}

std::string roxal::objEventInstanceToString(const ObjEventInstance* ev)
{
    if (!ev)
        return std::string("<event instance>");
    std::string typeName;
    if (ev->typeHandle.isAlive() && isEventType(ev->typeHandle)) {
        asEventType(ev->typeHandle)->name.toUTF8String(typeName);
    }
    if (typeName.empty())
        typeName = "event";
    return std::string("<event ") + typeName + " instance>";
}



ObjLibrary::~ObjLibrary()
{
    if (handle)
        dlclose(handle);
}

unique_ptr<ObjLibrary, UnreleasedObj> roxal::newLibraryObj(void* handle)
{
    #ifdef DEBUG_BUILD
    return newObj<ObjLibrary>(__func__, __FILE__, __LINE__, handle);
    #else
    return newObj<ObjLibrary>(handle);
    #endif
}

std::string roxal::objLibraryToString(const ObjLibrary* lib)
{
    std::ostringstream oss;
    oss << "<library " << lib->handle << ">";
    return oss.str();
}

unique_ptr<ObjForeignPtr, UnreleasedObj> roxal::newForeignPtrObj(void* ptr)
{
    #ifdef DEBUG_BUILD
    return newObj<ObjForeignPtr>(__func__, __FILE__, __LINE__, ptr);
    #else
    return newObj<ObjForeignPtr>(ptr);
    #endif
}

std::string roxal::objForeignPtrToString(const ObjForeignPtr* fp)
{
    std::ostringstream oss;
    oss << "<ptr " << fp->ptr << ">";
    return oss.str();
}

unique_ptr<ObjFile, UnreleasedObj> roxal::newFileObj(roxal::ptr<std::fstream> f, bool binary)
{
    #ifdef DEBUG_BUILD
    return newObj<ObjFile>(__func__, __FILE__, __LINE__, std::move(f), binary);
    #else
    return newObj<ObjFile>(std::move(f), binary);
    #endif
}

std::string roxal::objFileToString(const ObjFile* f)
{
    std::ostringstream oss;
    oss << "<file";
    std::lock_guard<std::mutex> lock(f->mutex);
    if (f->file && f->file->is_open()) oss << " open";
    oss << ">";
    return oss.str();
}

unique_ptr<ObjException, UnreleasedObj> roxal::newExceptionObj(Value message,
                                                               Value exType,
                                                               Value stackTrace,
                                                               Value detail)
{
    #ifdef DEBUG_BUILD
    return newObj<ObjException>(__func__, __FILE__, __LINE__, message, exType, stackTrace, detail);
    #else
    return newObj<ObjException>(message, exType, stackTrace, detail);
    #endif
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
        Value funcVal = d->at(Value::stringVal(UnicodeString("function")));
        Value lineVal = d->at(Value::stringVal(UnicodeString("line")));
        Value colVal  = d->at(Value::stringVal(UnicodeString("col")));
        Value fileVal = d->at(Value::stringVal(UnicodeString("filename")));

        UnicodeString funcName = isString(funcVal) ? asStringObj(funcVal)->s : UnicodeString("<script>");
        int line = lineVal.isNumber() ? lineVal.asInt() : -1;
        int col  = colVal.isNumber() ? colVal.asInt() : -1;
        std::string fname = isString(fileVal) ? toUTF8StdString(asStringObj(fileVal)->s) : "";

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
        return Value::vectorVal(vals);
    } else if (isRange(row)) {
        ObjRange* rr = asRange(row);
        int rowCount = rr->length(rows());
        Eigen::MatrixXd m(rowCount, cols());
        for(int i=0;i<rowCount;++i) {
            int target = rr->targetIndex(i, rows());
            if (target >=0 && target < rows())
                m.row(i) = mat.row(target);
        }
        return Value::matrixVal(m);
    }
    throw std::invalid_argument("Matrix indexing subscript must be a number or a range.");
    return Value::nilVal();
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
        return Value::realVal(mat(r,c));
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
        return Value::realVal(mat(rowIdx[0], colIdx[0]));
    } else if (rowIdx.size()==1 && colIdx.size()>1) {
        Eigen::VectorXd vals(colIdx.size());
        for(size_t j=0;j<colIdx.size();++j)
            vals[j] = mat(rowIdx[0], colIdx[j]);
        return Value::vectorVal(vals);
    } else if (rowIdx.size()>1 && colIdx.size()==1) {
        Eigen::VectorXd vals(rowIdx.size());
        for(size_t i=0;i<rowIdx.size();++i)
            vals[i] = mat(rowIdx[i], colIdx[0]);
        return Value::vectorVal(vals);
    } else {
        Eigen::MatrixXd sub(rowIdx.size(), colIdx.size());
        for(size_t i=0;i<rowIdx.size();++i)
            for(size_t j=0;j<colIdx.size();++j)
                sub(i,j) = mat(rowIdx[i], colIdx[j]);
        return Value::matrixVal(sub);
    }
    return Value::nilVal();
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
            return objFunctionToString(asFunction(asClosure(v)->function));
        }
        case ObjType::Upvalue: {
            return "upvalue";
        }
        case ObjType::Bool: {
            return asObjPrimitive(v)->as.boolean ? "true" : "false";
        }
        case ObjType::Int: {
            return std::to_string(asObjPrimitive(v)->as.integer);
        }
        case ObjType::Real: {
            return format("%g", asObjPrimitive(v)->as.real);
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
        case ObjType::File: {
            return objFileToString(asFile(v));
        }
        case ObjType::EventType: {
            return objEventTypeToString(asEventType(v));
        }
        case ObjType::EventInstance: {
            return objEventInstanceToString(asEventInstance(v));
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
            if (isObjPrimitive(v))
                return to_string(asObjPrimitive(v)->as.btype);

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
            ObjObjectType* type = asObjectType(inst->instanceType);
            if (ObjObjectType* timeType = sysTimeType()) {
                if (type == timeType)
                    return sysTimeDefaultString(inst);
            }
            if (ObjObjectType* spanType = sysTimeSpanType()) {
                if (type == spanType)
                    return sysTimeSpanDefaultString(inst);
            }
            return std::string("object "+toUTF8StdString(type->name));
        }
        case ObjType::Actor: {
            ActorInstance* inst = asActorInstance(v);
            return std::string("actor "+toUTF8StdString(asObjectType(inst->instanceType)->name));
        }
        case ObjType::BoundMethod: {
            return objFunctionToString(asFunction(asClosure(asBoundMethod(v)->method)->function));
        }
        case ObjType::BoundNative: {
            return std::string("<native method>");
        }
        case ObjType::Future: {
            Value fv = v;
            fv.resolveFuture();
            return toString(fv);
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



ObjFunction::ObjFunction(const icu::UnicodeString& name,
                         const icu::UnicodeString& packageName,
                         const icu::UnicodeString& moduleName,
                         const icu::UnicodeString& sourceName)
    : arity(0), upvalueCount(0), name(name), strict(false), ownerType(Value::nilVal())
{
    type = ObjType::Function;
    chunk = make_ptr<Chunk>(packageName, moduleName, sourceName);
}

void ObjFunction::clear()
{
    arity = 0;
    upvalueCount = 0;
    name = icu::UnicodeString();
    strict = false;
    ownerType = Value::nilVal();
    moduleType = Value::nilVal();
    if (chunk) {
        chunk->code.clear();
        chunk->constants.clear();
        chunk.reset();
    }
    paramDefaultFunc.clear();
    if (nativeSpec) {
        delete static_cast<FFIWrapper*>(nativeSpec);
        nativeSpec = nullptr;
    }
    nativeDefaults.clear();
}

ObjFunction::~ObjFunction()
{
    clear();
}






ObjNative::ObjNative(NativeFn _function, void* _data,
                     ptr<roxal::type::Type> _funcType,
                     std::vector<Value> defaults)
    : function(_function), data(_data), funcType(_funcType), defaultValues(std::move(defaults))
{
    type = ObjType::Native;
}


unique_ptr<ObjNative, UnreleasedObj> roxal::newNativeObj(NativeFn function, void* data,
                           ptr<roxal::type::Type> funcType,
                           std::vector<Value> defaults)
{
    #ifdef DEBUG_BUILD
    return newObj<ObjNative>(std::string(__func__)+" "+(funcType!=nullptr?funcType->toString():""), __FILE__, __LINE__, function, data, funcType, std::move(defaults));
    #else
    return newObj<ObjNative>(function, data, funcType, std::move(defaults));
    #endif
}




std::unordered_map<uint16_t, roxal::ObjObjectType*> ObjObjectType::enumTypes {};

ObjObjectType::ObjObjectType(const icu::UnicodeString& typeName, bool isactor, bool isinterface, bool isenumeration)
    : name(typeName), isActor(isactor), isInterface(isinterface), isEnumeration(isenumeration), superType(Value::nilVal())
{
    typeValue = ValueType::Object;
    if (isActor)
        typeValue = ValueType::Actor;
    else if (isEnumeration) {
        typeValue = ValueType::Enum;
if (isActor && name!="_DataflowEngine") std::cout << "Actor ObjObjectType created" << std::endl;//!!!
        // register ourselves in the global map of enum types, referenced in the enum values
        enumTypeId = randomUint16(1); // generate unique random id (1..max)
        while (ObjObjectType::enumTypes.find(enumTypeId) != ObjObjectType::enumTypes.end())
            enumTypeId = randomUint16(1);
        //std::cout << "registered new enum id " << enumTypeId << std::endl;
        enumTypes[enumTypeId] = this;
    }
}

unique_ptr<ObjObjectType, UnreleasedObj> roxal::newObjectTypeObj(const icu::UnicodeString& typeName, bool isActor, bool isInterface, bool isEnumeration)
{
    #ifdef DEBUG_BUILD
    return newObj<ObjObjectType>((std::string(__func__)+" "+toUTF8StdString(typeName)), __FILE__, __LINE__, typeName, isActor, isInterface, isEnumeration);
    #else
    return newObj<ObjObjectType>(typeName, isActor, isInterface, isEnumeration);
    #endif
}



ObjModuleType::ObjModuleType(const icu::UnicodeString& typeName)
    : name(typeName)
{
    typeValue = ValueType::Module;
    fullName = typeName;
}

unique_ptr<ObjModuleType, UnreleasedObj> roxal::newModuleTypeObj(const icu::UnicodeString& typeName)
{
    #ifdef DEBUG_BUILD
    auto mt = newObj<ObjModuleType>(std::string(__func__)+" "+toUTF8StdString(typeName), __FILE__, __LINE__, typeName);
    #else
    auto mt = newObj<ObjModuleType>(typeName);
    #endif
    return mt;
}

ObjModuleType::~ObjModuleType() {}




ObjectInstance::ObjectInstance(const Value& objectType)
{
    type = ObjType::Instance;
    debug_assert_msg(isObjectType(objectType),
                     "ObjectInstance created with object type");
    instanceType = objectType.strongRef();

    ObjObjectType* ot = asObjectType(objectType);
    for(const auto& property : ot->properties) {
        const auto& prop { property.second };
        auto propInitialvalue { prop.initialValue };
        // Clone reference types to avoid sharing between instances
        if (!propInitialvalue.isPrimitive()) {
            // Special handling for signals - only clone clocks and source signals
            if (isSignal(propInitialvalue)) {
                auto sig = asSignal(propInitialvalue)->signal;
                if (!sig) {
                    throw std::runtime_error("cannot clone null signal");
                }
                if (sig->isClockSignal()) {
                    // Create new independent clock with same frequency
                    auto newSig = df::Signal::newClockSignal(sig->frequency());
                    propInitialvalue = Value::signalVal(newSig);
                } else if (sig->isSourceSignal()) {
                    // Create new independent source signal with same frequency/initial value
                    auto newSig = df::Signal::newSourceSignal(sig->frequency(), sig->lastValue());
                    propInitialvalue = Value::signalVal(newSig);
                } else {
                    // Derived signals cannot be used as member defaults
                    throw std::runtime_error("cannot use derived signals as member defaults");
                }
            } else {
                propInitialvalue = propInitialvalue.clone();
            }
        }
        auto hash = prop.name.hashCode();
        auto& slot = properties[hash];
        slot.clearSignal();
        slot.value = propInitialvalue;
    }
}

ObjectInstance::~ObjectInstance() {}

Value ObjectInstance::getProperty(const icu::UnicodeString& name) const
{
    auto it = properties.find(name.hashCode());
    if (it != properties.end())
        return it->second.value;
    return Value::nilVal();
}

void ObjectInstance::setProperty(const icu::UnicodeString& name, Value value)
{
    properties[name.hashCode()].assign(value);
}

Value ObjectInstance::ensurePropertySignal(int32_t nameHash, const std::string& signalName)
{
    auto it = properties.find(nameHash);
    if (it == properties.end())
        return Value::nilVal();
    return it->second.ensureSignal(signalName);
}


unique_ptr<ObjectInstance, UnreleasedObj> roxal::newObjectInstance(const Value& objectType)
{
    #ifdef DEBUG_BUILD
    debug_assert_msg(isObjectType(objectType),
                     "objectInstanceVal called with object type");
    return newObj<ObjectInstance>(__func__, __FILE__, __LINE__, objectType);
    #else
    return newObj<ObjectInstance>(objectType);
    #endif
}


unique_ptr<Obj, UnreleasedObj> ObjectInstance::clone() const
{
    auto newobj = newObjectInstance(instanceType);

    for(const auto& index_value : properties) {
        const auto index { index_value.first };
        const auto& slot { index_value.second };
        const Value& value { slot.value };

        auto& targetSlot = newobj->properties[index];
        targetSlot.clearSignal();

        if (isActorInstance(value))
            throw std::runtime_error("clone of type actor unsupported");

        targetSlot.value = value.clone();

    }

    return newobj;
}



ActorInstance::ActorInstance(ActorInstance::UninitializedTag)
{
    type = ObjType::Actor;
    instanceType = Value::nilVal();
}

ActorInstance::ActorInstance(const Value& objectType)
    : ActorInstance(UninitializedTag{})
{
    initialize(objectType);
}

void ActorInstance::initialize(const Value& objectType)
{
    debug_assert_msg(isObjectType(objectType) && asObjectType(objectType)->isActor,
                     "ActorInstance created with actor type");
    instanceType = objectType.strongRef();
    properties.clear();

    // initialize instance properties from actor type definition
    ObjObjectType* ot = asObjectType(objectType);
    for(const auto& property : ot->properties) {
        const auto& prop { property.second };
        auto propInitialvalue { prop.initialValue };
        // Clone reference types to avoid sharing between instances
        if (!propInitialvalue.isPrimitive()) {
            // Special handling for signals - only clone clocks and source signals
            if (isSignal(propInitialvalue)) {
                auto sig = asSignal(propInitialvalue)->signal;
                if (!sig) {
                    throw std::runtime_error("cannot clone null signal");
                }
                if (sig->isClockSignal()) {
                    // Create new independent clock with same frequency
                    auto newSig = df::Signal::newClockSignal(sig->frequency());
                    propInitialvalue = Value::signalVal(newSig);
                } else if (sig->isSourceSignal()) {
                    // Create new independent source signal with same frequency/initial value
                    auto newSig = df::Signal::newSourceSignal(sig->frequency(), sig->lastValue());
                    propInitialvalue = Value::signalVal(newSig);
                } else {
                    // Derived signals cannot be used as member defaults
                    throw std::runtime_error("cannot use derived signals as member defaults");
                }
            } else {
                propInitialvalue = propInitialvalue.clone();
            }
        }
        auto hash = prop.name.hashCode();
        auto& slot = properties[hash];
        slot.clearSignal();
        slot.value = propInitialvalue;
    }
}

ActorInstance::~ActorInstance()
{
    // The VM ensures the worker thread has already been joined (or detached in
    // the self-join case) before destroying the actor instance, so clearing the
    // weak thread reference here is just bookkeeping.
    thread.reset();
}

void ActorInstance::trace(ValueVisitor& visitor) const
{
    visitor.visit(instanceType);
    for (const auto& entry : properties) {
        visitor.visit(entry.second.value);
        visitor.visit(entry.second.signal);
    }
    callQueue.forEach([&visitor](const MethodCallInfo& info) {
        visitor.visit(info.callee);
        for (const auto& arg : info.args) {
            visitor.visit(arg);
        }
        visitor.visit(info.returnFuture);
    });
}

void ActorInstance::dropReferences()
{
    instanceType = Value::nilVal();
    for (auto& entry : properties) {
        entry.second.value = Value::nilVal();
        entry.second.signal = Value::nilVal();
    }
    properties.clear();
    while (!callQueue.empty()) {
        MethodCallInfo info = callQueue.pop();
        info.callee = Value::nilVal();
        info.args.clear();
        info.returnFuture = Value::nilVal();
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
    callInfo.returnFuture = Value::nilVal();

    if (isBoundMethod(callee)) {
        auto funcObj = asFunction(asClosure(asBoundMethod(callee)->method)->function);
        if (funcObj->funcType.has_value()) {
            ptr<roxal::type::Type> funcType { funcObj->funcType.value() };
            assert(funcType->func.has_value());
            if (!funcType->func.value().isProc) {
                callInfo.returnPromise = make_ptr<std::promise<Value>>();
                std::shared_future<Value> sf = callInfo.returnPromise->get_future().share();
                callInfo.returnFuture = Value::objVal(newFutureObj(sf));
            }
        }
    }
    else if (isBoundNative(callee)) {
        // For builtin methods, check if it's a proc or func
        auto bound = asBoundNative(callee);

        // Only create a return promise if it's NOT a proc (i.e., it's a func)
        if (!bound->isProc) {
            callInfo.returnPromise = make_ptr<std::promise<Value>>();
            std::shared_future<Value> sf = callInfo.returnPromise->get_future().share();
            callInfo.returnFuture = Value::objVal(newFutureObj(sf));
        }

        // Convert arguments into parameter order so actor thread receives a complete list.
        bool needsNormalization = callSpec.namedArgs();

        if (needsNormalization && bound->funcType && bound->funcType->func.has_value()) {
            auto& funcInfo = bound->funcType->func.value();
            std::vector<Value> originalArgs = callInfo.args;
            std::vector<Value> normalized;
            normalized.reserve(funcInfo.params.size());

            std::vector<int8_t> positions = callSpec.paramPositions(bound->funcType, false);
            for (size_t pi = 0; pi < funcInfo.params.size(); ++pi) {
                Value arg = Value::nilVal();
                if (pi < positions.size()) {
                    int pos = positions[pi];
                    if (pos >= 0 && static_cast<size_t>(pos) < originalArgs.size())
                        arg = originalArgs[pos];
                }
                if (arg.isNil() && pi < bound->defaultValues.size())
                    arg = bound->defaultValues[pi];
                if (!arg.isPrimitive())
                    arg = arg.clone();
                normalized.push_back(arg);
            }

            std::reverse(normalized.begin(), normalized.end());
            callInfo.args = std::move(normalized);
            callInfo.callSpec = CallSpec(static_cast<uint16_t>(callInfo.args.size()));
        }
    }

    callQueue.push(callInfo);

    queueConditionVar.notify_one();

    Value futureReturn {}; // nil by default
    if (!callInfo.returnFuture.isNil())
        futureReturn = callInfo.returnFuture;
#ifdef DEBUG_BUILD
    else {
        if (isBoundMethod(callee)) {
            auto fn = asFunction(asClosure(asBoundMethod(callee)->method)->function);
            std::string name;
            fn->name.toUTF8String(name);
            std::cerr << "queueCall: no future created for method '" << name << "'" << std::endl;
        }
    }
#endif

    return futureReturn;
}


unique_ptr<ActorInstance, UnreleasedObj> roxal::newActorInstance(ActorInstance::UninitializedTag tag)
{
    #ifdef DEBUG_BUILD
    return newObj<ActorInstance>(std::string(__func__), __FILE__, __LINE__, tag);
    #else
    return newObj<ActorInstance>(tag);
    #endif
}

unique_ptr<ActorInstance, UnreleasedObj> roxal::newActorInstance(const Value& objectType)
{
    #ifdef DEBUG_BUILD
    debug_assert_msg(isObjectType(objectType) && asObjectType(objectType)->typeValue == ValueType::Actor,
                     "newActorInstance called with actor type");
    auto actor = newObj<ActorInstance>(std::string(__func__) +
                                       toUTF8StdString(asObjectType(objectType)->name),
                                       __FILE__, __LINE__, ActorInstance::UninitializedTag{});
    #else
    auto actor = newObj<ActorInstance>(ActorInstance::UninitializedTag{});
    #endif
    actor->initialize(objectType);
    return actor;
}

ObjBoundMethod::ObjBoundMethod(const Value& instance, const Value& closure)
    : receiver(instance), method(closure.weakRef())
{
    debug_assert_msg(isClosure(closure), "ObjBoundMethod constructed with non-closure");
    type = ObjType::BoundMethod;
}

ObjBoundMethod::~ObjBoundMethod() {}






bool roxal::objsEqual(const Value& l, const Value& r)
{
    if (!l.isObj() || !r.isObj())
        return false;

    if (objType(l) != objType(r))
        return false;

    switch (objType(l)) {
        case ObjType::String: {
            auto ls = asStringObj(l);
            auto rs = asStringObj(r);
            if (ls == rs) // identical object
                return true;

            // Trust hash.  Possible different strings with hash collisions will
            // compare as equal (low probability).
            // TODO: consider doing full char comparison for equality if hashes match
            return ls->hash == rs->hash;
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
    case ObjType::File: return "file";
    case ObjType::EventType: return "event";
    case ObjType::EventInstance: return "event";
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

    Value l1 { Value::listVal(std::vector<Value>{i0,i1,i2}) };

    assert(isList(l1));
    assert(!l1.isNil());
    ObjList* lp = static_cast<ObjList*>(l1.asObj());
    assert(lp->elts.size() == 3);

    Value l2 = l1;
    assert(isList(l2));
    assert(!l2.isNil());

    assert(l1 == l2);
    assert(!(l2 == Value::nilVal()));
}
#endif
