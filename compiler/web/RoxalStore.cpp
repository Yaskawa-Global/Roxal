#ifdef __EMSCRIPTEN__

#include "RoxalStore.h"
#include "JsBridge.h"

#include "GCRoots.h"
#include "VM.h"
#include "dataflow/Signal.h"

#include <algorithm>
#include <iostream>
#include <unordered_set>

using namespace roxal;
using namespace roxal::web;

// ============================================================
// RoxalStore
// ============================================================

RoxalStore::RoxalStore(std::string name, const Value& obj)
    : obj_(obj), name_(std::move(name)), alive_(std::make_shared<std::atomic<bool>>(true))
{
    buildRoles();
    buildMethods();
    hookChanges();
}

RoxalStore::~RoxalStore()
{
    *alive_ = false;   // any late change callback becomes a no-op
}

void RoxalStore::buildRoles()
{
    roles_.clear();
    if (!isObjectInstance(obj_)) return;
    ObjObjectType* t = asObjectType(asObjectInstance(obj_)->instanceType);
    if (!t) return;

    for (const auto& pv : t->orderedPublicProperties()) {
        Role r;
        r.uname    = pv.property->name;
        r.nameHash = pv.property->name.hashCode();
        r.name     = toUTF8StdString(pv.property->name);
        r.editable = !pv.property->isConst;
        roles_.push_back(r);
    }

    // Computed (accessor) properties are not in orderedPublicProperties -- they
    // exist as synthesized __get_<name> / __set_<name> methods. Expose each public
    // one: read via the getter, write via the setter (get-only -> read-only).
    std::unordered_set<int32_t> seen;
    for (const auto& r : roles_) seen.insert(r.nameHash);
    const ustring kGet("__get_");
    for (const auto& mentry : t->methods) {
        for (const auto& m : mentry.second.overloads) {
            if (m.access != ast::Access::Public || !m.name.startsWith(kGet))
                continue;
            ustring propName = m.name.tempSubString(kGet.length());
            const int32_t propHash = propName.hashCode();
            if (!seen.insert(propHash).second) continue;

            Role r;
            r.uname       = propName;
            r.nameHash    = propHash;
            r.name        = toUTF8StdString(propName);
            r.computed    = true;
            r.getterName  = m.name;
            r.setterName  = ustring("__set_") + propName;
            r.backingHash = (ustring("_") + propName).hashCode();
            ObjObjectType::Method* setter = t->findUniqueMethod(r.setterName.hashCode());
            r.editable    = (setter != nullptr && setter->access == ast::Access::Public);
            roles_.push_back(r);
        }
    }
}

void RoxalStore::buildMethods()
{
    methodNames_.clear();
    if (!isObjectInstance(obj_)) return;
    ObjObjectType* t = asObjectType(asObjectInstance(obj_)->instanceType);
    if (!t) return;

    const ustring kGet("__get_"), kSet("__set_"), kInit("init");
    for (const auto& mentry : t->methods) {
        const auto& overloads = mentry.second.overloads;
        if (overloads.size() != 1) continue;            // invokeMethod needs a UNIQUE method
        const ObjObjectType::Method& m = overloads[0];
        if (m.access != ast::Access::Public) continue;
        if (m.name == kInit) continue;                  // the constructor is not an action
        if (m.name.startsWith(kGet) || m.name.startsWith(kSet)) continue;
        if (m.name.startsWith("_")) continue;           // private by naming convention
        methodNames_.push_back(m.name);
    }
}

Value RoxalStore::readRole(const Role& role) const
{
    if (!isObjectInstance(obj_)) return Value::nilVal();
    if (role.computed) {
        auto [status, val] = VM::instance().invokeMethod(obj_, role.getterName, {});
        return status == ExecutionStatus::OK ? val : Value::nilVal();
    }
    return asObjectInstance(obj_)->getProperty(role.uname);
}

void RoxalStore::hookChanges()
{
    if (!isObjectInstance(obj_)) return;
    ObjectInstance* inst = asObjectInstance(obj_);
    std::shared_ptr<std::atomic<bool>> alive = alive_;
    RoxalStore* self = this;

    for (const auto& r : roles_) {
        // ChangeNotifier rather than a dataflow signal: exposing an object creates
        // no signal machinery. Fires on the VM thread when a write actually changes
        // the value (assign() gates no-op writes). A computed property observes its
        // `_<name>` backing field; getters computed from OTHER fields will not
        // auto-fire, which is what web.notify() is for.
        Role role = r;
        const int32_t observeHash = role.computed ? role.backingHash : role.nameHash;
        const ustring observeName = role.computed ? (ustring("_") + role.uname) : role.uname;
        inst->observePropertyChange(observeHash, toUTF8StdString(observeName),
            [alive, self, role](TimePoint, ptr<df::Signal>, const Value&) {
                if (!alive->load()) return;
                self->onRoxalChange(role);
            });

        // A signal-valued property needs a SECOND hook. The property slot holds the
        // same ObjSignal for the object's whole life -- the value changes inside the
        // signal -- so the slot observer above never fires for it. Subscribe to the
        // signal itself. Note this does NOT rate-limit: a source signal fires on
        // every set(), and its declared frequency is not a throttle. Coalescing in
        // flushDirty (one push per pump) is the only smoothing there is today.
        //
        // This callback arrives on the dataflow engine thread. It only inserts a role
        // hash under dirtyMutex_ -- no VM state is touched here; the value is read
        // later, on the VM thread, by flushDirty().
        const Value current = readRole(r);
        if (isSignal(current)) {
            asSignal(current)->signal->addValueChangedCallback(
                [alive, self, role](TimePoint, ptr<df::Signal>, const Value&) {
                    if (!alive->load()) return;
                    self->onRoxalChange(role);
                });
        }
    }
}

const RoxalStore::Role* RoxalStore::roleByName(const std::string& name) const
{
    for (const auto& r : roles_)
        if (r.name == name) return &r;
    return nullptr;
}

void RoxalStore::onRoxalChange(const Role& role)
{
    if (suppress_ == role.name)
        return;   // this change is the echo of the JS write we are servicing
    // Record only. Pushing here would cost a main-thread turn per assignment, and
    // a view only needs the latest value -- so coalesce and push once per turn.
    std::lock_guard<std::mutex> lock(dirtyMutex_);
    dirty_.insert(role.nameHash);
}

void RoxalStore::markDirty(const ustring& prop)
{
    std::lock_guard<std::mutex> lock(dirtyMutex_);
    dirty_.insert(prop.hashCode());
}

void RoxalStore::markAllDirty()
{
    std::lock_guard<std::mutex> lock(dirtyMutex_);
    for (const auto& r : roles_) dirty_.insert(r.nameHash);
}

void RoxalStore::define()
{
    Encoder e;
    e.op(Op::StoreDefine);
    e.str(name_);

    e.tag(Tag::Dict);
    e.u32(static_cast<uint32_t>(roles_.size()));
    for (const auto& r : roles_) {
        e.str(r.name);
        try { e.value(readRole(r)); }
        catch (const std::exception&) { e.tag(Tag::Nil); }   // not representable -> null
    }

    e.tag(Tag::List);
    e.u32(static_cast<uint32_t>(methodNames_.size()));
    for (const auto& m : methodNames_) e.str(toUTF8StdString(m));

    exec(e);
    { std::lock_guard<std::mutex> lock(dirtyMutex_); dirty_.clear(); }
}

void RoxalStore::flushDirty()
{
    // Take the whole set under the lock and encode outside it: reading a role can
    // re-enter the VM (a computed property calls its getter), which must not
    // happen while holding a lock the engine thread wants.
    std::set<int32_t> dirty;
    {
        std::lock_guard<std::mutex> lock(dirtyMutex_);
        if (dirty_.empty()) return;
        dirty.swap(dirty_);
    }

    Encoder e;
    e.op(Op::StorePatch);
    e.str(name_);
    e.tag(Tag::Dict);
    e.u32(static_cast<uint32_t>(dirty.size()));
    for (int32_t hash : dirty) {
        const Role* role = nullptr;
        for (const auto& r : roles_) if (r.nameHash == hash) { role = &r; break; }
        if (!role) { e.str(std::string("?")); e.tag(Tag::Nil); continue; }
        e.str(role->name);
        try { e.value(readRole(*role)); }
        catch (const std::exception&) { e.tag(Tag::Nil); }
    }
    defer(e);   // a UI update never justifies a round trip
}

void RoxalStore::applyWrite(const std::string& prop, const Value& value)
{
    const Role* r = roleByName(prop);
    if (!r || !isObjectInstance(obj_)) return;
    if (!r->editable) {
        std::cerr << "web: ignoring write to read-only '" << name_ << "." << prop << "'"
                  << std::endl;
        return;
    }

    suppress_ = prop;   // do not echo our own write back to JS
    if (r->computed) {
        VM::instance().invokeMethod(obj_, r->setterName, { value });
    } else {
        // A signal-valued property must have the value pushed INTO the signal.
        // Assigning over the slot would replace the signal object itself, which
        // silently demolishes whatever dataflow network it participates in --
        // the property would still read fine, and every derived node feeding off
        // it would quietly stop updating.
        const Value current = readRole(*r);
        if (isSignal(current))
            asSignal(current)->signal->set(value);
        else
            asObjectInstance(obj_)->propertySlot(r->nameHash).assign(value);
    }
    suppress_.clear();

    // Push the post-write value: a transforming setter may store something other
    // than what JS sent, and the view must show what Roxal actually holds.
    dirty_.insert(r->nameHash);
}

void RoxalStore::invoke(const std::string& method, const std::vector<Value>& args,
                        uint32_t callId)
{
    Encoder e;
    e.op(Op::StoreResolve);
    e.u32(callId);

    if (!isObjectInstance(obj_)) {
        e.tag(Tag::Nil);
        e.str(std::string("store '") + name_ + "' is not an object");
        defer(e);
        return;
    }

    // invokeMethod re-enters the dispatch loop; safe here because this runs from
    // the host-loop pump, which is itself inside the loop.
    auto [status, result] = VM::instance().invokeMethod(
        obj_, ustring::fromUTF8(method), args);

    if (status != ExecutionStatus::OK) {
        e.tag(Tag::Nil);
        e.str(std::string("Roxal method '") + method + "' failed");
    } else {
        try {
            e.value(result);
            e.tag(Tag::Nil);           // no error
        } catch (const std::exception& ex) {
            Encoder fresh;             // the partial value must not reach JS
            fresh.op(Op::StoreResolve);
            fresh.u32(callId);
            fresh.tag(Tag::Nil);
            fresh.str(std::string(ex.what()));
            defer(fresh);
            return;
        }
    }
    defer(e);
}

// ============================================================
// WebStoreHub
// ============================================================

struct WebStoreHub::Impl {
    // Each exposed object stays reachable for as long as it is exposed; its
    // properties and change observers follow from that.
    static void traceStores(ValueVisitor& visitor,
                            const std::vector<std::unique_ptr<RoxalStore>>& stores) {
        for (auto& s : stores)
            if (s) visitor.visit(s->objValue());
    }
    TracedMember<std::vector<std::unique_ptr<RoxalStore>>> stores { &Impl::traceStores };
    std::unordered_map<std::string, RoxalStore*> byName;
};

WebStoreHub::WebStoreHub() : impl_(std::make_unique<Impl>()) {}
WebStoreHub::~WebStoreHub() { shutdown(); }

WebStoreHub& WebStoreHub::instance()
{
    static WebStoreHub hub;
    return hub;
}

RoxalStore* WebStoreHub::expose(const std::string& name, const Value& obj)
{
    if (!isObjectInstance(obj)) return nullptr;

    auto it = impl_->byName.find(name);
    if (it != impl_->byName.end()) {
        if (it->second->objValue().asObj() == obj.asObj())
            return it->second;                    // same object: nothing to do

        // Different object under the same name -- an edited script re-run. Replace
        // it: reusing the old object would silently ignore the user's edit, which
        // is the worst possible behaviour in a live editor. The old store's alive_
        // flag drops, so its pending callbacks become no-ops.
        RoxalStore* stale = it->second;
        auto& all = *impl_->stores;
        all.erase(std::remove_if(all.begin(), all.end(),
                                 [stale](const std::unique_ptr<RoxalStore>& s) {
                                     return s.get() == stale;
                                 }), all.end());
        impl_->byName.erase(it);
    }

    auto s = std::make_unique<RoxalStore>(name, obj);
    RoxalStore* raw = s.get();
    impl_->stores->push_back(std::move(s));
    impl_->byName[name] = raw;
    raw->define();
    return raw;
}

RoxalStore* WebStoreHub::lookup(const std::string& name)
{
    auto it = impl_->byName.find(name);
    return it != impl_->byName.end() ? it->second : nullptr;
}

void WebStoreHub::flushAll()
{
    for (auto& s : *impl_->stores)
        if (s) s->flushDirty();
}

void WebStoreHub::shutdown()
{
    impl_->byName.clear();
    impl_->stores->clear();
}

#endif // __EMSCRIPTEN__
