#pragma once

// Typed persistent GC roots.
//
// Conservative stack scanning covers Values in C++ STACK
// locals; Values retained in heap-resident C++ state (module containers,
// engine members) still need explicit rooting.  Instead of hand-written
// ExternalRootProvider::visitRoots() enumerations -- where forgetting one
// member is silent heap corruption -- the member itself becomes a traced
// type:
//
//     class ModuleFoo : public BuiltinModule {
//         PersistentRoot<Value> defaultThing;
//         TracedMember<std::unordered_map<std::string, Value>> things;
//         ...
//     };
//
// The wrapper self-registers with the collector on construction and
// unregisters on destruction; the mark phase traces every registered root.
// Containers of module-private structs pass a custom tracer function:
//
//     TracedMember<std::vector<Binding>> bindings { &traceBindings };
//
// Registration protocol (deliberately the simplest one): one mutex guards
// register/unregister AND the collector's mark-phase iteration -- a root
// can be constructed on a thread with no GC coverage yet, so iteration
// cannot rely on the stopped world alone.  Roots may not be created or
// destroyed inside an RT yield section (debug assert).
//
// Rule (greppable + audited; clang-tidy planned): module/engine classes must
// not hold raw `Value` (or Value-container) members -- wrap them.

#include <map>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "SimpleMarkSweepGC.h"
#include "Value.h"

namespace roxal {

// How to visit the Values retained by a T.  Specialise for further container
// shapes as they come up; types with module-private internals use the custom
// tracer constructor instead.
template<typename T>
struct GCTraceAdapter;  // no primary definition: unadapted T without a
                        // custom tracer is a compile error, on purpose

template<>
struct GCTraceAdapter<Value> {
    static void trace(ValueVisitor& visitor, const Value& v) {
        if (v.isObj())
            visitor.visit(v);
    }
};

template<typename K, typename H, typename E, typename A>
struct GCTraceAdapter<std::unordered_map<K, Value, H, E, A>> {
    static void trace(ValueVisitor& visitor,
                      const std::unordered_map<K, Value, H, E, A>& map) {
        for (const auto& entry : map)
            if (entry.second.isObj())
                visitor.visit(entry.second);
    }
};

template<typename K, typename C, typename A>
struct GCTraceAdapter<std::map<K, Value, C, A>> {
    static void trace(ValueVisitor& visitor, const std::map<K, Value, C, A>& map) {
        for (const auto& entry : map)
            if (entry.second.isObj())
                visitor.visit(entry.second);
    }
};

template<typename A>
struct GCTraceAdapter<std::vector<Value, A>> {
    static void trace(ValueVisitor& visitor, const std::vector<Value, A>& vec) {
        for (const Value& v : vec)
            if (v.isObj())
                visitor.visit(v);
    }
};

// Detection: does GCTraceAdapter<T> provide a trace()?  Types without one
// MUST be constructed with a custom tracer (compile-enforced below) -- a
// TracedMember that silently traced nothing would reintroduce the exact
// silent-root-hole failure mode this type exists to eliminate.
template<typename T, typename = void>
struct GCHasTraceAdapter : std::false_type {};
template<typename T>
struct GCHasTraceAdapter<T,
    std::void_t<decltype(GCTraceAdapter<T>::trace(
        std::declval<ValueVisitor&>(), std::declval<const T&>()))>>
    : std::true_type {};

// Owns a T and keeps its retained Values visible to the mark phase for the
// member's whole lifetime.  Access the payload via *, ->, get(), or the
// implicit conversion; assignment forwards to the payload.
template<typename T>
class TracedMember : public GCRootBase {
public:
    using Tracer = void (*)(ValueVisitor&, const T&);

    TracedMember() {
        static_assert(GCHasTraceAdapter<T>::value,
                      "no GCTraceAdapter<T>: pass a custom tracer "
                      "(TracedMember<T> member { &traceFn };)");
    }
    explicit TracedMember(Tracer tracer) : tracer_(tracer) {}
    explicit TracedMember(T initial) : value_(std::move(initial)) {
        static_assert(GCHasTraceAdapter<T>::value,
                      "no GCTraceAdapter<T>: pass a custom tracer "
                      "(TracedMember<T> member { &traceFn };)");
    }
    ~TracedMember() override = default;

    TracedMember& operator=(const T& v) { value_ = v; return *this; }
    TracedMember& operator=(T&& v) { value_ = std::move(v); return *this; }

    T& operator*() { return value_; }
    const T& operator*() const { return value_; }
    T* operator->() { return &value_; }
    const T* operator->() const { return &value_; }
    operator T&() { return value_; }
    operator const T&() const { return value_; }
    T& get() { return value_; }
    const T& get() const { return value_; }

    void traceRoot(ValueVisitor& visitor) const override {
        if constexpr (GCHasTraceAdapter<T>::value) {
            if (tracer_)
                tracer_(visitor, value_);
            else
                GCTraceAdapter<T>::trace(visitor, value_);
        } else {
            // Construction statically guarantees a tracer for unadapted T.
            tracer_(visitor, value_);
        }
    }

private:
    T value_ {};
    Tracer tracer_ { nullptr };
};

// Spec vocabulary: a single retained Value (or other scalar) root.
template<typename T>
using PersistentRoot = TracedMember<T>;

// Reference-semantics root: keeps an EXISTING variable's retained Values
// visible to the mark phase without copying it -- the replacement for
// participant setRootVisitor() lambdas over function locals and thread-
// lambda captures.  (Thread-lambda captures live in the closure's heap
// storage, which neither the conservative stack scan nor the address
// oracle covers -- these roots are load-bearing there, not belt-and-
// braces.)  Lifetime contract: declare AFTER the target so the root dies
// first; the target must outlive it (enforced by scoping, as with any
// reference).
template<typename T>
class TracedRef : public GCRootBase {
public:
    using Tracer = void (*)(ValueVisitor&, const T&);

    explicit TracedRef(const T& target) : target_(&target) {
        static_assert(GCHasTraceAdapter<T>::value,
                      "no GCTraceAdapter<T>: pass a custom tracer "
                      "(TracedRef<T> root { target, &traceFn };)");
    }
    TracedRef(const T& target, Tracer tracer)
        : target_(&target), tracer_(tracer) {}

    void traceRoot(ValueVisitor& visitor) const override {
        if constexpr (GCHasTraceAdapter<T>::value) {
            if (tracer_)
                tracer_(visitor, *target_);
            else
                GCTraceAdapter<T>::trace(visitor, *target_);
        } else {
            tracer_(visitor, *target_);
        }
    }

private:
    const T* target_;
    Tracer tracer_ { nullptr };
};

} // namespace roxal
