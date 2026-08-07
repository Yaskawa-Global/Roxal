#pragma once

#ifdef __EMSCRIPTEN__

#include "Object.h"
#include "Value.h"
#include "core/ustring.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace roxal {
namespace web {

// Exposes ONE Roxal object to JavaScript as a subscribable store: a snapshot of
// its public properties plus its public methods.
//
// A direct port of the qt module's RoxalPropertyMap (compiler/qt/QtBindable.*),
// role for role — stored properties from orderedPublicProperties(), computed ones
// discovered from their __get_/__set_ methods, changes observed through
// ChangeNotifier. Only the far side differs: QML binds to a QQmlPropertyMap,
// whereas here a delta is pushed across the bridge and JS notifies subscribers.
//
// Two differences that follow from the bridge rather than from taste:
//
//  * Changes are COALESCED, not pushed per write. Each push costs a main-thread
//    turn, and a view only ever needs the latest value — so dirty roles collect
//    and flush once per turn.
//  * A `signal`-valued property crosses as its current value, and is observed on
//    the signal itself -- the property slot holds the same ObjSignal forever, so
//    the slot observer never fires for it. Note the frequency argument does NOT
//    throttle a source signal: it fires on every set().
class RoxalStore {
public:
    RoxalStore(std::string name, const Value& obj);
    ~RoxalStore();

    const Value& objValue() const { return obj_; }          // for the hub's GC root
    const std::string& name() const { return name_; }

    // Send the store's shape and initial values to the main thread.
    void define();

    // Push accumulated changes, if any. Called once per turn from the host loop.
    void flushDirty();

    // Mark every role dirty — for web.notify(), where a contained collection was
    // mutated in place so no assignment (and so no change notification) happened.
    void markAllDirty();
    void markDirty(const ustring& prop);

    // --- JS -> Roxal ---
    void applyWrite(const std::string& prop, const Value& value);
    void invoke(const std::string& method, const std::vector<Value>& args, uint32_t callId);

private:
    struct Role {
        ustring uname;
        std::string name;
        int32_t nameHash = 0;
        bool editable = false;
        bool computed = false;          // accessor property: read __get_, write __set_
        ustring getterName;
        ustring setterName;
        int32_t backingHash = 0;        // "_<name>" backing field (computed; auto-notify)
    };

    void buildRoles();
    void buildMethods();
    void hookChanges();
    Value readRole(const Role& role) const;
    const Role* roleByName(const std::string& name) const;
    void onRoxalChange(const Role& role);

    Value obj_;                                   // traced by the hub
    std::string name_;
    std::vector<Role> roles_;
    std::vector<ustring> methodNames_;
    std::shared_ptr<std::atomic<bool>> alive_;    // guards callbacks past destruction
    // Guarded: a signal-valued role is marked dirty from the DATAFLOW ENGINE
    // thread, not the VM thread. Only role hashes cross that boundary -- no VM
    // state -- but the container itself still needs a lock.
    mutable std::mutex dirtyMutex_;
    std::set<int32_t> dirty_;                     // role hashes awaiting a push
    std::string suppress_;                        // re-entrancy guard during a JS write
};

// Owns every live store and keeps the wrapped objects GC-alive. Singleton,
// bracketed by the web module's load/shutdown — mirrors QtBindHub.
class WebStoreHub {
public:
    static WebStoreHub& instance();

    // Expose `obj` under `name` (idempotent per name). Returns nullptr if obj is
    // not an object instance.
    RoxalStore* expose(const std::string& name, const Value& obj);
    RoxalStore* lookup(const std::string& name);

    void flushAll();     // push every store's pending changes
    void shutdown();

private:
    WebStoreHub();
    ~WebStoreHub();
    WebStoreHub(const WebStoreHub&) = delete;
    WebStoreHub& operator=(const WebStoreHub&) = delete;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace web
} // namespace roxal

#endif // __EMSCRIPTEN__
