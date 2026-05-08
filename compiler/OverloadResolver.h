#pragma once

#include <vector>
#include <string>
#include <cstdint>

#include <core/types.h>
#include <core/common.h>
#include <unicode/unistr.h>

#include <compiler/Value.h>


namespace roxal {


class VM;


// Selects an overload from a candidate set by ranking each candidate against
// the supplied argument types. The same algorithm drives:
//  - Compile-time resolution in RoxalCompiler::visit(Call). Argument types
//    come from TypeDeducer-deduced ast->type attributes; unknown ones (no
//    type attr) are passed as nullptr.
//  - Runtime resolution in VM::callValue / VM::invoke when the callee is an
//    OverloadSet that survived to runtime (because compile-time information
//    was insufficient).
//
// The resolver does NOT touch the VM stack itself — callers extract argument
// types into ArgInfo and consume the chosen index.
class OverloadResolver
{
public:
    // Argument observed at the call site. type is null when unknown
    // (e.g. compile-time call with no deduced type for that arg).
    struct ArgInfo {
        ptr<type::Type> type;
        bool            isNamed = false;
        int32_t         nameHash = 0;
    };

    // One overload to consider. funcType describes the parameter signature
    // and return types; target is the closure (function) or method closure
    // that the caller will actually invoke if this candidate wins.
    struct Candidate {
        ptr<type::Type> funcType;
        Value           target;
        bool            isMethod = false;  // affects 'this' arity bookkeeping
    };

    // A per-candidate, per-call score. Encoded as a sum of per-arg type
    // ranks, with smaller = better, plus a tie-breaker for "params filled
    // by their default value". Per-arg type ranks (best -> worst):
    //   0: exact (same builtin or same object/actor type name)
    //   1: subtype (extends/implements chain)
    //   2: strict implicit conversion (widening that's valid in any context,
    //      e.g. byte -> int, int -> real)
    //   3: untyped (param has no declared type — wildcard)
    //   4: non-strict-only implicit conversion (lossy or surprising, e.g.
    //      string -> int — only feasible when the call site is non-strict;
    //      ranked below untyped so a wildcard catches it instead of silently
    //      coercing)
    //   5: user-defined implicit conversion (operator T() on source, or
    //      implicit init(S) on target). Admitted permissively by the
    //      resolver — VM::tryConvertValue is the actual gatekeeper at the
    //      call boundary and rejects explicit-only conversions.
    //   6: variadic absorption
    // isBetter() compares lexicographically: feasibility, then summed type
    // rank (lower is better), then defaultsActivated (lower is better).
    struct Score {
        bool     feasible           = false;
        uint32_t totalRank          = 0;  // sum of per-arg ranks
        uint16_t defaultsActivated  = 0;  // params satisfied by defaults
    };

    struct ResolveResult {
        enum Kind {
            ResolvedUnique,  // exactly one best candidate; chosenIndex valid
            Ambiguous,       // multiple equally-good candidates; tiedIndices populated
            NoMatch,         // no candidate is feasible
            NeedsRuntime     // unique with known types, but unknown types could flip the order
        };
        Kind                  kind = NoMatch;
        uint16_t              chosenIndex = 0;
        std::vector<uint16_t> tiedIndices;
    };

    // For runtime resolution, the resolver can probe user-defined implicit
    // conversions and walk runtime inheritance chains via the supplied VM.
    // For compile-time resolution, pass nullptr — only static information
    // (the type::Type tree) is used for ranking.
    explicit OverloadResolver(VM* vm = nullptr) : vm_(vm) {}

    // Main entry point. staticDispatchAttempt controls how unknown arg types
    // are handled:
    //   true  - unknowns are tolerated (compatible with any param), and the
    //           result is ResolvedUnique only when the chosen candidate is
    //           strictly better than every other candidate even allowing for
    //           the unknowns flipping the score. Otherwise NeedsRuntime.
    //   false - unknown args should not occur (runtime path); each Value's
    //           type is resolvable via the VM helper.
    ResolveResult resolve(const std::vector<Candidate>& candidates,
                          const std::vector<ArgInfo>&   args,
                          bool                          staticDispatchAttempt,
                          bool                          strictMode);

    // Score one candidate against the args.
    Score scoreOne(const Candidate&            candidate,
                   const std::vector<ArgInfo>& args,
                   bool                        strictMode);

    // True if a is strictly better than b.
    static bool isBetter(const Score& a, const Score& b);

    // Build a human-readable diagnostic for an Ambiguous or NoMatch result.
    // callName is the user-facing name of the call ("foo", "obj.method").
    std::string ambiguityDiagnostic(const icu::UnicodeString&     callName,
                                    const std::vector<Candidate>& candidates,
                                    const std::vector<uint16_t>&  tied,
                                    const std::vector<ArgInfo>&   args);

    std::string noMatchDiagnostic(const icu::UnicodeString&     callName,
                                  const std::vector<Candidate>& candidates,
                                  const std::vector<ArgInfo>&   args);

    // Interface conformance: true if `concrete` is a valid implementation
    // of `abstract`. Requires same parameter count, exact-match parameter
    // types (invariant params: same builtin or same ObjectType name), and
    // a covariant return type (concrete's return is the same as or a
    // subtype of abstract's). Both arguments are FuncType-bearing
    // type::Type ptrs; null or non-FuncType inputs return false.
    static bool signatureCompatibleForOverride(const ptr<type::Type>& abstract,
                                               const ptr<type::Type>& concrete);

    // Render a one-line description of a method signature, like
    // "f(x :int, y :string) -> bool", suitable for error messages.
    static std::string signatureToString(const icu::UnicodeString& name,
                                         const ptr<type::Type>&    funcType);

private:
    VM* vm_;  // optional - non-null only for runtime resolution
};


// Helper used by the runtime path: derive a static type::Type representation
// from a runtime Value, sufficient for the resolver's compatibility tests.
// For object/actor instances, the returned Type carries the instance type's
// name so subtype walks can climb it; for primitives, the BuiltinType is
// set. Returns nullptr if the Value has no meaningful type for ranking.
ptr<type::Type> valueRuntimeType(const Value& v);


}  // namespace roxal
