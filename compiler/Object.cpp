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

#ifdef ROXAL_ENABLE_ONNX
#include "CudaRuntime.h"
#endif

#include <core/types.h>
#include "VM.h"
#include "FFI.h"
#include "Value.h"
#include "Thread.h"
#ifdef ROXAL_COMPUTE_SERVER
#include "ComputeConnection.h"
#endif
#include "dataflow/Signal.h"
#include "dataflow/DataflowEngine.h"
#include "Object.h"
#include "ModuleSys.h"

using namespace roxal;

namespace {
inline int32_t checkedInt32(int64_t v, const char* what) {
    if (v < std::numeric_limits<int32_t>::min() || v > std::numeric_limits<int32_t>::max())
        throw std::runtime_error(what);
    return static_cast<int32_t>(v);
}
}


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
    uint8_t ic = t.isConst ? 1 : 0;
    out.write(reinterpret_cast<char*>(&ic),1);
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
                uint8_t vd = p->variadic?1:0; out.write(reinterpret_cast<char*>(&vd),1);
            }
        }
        uint32_t rc = ft.returnTypes.size();
        out.write(reinterpret_cast<char*>(&rc),4);
        for(const auto& rt : ft.returnTypes)
            writeTypeInfo(out, *rt);
    } else if (t.builtin == type::BuiltinType::Func) {
        uint8_t hasFunc = 0; out.write(reinterpret_cast<char*>(&hasFunc),1);
    }
    // Serialize obj.name for Object/Actor types (used by runtime param conversion)
    if ((t.builtin == type::BuiltinType::Object || t.builtin == type::BuiltinType::Actor)
        && t.obj.has_value()) {
        uint8_t hasObj = 1; out.write(reinterpret_cast<char*>(&hasObj),1);
        writeString(out, t.obj.value().name);
    } else if (t.builtin == type::BuiltinType::Object || t.builtin == type::BuiltinType::Actor) {
        uint8_t hasObj = 0; out.write(reinterpret_cast<char*>(&hasObj),1);
    }
}

ptr<type::Type> readTypeInfo(std::istream& in) {
    uint8_t b; in.read(reinterpret_cast<char*>(&b),1);
    auto t = make_ptr<type::Type>(static_cast<type::BuiltinType>(b));
    uint8_t ic; in.read(reinterpret_cast<char*>(&ic),1);
    t->isConst = ic != 0;
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
                    uint8_t vd; in.read(reinterpret_cast<char*>(&vd),1); param.variadic = vd!=0;
                    ft.params[i] = param;
                }
            }
            uint32_t rc; in.read(reinterpret_cast<char*>(&rc),4);
            for(uint32_t i=0;i<rc;i++)
                ft.returnTypes.push_back(readTypeInfo(in));
        }
    }
    // Deserialize obj.name for Object/Actor types
    if (t->builtin == type::BuiltinType::Object || t->builtin == type::BuiltinType::Actor) {
        uint8_t hasObj; in.read(reinterpret_cast<char*>(&hasObj),1);
        if (hasObj) {
            t->obj = type::Type::ObjectType{};
            t->obj->name = readString(in);
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
        } else if (std::holds_alternative<int64_t>(n->num)) {
            uint8_t ty=2; out.write(reinterpret_cast<char*>(&ty),1);
            int64_t v=std::get<int64_t>(n->num); out.write(reinterpret_cast<char*>(&v),8);
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
            } else if (ty==2) {
                int64_t v; in.read(reinterpret_cast<char*>(&v),8);
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
        case ObjType::Orient: return ValueType::Orient;
        case ObjType::Tensor: return ValueType::Tensor;
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
    // Clean up MVCC version chain and snapshot token
    cleanupMVCC();
}

unique_ptr<Obj, UnreleasedObj> Obj::shallowClone() const
{
    // Default: types that don't support shallow cloning return nullptr
    return nullptr;
}

void Obj::saveVersion()
{
    // Version save deduplication: skip if no new snapshot was created since last save
    if (control->lastSaveEpoch >= latestSnapshotCreationEpoch.load(std::memory_order_acquire))
        return;

    auto snap = shallowClone();
    if (!snap)
        return; // type doesn't support shallow cloning (e.g. immutable types)

    // Transfer ownership of the shallow clone to the version node.
    // The clone gets a strong ref to keep it alive.
    Obj* snapObj = snap.release();
    snapObj->incRef();

    auto* ver = new ObjVersion();
    ver->epoch = control->writeEpoch.load(std::memory_order_relaxed);
    ver->snapshot = snapObj;

    // CAS-prepend to the version chain (lock-free)
    ObjVersion* head = control->versionChain.load(std::memory_order_relaxed);
    do {
        ver->prev = head;
    } while (!control->versionChain.compare_exchange_weak(head, ver,
             std::memory_order_release, std::memory_order_relaxed));

    control->lastSaveEpoch = globalWriteEpoch.load(std::memory_order_relaxed);
}


void Obj::cleanupMVCC()
{
    // Free version chain
    ObjVersion* ver = control->versionChain.load(std::memory_order_relaxed);
    control->versionChain.store(nullptr, std::memory_order_relaxed);
    while (ver) {
        ObjVersion* prev = ver->prev;
        if (ver->snapshot) {
            ver->snapshot->decRef();
            ver->snapshot = nullptr;
        }
        delete ver;
        ver = prev;
    }

    // Release snapshot token (for frozen clones)
    if (control->snapshotToken) {
        if (control->snapshotToken->decRef()) {
            // Last frozen clone for this snapshot — decrement global count
            snapshotEpochTracker.remove(control->snapshotToken->epoch);
            activeSnapshotCount.fetch_sub(1, std::memory_order_relaxed);
            delete control->snapshotToken;
        }
        control->snapshotToken = nullptr;
    }
}


void Obj::trimVersionChain(uint64_t minEpoch)
{
    ObjVersion* chain = control->versionChain.load(std::memory_order_relaxed);
    if (!chain)
        return;

    // No active snapshots — discard entire chain
    if (minEpoch == UINT64_MAX) {
        control->versionChain.store(nullptr, std::memory_order_relaxed);
        control->lastSaveEpoch = 0;
        while (chain) {
            ObjVersion* prev = chain->prev;
            if (chain->snapshot) {
                chain->snapshot->decRef();
                chain->snapshot = nullptr;
            }
            delete chain;
            chain = prev;
        }
        return;
    }

    // Find the newest version with epoch < minEpoch (the floor version).
    // Keep it and everything newer; discard everything older.
    ObjVersion* cur = chain;
    ObjVersion* floor = nullptr;
    while (cur) {
        if (cur->epoch < minEpoch) {
            floor = cur;
            break;  // chain is newest-first, so first match is the newest floor
        }
        cur = cur->prev;
    }

    if (!floor)
        return;  // all versions are >= minEpoch, nothing to trim

    // Free everything after the floor version
    ObjVersion* toFree = floor->prev;
    floor->prev = nullptr;
    while (toFree) {
        ObjVersion* prev = toFree->prev;
        if (toFree->snapshot) {
            toFree->snapshot->decRef();
            toFree->snapshot = nullptr;
        }
        delete toFree;
        toFree = prev;
    }
}


/// Check whether the object graph rooted at `root` is isolated: every mutable
/// interior object is referenced only from within the graph (no external aliases).
///
/// Algorithm (two-pass):
///   Phase 1: Traverse from root via trace(), collecting all reachable Obj*
///            into a set.
///   Phase 2: For each reachable object, trace again, counting how many
///            references land on other reachable objects (internal refs).
///            Then verify: for each mutable container (List, Dict, Instance),
///            strong_count == internal_ref_count.
///
/// The root is excluded from the check — its sole-ownership is verified by the
/// caller (strong <= 1 in createFrozenSnapshot, <= 2 in queueCall).
///
/// Immutable types (String, Function, ObjectType, etc.) are skipped: they cannot
/// be mutated through an alias, so sharing is safe.
///
/// Performance: O(V + E) where V = reachable objects, E = reference edges.
/// For the common case (list of primitives), V = 1 and returns true immediately.
bool roxal::isIsolatedGraph(Obj* root)
{
    if (!root) return true;

    // Phase 1: Collect all reachable objects via DFS
    std::unordered_set<Obj*> reachable;
    reachable.insert(root);

    struct CollectVisitor : ValueVisitor {
        std::unordered_set<Obj*>& reachable;
        std::vector<Obj*> worklist;

        explicit CollectVisitor(std::unordered_set<Obj*>& r) : reachable(r) {}

        void visit(const Value& value) override {
            if (!value.isObj() || value.isWeak()) return;
            Obj* obj = value.asObj();
            if (!obj || !obj->control) return;
            if (reachable.count(obj)) return;
            reachable.insert(obj);
            worklist.push_back(obj);
        }

        void drain() {
            while (!worklist.empty()) {
                Obj* current = worklist.back();
                worklist.pop_back();
                current->trace(*this);
            }
        }
    };

    CollectVisitor collector(reachable);
    root->trace(collector);
    collector.drain();

    // If only the root is reachable (e.g. list of primitives), trivially isolated.
    if (reachable.size() <= 1) return true;

    // Phase 2: Count internal references for each reachable object.
    std::unordered_map<Obj*, int32_t> internalRefs;

    struct CountVisitor : ValueVisitor {
        const std::unordered_set<Obj*>& reachable;
        std::unordered_map<Obj*, int32_t>& internalRefs;

        CountVisitor(const std::unordered_set<Obj*>& r,
                     std::unordered_map<Obj*, int32_t>& ir)
            : reachable(r), internalRefs(ir) {}

        void visit(const Value& value) override {
            if (!value.isObj() || value.isWeak()) return;
            Obj* obj = value.asObj();
            if (!obj || !obj->control) return;
            if (reachable.count(obj))
                internalRefs[obj]++;
        }
    };

    CountVisitor counter(reachable, internalRefs);
    for (Obj* obj : reachable)
        obj->trace(counter);

    // Check isolation: every mutable interior object must have no external aliases.
    for (Obj* obj : reachable) {
        if (obj == root) continue;
        if (!isMutableRefContainerType(obj->type)) continue;

        int32_t strong = obj->control->strong.load(std::memory_order_acquire);
        int32_t internal = 0;
        auto it = internalRefs.find(obj);
        if (it != internalRefs.end()) internal = it->second;

        if (strong > internal)
            return false; // has external alias
    }

    return true;
}


Value roxal::createFrozenSnapshot(const Value& v)
{
    // Const-to-const passthrough: already frozen, reuse as-is
    if (v.isConst()) return v;

    // Non-reference types are inherently immutable
    if (!v.isObj()) return v;

    Obj* obj = v.asObj();
    if (!obj) return v;

    // Sole-owner fast path: if no other live references exist AND the root has
    // no object-type children, freeze in-place (zero-copy).
    // When object-type children exist, we must fall through to the shallow-clone
    // + MVCC path because interior objects may have external aliases that could
    // mutate them, breaking the immutability guarantee of the frozen snapshot.
    if (obj->control && obj->control->strong.load(std::memory_order_acquire) <= 1) {
        struct HasObjChildVisitor : ValueVisitor {
            bool found = false;
            void visit(const Value& v) override {
                if (!found && v.isObj() && !v.isWeak())
                    found = true;
            }
        };
        HasObjChildVisitor checker;
        obj->trace(checker);
        if (!checker.found)
            return v.constRef();
        // Fall through to shallow-clone + MVCC
    }

    // Shallow-clone the root object
    auto snap = obj->shallowClone();
    if (!snap) {
        // Type doesn't support shallow cloning (e.g. immutable type like ObjString).
        // Just return with const bit set.
        return v.constRef();
    }

    // Allocate SnapshotToken
    uint64_t epoch = globalWriteEpoch.load(std::memory_order_acquire);
    auto* token = new SnapshotToken(epoch);

    // Configure the frozen clone before transferring ownership
    Obj* frozenObj = snap.get();
    frozenObj->control->snapshotToken = token;
    // The frozen clone's writeEpoch is irrelevant (it's never mutated),
    // but set it to 0 so it's clearly distinguishable from live objects.
    frozenObj->control->writeEpoch.store(0, std::memory_order_relaxed);

    // Update global snapshot tracking
    // latestSnapshotCreationEpoch must be updated BEFORE activeSnapshotCount
    // (see proposal: memory ordering for version save deduplication)
    latestSnapshotCreationEpoch.store(epoch, std::memory_order_release);
    snapshotEpochTracker.add(epoch);
    activeSnapshotCount.fetch_add(1, std::memory_order_release);

    // Transfer ownership to Value (objVal does incRef via Value constructor)
    Value result = Value::objVal(std::move(snap));
    return result.constRef();
}


/// Find the correct version of an object for a given snapshot epoch.
/// Returns the snapshot Obj* from the version chain, or nullptr if the
/// object's current state is valid for this epoch.
static Obj* findVersionForEpoch(Obj* obj, uint64_t snapshotEpoch)
{
    uint64_t objEpoch = obj->control->writeEpoch.load(std::memory_order_acquire);
    if (objEpoch < snapshotEpoch) {
        // Object was not mutated since the snapshot — current state is valid
        return nullptr;
    }

    // Object was mutated at or after the snapshot epoch.
    // Walk version chain to find newest version with epoch < snapshotEpoch.
    ObjVersion* ver = obj->control->versionChain.load(std::memory_order_acquire);
    ObjVersion* best = nullptr;
    while (ver) {
        if (ver->epoch < snapshotEpoch) {
            if (!best || ver->epoch > best->epoch)
                best = ver;
            break;  // chain is newest-first, so first match with epoch < snapshotEpoch is best
        }
        ver = ver->prev;
    }

    // Invariant: a version must exist — the oldest version's epoch is the object's
    // writeEpoch at its first-ever mutation (0 for newly created objects), which is
    // ≤ any valid snapshot epoch ≥ 1.
    debug_assert_msg(best != nullptr, "MVCC: no version found for epoch");
    return best ? best->snapshot : nullptr;
}


Value roxal::resolveConstChild(const Value& child, SnapshotToken* token, Value* cacheSlot)
{
    // Primitives and non-objects: return directly (no cloning needed)
    if (!child.isObj()) return child;

    // Already resolved (cached from prior access): return as-is
    if (child.isConst()) return child;

    Obj* childObj = child.asObj();
    if (!childObj) return child;

    uint64_t epoch = token->epoch;

    // Check the per-snapshot cloneMap for alias/cycle preservation.
    // If we've already materialized a frozen clone for this object in this
    // snapshot, reuse it to preserve 'is'-identity.
    // The cloneMap stores weak refs — the parent container/object holds the
    // strong ref (via cacheSlot or container element caching).
    auto it = token->cloneMap.find(childObj);
    if (it != token->cloneMap.end()) {
        Value weak = it->second;
        if (weak.isAlive()) {
            // Promote weak ref to strong for return, preserving const bit
            Value cached = weak.strongRef().constRef();
            if (cacheSlot) *cacheSlot = cached;
            return cached;
        }
        // Weak ref expired — remove stale entry and re-create below
        token->cloneMap.erase(it);
    }

    // Determine the source state to clone from.
    // Try version chain first (immutable snapshots — no lock needed).
    Obj* versionObj = findVersionForEpoch(childObj, epoch);
    unique_ptr<Obj, UnreleasedObj> snap;
    if (versionObj) {
        // Version chain has the correct pre-mutation state — clone from immutable version
        snap = versionObj->shallowClone();
    } else {
        // Current state appears valid for this epoch. Lock and re-verify,
        // because the owning thread may be mid-mutation (between ensureUnique
        // and writeEpoch bump) — see CowGuard in mutation methods.
        childObj->control->lockCow();
        uint64_t currentEpoch = childObj->control->writeEpoch.load(std::memory_order_acquire);
        if (currentEpoch < epoch) {
            // Still valid under lock — clone current state
            snap = childObj->shallowClone();
            childObj->control->unlockCow();
        } else {
            // Race: mutation happened between our initial check and lock acquisition.
            // The version chain now has the pre-mutation state we need.
            childObj->control->unlockCow();
            versionObj = findVersionForEpoch(childObj, epoch);
            debug_assert_msg(versionObj != nullptr, "MVCC: no version found after race in resolveConstChild");
            snap = versionObj->shallowClone();
        }
    }
    if (!snap) {
        // Immutable type (e.g. ObjString) — just return with const bit
        Value result = child.constRef();
        if (cacheSlot) *cacheSlot = result;
        return result;
    }

    // Configure frozen child before transferring ownership
    Obj* frozenChild = snap.get();

    // Attach to the same snapshot token (increment refcount)
    token->incRef();
    frozenChild->control->snapshotToken = token;
    frozenChild->control->writeEpoch.store(0, std::memory_order_relaxed);

    // Transfer ownership to Value and set const bit
    Value result = Value::objVal(std::move(snap)).constRef();

    // Register weak ref in cloneMap for alias/identity preservation.
    // The parent container holds the strong ref (via cacheSlot or element caching).
    token->cloneMap[childObj] = result.weakRef();

    // Cache in parent's slot
    if (cacheSlot) *cacheSlot = result;

    return result;
}





// interned strings table
static atomic_unordered_map<uint64_t, ObjString*> strings {};

namespace {

// 64-bit FNV-1a over UTF-16 code units, with optional salt to rehash on collision.
uint64_t fnv1a64(const UnicodeString& s, uint64_t salt = 0)
{
    uint64_t hash = 1469598103934665603ULL ^ salt;
    const char16_t* buf = s.getBuffer();
    const int32_t len = s.length();
    for (int32_t i = 0; i < len; ++i) {
        hash ^= static_cast<uint16_t>(buf[i]);
        hash *= 1099511628211ULL;
    }
    return hash;
}

// Blend multiple cheap discriminators into a 64-bit key and allow deterministic
// perturbation via salt so we can walk away from an observed collision.
uint64_t computeInternKey(const UnicodeString& s, uint32_t salt = 0)
{
    const uint64_t base = fnv1a64(s);
    const uint64_t icuHash = static_cast<uint32_t>(s.hashCode());
    const uint64_t len = static_cast<uint32_t>(s.length());
    const char16_t c0 = s.length() > 0 ? s.charAt(0) : 0;
    const char16_t c1 = s.length() > 1 ? s.charAt(1) : 0;
    const char16_t c2 = s.length() > 1 ? s.charAt(s.length() - 2) : 0;
    const char16_t c3 = s.length() > 0 ? s.charAt(s.length() - 1) : 0;
    uint64_t edge = (static_cast<uint64_t>(c0) << 48) ^
                    (static_cast<uint64_t>(c1) << 32) ^
                    (static_cast<uint64_t>(c2) << 16) ^
                    static_cast<uint64_t>(c3);
    uint64_t mixed = base ^ (icuHash * 0x9e3779b97f4a7c15ULL) ^ (len * 0xbf58476d1ce4e5b9ULL) ^ edge;
    mixed ^= static_cast<uint64_t>(salt) * 0x94d049bb133111ebULL;
    return mixed;
}

} // namespace

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
    std::vector<uint64_t> deadHashes;
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

    for (uint64_t hash : deadHashes) {
        strings.erase(hash);
    }
}

ObjString::ObjString()
    : s(), internKey(0)
{
    type = ObjType::String;
    hash = 0;
}

ObjString::ObjString(const UnicodeString& us)
    :  s(us), internKey(0)
{
    type = ObjType::String;
    hash = s.hashCode();
}


ObjString::~ObjString()
{
    // remove ourself from the strings intern table
    if (internKey != 0 || !s.isEmpty()) {
        auto existing = strings.lookup(internKey);
        if (existing.has_value() && existing.value() == this)
            strings.erase(internKey);
    }
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


unique_ptr<ObjString, UnreleasedObj> roxal::newObjString(const UnicodeString& s, bool* wasInterned)
{
    if (wasInterned) *wasInterned = false;

    uint32_t attempt = 0;
    while (true) {
        uint64_t key = computeInternKey(s, attempt);
        auto existing = strings.lookup(key);
        if (existing.has_value()) {
            ObjString* objStr = existing.value();
            if (objStr && objStr->s == s) {
                // Atomically try to take a strong reference to prevent
                // the string from being freed by another thread before
                // the caller's Value::incRef() runs.
                if (objStr->tryIncRef()) {
                    if (wasInterned) *wasInterned = true;
                    return unique_ptr<ObjString, UnreleasedObj>(objStr);
                }
                // String is dying (strong == 0); remove stale entry and create new
                strings.erase(key);
                break;
            }
            ++attempt;
            continue;
        }
        break;
    }

    // create new
    #ifdef DEBUG_BUILD
    auto objStr = newObj<ObjString>(std::string(__func__)+" '" + toUTF8StdString(s) + "'",__FILE__,__LINE__,s);
    #else
    auto objStr = newObj<ObjString>(s);
    #endif
    uint64_t key = computeInternKey(s, 0);
    // Check for collision with a different string at this key
    uint32_t attempt2 = 0;
    while (true) {
        key = computeInternKey(s, attempt2);
        auto existing = strings.lookup(key);
        if (existing.has_value()) {
            ObjString* other = existing.value();
            if (other && other->s != s) {
                ++attempt2;
                continue;
            }
        }
        break;
    }
    objStr->internKey = key;
    strings.store(key, objStr.get());
    return objStr;
}

void roxal::updateInternedString(ObjString* obj, const UnicodeString& newVal)
{
    if (!obj) return;
    if (obj->internKey != 0)
        strings.erase(obj->internKey);
    obj->s = newVal;
    obj->hash = obj->s.hashCode();

    uint32_t attempt = 0;
    while (true) {
        uint64_t key = computeInternKey(obj->s, attempt);
        auto existing = strings.lookup(key);
        if (existing.has_value()) {
            ObjString* other = existing.value();
            if (other == obj || (other && other->s == obj->s)) {
                obj->internKey = key;
                return;
            }
            ++attempt;
            continue;
        }
        obj->internKey = key;
        strings.store(key, obj);
        break;
    }
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
    : vec_(make_ptr<Eigen::VectorXd>(values))
{
    type = ObjType::Vector;
}

ObjVector::ObjVector(int32_t size)
    : vec_(make_ptr<Eigen::VectorXd>(size))
{
    type = ObjType::Vector;
    vec_->setZero();
}

Value ObjVector::index(const Value& i) const
{
    if (i.isNumber()) {
        auto index = i.asInt();
        if (index < 0 || index >= length())
            throw std::invalid_argument("Vector index out-of-range.");
        return Value::realVal(vec()[index]);
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
                elts.push_back(vec()[targetIndex]);
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

Eigen::VectorXd& ObjVector::vecMut()
{
    ensureMutable();
    if (activeSnapshotCount.load(std::memory_order_relaxed) > 0) saveVersion();
    ensureUnique();
    control->writeEpoch.store(globalWriteEpoch.fetch_add(1, std::memory_order_relaxed), std::memory_order_release);
    return *vec_;
}

void ObjVector::setIndex(const Value& i, const Value& v)
{
    ensureMutable();
    if (activeSnapshotCount.load(std::memory_order_relaxed) > 0) saveVersion();
    ensureUnique();  // COW: copy before mutation
    if (i.isNumber()) {
        auto index = i.asInt();
        if (index < 0 || index >= length())
            throw std::invalid_argument("Vector index out-of-range.");
        Value rv = toType(ValueType::Real, v, /*strict=*/false);
        (*vec_)[index] = rv.asReal();
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
                    (*vec_)[targetIndex] = rhsVec->vec()[j];
            }
        }
    }
    else {
        throw std::invalid_argument("Vector indexing subscript must be a number or a range (not "+to_string(i.type())+").");
    }
    control->writeEpoch.store(globalWriteEpoch.fetch_add(1, std::memory_order_relaxed), std::memory_order_release);
}



int32_t ObjRange::length(int32_t targetLen) const
{
    // handle common case of ints first
    if (   (start.isInt() || start.isNil())
         && (stop.isInt() || stop.isNil())
         && (step.isInt() || step.isNil())) {

        int64_t stepi = step.isNil() ? 1 : step.asInt();

        if (stepi > 0) { // normal order
            int64_t starti = start.isNil() ? 0 : start.asInt();
            int64_t stopi = stop.isNil() ? targetLen : stop.asInt();
            if (starti < 0) starti = targetLen + starti;
            if (stopi < 0) stopi = targetLen + stopi;

            if (!closed) {
                //return abs(stopi - starti)/stepi;
                if (starti >= stopi) return 0;
                return checkedInt32((stopi - starti - 1) / stepi + 1,
                                    "range length overflow");
            }
            else {
                //return abs(stopi - starti + 1)/stepi;
                if (starti > stopi) return 0;
                return checkedInt32(((stopi+1) - starti - 1) / stepi + 1,
                                    "range length overflow");
            }
        }
        else { // reverse order e.g. -2:1:-2
            int64_t starti = start.isNil() ? targetLen-1 : start.asInt();
            int64_t stopi = stop.isNil() ? (closed?0:-1) : stop.asInt();
            if (starti < 0) starti = targetLen + starti;
            if ((stopi < 0) && !stop.isNil()) stopi = targetLen + stopi;

            if (!closed)
                return checkedInt32((starti - stopi - 1)/-stepi + 1,
                                    "range length overflow");
            else
                return checkedInt32((starti - (stopi-1) - 1)/-stepi + 1,
                                    "range length overflow");
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

        int64_t stepi = step.isNil() ? 1 : step.asInt();

        if (stepi > 0) { // normal order
            if (stop.isNil()) return -1;

            int64_t starti = start.isNil() ? 0 : start.asInt();
            int64_t stopi = stop.asInt();
            //if ((starti < 0) || (stopi < 0)) return -1;
            //std::cout << " length() starti:" << starti << " stopi:" << stopi << " stepi:" << stepi << " closed:" << closed << std::endl;
            if (!closed) {
                if (starti >= stopi) return 0;
                return checkedInt32((stopi - starti - 1) / stepi + 1,
                                    "range length overflow");
            }
            else {
                if (starti > stopi) return 0;
                return checkedInt32(((stopi+1) - starti - 1) / stepi + 1,
                                    "range length overflow");
            }

        } else { // reverse order e.g. -2:1:-2
            //std::cout << "length()" << objRangeToString(this) << std::endl;
            if (start.isNil()) return -1;

            int64_t starti = start.asInt();
            int64_t stopi = stop.isNil() ? (closed?0:-1) : stop.asInt();
            //if ((starti < 0) || ((stopi < 0) && !stop.isNil())) return -1;
            //std::cout << " length() starti:" << starti << " stopi:" << stopi << " stepi:" << stepi << (closed?" closed":" open") << std::endl;

            if (!closed)
                return checkedInt32((starti - stopi - 1)/-stepi + 1,
                                    "range length overflow");
            else
                return checkedInt32((starti - (stopi-1) - 1)/-stepi + 1,
                                    "range length overflow");
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

        int64_t stepi = step.isNil() ? 1 : step.asInt();
        int64_t starti, stopi;

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

        int64_t result = starti + int64_t(index) * stepi;
        return checkedInt32(result, "range index overflow");
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

unique_ptr<Obj, UnreleasedObj> ObjRange::clone(roxal::ptr<CloneContext> ctx) const
{
    if (ctx) {
        auto it = ctx->originalToClone.find(this);
        if (it != ctx->originalToClone.end()) {
            it->second->incRef();
            return unique_ptr<Obj, UnreleasedObj>(it->second);
        }
    }

    auto newr = newRangeObj();

    if (ctx) {
        ctx->originalToClone[this] = newr.get();
    }

    newr->start = start.clone(ctx);
    newr->stop = stop.clone(ctx);
    newr->step = step.clone(ctx);
    newr->closed = closed;

    return newr;
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
    : elts_(make_ptr<std::vector<Value>>())
{
    type = ObjType::List;
    int32_t rangeLen = r->length();
    if (rangeLen > 0)
        elts_->reserve(rangeLen);
    for(int32_t i=0; i<rangeLen; i++)
        elts_->push_back(Value::intVal(r->targetIndex(i,-1)));
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
        return (*elts_)[index];
    }
    else if (isRange(i)) {
        auto sublist = newListObj();
        auto r = asRange(i);
        auto listLen = length();
        auto rangeLen = r->length(listLen);

        for(auto i=0; i<rangeLen; i++) {
            auto targetIndex = r->targetIndex(i,listLen);
            if ((targetIndex >= 0) && (targetIndex < listLen))
                sublist->elts_->push_back((*elts_)[targetIndex]);
        }

        return Value::objVal(std::move(sublist));
    }
    else
        throw std::invalid_argument("List indexing subscript must be a number or a range.");
    return Value::nilVal();
}


void ObjList::setElement(size_t i, const Value& v)
{
    ensureMutable();
    CowGuard guard(control);
    if (guard.active()) saveVersion();
    ensureUnique();
    (*elts_)[i] = v;
    if (guard.active()) control->writeEpoch.store(globalWriteEpoch.fetch_add(1, std::memory_order_relaxed), std::memory_order_release);
}

void ObjList::setElements(const std::vector<Value>& v)
{
    ensureMutable();
    CowGuard guard(control);
    if (guard.active()) saveVersion();
    ensureUnique();
    *elts_ = v;
    if (guard.active()) control->writeEpoch.store(globalWriteEpoch.fetch_add(1, std::memory_order_relaxed), std::memory_order_release);
}

void ObjList::setIndex(const Value& i, const Value& v)
{
    ensureMutable();
    CowGuard guard(control);
    if (guard.active()) saveVersion();
    ensureUnique();
    if (i.isNumber()) {
        auto index = i.asInt();
        auto len = length();
        if (index < 0)
            index = len - (-index);
        if (index < 0 || index >= len)
            throw std::invalid_argument("List index out-of-range.");
        (*elts_)[index] = v;
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
                    (*elts_)[targetIndex] = (*rhsList->elts_)[i];
            }
        }
    }
    else
        throw std::invalid_argument("List indexing subscript must be a number or a range (not "+to_string(i.type())+").");
    if (guard.active()) control->writeEpoch.store(globalWriteEpoch.fetch_add(1, std::memory_order_relaxed), std::memory_order_release);
}

void ObjList::concatenate(const ObjList* other)
{
    ensureMutable();
    CowGuard guard(control);
    if (guard.active()) saveVersion();
    ensureUnique();
    const auto& otherElts = *other->elts_;
    elts_->reserve(elts_->size() + otherElts.size());
    elts_->insert(elts_->end(), otherElts.begin(), otherElts.end());
    if (guard.active()) control->writeEpoch.store(globalWriteEpoch.fetch_add(1, std::memory_order_relaxed), std::memory_order_release);
}

void ObjList::append(const Value& value)
{
    ensureMutable();
    CowGuard guard(control);
    if (guard.active()) saveVersion();
    ensureUnique();
    elts_->push_back(value);
    if (guard.active()) control->writeEpoch.store(globalWriteEpoch.fetch_add(1, std::memory_order_relaxed), std::memory_order_release);
}

void ObjList::write(std::ostream& out, roxal::ptr<SerializationContext> ctx) const
{
    uint32_t len = length();
    out.write(reinterpret_cast<char*>(&len), 4);
    for(const auto& v : *elts_)
        writeValue(out, v, ctx);
}

void ObjList::read(std::istream& in, roxal::ptr<SerializationContext> ctx)
{
    ensureMutable();
    CowGuard guard(control);
    if (guard.active()) saveVersion();
    ensureUnique();
    uint32_t len;
    in.read(reinterpret_cast<char*>(&len), 4);
    elts_->clear();
    for(uint32_t i=0;i<len;i++)
        elts_->push_back(readValue(in, ctx));
    if (guard.active()) control->writeEpoch.store(globalWriteEpoch.fetch_add(1, std::memory_order_relaxed), std::memory_order_release);
}

void ObjList::trace(ValueVisitor& visitor) const
{
    for (const auto& value : *elts_) {
        visitor.visit(value);
    }
}

void ObjList::dropReferences()
{
    cleanupMVCC();
    elts_ = make_ptr<std::vector<Value>>();
}

void ObjList::set(const ObjList* other)
{
    ensureMutable();
    CowGuard guard(control);
    if (guard.active()) saveVersion();
    ensureUnique();
    *elts_ = *other->elts_;
    if (guard.active()) control->writeEpoch.store(globalWriteEpoch.fetch_add(1, std::memory_order_relaxed), std::memory_order_release);
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
    l->setElements(elts);
    return l;
}

unique_ptr<Obj, UnreleasedObj> ObjList::clone(roxal::ptr<CloneContext> ctx) const
{
    // Check if already cloned (preserves shared references and handles cycles)
    if (ctx) {
        auto it = ctx->originalToClone.find(this);
        if (it != ctx->originalToClone.end()) {
            it->second->incRef();
            return unique_ptr<Obj, UnreleasedObj>(it->second);
        }
    }

    // Create new clone
    auto newl = newListObj();

    // Register BEFORE recursing (critical for cycle handling)
    if (ctx) {
        ctx->originalToClone[this] = newl.get();
    }

    // Clone children with context
    auto lsize = elts_->size();
    newl->elts_->reserve(lsize);
    for (size_t i = 0; i < lsize; i++)
        newl->elts_->push_back((*elts_)[i].clone(ctx));

    return newl;
}

unique_ptr<Obj, UnreleasedObj> ObjList::shallowClone() const
{
    auto newl = newListObj();
    // COW: share the storage pointer (O(1) refcount bump).
    // Mutations on either side will trigger ensureUnique() to copy-on-write.
    newl->elts_ = elts_;
    return newl;
}

bool ObjList::equals(const ObjList* other) const
{
    if (other == nullptr)
        return false;

    const auto& lst1 = *elts_;
    const auto& lst2 = *other->elts_;

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
    auto list { ol->getElements() };
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

unique_ptr<Obj, UnreleasedObj> ObjDict::clone(roxal::ptr<CloneContext> ctx) const
{
    // Check if already cloned (preserves shared references and handles cycles)
    if (ctx) {
        auto it = ctx->originalToClone.find(this);
        if (it != ctx->originalToClone.end()) {
            it->second->incRef();
            return unique_ptr<Obj, UnreleasedObj>(it->second);
        }
    }

    // Create new clone
    auto newd = newDictObj();

    // Register BEFORE recursing (critical for cycle handling)
    if (ctx) {
        ctx->originalToClone[this] = newd.get();
    }

    // Clone children with context
    const auto dkeys = keys();
    for (const auto& dkey : dkeys)
        newd->store(dkey.clone(ctx), at(dkey).clone(ctx));

    return newd;
}

unique_ptr<Obj, UnreleasedObj> ObjDict::shallowClone() const
{
    auto newd = newDictObj();
    // COW: share the storage pointer (O(1) refcount bump).
    // Mutations on either side will trigger ensureUnique() to copy-on-write.
    newd->data_ = data_;
    return newd;
}

void ObjDict::store(const Value& key, const Value& val)
{
    ensureMutable();
    CowGuard guard(control);
    if (guard.active()) saveVersion();
    ensureUnique();
    if (data_->entries.find(key) == data_->entries.end())
        data_->m_keys.push_back(key);
    data_->entries[key] = val;
    if (guard.active()) control->writeEpoch.store(globalWriteEpoch.fetch_add(1, std::memory_order_relaxed), std::memory_order_release);
}

void ObjDict::erase(const Value& key)
{
    ensureMutable();
    CowGuard guard(control);
    if (guard.active()) saveVersion();
    ensureUnique();
    auto it = data_->entries.find(key);
    if (it != data_->entries.end()) {
        data_->entries.erase(it);
        data_->m_keys.erase(std::remove(data_->m_keys.begin(), data_->m_keys.end(), key), data_->m_keys.end());
    }
    if (guard.active()) control->writeEpoch.store(globalWriteEpoch.fetch_add(1, std::memory_order_relaxed), std::memory_order_release);
}

void ObjDict::set(const ObjDict* other)
{
    ensureMutable();
    CowGuard guard(control);
    if (guard.active()) saveVersion();
    ensureUnique();
    *data_ = *other->data_;
    if (guard.active()) control->writeEpoch.store(globalWriteEpoch.fetch_add(1, std::memory_order_relaxed), std::memory_order_release);
}

bool ObjDict::equals(const ObjDict* other) const
{
    if (other == nullptr)
        return false;
    if (other == this)
        return true;

    const auto& entries = data_->entries;
    const auto& otherEntries = other->data_->entries;

    if (entries.size() != otherEntries.size())
        return false;

    // entries is a std::map keyed by Value, which provides ordering
    // irrespective of insertion order.  Compare by keys and values
    for(const auto& [key, val] : entries) {
        auto it = otherEntries.find(key);
        if (it == otherEntries.end())
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
    ensureMutable();
    CowGuard guard(control);
    if (guard.active()) saveVersion();
    ensureUnique();
    uint32_t len;
    in.read(reinterpret_cast<char*>(&len), 4);
    data_->m_keys.clear();
    data_->entries.clear();
    for(uint32_t i=0;i<len;i++) {
        Value k = readValue(in, ctx);
        Value v = readValue(in, ctx);
        data_->m_keys.push_back(k);
        data_->entries[k] = v;
    }
    if (guard.active()) control->writeEpoch.store(globalWriteEpoch.fetch_add(1, std::memory_order_relaxed), std::memory_order_release);
}

void ObjDict::trace(ValueVisitor& visitor) const
{
    for (const auto& entry : data_->entries) {
        visitor.visit(entry.first);
        visitor.visit(entry.second);
    }
}

void ObjDict::dropReferences()
{
    cleanupMVCC();
    data_ = make_ptr<DictData>();
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

unique_ptr<Obj, UnreleasedObj> ObjVector::clone(roxal::ptr<CloneContext> ctx) const
{
    (void)ctx; // vector has value semantics, no object references to track
    auto newv = newVectorObj(0);  // create minimal vector
    newv->vec_ = vec_;  // COW: share the data ptr
    return newv;
}

unique_ptr<Obj, UnreleasedObj> ObjVector::shallowClone() const
{
    auto newv = newVectorObj(0);
    newv->vec_ = vec_;  // COW: share the data ptr
    return newv;
}

void ObjVector::set(const ObjVector* other)
{
    ensureMutable();
    if (activeSnapshotCount.load(std::memory_order_relaxed) > 0) saveVersion();
    vec_ = other->vec_;  // COW: share the data ptr
    control->writeEpoch.store(globalWriteEpoch.fetch_add(1, std::memory_order_relaxed), std::memory_order_release);
}


std::string roxal::objVectorToString(const ObjVector* ov)
{
    std::ostringstream os;
    os << "[";
    for(int i=0; i<ov->vec().size(); ++i) {
        os << ov->vec()[i];
        if (i != ov->vec().size()-1)
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
    if (vec().size() != other->vec().size())
        return false;

    // Use Eigen's isApprox for element-wise comparison with tolerance
    return vec().isApprox(other->vec(), eps);
}

void ObjVector::write(std::ostream& out, roxal::ptr<SerializationContext> ctx) const
{
    uint32_t len = vec().size();
    out.write(reinterpret_cast<char*>(&len), 4);
    for(uint32_t i=0;i<len;i++) {
        double d = vec()[i];
        out.write(reinterpret_cast<char*>(&d), 8);
    }
}

void ObjVector::read(std::istream& in, roxal::ptr<SerializationContext> ctx)
{
    ensureMutable();
    if (activeSnapshotCount.load(std::memory_order_relaxed) > 0) saveVersion();
    uint32_t len;
    in.read(reinterpret_cast<char*>(&len), 4);
    vec_ = make_ptr<Eigen::VectorXd>(len);  // create new storage for deserialization
    for(uint32_t i=0;i<len;i++) {
        double d; in.read(reinterpret_cast<char*>(&d), 8);
        (*vec_)[i] = d;
    }
    control->writeEpoch.store(globalWriteEpoch.fetch_add(1, std::memory_order_relaxed), std::memory_order_release);
}


ObjMatrix::ObjMatrix(const Eigen::MatrixXd& values)
    : mat_(make_ptr<Eigen::MatrixXd>(values))
{
    type = ObjType::Matrix;
}

ObjMatrix::ObjMatrix(int32_t rows, int32_t cols)
    : mat_(make_ptr<Eigen::MatrixXd>(rows, cols))
{
    type = ObjType::Matrix;
    mat_->setZero();
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

unique_ptr<Obj, UnreleasedObj> ObjMatrix::clone(roxal::ptr<CloneContext> ctx) const
{
    (void)ctx; // matrix has value semantics, no object references to track
    auto newm = newMatrixObj(0, 0);  // create minimal matrix
    newm->mat_ = mat_;  // COW: share the data ptr
    return newm;
}

unique_ptr<Obj, UnreleasedObj> ObjMatrix::shallowClone() const
{
    auto newm = newMatrixObj(0, 0);
    newm->mat_ = mat_;  // COW: share the data ptr
    return newm;
}

Eigen::MatrixXd& ObjMatrix::matMut()
{
    ensureMutable();
    if (activeSnapshotCount.load(std::memory_order_relaxed) > 0) saveVersion();
    ensureUnique();
    control->writeEpoch.store(globalWriteEpoch.fetch_add(1, std::memory_order_relaxed), std::memory_order_release);
    return *mat_;
}

void ObjMatrix::set(const ObjMatrix* other)
{
    ensureMutable();
    if (activeSnapshotCount.load(std::memory_order_relaxed) > 0) saveVersion();
    mat_ = other->mat_;  // COW: share the data ptr
    control->writeEpoch.store(globalWriteEpoch.fetch_add(1, std::memory_order_relaxed), std::memory_order_release);
}

unique_ptr<Obj, UnreleasedObj> ObjPrimitive::clone(roxal::ptr<CloneContext> ctx) const
{
    (void)ctx; // primitives are value types, no object references to track
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
// Signals are shared state - cloning returns the same signal (like interned strings)
unique_ptr<Obj, UnreleasedObj> ObjSignal::clone(roxal::ptr<CloneContext> ctx) const {
    (void)ctx; // signals are shared, not cloned
    return unique_ptr<Obj, UnreleasedObj>(const_cast<ObjSignal*>(this));
}
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

unique_ptr<Obj, UnreleasedObj> ObjEventType::clone(roxal::ptr<CloneContext> ctx) const {
    (void)ctx; // event type definitions are immutable once created; share reference
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
        for (const auto& prop : typeObj->payloadProperties) {
            auto propInitialValue = prop.initialValue;
            // Clone reference types to avoid sharing between instances
            if (!propInitialValue.isPrimitive()) {
                // Events should not have signal members
                if (isSignal(propInitialValue)) {
                    throw std::runtime_error("events cannot have signal members");
                }
                ptr<CloneContext> cloneCtx = make_ptr<CloneContext>();
                propInitialValue = propInitialValue.clone(cloneCtx);
            }
            payload[prop.name.hashCode()] = propInitialValue;
        }
    }
}

unique_ptr<Obj, UnreleasedObj> ObjEventInstance::clone(roxal::ptr<CloneContext> ctx) const {
    (void)ctx; // event instances are immutable snapshots; share reference
    return unique_ptr<Obj, UnreleasedObj>(const_cast<ObjEventInstance*>(this));
}

void ObjEventInstance::write(std::ostream& out, roxal::ptr<SerializationContext> ctx) const
{
    uint8_t tag = static_cast<uint8_t>(ObjType::EventInstance);
    out.write(reinterpret_cast<char*>(&tag), 1);
    writeValue(out, typeHandle, ctx);

    uint32_t count = payload.size();
    out.write(reinterpret_cast<char*>(&count), 4);
    for (const auto& [key, value] : payload) {
        out.write(reinterpret_cast<const char*>(&key), sizeof(key));
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
    for (uint32_t i = 0; i < count; ++i) {
        int32_t key;
        in.read(reinterpret_cast<char*>(&key), sizeof(key));
        payload[key] = readValue(in, ctx);
    }

    type = ObjType::EventInstance;
}

void ObjEventInstance::trace(ValueVisitor& visitor) const
{
    visitor.visit(typeHandle);
    for (const auto& [key, value] : payload) {
        visitor.visit(value);
    }
}

void ObjEventInstance::dropReferences()
{
    typeHandle = Value::nilVal();
    payload.clear();
}

unique_ptr<Obj, UnreleasedObj> ObjLibrary::clone(roxal::ptr<CloneContext> ctx) const {
    (void)ctx; // dynamic libraries are represented by handles; share the handle
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
unique_ptr<Obj, UnreleasedObj> ObjForeignPtr::clone(roxal::ptr<CloneContext> ctx) const {
    (void)ctx; // foreign pointers are opaque handles; cloning would be unsafe
    return unique_ptr<Obj, UnreleasedObj>(const_cast<ObjForeignPtr*>(this));
}
void ObjForeignPtr::write(std::ostream&, roxal::ptr<SerializationContext>) const { throw std::runtime_error("Cannot serialize foreign pointers"); }
void ObjForeignPtr::read(std::istream&, roxal::ptr<SerializationContext>) { throw std::runtime_error("Cannot deserialize foreign pointers"); }
unique_ptr<Obj, UnreleasedObj> ObjFile::clone(roxal::ptr<CloneContext> ctx) const {
    (void)ctx; // files cannot be duplicated; share the underlying handle
    return unique_ptr<Obj, UnreleasedObj>(const_cast<ObjFile*>(this));
}
void ObjFile::write(std::ostream&, roxal::ptr<SerializationContext>) const { throw std::runtime_error("Cannot serialize file handles"); }
void ObjFile::read(std::istream&, roxal::ptr<SerializationContext>) { throw std::runtime_error("Cannot deserialize file handles"); }
unique_ptr<Obj, UnreleasedObj> ObjException::clone(roxal::ptr<CloneContext> ctx) const { (void)ctx; throw std::runtime_error("cannot clone exceptions"); }
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
unique_ptr<Obj, UnreleasedObj> ObjFunction::clone(roxal::ptr<CloneContext> ctx) const {
    (void)ctx; // function objects are immutable; share the reference
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
    uint8_t mods = static_cast<uint8_t>(methodModifiers); out.write(reinterpret_cast<char*>(&mods),1);

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
    uint8_t hasNativeImpl = (builtinInfo != nullptr) ? 1 : 0;
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
    uint8_t mods; in.read(reinterpret_cast<char*>(&mods),1); methodModifiers = mods;

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
                    if (liveFunc->builtinInfo) {
                        const auto& srcInfo = *liveFunc->builtinInfo;
                        builtinInfo = make_ptr<BuiltinFuncInfo>(
                            srcInfo.function, srcInfo.defaultValues, srcInfo.resolveArgMask);
                    }
                }
            }
        }
        if (!builtinInfo) {
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
    if (builtinInfo) {
        for (const auto& def : builtinInfo->defaultValues) {
            visitor.visit(def);
        }
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

    builtinInfo.reset();
    paramDefaultFunc.clear();
}

unique_ptr<Obj, UnreleasedObj> ObjUpvalue::clone(roxal::ptr<CloneContext> ctx) const
{
    if (ctx) {
        auto it = ctx->originalToClone.find(this);
        if (it != ctx->originalToClone.end()) {
            it->second->incRef();
            return unique_ptr<Obj, UnreleasedObj>(it->second);
        }
    }

    auto newup = newUpvalueObj(location);

    if (ctx) {
        ctx->originalToClone[this] = newup.get();
    }

    newup->closed = newup->location->clone(ctx);
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

unique_ptr<Obj, UnreleasedObj> ObjClosure::clone(roxal::ptr<CloneContext> ctx) const
{
    if (ctx) {
        auto it = ctx->originalToClone.find(this);
        if (it != ctx->originalToClone.end()) {
            it->second->incRef();
            return unique_ptr<Obj, UnreleasedObj>(it->second);
        }
    }

    auto newc = newClosureObj(function);

    if (ctx) {
        ctx->originalToClone[this] = newc.get();
    }

    newc->upvalues.resize(upvalues.size());
    for (size_t i = 0; i < upvalues.size(); i++)
        newc->upvalues[i] = upvalues.at(i).clone(ctx);
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

unique_ptr<Obj, UnreleasedObj> ObjFuture::clone(roxal::ptr<CloneContext> ctx) const
{
    (void)ctx; // future resolves to a boxed primitive
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
unique_ptr<Obj, UnreleasedObj> ObjNative::clone(roxal::ptr<CloneContext> ctx) const {
    (void)ctx; // native functions are immutable; share the reference
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
unique_ptr<Obj, UnreleasedObj> ObjTypeSpec::clone(roxal::ptr<CloneContext> ctx) const {
    (void)ctx; // type metadata is immutable; share the reference
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
unique_ptr<Obj, UnreleasedObj> ObjObjectType::clone(roxal::ptr<CloneContext> ctx) const {
    (void)ctx; // object type definitions are immutable once created; share reference
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
        writeValue(out, prop.ownerType, ctx);
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
        uint8_t mods = static_cast<uint8_t>(method.methodModifiers);
        out.write(reinterpret_cast<char*>(&mods),1);
        writeValue(out, method.ownerType, ctx);
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

    uint32_t ntcount = nestedTypes.size();
    out.write(reinterpret_cast<char*>(&ntcount),4);
    for(const auto& kv : nestedTypes) {
        const auto& nt = kv.second;
        std::string nn; nt.name.toUTF8String(nn);
        uint32_t nlen = nn.size();
        out.write(reinterpret_cast<char*>(&nlen),4);
        out.write(nn.data(), nlen);
        writeValue(out, nt.type, ctx);
        uint8_t acc = static_cast<uint8_t>(nt.access);
        out.write(reinterpret_cast<char*>(&acc),1);
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
        Value ownerType = readValue(in, ctx);
        if (!ownerType.isNil())
            ownerType = ownerType.weakRef();
        uint8_t hasC; in.read(reinterpret_cast<char*>(&hasC),1);
        std::optional<icu::UnicodeString> ct;
        if(hasC) {
            uint32_t ctlen; in.read(reinterpret_cast<char*>(&ctlen),4);
            std::string cts(ctlen,'\0'); if(ctlen>0) in.read(cts.data(), ctlen);
            ct = icu::UnicodeString::fromUTF8(cts);
        }
        int32_t hash = uname.hashCode();
        if (ownerType.isNil())
            ownerType = Value::objRef(this).weakRef();
        Property prop{uname, ptype, init, static_cast<ast::Access>(acc), isConst != 0, ownerType, ct};
        // Re-freeze initial value for const members with const type (const bit lost during serialization)
        if (prop.isConst && (prop.type.isNil() || prop.type.isConst())
            && prop.initialValue.isObj() && !prop.initialValue.isConst())
            prop.initialValue = prop.initialValue.constRef();
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
        uint8_t mods; in.read(reinterpret_cast<char*>(&mods),1);
        Value ownerType = readValue(in, ctx);
        if (!ownerType.isNil())
            ownerType = ownerType.weakRef();
        int32_t hash = uname.hashCode();
        if (ownerType.isNil())
            ownerType = Value::objRef(this).weakRef();
        Method m{uname, clos, static_cast<ast::Access>(acc),
                 static_cast<ast::MethodModifiers>(mods), ownerType};
        methods[hash] = m;
        if (ast::hasModifier(m.methodModifiers, ast::MethodModifier::StatementAction))
            statementActionMethodHash = hash;
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

    uint32_t ntcount; in.read(reinterpret_cast<char*>(&ntcount),4);
    nestedTypes.clear();
    for(uint32_t i=0;i<ntcount;i++) {
        uint32_t nlen; in.read(reinterpret_cast<char*>(&nlen),4);
        std::string nn(nlen,'\0'); if(nlen>0) in.read(nn.data(), nlen);
        icu::UnicodeString uname = icu::UnicodeString::fromUTF8(nn);
        Value val = readValue(in, ctx);
        uint8_t acc; in.read(reinterpret_cast<char*>(&acc),1);
        int32_t hash = uname.hashCode();
        nestedTypes[hash] = {uname, val, static_cast<ast::Access>(acc)};
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
    for (const auto& entry : nestedTypes) {
        visitor.visit(entry.second.type);
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
    for (auto& entry : nestedTypes) {
        entry.second.type = Value::nilVal();
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
unique_ptr<Obj, UnreleasedObj> ObjPackageType::clone(roxal::ptr<CloneContext> ctx) const {
    (void)ctx; // package types contain no mutable state; share the reference
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
unique_ptr<Obj, UnreleasedObj> ObjModuleType::clone(roxal::ptr<CloneContext> ctx) const {
    (void)ctx; // module types are immutable; share the reference
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
    uint32_t count = properties_->size();
    out.write(reinterpret_cast<char*>(&count),4);
    for(const auto& kv : *properties_) {
        int32_t h = kv.first;
        out.write(reinterpret_cast<char*>(&h),4);
        writeValue(out, kv.second.value, ctx);
    }
}

void ObjectInstance::read(std::istream& in, roxal::ptr<SerializationContext> ctx)
{
    ensureMutable();
    CowGuard guard(control);
    if (guard.active()) saveVersion();
    ensureUnique();
    // Reference tracking is handled by readValue().  Just read the object
    // contents here.
    instanceType = readValue(in, ctx);
    uint32_t count; in.read(reinterpret_cast<char*>(&count),4);
    properties_->clear();
    for(uint32_t i=0;i<count;i++) {
        int32_t h; in.read(reinterpret_cast<char*>(&h),4);
        Value v = readValue(in, ctx);
        auto& slot = (*properties_)[h];
        slot.clearSignal();
        slot.value = v;
    }
    if (guard.active()) control->writeEpoch.store(globalWriteEpoch.fetch_add(1, std::memory_order_relaxed), std::memory_order_release);
}

void ObjectInstance::trace(ValueVisitor& visitor) const
{
    visitor.visit(instanceType);
    for (const auto& entry : *properties_) {
        visitor.visit(entry.second.value);
        visitor.visit(entry.second.signal);
    }
}

void ObjectInstance::dropReferences()
{
    cleanupMVCC();
    instanceType = Value::nilVal();
    properties_ = make_ptr<PropertyMap>();
}

unique_ptr<Obj, UnreleasedObj> ObjBoundMethod::clone(roxal::ptr<CloneContext> ctx) const
{
    if (ctx) {
        auto it = ctx->originalToClone.find(this);
        if (it != ctx->originalToClone.end()) {
            it->second->incRef();
            return unique_ptr<Obj, UnreleasedObj>(it->second);
        }
    }

    auto newmb = newBoundMethodObj(receiver, method);

    if (ctx) {
        ctx->originalToClone[this] = newmb.get();
    }

    newmb->receiver = newmb->receiver.clone(ctx);
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

unique_ptr<Obj, UnreleasedObj> ObjBoundNative::clone(roxal::ptr<CloneContext> ctx) const
{
    if (ctx) {
        auto it = ctx->originalToClone.find(this);
        if (it != ctx->originalToClone.end()) {
            it->second->incRef();
            return unique_ptr<Obj, UnreleasedObj>(it->second);
        }
    }

    auto newbm = newBoundNativeObj(receiver, function, isProc, funcType, defaultValues, declFunction);

    if (ctx) {
        ctx->originalToClone[this] = newbm.get();
    }

    newbm->receiver = newbm->receiver.clone(ctx);
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
    uint32_t rows = mat().rows();
    uint32_t cols = mat().cols();
    out.write(reinterpret_cast<char*>(&rows), 4);
    out.write(reinterpret_cast<char*>(&cols), 4);
    for(uint32_t r=0;r<rows;r++)
        for(uint32_t c=0;c<cols;c++) {
            double d = mat()(r,c);
            out.write(reinterpret_cast<char*>(&d), 8);
        }
}

void ObjMatrix::read(std::istream& in, roxal::ptr<SerializationContext> ctx)
{
    ensureMutable();
    if (activeSnapshotCount.load(std::memory_order_relaxed) > 0) saveVersion();
    uint32_t rows, cols;
    in.read(reinterpret_cast<char*>(&rows), 4);
    in.read(reinterpret_cast<char*>(&cols), 4);
    mat_ = make_ptr<Eigen::MatrixXd>(rows, cols);  // create new storage for deserialization
    for(uint32_t r=0;r<rows;r++)
        for(uint32_t c=0;c<cols;c++) {
            double d; in.read(reinterpret_cast<char*>(&d), 8);
            (*mat_)(r,c) = d;
        }
    control->writeEpoch.store(globalWriteEpoch.fetch_add(1, std::memory_order_relaxed), std::memory_order_release);
}


//
// ObjTensor implementation
//

std::string roxal::to_string(TensorDType dtype)
{
    switch (dtype) {
        case TensorDType::Float16: return "float16";
        case TensorDType::Float32: return "float32";
        case TensorDType::Float64: return "float64";
        case TensorDType::Int8:    return "int8";
        case TensorDType::Int16:   return "int16";
        case TensorDType::Int32:   return "int32";
        case TensorDType::Int64:   return "int64";
        case TensorDType::UInt8:   return "uint8";
        case TensorDType::Bool:    return "bool";
        default: return "unknown";
    }
}

TensorDType roxal::tensorDTypeFromString(const std::string& s)
{
    if (s == "float16" || s == "half") return TensorDType::Float16;
    if (s == "float32" || s == "float") return TensorDType::Float32;
    if (s == "float64" || s == "double") return TensorDType::Float64;
    if (s == "int8") return TensorDType::Int8;
    if (s == "int16") return TensorDType::Int16;
    if (s == "int32" || s == "int") return TensorDType::Int32;
    if (s == "int64" || s == "long") return TensorDType::Int64;
    if (s == "uint8" || s == "byte") return TensorDType::UInt8;
    if (s == "bool") return TensorDType::Bool;
    throw std::runtime_error("Unknown tensor dtype: " + s);
}

#ifdef ROXAL_ENABLE_ONNX
// Forward declarations of helpers (defined below)
static ONNXTensorElementDataType tensorDTypeToOrt(TensorDType dtype);
static TensorDType tensorDTypeFromOrt(ONNXTensorElementDataType ortType);
static size_t tensorDTypeSize(TensorDType dtype);
static double ortElementAsDouble(const Ort::Value& val, TensorDType dtype, int64_t idx);
static void ortSetElementFromDouble(Ort::Value& val, TensorDType dtype, int64_t idx, double v);
#endif

ObjTensor::ObjTensor()
#ifndef ROXAL_ENABLE_ONNX
    : data_(make_ptr<std::vector<double>>())
#endif
{
    type = ObjType::Tensor;
}

ObjTensor::ObjTensor(const std::vector<int64_t>& shape, TensorDType dtype)
    : shape_(shape), dtype_(dtype)
{
    type = ObjType::Tensor;
    computeStrides();
    int64_t n = numel();

#ifdef ROXAL_ENABLE_ONNX
    // Allocate via ONNX Runtime (ORT-owned memory)
    Ort::AllocatorWithDefaultOptions allocator;
    auto ortDtype = tensorDTypeToOrt(dtype);
    ort_value_ = std::make_shared<Ort::Value>(
        Ort::Value::CreateTensor(allocator, shape_.data(), shape_.size(), ortDtype));
    // Zero-initialize
    auto elemSize = tensorDTypeSize(dtype);
    std::memset(ort_value_->GetTensorMutableRawData(), 0, n * elemSize);
#else
    data_ = make_ptr<std::vector<double>>(n, 0.0);
#endif
}

#ifdef ROXAL_ENABLE_ONNX

// Mapping helpers between TensorDType and ONNXTensorElementDataType

static ONNXTensorElementDataType tensorDTypeToOrt(TensorDType dtype)
{
    switch (dtype) {
        case TensorDType::Float16: return ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16;
        case TensorDType::Float32: return ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
        case TensorDType::Float64: return ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE;
        case TensorDType::Int8:    return ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8;
        case TensorDType::Int16:   return ONNX_TENSOR_ELEMENT_DATA_TYPE_INT16;
        case TensorDType::Int32:   return ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32;
        case TensorDType::Int64:   return ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64;
        case TensorDType::UInt8:   return ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8;
        case TensorDType::Bool:    return ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL;
        default: throw std::runtime_error("Unsupported TensorDType for ORT conversion");
    }
}

static TensorDType tensorDTypeFromOrt(ONNXTensorElementDataType ortType)
{
    switch (ortType) {
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16: return TensorDType::Float16;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:   return TensorDType::Float32;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE:  return TensorDType::Float64;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8:    return TensorDType::Int8;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT16:   return TensorDType::Int16;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32:   return TensorDType::Int32;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64:   return TensorDType::Int64;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8:   return TensorDType::UInt8;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL:    return TensorDType::Bool;
        default: throw std::runtime_error("Unsupported ORT element type");
    }
}

static size_t tensorDTypeSize(TensorDType dtype)
{
    switch (dtype) {
        case TensorDType::Float16: return 2;
        case TensorDType::Float32: return 4;
        case TensorDType::Float64: return 8;
        case TensorDType::Int8:    return 1;
        case TensorDType::Int16:   return 2;
        case TensorDType::Int32:   return 4;
        case TensorDType::Int64:   return 8;
        case TensorDType::UInt8:   return 1;
        case TensorDType::Bool:    return 1;
        default: return 0;
    }
}

ObjTensor::ObjTensor(Ort::Value&& ortValue)
    : ort_value_(std::make_shared<Ort::Value>(std::move(ortValue)))
{
    type = ObjType::Tensor;

    // Read shape and dtype from the Ort::Value
    auto info = ort_value_->GetTensorTypeAndShapeInfo();
    shape_ = info.GetShape();
    dtype_ = tensorDTypeFromOrt(info.GetElementType());
    computeStrides();
}

const Ort::Value& ObjTensor::ortValue() const
{
    if (!ort_value_)
        throw std::runtime_error("Tensor is not ORT-backed");
    return *ort_value_;
}

Ort::Value& ObjTensor::ortValueMut()
{
    if (!ort_value_)
        throw std::runtime_error("Tensor is not ORT-backed");
    ensureOrtUnique();
    return *ort_value_;
}

void ObjTensor::ensureOrtUnique()
{
    if (ort_value_ && ort_value_.use_count() > 1) {
        // Callers must call ensureCpu() first — COW deep-copy is CPU-only
        assert(!isOnGpu() && "ensureOrtUnique() called on GPU tensor; call ensureCpu() first");

        // Deep-copy: allocate a new ORT tensor and copy the raw data
        auto info = ort_value_->GetTensorTypeAndShapeInfo();
        auto ortDtype = info.GetElementType();
        auto shape = info.GetShape();
        size_t byteCount = info.GetElementCount() * tensorDTypeSize(dtype_);

        Ort::AllocatorWithDefaultOptions allocator;
        auto newVal = Ort::Value::CreateTensor(allocator, shape.data(), shape.size(), ortDtype);
        std::memcpy(newVal.GetTensorMutableRawData(),
                     ort_value_->GetTensorData<void>(), byteCount);
        ort_value_ = std::make_shared<Ort::Value>(std::move(newVal));
    }
}

bool ObjTensor::isOnGpu() const
{
    if (!ort_value_) return false;
    auto memInfo = ort_value_->GetTensorMemoryInfo();
    return memInfo.GetDeviceType() == OrtMemoryInfoDeviceType_GPU;
}

void ObjTensor::ensureCpu() const
{
    if (!isOnGpu()) return;

    auto& cuda = CudaRuntime::instance();
    if (!cuda.available())
        throw std::runtime_error("GPU tensor element access requires CUDA runtime "
                                 "(libcudart.so not found)");

    auto info = ort_value_->GetTensorTypeAndShapeInfo();
    auto ortDtype = info.GetElementType();
    auto shape = info.GetShape();
    size_t byteCount = info.GetElementCount() * tensorDTypeSize(dtype_);

    // Allocate a new CPU tensor and copy data from GPU
    Ort::AllocatorWithDefaultOptions cpuAllocator;
    auto cpuVal = Ort::Value::CreateTensor(cpuAllocator, shape.data(), shape.size(), ortDtype);
    constexpr int cudaMemcpyDeviceToHost = 2;
    int err = cuda.memcpy(cpuVal.GetTensorMutableRawData(),
                          ort_value_->GetTensorData<void>(),
                          byteCount, cudaMemcpyDeviceToHost);
    if (err != 0)
        throw std::runtime_error(std::string("cudaMemcpy D2H failed: ") + cuda.getErrorString(err));

    ort_value_ = std::make_shared<Ort::Value>(std::move(cpuVal));
}

// Helper: read a single element from ORT buffer as double
static double ortElementAsDouble(const Ort::Value& val, TensorDType dtype, int64_t idx)
{
    switch (dtype) {
        case TensorDType::Float32: return static_cast<double>(val.GetTensorData<float>()[idx]);
        case TensorDType::Float64: return val.GetTensorData<double>()[idx];
        case TensorDType::Float16: return static_cast<double>(val.GetTensorData<Ort::Float16_t>()[idx].ToFloat());
        case TensorDType::Int32:   return static_cast<double>(val.GetTensorData<int32_t>()[idx]);
        case TensorDType::Int64:   return static_cast<double>(val.GetTensorData<int64_t>()[idx]);
        case TensorDType::Int8:    return static_cast<double>(val.GetTensorData<int8_t>()[idx]);
        case TensorDType::Int16:   return static_cast<double>(val.GetTensorData<int16_t>()[idx]);
        case TensorDType::UInt8:   return static_cast<double>(val.GetTensorData<uint8_t>()[idx]);
        case TensorDType::Bool:    return val.GetTensorData<bool>()[idx] ? 1.0 : 0.0;
        default: throw std::runtime_error("Unsupported dtype for element access");
    }
}

// Helper: write a single element to ORT buffer from double
static void ortSetElementFromDouble(Ort::Value& val, TensorDType dtype, int64_t idx, double v)
{
    switch (dtype) {
        case TensorDType::Float32: val.GetTensorMutableData<float>()[idx] = static_cast<float>(v); break;
        case TensorDType::Float64: val.GetTensorMutableData<double>()[idx] = v; break;
        case TensorDType::Float16: val.GetTensorMutableData<Ort::Float16_t>()[idx] = Ort::Float16_t(static_cast<float>(v)); break;
        case TensorDType::Int32:   val.GetTensorMutableData<int32_t>()[idx] = static_cast<int32_t>(v); break;
        case TensorDType::Int64:   val.GetTensorMutableData<int64_t>()[idx] = static_cast<int64_t>(v); break;
        case TensorDType::Int8:    val.GetTensorMutableData<int8_t>()[idx] = static_cast<int8_t>(v); break;
        case TensorDType::Int16:   val.GetTensorMutableData<int16_t>()[idx] = static_cast<int16_t>(v); break;
        case TensorDType::UInt8:   val.GetTensorMutableData<uint8_t>()[idx] = static_cast<uint8_t>(v); break;
        case TensorDType::Bool:    val.GetTensorMutableData<bool>()[idx] = (v != 0.0); break;
        default: throw std::runtime_error("Unsupported dtype for element write");
    }
}

#endif // ROXAL_ENABLE_ONNX

double ObjTensor::at(int64_t flatIdx) const
{
#ifdef ROXAL_ENABLE_ONNX
    ensureCpu();
    return ortElementAsDouble(*ort_value_, dtype_, flatIdx);
#else
    return (*data_)[flatIdx];
#endif
}

void ObjTensor::setAt(int64_t flatIdx, double v)
{
    ensureMutable();
    if (activeSnapshotCount.load(std::memory_order_relaxed) > 0) saveVersion();
#ifdef ROXAL_ENABLE_ONNX
    ensureCpu();
    ensureOrtUnique();
    ortSetElementFromDouble(*ort_value_, dtype_, flatIdx, v);
#else
    ensureUnique();
    (*data_)[flatIdx] = v;
#endif
    control->writeEpoch.store(globalWriteEpoch.fetch_add(1, std::memory_order_relaxed), std::memory_order_release);
}

const double* ObjTensor::data() const
{
#ifdef ROXAL_ENABLE_ONNX
    if (dtype_ != TensorDType::Float64)
        throw std::runtime_error("data() requires Float64 dtype; use at() for type-safe access");
    ensureCpu();
    return ort_value_->GetTensorData<double>();
#else
    return data_->data();
#endif
}

double* ObjTensor::dataMut()
{
    ensureMutable();
    if (activeSnapshotCount.load(std::memory_order_relaxed) > 0) saveVersion();
#ifdef ROXAL_ENABLE_ONNX
    if (dtype_ != TensorDType::Float64)
        throw std::runtime_error("dataMut() requires Float64 dtype; use setAt() for type-safe access");
    ensureCpu();
    ensureOrtUnique();
    control->writeEpoch.store(globalWriteEpoch.fetch_add(1, std::memory_order_relaxed), std::memory_order_release);
    return ort_value_->GetTensorMutableData<double>();
#else
    ensureUnique();
    control->writeEpoch.store(globalWriteEpoch.fetch_add(1, std::memory_order_relaxed), std::memory_order_release);
    return data_->data();
#endif
}

const void* ObjTensor::rawData() const
{
#ifdef ROXAL_ENABLE_ONNX
    ensureCpu();
    return ort_value_->GetTensorData<void>();
#else
    return data_->data();
#endif
}

void* ObjTensor::rawDataMut()
{
    ensureMutable();
    if (activeSnapshotCount.load(std::memory_order_relaxed) > 0) saveVersion();
#ifdef ROXAL_ENABLE_ONNX
    ensureCpu();
    ensureOrtUnique();
    control->writeEpoch.store(globalWriteEpoch.fetch_add(1, std::memory_order_relaxed), std::memory_order_release);
    return ort_value_->GetTensorMutableRawData();
#else
    ensureUnique();
    control->writeEpoch.store(globalWriteEpoch.fetch_add(1, std::memory_order_relaxed), std::memory_order_release);
    return data_->data();
#endif
}

void ObjTensor::computeStrides()
{
    strides_.resize(shape_.size());
    if (shape_.empty()) return;

    // Row-major strides (C-order)
    int64_t stride = 1;
    for (int64_t i = static_cast<int64_t>(shape_.size()) - 1; i >= 0; --i) {
        strides_[i] = stride;
        stride *= shape_[i];
    }
}

int64_t ObjTensor::numel() const
{
    if (shape_.empty()) return 0;
    int64_t n = 1;
    for (auto s : shape_) n *= s;
    return n;
}

int64_t ObjTensor::flatIndex(const std::vector<int64_t>& indices) const
{
    if (indices.size() != shape_.size())
        throw std::runtime_error("Tensor index rank mismatch");

    int64_t idx = 0;
    for (size_t i = 0; i < indices.size(); ++i) {
        int64_t ix = indices[i];
        if (ix < 0 || ix >= shape_[i])
            throw std::runtime_error("Tensor index out of bounds");
        idx += ix * strides_[i];
    }
    return idx;
}

Value ObjTensor::index(const std::vector<Value>& indices) const
{
    if (indices.size() != shape_.size())
        throw std::runtime_error("Tensor index rank mismatch: expected " +
            std::to_string(shape_.size()) + " indices, got " + std::to_string(indices.size()));

    // Check if any index is a range
    bool hasRange = false;
    for (const auto& v : indices) {
        if (isRange(v)) {
            hasRange = true;
            break;
        }
    }

    if (!hasRange) {
        // All integer indices - return scalar
        std::vector<int64_t> idxs;
        idxs.reserve(indices.size());
        for (const auto& v : indices) {
            if (!v.isInt())
                throw std::runtime_error("Tensor index must be integer or range");
            idxs.push_back(v.asInt());
        }
        int64_t flatIdx = flatIndex(idxs);
        return Value::realVal(at(flatIdx));
    }

    // Has ranges - build sub-tensor
    // For each dimension, collect the list of indices to extract
    std::vector<std::vector<int64_t>> dimIndices;
    dimIndices.reserve(indices.size());
    std::vector<int64_t> resultShape;

    for (size_t d = 0; d < indices.size(); ++d) {
        const Value& idx = indices[d];
        int64_t dimSize = shape_[d];

        if (idx.isInt()) {
            int64_t i = idx.asInt();
            if (i < 0 || i >= dimSize)
                throw std::runtime_error("Tensor index out of bounds at dimension " + std::to_string(d));
            dimIndices.push_back({i});
            // Single index - dimension is squeezed out (not added to result shape)
        } else if (isRange(idx)) {
            ObjRange* r = asRange(idx);
            int64_t rangeLen = r->length(dimSize);
            std::vector<int64_t> dimIdx;
            dimIdx.reserve(rangeLen);
            for (int64_t j = 0; j < rangeLen; ++j) {
                int64_t target = r->targetIndex(j, dimSize);
                if (target >= 0 && target < dimSize)
                    dimIdx.push_back(target);
            }
            dimIndices.push_back(dimIdx);
            // Range - add dimension to result shape
            resultShape.push_back(dimIdx.size());
        } else {
            throw std::runtime_error("Tensor index must be integer or range");
        }
    }

    // If all ranges resulted in single elements, we still return a tensor (not scalar)
    // This preserves type through indexing
    if (resultShape.empty()) {
        // All indices were single values - return 0D tensor? Or scalar?
        // For consistency, return scalar since all indices were explicit
        std::vector<int64_t> idxs;
        for (const auto& di : dimIndices)
            idxs.push_back(di[0]);
        int64_t flatIdx = flatIndex(idxs);
        return Value::realVal(at(flatIdx));
    }

    // Build result tensor
    int64_t resultNumel = 1;
    for (auto s : resultShape) resultNumel *= s;
    std::vector<double> resultData;
    resultData.reserve(resultNumel);

    // Recursive helper to iterate through all combinations of indices
    std::function<void(size_t, std::vector<int64_t>&)> collectData;
    collectData = [&](size_t dim, std::vector<int64_t>& currentIdx) {
        if (dim == dimIndices.size()) {
            // Compute flat index in source tensor
            int64_t srcIdx = 0;
            for (size_t i = 0; i < currentIdx.size(); ++i)
                srcIdx += currentIdx[i] * strides_[i];
            resultData.push_back(at(srcIdx));
            return;
        }
        for (int64_t i : dimIndices[dim]) {
            currentIdx.push_back(i);
            collectData(dim + 1, currentIdx);
            currentIdx.pop_back();
        }
    };

    std::vector<int64_t> currentIdx;
    collectData(0, currentIdx);

    return Value::tensorVal(resultShape, resultData, dtype_);
}

void ObjTensor::setIndex(const std::vector<Value>& indices, const Value& v)
{
    std::vector<int64_t> idxs;
    idxs.reserve(indices.size());
    for (const auto& idx : indices) {
        if (!idx.isInt())
            throw std::runtime_error("Tensor index must be integer");
        idxs.push_back(idx.asInt());
    }
    int64_t flatIdx = flatIndex(idxs);

    if (!v.isNumber())
        throw std::runtime_error("Tensor element must be numeric");
    double dv = v.isInt() ? static_cast<double>(v.asInt()) : v.asReal();
    setAt(flatIdx, dv);
}

Value ObjTensor::reshape(const std::vector<int64_t>& newShape) const
{
    int64_t newNumel = 1;
    for (auto s : newShape) newNumel *= s;
    if (newNumel != numel())
        throw std::runtime_error("Tensor reshape: total elements must match");

    // Create result and copy data element-by-element (works with both backends)
    auto result = newTensorObj(newShape, dtype_);
    for (int64_t i = 0; i < newNumel; ++i)
        result->setAt(i, at(i));
    return Value::objVal(std::move(result));
}

bool ObjTensor::equals(const ObjTensor* other, double eps) const
{
    if (shape_ != other->shape_) return false;
    if (dtype_ != other->dtype_) return false;

#ifdef ROXAL_ENABLE_ONNX
    // COW identity: if both tensors share the same underlying Ort::Value,
    // their data is identical — no need for element comparison.
    if (ort_value_ && other->ort_value_ && ort_value_.get() == other->ort_value_.get())
        return true;

    // For GPU-resident tensors, avoid expensive GPU→CPU copy for comparison.
    // Use identity comparison only — different ORT values are conservatively
    // considered unequal.  This is correct for signal change detection (each
    // inference produces a new ORT value) and avoids PCIe transfer overhead.
    if (isOnGpu() || other->isOnGpu())
        return false;  // different pointers already checked above
#else
    // COW identity: shared data_ pointer means identical contents.
    if (data_ && other->data_ && data_.get() == other->data_.get())
        return true;
#endif

    int64_t n = numel();
    for (int64_t i = 0; i < n; ++i) {
        if (std::abs(at(i) - other->at(i)) > eps)
            return false;
    }
    return true;
}

void ObjTensor::set(const ObjTensor* other)
{
    ensureMutable();
    if (activeSnapshotCount.load(std::memory_order_relaxed) > 0) saveVersion();
    shape_ = other->shape_;
    strides_ = other->strides_;
    dtype_ = other->dtype_;
#ifdef ROXAL_ENABLE_ONNX
    ort_value_ = other->ort_value_;  // COW: share the data
#else
    data_ = other->data_;  // COW: share the data
#endif
    control->writeEpoch.store(globalWriteEpoch.fetch_add(1, std::memory_order_relaxed), std::memory_order_release);
}

unique_ptr<Obj, UnreleasedObj> ObjTensor::clone(roxal::ptr<CloneContext> ctx) const
{
    (void)ctx; // tensor has value semantics, no object references to track
    auto newt = newTensorObj();
    newt->shape_ = shape_;
    newt->strides_ = strides_;
    newt->dtype_ = dtype_;
#ifdef ROXAL_ENABLE_ONNX
    newt->ort_value_ = ort_value_;  // COW: share the data
#else
    newt->data_ = data_;  // COW: share the data
#endif
    return newt;
}

unique_ptr<Obj, UnreleasedObj> ObjTensor::shallowClone() const
{
    auto newt = newTensorObj();
    newt->shape_ = shape_;
    newt->strides_ = strides_;
    newt->dtype_ = dtype_;
#ifdef ROXAL_ENABLE_ONNX
    newt->ort_value_ = ort_value_;  // COW: share the data
#else
    newt->data_ = data_;  // COW: share the data
#endif
    return newt;
}

void ObjTensor::write(std::ostream& out, roxal::ptr<SerializationContext> ctx) const
{
    // Write dtype
    uint8_t dt = static_cast<uint8_t>(dtype_);
    out.write(reinterpret_cast<char*>(&dt), 1);

    // Write rank
    uint32_t rank = static_cast<uint32_t>(shape_.size());
    out.write(reinterpret_cast<char*>(&rank), 4);

    // Write shape
    for (auto s : shape_) {
        int64_t dim = s;
        out.write(reinterpret_cast<char*>(&dim), 8);
    }

    // Write data (always serialized as doubles for portability)
    int64_t n = numel();
    for (int64_t i = 0; i < n; ++i) {
        double d = at(i);
        out.write(reinterpret_cast<const char*>(&d), 8);
    }
}

void ObjTensor::read(std::istream& in, roxal::ptr<SerializationContext> ctx)
{
    ensureMutable();
    if (activeSnapshotCount.load(std::memory_order_relaxed) > 0) saveVersion();
    // Read dtype
    uint8_t dt;
    in.read(reinterpret_cast<char*>(&dt), 1);
    dtype_ = static_cast<TensorDType>(dt);

    // Read rank
    uint32_t rank;
    in.read(reinterpret_cast<char*>(&rank), 4);

    // Read shape
    shape_.resize(rank);
    for (uint32_t i = 0; i < rank; ++i) {
        int64_t dim;
        in.read(reinterpret_cast<char*>(&dim), 8);
        shape_[i] = dim;
    }

    computeStrides();

    // Read data
    int64_t n = numel();
#ifdef ROXAL_ENABLE_ONNX
    Ort::AllocatorWithDefaultOptions allocator;
    auto ortDtype = tensorDTypeToOrt(dtype_);
    ort_value_ = std::make_shared<Ort::Value>(
        Ort::Value::CreateTensor(allocator, shape_.data(), shape_.size(), ortDtype));
    for (int64_t i = 0; i < n; ++i) {
        double d;
        in.read(reinterpret_cast<char*>(&d), 8);
        ortSetElementFromDouble(*ort_value_, dtype_, i, d);
    }
#else
    data_ = make_ptr<std::vector<double>>(n);
    for (int64_t i = 0; i < n; ++i) {
        double d;
        in.read(reinterpret_cast<char*>(&d), 8);
        (*data_)[i] = d;
    }
#endif
    control->writeEpoch.store(globalWriteEpoch.fetch_add(1, std::memory_order_relaxed), std::memory_order_release);
}

unique_ptr<ObjTensor, UnreleasedObj> roxal::newTensorObj()
{
    #ifdef DEBUG_BUILD
    return newObj<ObjTensor>(__func__, __FILE__, __LINE__);
    #else
    return newObj<ObjTensor>();
    #endif
}

unique_ptr<ObjTensor, UnreleasedObj> roxal::newTensorObj(const std::vector<int64_t>& shape, TensorDType dtype)
{
    #ifdef DEBUG_BUILD
    return newObj<ObjTensor>(__func__, __FILE__, __LINE__, shape, dtype);
    #else
    return newObj<ObjTensor>(shape, dtype);
    #endif
}

unique_ptr<ObjTensor, UnreleasedObj> roxal::newTensorObj(const std::vector<int64_t>& shape,
                                                          const std::vector<double>& data,
                                                          TensorDType dtype)
{
    auto t = newTensorObj(shape, dtype);
    if (data.size() != static_cast<size_t>(t->numel()))
        throw std::runtime_error("Tensor data size mismatch");
    for (int64_t i = 0; i < static_cast<int64_t>(data.size()); ++i)
        t->setAt(i, data[i]);
    return t;
}

#ifdef ROXAL_ENABLE_ONNX
unique_ptr<ObjTensor, UnreleasedObj> roxal::newTensorObj(Ort::Value&& ortValue)
{
    #ifdef DEBUG_BUILD
    return newObj<ObjTensor>(__func__, __FILE__, __LINE__, std::move(ortValue));
    #else
    return newObj<ObjTensor>(std::move(ortValue));
    #endif
}
#endif

std::string roxal::objTensorToString(const ObjTensor* ot)
{
    std::ostringstream os;
    os << "tensor(shape=[";
    for (size_t i = 0; i < ot->shape().size(); ++i) {
        if (i > 0) os << ", ";
        os << ot->shape()[i];
    }
    os << "], dtype='" << to_string(ot->dtype()) << "')";
    return os.str();
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
            std::unordered_map<int32_t, Value> payload;
            // Payload properties keyed by name hash
            payload[toUnicodeString("value").hashCode()] = sample;
            if (useSpan) {
                // Emit the elapsed steady-clock time since engine start as a
                // TimeSpan instance when the sys module is available.
                payload[toUnicodeString("timestamp").hashCode()] = sysNewTimeSpan(t.microSecs());
            } else {
                payload[toUnicodeString("timestamp").hashCode()] = Value::intVal(static_cast<int32_t>(t.microSecs()));
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
            payload[toUnicodeString("tick").hashCode()] = Value::intVal(static_cast<int32_t>(tickIndex));
            payload[toUnicodeString("source_signal").hashCode()] = Value::signalVal(sig);

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

    // Add built-in 'target' property to all event types (nil by default)
    ObjEventType::PayloadProperty targetProp { toUnicodeString("target"), Value::nilVal(), Value::nilVal() };
    eventType->payloadProperties.push_back(targetProp);
    eventType->propertyLookup[targetProp.name.hashCode()] = 0;

    return eventType;
}

unique_ptr<ObjEventInstance, UnreleasedObj> roxal::newEventInstanceObj(const Value& eventType,
                                                                       std::unordered_map<int32_t, Value> payload)
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
    auto list = listObj->getElements();
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
        // Return a 1-row matrix (preserves type - use vector(matrix[i]) to get a vector)
        Eigen::MatrixXd rowMat = mat().row(r);
        return Value::matrixVal(rowMat);
    } else if (isRange(row)) {
        ObjRange* rr = asRange(row);
        int rowCount = rr->length(rows());
        Eigen::MatrixXd m(rowCount, cols());
        for(int i=0;i<rowCount;++i) {
            int target = rr->targetIndex(i, rows());
            if (target >=0 && target < rows())
                m.row(i) = mat().row(target);
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
        return Value::realVal(mat()(r,c));
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
        return Value::realVal(mat()(rowIdx[0], colIdx[0]));
    } else if (rowIdx.size()==1 && colIdx.size()>1) {
        Eigen::VectorXd vals(colIdx.size());
        for(size_t j=0;j<colIdx.size();++j)
            vals[j] = mat()(rowIdx[0], colIdx[j]);
        return Value::vectorVal(vals);
    } else if (rowIdx.size()>1 && colIdx.size()==1) {
        Eigen::VectorXd vals(rowIdx.size());
        for(size_t i=0;i<rowIdx.size();++i)
            vals[i] = mat()(rowIdx[i], colIdx[0]);
        return Value::vectorVal(vals);
    } else {
        Eigen::MatrixXd sub(rowIdx.size(), colIdx.size());
        for(size_t i=0;i<rowIdx.size();++i)
            for(size_t j=0;j<colIdx.size();++j)
                sub(i,j) = mat()(rowIdx[i], colIdx[j]);
        return Value::matrixVal(sub);
    }
    return Value::nilVal();
}

void ObjMatrix::setIndex(const Value& row, const Value& value)
{
    ensureMutable();
    if (activeSnapshotCount.load(std::memory_order_relaxed) > 0) saveVersion();
    ensureUnique();  // COW: copy before mutation
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
            (*mat_)(rowIdx[0], c) = vec->vec()[c];
        return;
    }

    ObjMatrix* rhs = valueToMatrix(value);
    if (rhs->cols() != cols() || rhs->rows() != (int)rowIdx.size())
        throw std::invalid_argument("Assignment to matrix rows requires a matrix of size ("+std::to_string(rowIdx.size())+","+std::to_string(cols())+")");

    for(size_t i=0;i<rowIdx.size();++i)
        mat_->row(rowIdx[i]) = rhs->mat().row(i);
    control->writeEpoch.store(globalWriteEpoch.fetch_add(1, std::memory_order_relaxed), std::memory_order_release);
}

void ObjMatrix::setIndex(const Value& row, const Value& col, const Value& value)
{
    ensureMutable();
    if (activeSnapshotCount.load(std::memory_order_relaxed) > 0) saveVersion();
    ensureUnique();
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
        (*mat_)(rowIdx[0], colIdx[0]) = scalar;
        return;
    }

    if (rowIdx.size()==1) {
        ObjVector* vec = valueToVector(value);
        if (vec->length() != (int)colIdx.size())
            throw std::invalid_argument("Assignment to matrix subrow requires vector length " + std::to_string(colIdx.size()));
        for(size_t j=0;j<colIdx.size();++j)
            (*mat_)(rowIdx[0], colIdx[j]) = vec->vec()[j];
        return;
    } else if (colIdx.size()==1) {
        ObjVector* vec = valueToVector(value);
        if (vec->length() != (int)rowIdx.size())
            throw std::invalid_argument("Assignment to matrix subcolumn requires vector length " + std::to_string(rowIdx.size()));
        for(size_t i=0;i<rowIdx.size();++i)
            (*mat_)(rowIdx[i], colIdx[0]) = vec->vec()[i];
        return;
    } else {
        ObjMatrix* rhs = valueToMatrix(value);
        if (rhs->rows()!= (int)rowIdx.size() || rhs->cols()!= (int)colIdx.size())
            throw std::invalid_argument("Assignment to matrix submatrix requires matrix of size ("+std::to_string(rowIdx.size())+","+std::to_string(colIdx.size())+")");
        for(size_t i=0;i<rowIdx.size();++i)
            for(size_t j=0;j<colIdx.size();++j)
                (*mat_)(rowIdx[i], colIdx[j]) = rhs->mat()(i,j);
    }
    control->writeEpoch.store(globalWriteEpoch.fetch_add(1, std::memory_order_relaxed), std::memory_order_release);
}

bool ObjMatrix::equals(const ObjMatrix* other, double eps) const
{
    if (other == nullptr)
        return false;

    // Check if dimensions match
    if (mat().rows() != other->mat().rows() || mat().cols() != other->mat().cols())
        return false;

    // Use Eigen's isApprox for element-wise comparison with tolerance
    return mat().isApprox(other->mat(), eps);
}

std::string roxal::objMatrixToString(const ObjMatrix* om)
{
    using std::min;

    const int rows = om->mat().rows();
    const int cols = om->mat().cols();

    const int firstRows = min(rows, 16);
    const int lastRows  = rows > 32 ? 16 : (rows - firstRows);
    const int firstCols = min(cols, 16);
    const int lastCols  = cols > 32 ? 16 : (cols - firstCols);

    std::vector<size_t> colWidthFirst(firstCols, 0);
    std::vector<size_t> colWidthLast(lastCols, 0);

    auto updateWidths = [&](int r) {
        for(int c=0; c<firstCols; ++c) {
            std::string s = format("%g", om->mat()(r,c));
            colWidthFirst[c] = std::max(colWidthFirst[c], s.size());
        }
        for(int c=0; c<lastCols; ++c) {
            std::string s = format("%g", om->mat()(r, cols-lastCols+c));
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
            std::string s = format("%g", om->mat()(r,c));
            os << std::left << std::setw(colWidthFirst[c]) << s;
            if(c != firstCols-1 || cols > firstCols)
                os << ' ';
        }
        if(cols > 32)
            os << "... ";
        for(int c=0; c<lastCols; ++c) {
            std::string s = format("%g", om->mat()(r, cols-lastCols+c));
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


// ObjOrient

ObjOrient::ObjOrient(const Eigen::Quaterniond& q)
    : quat_(make_ptr<Eigen::Quaterniond>(q.normalized()))
{
    type = ObjType::Orient;
}

Eigen::Quaterniond& ObjOrient::quatMut()
{
    ensureMutable();
    if (activeSnapshotCount.load(std::memory_order_relaxed) > 0) saveVersion();
    ensureUnique();
    control->writeEpoch.store(globalWriteEpoch.fetch_add(1, std::memory_order_relaxed), std::memory_order_release);
    return *quat_;
}

bool ObjOrient::equals(const ObjOrient* other, double eps) const
{
    if (!other) return false;
    // q and -q represent the same rotation
    double dot = quat().dot(other->quat());
    return std::abs(std::abs(dot) - 1.0) < eps;
}

void ObjOrient::set(const ObjOrient* other)
{
    ensureMutable();
    if (activeSnapshotCount.load(std::memory_order_relaxed) > 0) saveVersion();
    quat_ = other->quat_;  // COW: share the data ptr
    control->writeEpoch.store(globalWriteEpoch.fetch_add(1, std::memory_order_relaxed), std::memory_order_release);
}

unique_ptr<Obj, UnreleasedObj> ObjOrient::clone(roxal::ptr<CloneContext> ctx) const
{
    (void)ctx; // orient has value semantics, no object references to track
    auto newo = newOrientObj();
    newo->quat_ = quat_;  // COW: share the data ptr
    return newo;
}

unique_ptr<Obj, UnreleasedObj> ObjOrient::shallowClone() const
{
    auto newo = newOrientObj();
    newo->quat_ = quat_;  // COW: share the data ptr
    return newo;
}

void ObjOrient::write(std::ostream& out, roxal::ptr<SerializationContext> ctx) const
{
    (void)ctx;
    double x = quat().x(), y = quat().y(), z = quat().z(), w = quat().w();
    out.write(reinterpret_cast<const char*>(&x), 8);
    out.write(reinterpret_cast<const char*>(&y), 8);
    out.write(reinterpret_cast<const char*>(&z), 8);
    out.write(reinterpret_cast<const char*>(&w), 8);
}

void ObjOrient::read(std::istream& in, roxal::ptr<SerializationContext> ctx)
{
    (void)ctx;
    ensureMutable();
    if (activeSnapshotCount.load(std::memory_order_relaxed) > 0) saveVersion();
    double x, y, z, w;
    in.read(reinterpret_cast<char*>(&x), 8);
    in.read(reinterpret_cast<char*>(&y), 8);
    in.read(reinterpret_cast<char*>(&z), 8);
    in.read(reinterpret_cast<char*>(&w), 8);
    quat_ = make_ptr<Eigen::Quaterniond>(w, x, y, z);  // Eigen ctor order: w,x,y,z
    quat_->normalize();
    control->writeEpoch.store(globalWriteEpoch.fetch_add(1, std::memory_order_relaxed), std::memory_order_release);
}

unique_ptr<ObjOrient, UnreleasedObj> roxal::newOrientObj()
{
    #ifdef DEBUG_BUILD
    return newObj<ObjOrient>(__func__, __FILE__, __LINE__);
    #else
    return newObj<ObjOrient>();
    #endif
}

unique_ptr<ObjOrient, UnreleasedObj> roxal::newOrientObj(const Eigen::Quaterniond& q)
{
    #ifdef DEBUG_BUILD
    return newObj<ObjOrient>(__func__, __FILE__, __LINE__, q);
    #else
    return newObj<ObjOrient>(q);
    #endif
}

std::string roxal::objOrientToString(const ObjOrient* oo)
{
    // Display as RPY in degrees for readability
    Eigen::Matrix3d m = oo->quat().toRotationMatrix();
    // eulerAngles(2,1,0) returns [yaw, pitch, roll] for ZYX (extrinsic XYZ = RPY)
    Eigen::Vector3d ea = m.canonicalEulerAngles(2, 1, 0);
    double roll  = ea[2] * 180.0 / M_PI;  if (roll == -0.0)  roll = 0.0;
    double pitch = ea[1] * 180.0 / M_PI; if (pitch == -0.0) pitch = 0.0;
    double yaw   = ea[0] * 180.0 / M_PI; if (yaw == -0.0)   yaw = 0.0;
    std::ostringstream os;
    os << "orient(r=" << roll << "\u00B0, p=" << pitch << "\u00B0, y=" << yaw << "\u00B0)";
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
        case ObjType::Orient: {
            return objOrientToString(asOrient(v));
        }
        case ObjType::Tensor: {
            return objTensorToString(asTensor(v));
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
            std::string constPrefix = v.isConst() ? "const " : "";
            if (isObjPrimitive(v))
                return to_string(asObjPrimitive(v)->as.btype);

            ObjTypeSpec* ts = asTypeSpec(v);
            if ((ts->typeValue != ValueType::Object) && (ts->typeValue != ValueType::Actor)) {
                return "<type "+constPrefix+to_string(ts->typeValue)+">";
            }
            else {
                ObjObjectType* obj = asObjectType(v);
                return std::string("<type ")+constPrefix+(obj->isActor ? "actor" :(obj->isInterface ? "interface" : (obj->isEnumeration ? "enum" : "object")))+" "+toUTF8StdString(obj->name)+">";
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
            if (ObjObjectType* quantityType = sysQuantityType()) {
                if (type == quantityType)
                    return sysQuantityDefaultString(inst);
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
    builtinInfo.reset();
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

        // Only register in enumTypes if name is non-empty (fresh compile).
        // During deserialization, name is empty and read() will register with the correct
        // enumTypeId from the cache. This prevents duplicate/stale entries in enumTypes.
        if (!typeName.isEmpty()) {
            // register ourselves in the global map of enum types, referenced in the enum values
            enumTypeId = randomUint16(1); // generate unique random id (1..max)
            while (ObjObjectType::enumTypes.find(enumTypeId) != ObjObjectType::enumTypes.end())
                enumTypeId = randomUint16(1);
            //std::cout << "registered new enum id " << enumTypeId << std::endl;
            enumTypes[enumTypeId] = this;
        }
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
    : properties_(make_ptr<PropertyMap>())
{
    type = ObjType::Instance;
    debug_assert_msg(isObjectType(objectType),
                     "ObjectInstance created with object type");
    instanceType = objectType.strongRef();

    ObjObjectType* ot = asObjectType(objectType);
    for(const auto& property : ot->properties) {
        const auto& prop { property.second };
        auto propInitialvalue { prop.initialValue };
        // Const members with const (or untyped) type are frozen and can be shared
        // across instances without cloning
        bool isConstType = prop.isConst && (prop.type.isNil() || prop.type.isConst());
        if (!isConstType && !propInitialvalue.isPrimitive()) {
            // Clone reference types to avoid sharing between instances
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
                ptr<CloneContext> cloneCtx = make_ptr<CloneContext>();
                propInitialvalue = propInitialvalue.clone(cloneCtx);
            }
        }
        auto hash = prop.name.hashCode();
        auto& slot = (*properties_)[hash];
        slot.clearSignal();
        slot.value = propInitialvalue;
    }
}

ObjectInstance::~ObjectInstance() {}

Value ObjectInstance::getProperty(const icu::UnicodeString& name) const
{
    auto it = properties_->find(name.hashCode());
    if (it != properties_->end())
        return it->second.value;

    // If property not found and name doesn't start with '_', check for backing field
    // (accessor properties store their data in _<name>)
    if (!name.startsWith("_")) {
        icu::UnicodeString backingName = UnicodeString("_") + name;
        it = properties_->find(backingName.hashCode());
        if (it != properties_->end())
            return it->second.value;
    }
    return Value::nilVal();
}

VariablesMap::MonitoredValue& ObjectInstance::propertySlot(int32_t hash)
{
    ensureMutable();
    CowGuard guard(control);
    if (guard.active()) saveVersion();
    ensureUnique();
    if (guard.active()) control->writeEpoch.store(globalWriteEpoch.fetch_add(1, std::memory_order_relaxed), std::memory_order_release);
    return (*properties_)[hash];
}

void ObjectInstance::assignProperty(int32_t hash, const Value& value)
{
    ensureMutable();
    CowGuard guard(control);
    if (guard.active()) saveVersion();
    ensureUnique();
    (*properties_)[hash].assign(value);
    if (guard.active()) control->writeEpoch.store(globalWriteEpoch.fetch_add(1, std::memory_order_relaxed), std::memory_order_release);
}

void ObjectInstance::emplaceProperty(int32_t hash, VariablesMap::MonitoredValue mv)
{
    ensureMutable();
    CowGuard guard(control);
    if (guard.active()) saveVersion();
    ensureUnique();
    properties_->emplace(hash, std::move(mv));
    if (guard.active()) control->writeEpoch.store(globalWriteEpoch.fetch_add(1, std::memory_order_relaxed), std::memory_order_release);
}

void ObjectInstance::setProperty(const icu::UnicodeString& name, Value value)
{
    ensureMutable();
    CowGuard guard(control);
    if (guard.active()) saveVersion();
    ensureUnique();
    // Check if this property has a backing field (accessor property)
    // If so, set the backing field instead of creating a separate property
    if (!name.startsWith("_")) {
        icu::UnicodeString backingName = UnicodeString("_") + name;
        auto it = properties_->find(backingName.hashCode());
        if (it != properties_->end()) {
            // Use backing field instead
            it->second.assign(value);
            if (guard.active()) control->writeEpoch.store(globalWriteEpoch.fetch_add(1, std::memory_order_relaxed), std::memory_order_release);
            return;
        }
    }
    (*properties_)[name.hashCode()].assign(value);
    if (guard.active()) control->writeEpoch.store(globalWriteEpoch.fetch_add(1, std::memory_order_relaxed), std::memory_order_release);
}

Value ObjectInstance::ensurePropertySignal(int32_t nameHash, const std::string& signalName)
{
    ensureUnique();
    auto it = properties_->find(nameHash);
    if (it == properties_->end())
        return Value::nilVal();
    return it->second.ensureSignal(signalName);
}


bool roxal::tryExtractQuantity(const Value& v, double& siValue, std::array<int32_t,4>& dims, bool& isDimensioned)
{
    // Bare zero (int or real) is compatible with any dimension
    if (v.isNumber()) {
        double val = v.isReal() ? v.asReal() : static_cast<double>(v.asInt());
        if (val == 0.0) {
            siValue = 0.0;
            // isDimensioned left unchanged — zero is compatible with any dimension
            return true;
        }
        return false; // non-zero bare number is not a quantity
    }

    // Check if it's a quantity object (duck-typed: has _v real and _d list of 4 ints)
    if (!isObjectInstance(v))
        return false;

    ObjectInstance* inst = asObjectInstance(v);
    Value vVal = inst->getProperty("_v");
    Value dVal = inst->getProperty("_d");

    if (vVal.isNil() || dVal.isNil())
        return false;
    if (!vVal.isNumber() || !isList(dVal))
        return false;

    ObjList* dList = asList(dVal);
    if (dList->length() != 4)
        return false;

    siValue = vVal.isReal() ? vVal.asReal() : static_cast<double>(vVal.asInt());

    std::array<int32_t,4> thisDims;
    for (int i = 0; i < 4; ++i) {
        Value elem = dList->getElement(i);
        if (!elem.isInt())
            return false;
        thisDims[i] = static_cast<int32_t>(elem.asInt());
    }

    if (!isDimensioned) {
        // First dimensioned element sets the reference dims
        dims = thisDims;
        isDimensioned = true;
    } else {
        // Subsequent elements must match
        if (dims != thisDims)
            throw std::runtime_error("vector elements have mismatched quantity dimensions");
    }

    return true;
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


unique_ptr<Obj, UnreleasedObj> ObjectInstance::clone(roxal::ptr<CloneContext> ctx) const
{
    // Check if already cloned (preserves shared references and handles cycles)
    if (ctx) {
        auto it = ctx->originalToClone.find(this);
        if (it != ctx->originalToClone.end()) {
            it->second->incRef();
            return unique_ptr<Obj, UnreleasedObj>(it->second);
        }
    }

    // Create new clone
    auto newobj = newObjectInstance(instanceType);

    // Register BEFORE recursing (critical for cycle handling)
    if (ctx) {
        ctx->originalToClone[this] = newobj.get();
    }

    // Clone properties with context
    for (const auto& index_value : *properties_) {
        const auto index { index_value.first };
        const auto& slot { index_value.second };
        const Value& value { slot.value };

        auto& targetSlot = newobj->propertySlot(index);
        targetSlot.clearSignal();

        if (isActorInstance(value))
            throw std::runtime_error("clone of type actor unsupported");

        targetSlot.value = value.clone(ctx);
    }

    return newobj;
}

unique_ptr<Obj, UnreleasedObj> ObjectInstance::shallowClone() const
{
    auto newobj = newObjectInstance(instanceType);
    // COW: share the properties pointer (O(1) refcount bump).
    // Mutations on either side will trigger ensureUnique() to copy-on-write.
    // Signals are intentionally NOT copied — they are per-instance change
    // notification handles tied to the dataflow engine and would be meaningless
    // on a clone (same convention as deep clone(), which calls clearSignal()).
    newobj->properties_ = properties_;
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

VariablesMap::MonitoredValue& ActorInstance::propertySlot(int32_t hash)
{
    ensureMutable();
    if (activeSnapshotCount.load(std::memory_order_relaxed) > 0) saveVersion();
    control->writeEpoch.store(globalWriteEpoch.fetch_add(1, std::memory_order_relaxed), std::memory_order_release);
    return properties[hash];
}

void ActorInstance::assignProperty(int32_t hash, const Value& value)
{
    ensureMutable();
    if (activeSnapshotCount.load(std::memory_order_relaxed) > 0) saveVersion();
    properties[hash].assign(value);
    control->writeEpoch.store(globalWriteEpoch.fetch_add(1, std::memory_order_relaxed), std::memory_order_release);
}

void ActorInstance::emplaceProperty(int32_t hash, VariablesMap::MonitoredValue mv)
{
    ensureMutable();
    if (activeSnapshotCount.load(std::memory_order_relaxed) > 0) saveVersion();
    properties.emplace(hash, std::move(mv));
    control->writeEpoch.store(globalWriteEpoch.fetch_add(1, std::memory_order_relaxed), std::memory_order_release);
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
                ptr<CloneContext> cloneCtx = make_ptr<CloneContext>();
                propInitialvalue = propInitialvalue.clone(cloneCtx);
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
#ifdef ROXAL_COMPUTE_SERVER
    if (isRemote && remoteActorId != 0 && remoteConnHold != nullptr) {
        try {
            remoteConnHold->sendActorDropped(remoteActorId);
        } catch (...) {
        }
    }
    remoteConn.reset();
    remoteConnHold.reset();
#endif
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

Value ActorInstance::queueCall(const Value& callee, const CallSpec& callSpec, Value* argsStackTop,
                               bool forceCompletionFuture)
{
    // queue producer for consumer Thread::act()
    #ifdef DEBUG_BUILD
    assert(isBoundMethod(callee) || isBoundNative(callee));
    Value recv = isBoundMethod(callee) ? asBoundMethod(callee)->receiver : asBoundNative(callee)->receiver;
    assert(isActorInstance(recv));
    #endif

    std::lock_guard<std::mutex> lock { queueMutex };

    // If the actor's thread has already exited, no one will service this call.
    // Return nil immediately so callers (resolveFuture etc.) don't block forever.
    if (!alive.load(std::memory_order_acquire)) {
        return Value::nilVal();
    }

    MethodCallInfo callInfo {};
    callInfo.callee = callee;
    callInfo.callSpec = callSpec;
#ifdef ROXAL_COMPUTE_SERVER
    callInfo.printTarget = VM::currentPrintTarget();
#endif

    // Extract function type info early so we can check param constness in the arg loop
    const std::vector<std::optional<type::Type::FuncType::ParamType>>* paramTypes = nullptr;
    if (isBoundMethod(callee)) {
        auto funcObj = asFunction(asClosure(asBoundMethod(callee)->method)->function);
        ptr<type::Type> retType;
        bool needsReturnFuture = forceCompletionFuture;
        if (funcObj->funcType.has_value()) {
            ptr<roxal::type::Type> funcType { funcObj->funcType.value() };
            assert(funcType->func.has_value());
            paramTypes = &funcType->func.value().params;
            needsReturnFuture = forceCompletionFuture || !funcType->func.value().isProc;
            if (needsReturnFuture) {
                // Extract return type for the future's promisedType
                if (!funcType->func.value().returnTypes.empty())
                    retType = funcType->func.value().returnTypes[0];
            }
        }
        if (needsReturnFuture) {
            callInfo.returnPromise = make_ptr<std::promise<Value>>();
            std::shared_future<Value> sf = callInfo.returnPromise->get_future().share();
            callInfo.returnFuture = Value::objVal(newFutureObj(sf, retType));
        }
    }

    // Marshal arguments: clone non-primitive args for actor isolation,
    // skip clone for sole-owner values, error for aliased mutable params.
    for(auto i=0; i<callSpec.argCount; i++) {
        Value arg = *(argsStackTop - i - 1);
        if (!arg.isPrimitive()) {
            Obj* obj = arg.asObj();
            bool soleOwner = obj && obj->control && obj->control->strong.load() <= 2;

            // Check if this is an explicitly mutable param (not implicitly const).
            // Actor params are implicitly const unless the param type exists and !isConst.
            // Args are pushed in reverse order, so arg i corresponds to param (argCount-1-i).
            bool isMutableParam = false;
            if (paramTypes) {
                size_t paramIdx = callSpec.argCount - 1 - i;
                if (paramIdx < paramTypes->size()) {
                    const auto& paramOpt = (*paramTypes)[paramIdx];
                    if (paramOpt.has_value() && paramOpt->type.has_value()) {
                        isMutableParam = !paramOpt->type.value()->isConst;
                    }
                }
            }

            if (isActorInstance(arg)) {
                // Actor instances are live shared references with their own concurrency
                // isolation — sole-owner and graph-isolation checks do not apply since
                // all actor state is protected by the actor's own isolation.
                // Apply const/mutable semantics directly: implicitly-const params carry
                // the const bit; explicitly-mutable params are passed as-is.
                if (!isMutableParam)
                    arg = arg.constRef();
            } else {
                if (isMutableParam && !soleOwner) {
                    throw std::runtime_error("Cannot pass aliased value as mutable actor parameter (use move() to transfer sole ownership)");
                }

                if (isMutableParam && soleOwner && !isIsolatedGraph(obj)) {
                    throw std::runtime_error("Cannot pass value with aliased interior objects as mutable actor parameter");
                }

                if (!isMutableParam) {
                    // Const value param: frozen snapshot for MVCC-based isolation.
                    // Sole-owner: createFrozenSnapshot just sets const bit (zero-copy).
                    // Shared: createFrozenSnapshot shallow-clones the root; children are
                    // lazily resolved via resolveConstChild on the actor thread, protected
                    // by the per-object cowLock_ spinlock against concurrent mutations.
                    arg = createFrozenSnapshot(arg);
                }
                // else: mutable sole-owner value passes through as-is (caller moved it).
            }
        }
        callInfo.args.push_back(arg);
    }

    if (isBoundNative(callee)) {
        // For builtin methods, check if it's a proc or func
        auto bound = asBoundNative(callee);

        // Only create a return promise if it's NOT a proc (i.e., it's a func)
        if (forceCompletionFuture || !bound->isProc) {
            ptr<type::Type> retType;
            if (bound->funcType && bound->funcType->func.has_value()
                && !bound->funcType->func.value().returnTypes.empty())
                retType = bound->funcType->func.value().returnTypes[0];
            callInfo.returnPromise = make_ptr<std::promise<Value>>();
            std::shared_future<Value> sf = callInfo.returnPromise->get_future().share();
            callInfo.returnFuture = Value::objVal(newFutureObj(sf, retType));
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
                    // originalArgs is in reverse stack order, so index from the end
                    if (pos >= 0 && static_cast<size_t>(pos) < originalArgs.size()) {
                        size_t reversedPos = originalArgs.size() - 1 - pos;
                        arg = originalArgs[reversedPos];
                    }
                }
                if (arg.isNil() && pi < bound->defaultValues.size())
                    arg = bound->defaultValues[pi];
                if (!arg.isPrimitive()) {
                    arg = createFrozenSnapshot(arg);
                }
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

#ifdef ROXAL_COMPUTE_SERVER
Value roxal::makeRemoteActor(const Value& actorType, int64_t remoteId, ptr<ComputeConnection> conn)
{
    Value actorVal = Value::objVal(newActorInstance(actorType));
    auto* actor = asActorInstance(actorVal);
    actor->isRemote = true;
    actor->remoteActorId = remoteId;
    actor->remoteConn = conn;
    actor->remoteConnHold = conn;

    ptr<Thread> newThread = make_ptr<Thread>();
    VM::instance().registerThread(newThread);
    actor->thread = newThread;
    newThread->act(actorVal);
    return actorVal;
}
#endif

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

            if (ls->internKey != 0 && rs->internKey != 0 && ls->internKey != rs->internKey)
                return false;

            return ls->s == rs->s;
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
    case ObjType::Orient: return "orient";
    case ObjType::Tensor: return "tensor";
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
    assert(lp->length() == 3);

    Value l2 = l1;
    assert(isList(l2));
    assert(!l2.isNil());

    assert(l1 == l2);
    assert(!(l2 == Value::nilVal()));
}
#endif
