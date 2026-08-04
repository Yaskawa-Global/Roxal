#include <compiler/OverloadResolver.h>

#include <compiler/Object.h>
#include <core/AST.h>

#include <sstream>


namespace roxal {


using type::BuiltinType;
using FuncType = type::Type::FuncType;
using ParamType = type::Type::FuncType::ParamType;
using ObjectType = type::Type::ObjectType;


// Per-argument compatibility result. The numeric value IS the rank used
// in scoring (lower = better). Two tiers of implicit conversion:
//  - StrictImplicitConv (rank 2) — safe widening (byte→int, int→real, etc.)
//    that is allowed in BOTH strict and non-strict contexts.
//  - NonStrictImplicitConv (rank 4) — lossy / surprising conversions
//    (e.g. string→int, real→int) that are only allowed in non-strict
//    context and are ranked below untyped so a wildcard catches them
//    rather than silently coercing.
enum class ArgRank : uint8_t {
    Exact                    = 0,
    Subtype                  = 1,
    StrictImplicitConv       = 2,
    Untyped                  = 3,
    NonStrictImplicitConv    = 4,
    UserDefinedImplicitConv  = 5,  // operator T() / implicit init(S) — runtime-verified
    VariadicAbsorb           = 6,
    Incompatible             = 255
};


// Look for a method on `obj`'s type that exactly matches `methodName` and
// is marked `implicit`. Walks the extends chain. Returns true if found
// and (when checkArity1Init is true) the matched method has arity 1
// (single-arg init) whose first param's type matches expectedParamBuiltin
// (and expectedParamObjName when non-empty).
//
// `expectedParamObjName` is passed by value (ustring is a string
// handle, cheap to copy) — empty means "don't constrain by name" for
// non-object-typed sources. This avoids needing a nullable ObjectType
// pointer parameter.
static bool hasImplicitMethod(const ObjectType&         obj,
                              const ustring& methodName,
                              bool                      checkArity1Init,
                              const ustring& expectedParamObjName,
                              type::BuiltinType         expectedParamBuiltin)
{
    for (const auto& mi : obj.methods) {
        if (mi.name != methodName) continue;
        if (!ast::hasModifier(mi.methodModifiers, ast::MethodModifier::Implicit)) continue;
        if (!mi.funcType) continue;
        if (checkArity1Init) {
            if (mi.funcType->params.size() != 1) continue;
            if (!mi.funcType->params[0].has_value()) continue;
            auto& p = mi.funcType->params[0].value();
            if (!p.type.has_value() || !p.type.value()) continue;
            if (p.type.value()->builtin != expectedParamBuiltin) continue;
            if (!expectedParamObjName.isEmpty() &&
                (!p.type.value()->obj.has_value() ||
                 p.type.value()->obj.value().name != expectedParamObjName))
                continue;
        }
        return true;
    }
    if (obj.extends.has_value() && obj.extends.value() &&
        obj.extends.value()->obj.has_value())
    {
        return hasImplicitMethod(obj.extends.value()->obj.value(),
                                 methodName, checkArity1Init,
                                 expectedParamObjName, expectedParamBuiltin);
    }
    return false;
}


// True if there's a plausible user-defined implicit conversion from
// srcType to dstType. Two paths are checked:
//   - src has `implicit operator->{dstName}()`
//   - dst has `implicit init(src)`
// Returns true if we can PROVE a path exists. Returns false only when we
// have full type info on BOTH sides and have proved no path exists.
// Returns true permissively in other cases — TypeDeducer's metadata may
// be incomplete and the runtime tryConvertValue is still the final
// gatekeeper, so a false negative here would silently prune a valid
// overload.
static bool userDefinedImplicitConvFeasible(const ptr<type::Type>& srcType,
                                            const ptr<type::Type>& dstType)
{
    if (!srcType || !dstType) return true;

    bool srcIsObj = (srcType->builtin == type::BuiltinType::Object ||
                     srcType->builtin == type::BuiltinType::Actor);
    bool dstIsObj = (dstType->builtin == type::BuiltinType::Object ||
                     dstType->builtin == type::BuiltinType::Actor);

    bool srcInfoOk = !srcIsObj || srcType->obj.has_value();
    bool dstInfoOk = !dstIsObj || dstType->obj.has_value();

    // Without complete type info on the relevant side(s), stay permissive.
    if (!srcInfoOk || !dstInfoOk)
        return true;

    // Source-side check: src has `implicit operator->{dst}()`?
    if (srcIsObj) {
        ustring convNameTarget;
        if (dstIsObj) {
            convNameTarget = dstType->obj.value().name;
        } else {
            std::string s = type::to_string(dstType->builtin);
            convNameTarget = toUnicodeString(s);
        }
        ustring operatorConvName = ustring("operator->") + convNameTarget;
        if (hasImplicitMethod(srcType->obj.value(), operatorConvName,
                              /*checkArity1Init=*/false,
                              ustring(), type::BuiltinType::Nil))
            return true;
    }

    // Target-side check: dst has `implicit init(src)`?
    if (dstIsObj) {
        ustring srcObjName = (srcIsObj && srcType->obj.has_value())
                                        ? srcType->obj.value().name
                                        : ustring();
        if (hasImplicitMethod(dstType->obj.value(),
                              ustring("init"),
                              /*checkArity1Init=*/true,
                              srcObjName, srcType->builtin))
            return true;
    }

    // Both sides fully checked, neither path matched.
    return false;
}


// Walk the extends/implements chain of an object type looking for a name match.
static bool objectTypeChainContains(const ObjectType& sub, const ustring& targetName)
{
    if (sub.name == targetName)
        return true;

    if (sub.extends.has_value()) {
        auto& parent = sub.extends.value();
        if (parent && parent->obj.has_value()) {
            if (objectTypeChainContains(parent->obj.value(), targetName))
                return true;
        }
    }

    for (auto& iface : sub.implements) {
        if (iface && iface->obj.has_value()) {
            if (objectTypeChainContains(iface->obj.value(), targetName))
                return true;
        }
    }

    return false;
}


static ArgRank classifyArg(const ParamType& param,
                           const ptr<type::Type>& argType,
                           bool strictMode,
                           bool argIsAbsorbedByVariadic)
{
    if (argIsAbsorbedByVariadic)
        return ArgRank::VariadicAbsorb;

    // Param has no declared type → wildcard.
    if (!param.type.has_value())
        return ArgRank::Untyped;

    auto& paramType = param.type.value();
    if (!paramType)
        return ArgRank::Untyped;

    // Unknown arg type at compile time → optimistically rank as exact match.
    // The caller (resolve) is responsible for tracking unknowns and turning
    // a tentative ResolvedUnique into NeedsRuntime when an unknown could
    // have flipped the decision.
    if (!argType)
        return ArgRank::Exact;

    BuiltinType pb = paramType->builtin;
    BuiltinType ab = argType->builtin;

    // Object/actor exact + subtype handling.
    if ((pb == BuiltinType::Object || pb == BuiltinType::Actor) &&
        paramType->obj.has_value() &&
        (ab == BuiltinType::Object || ab == BuiltinType::Actor) &&
        argType->obj.has_value())
    {
        auto& argObj = argType->obj.value();
        auto& paramObj = paramType->obj.value();

        if (argObj.name == paramObj.name)
            return ArgRank::Exact;

        if (objectTypeChainContains(argObj, paramObj.name))
            return ArgRank::Subtype;

        // Object→non-matching-Object: feasible only if the source has an
        // `implicit operator->{dstName}()` or the target has an
        // `implicit init(srcType)` — checked statically using the
        // method modifiers now carried on compile-time MethodInfo.
        // userDefinedImplicitConvFeasible stays permissive when type info
        // is incomplete; VM::tryConvertValue is the runtime gatekeeper.
        if (userDefinedImplicitConvFeasible(argType, paramType))
            return ArgRank::UserDefinedImplicitConv;
        return ArgRank::Incompatible;
    }

    // Builtin exact match.
    if (pb == ab)
        return ArgRank::Exact;

    // Two-tier builtin implicit conversion: strict-widening vs non-strict-only.
    bool strictOk = type::convertibleTo(ab, pb, /*strict=*/true);
    if (strictOk)
        return ArgRank::StrictImplicitConv;
    bool nonStrictOk = type::convertibleTo(ab, pb, /*strict=*/false);
    if (nonStrictOk && !strictMode)
        return ArgRank::NonStrictImplicitConv;

    // Builtin source → object/actor target: feasible only if the target
    // has an `implicit init(srcBuiltin)` overload. Permissive when type
    // info on either side is incomplete.
    if (pb == BuiltinType::Object || pb == BuiltinType::Actor) {
        if (userDefinedImplicitConvFeasible(argType, paramType))
            return ArgRank::UserDefinedImplicitConv;
    }

    return ArgRank::Incompatible;
}


OverloadResolver::Score
OverloadResolver::scoreOne(const Candidate&            cand,
                           const std::vector<ArgInfo>& args,
                           bool                        strictMode)
{
    Score s;

    if (!cand.funcType || !cand.funcType->func.has_value()) {
        return s;  // not a callable — infeasible
    }
    auto& fn = cand.funcType->func.value();
    auto& params = fn.params;

    // Determine required vs defaulted regular param counts; identify variadic.
    int regularCount = 0;
    int requiredCount = 0;
    bool hasVariadic = false;
    int variadicAt = -1;

    for (size_t i = 0; i < params.size(); ++i) {
        if (!params[i].has_value()) {
            // unnamed param slot — treat as a regular required positional.
            ++regularCount;
            ++requiredCount;
            continue;
        }
        auto& p = params[i].value();
        if (p.variadic) {
            hasVariadic = true;
            variadicAt = static_cast<int>(i);
            break;  // variadic must be last
        }
        ++regularCount;
        if (!p.hasDefault)
            ++requiredCount;
    }

    int argCount = static_cast<int>(args.size());

    // Arity feasibility.
    if (hasVariadic) {
        if (argCount < requiredCount)
            return s;
    } else {
        if (argCount < requiredCount || argCount > regularCount)
            return s;
    }

    s.feasible = true;

    // Per-arg ranking.
    for (int i = 0; i < argCount; ++i) {
        bool absorbed = hasVariadic && i >= variadicAt;
        ArgRank rank;

        if (absorbed) {
            // The variadic param's element type, if any, can apply per-element.
            // For now treat absorption as a sticky variadic match without
            // discriminating element types. (Refine later if needed.)
            rank = ArgRank::VariadicAbsorb;
        } else if (static_cast<size_t>(i) >= params.size() || !params[i].has_value()) {
            // Position with no param slot (shouldn't happen given arity check).
            rank = ArgRank::Incompatible;
        } else {
            rank = classifyArg(params[i].value(), args[i].type, strictMode, false);
        }

        if (rank == ArgRank::Incompatible) {
            s.feasible = false;
            return s;
        }
        s.totalRank += static_cast<uint32_t>(rank);
    }

    // Count params satisfied by their default value (params past argCount
    // that have a default). Used as a tie-breaker so a candidate with
    // matching arity beats one that fills the gap with defaults.
    if (!hasVariadic) {
        for (int i = argCount; i < (int)params.size(); ++i) {
            if (params[i].has_value() && params[i]->hasDefault)
                ++s.defaultsActivated;
        }
    }

    return s;
}


bool OverloadResolver::isBetter(const Score& a, const Score& b)
{
    if (a.feasible != b.feasible)
        return a.feasible && !b.feasible;
    if (a.totalRank != b.totalRank)
        return a.totalRank < b.totalRank;
    if (a.defaultsActivated != b.defaultsActivated)
        return a.defaultsActivated < b.defaultsActivated;
    return false;  // tied
}


OverloadResolver::ResolveResult
OverloadResolver::resolve(const std::vector<Candidate>& candidates,
                          const std::vector<ArgInfo>&   args,
                          bool                          staticDispatchAttempt,
                          bool                          strictMode)
{
    ResolveResult r;

    if (candidates.empty()) {
        r.kind = ResolveResult::NoMatch;
        return r;
    }

    bool anyUnknownArg = false;
    if (staticDispatchAttempt) {
        for (auto& a : args) {
            if (!a.type) { anyUnknownArg = true; break; }
        }
    }

    std::vector<Score> scores;
    scores.reserve(candidates.size());
    for (auto& c : candidates)
        scores.push_back(scoreOne(c, args, strictMode));

    // Find best feasible.
    int bestIdx = -1;
    for (size_t i = 0; i < scores.size(); ++i) {
        if (!scores[i].feasible) continue;
        if (bestIdx < 0 || isBetter(scores[i], scores[bestIdx]))
            bestIdx = static_cast<int>(i);
    }

    if (bestIdx < 0) {
        r.kind = ResolveResult::NoMatch;
        return r;
    }

    // Count ties at the best score.
    std::vector<uint16_t> tied;
    for (size_t i = 0; i < scores.size(); ++i) {
        if (!scores[i].feasible) continue;
        if (static_cast<int>(i) == bestIdx) { tied.push_back(static_cast<uint16_t>(i)); continue; }
        if (!isBetter(scores[bestIdx], scores[i]))
            tied.push_back(static_cast<uint16_t>(i));  // not strictly worse → tied
    }

    if (tied.size() > 1) {
        // Multiple equally-good candidates.
        if (staticDispatchAttempt && anyUnknownArg) {
            r.kind = ResolveResult::NeedsRuntime;
            r.tiedIndices = std::move(tied);
            return r;
        }
        r.kind = ResolveResult::Ambiguous;
        r.tiedIndices = std::move(tied);
        return r;
    }

    // Unique best at compile time, but unknown args might have flipped any
    // feasible candidate from worse to tied. Conservatively defer to runtime.
    if (staticDispatchAttempt && anyUnknownArg) {
        // Check: is there any other feasible candidate that an unknown arg
        // could plausibly promote to a tie? For now, if any other feasible
        // candidate exists, defer.
        size_t feasibleCount = 0;
        for (auto& s : scores) if (s.feasible) ++feasibleCount;
        if (feasibleCount > 1) {
            r.kind = ResolveResult::NeedsRuntime;
            r.chosenIndex = static_cast<uint16_t>(bestIdx);
            return r;
        }
    }

    r.kind = ResolveResult::ResolvedUnique;
    r.chosenIndex = static_cast<uint16_t>(bestIdx);
    return r;
}


std::string OverloadResolver::ambiguityDiagnostic(
    const ustring&     callName,
    const std::vector<Candidate>& candidates,
    const std::vector<uint16_t>&  tied,
    const std::vector<ArgInfo>&   args)
{
    std::ostringstream os;
    std::string name;
    callName.toUTF8String(name);
    os << "Ambiguous call to '" << name << "' — ";
    os << tied.size() << " overloads match equally well:\n";
    for (auto idx : tied) {
        if (idx < candidates.size() && candidates[idx].funcType) {
            os << "  " << candidates[idx].funcType->toString() << "\n";
        }
    }
    return os.str();
}


std::string OverloadResolver::noMatchDiagnostic(
    const ustring&     callName,
    const std::vector<Candidate>& candidates,
    const std::vector<ArgInfo>&   args)
{
    std::ostringstream os;
    std::string name;
    callName.toUTF8String(name);
    os << "No matching overload for call to '" << name << "' (" << args.size() << " args).\n";
    os << "Candidates:\n";
    for (auto& c : candidates) {
        if (c.funcType) os << "  " << c.funcType->toString() << "\n";
    }
    return os.str();
}


ptr<type::Type> valueRuntimeType(const Value& v)
{
    // Map a runtime Value to a minimal type::Type suitable for ranking. For
    // primitives we set the BuiltinType. For object/actor instances we also
    // populate ObjectType with the type's name so exact-match comparisons
    // against parameter types work. For subtype matching across object
    // inheritance, the resolver will need VM access; this helper only
    // captures local information.
    if (v.isNil()) {
        auto t = make_ptr<type::Type>();
        t->builtin = type::BuiltinType::Nil;
        return t;
    }
    if (v.isBool())  { auto t = make_ptr<type::Type>(); t->builtin = type::BuiltinType::Bool;  return t; }
    if (v.isByte())  { auto t = make_ptr<type::Type>(); t->builtin = type::BuiltinType::Byte;  return t; }
    if (v.isInt())   { auto t = make_ptr<type::Type>(); t->builtin = type::BuiltinType::Int;   return t; }
    if (v.isReal())  { auto t = make_ptr<type::Type>(); t->builtin = type::BuiltinType::Real;  return t; }

    if (!v.isObj())
        return nullptr;

    auto* obj = v.asObj();
    switch (obj->type) {
        case ObjType::String:  { auto t = make_ptr<type::Type>(); t->builtin = type::BuiltinType::String; return t; }
        case ObjType::Range:   { auto t = make_ptr<type::Type>(); t->builtin = type::BuiltinType::Range;  return t; }
        case ObjType::List:    { auto t = make_ptr<type::Type>(); t->builtin = type::BuiltinType::List;   return t; }
        case ObjType::Dict:    { auto t = make_ptr<type::Type>(); t->builtin = type::BuiltinType::Dict;   return t; }
        case ObjType::Vector:  { auto t = make_ptr<type::Type>(); t->builtin = type::BuiltinType::Vector; return t; }
        case ObjType::Matrix:  { auto t = make_ptr<type::Type>(); t->builtin = type::BuiltinType::Matrix; return t; }
        case ObjType::Tensor:  { auto t = make_ptr<type::Type>(); t->builtin = type::BuiltinType::Tensor; return t; }
        case ObjType::Orient:  { auto t = make_ptr<type::Type>(); t->builtin = type::BuiltinType::Orient; return t; }
        case ObjType::Signal:  { auto t = make_ptr<type::Type>(); t->builtin = type::BuiltinType::Signal; return t; }
        case ObjType::Instance: {
            auto t = make_ptr<type::Type>();
            auto* inst = static_cast<ObjectInstance*>(obj);
            t->builtin = type::BuiltinType::Object;
            ObjectType ot;
            if (inst && inst->instanceType.isObj()) {
                auto* ity = static_cast<ObjObjectType*>(inst->instanceType.asObj());
                if (ity) ot.name = ity->name;
            }
            t->obj = ot;
            return t;
        }
        case ObjType::Actor: {
            auto t = make_ptr<type::Type>();
            auto* inst = static_cast<ActorInstance*>(obj);
            t->builtin = type::BuiltinType::Actor;
            ObjectType ot;
            if (inst && inst->instanceType.isObj()) {
                auto* ity = static_cast<ObjObjectType*>(inst->instanceType.asObj());
                if (ity) ot.name = ity->name;
            }
            t->obj = ot;
            return t;
        }
        default:
            return nullptr;
    }
}


// Compare a single param type for invariant equality, used by the
// override-compatibility check. Both sides must be either both untyped
// (no declared type) or both typed with the same builtin and (for
// object/actor types) the same ObjectType name.
static bool paramTypeEqualsForOverride(const std::optional<ParamType>& a,
                                       const std::optional<ParamType>& b)
{
    if (a.has_value() != b.has_value()) return false;
    if (!a.has_value()) return true;
    auto& at = a->type;
    auto& bt = b->type;
    if (at.has_value() != bt.has_value()) return false;
    if (!at.has_value()) return true;  // both untyped — match
    if (!at.value() || !bt.value()) return false;
    if (at.value()->builtin != bt.value()->builtin) return false;
    bool isObj = (at.value()->builtin == BuiltinType::Object ||
                  at.value()->builtin == BuiltinType::Actor);
    if (isObj) {
        if (at.value()->obj.has_value() != bt.value()->obj.has_value()) return false;
        if (at.value()->obj.has_value() &&
            at.value()->obj.value().name != bt.value()->obj.value().name) return false;
    }
    if (a->variadic != b->variadic) return false;
    return true;
}


// Return type covariance: concrete's return type must be the same as or
// a subtype of abstract's. Builtin equality is required. For object/actor
// types, the compile-time type::Type::ObjectType doesn't reliably carry
// the extends/implements chain at the FuncType level (those fields are
// populated on the TypeDecl's own type::Type but not always on a FuncType's
// referenced return type). To avoid false rejections, we admit any
// object→object pairing and let the runtime type-assignment check at the
// actual call site enforce subtype safety. Same pattern as user-defined
// implicit conversion admission elsewhere in the resolver.
static bool returnTypeCovariant(const std::vector<ptr<type::Type>>& abs,
                                const std::vector<ptr<type::Type>>& con)
{
    if (abs.empty() && con.empty()) return true;
    if (abs.size() != con.size()) return false;
    for (size_t i = 0; i < abs.size(); ++i) {
        auto& a = abs[i];
        auto& c = con[i];
        if (!a || !c) return false;
        if (a->builtin != c->builtin) return false;
        if (a->builtin == BuiltinType::Object || a->builtin == BuiltinType::Actor) {
            // Both are object/actor; admit permissively and rely on the
            // runtime call-site type assignment to enforce subtype safety.
            // (The compile-time type::Type may not have obj.has_value() or
            // a populated extends chain on a FuncType return type ref.)
            continue;
        }
        // Otherwise builtin equality already verified.
    }
    return true;
}


bool OverloadResolver::signatureCompatibleForOverride(const ptr<type::Type>& abstract,
                                                      const ptr<type::Type>& concrete)
{
    if (!abstract || !concrete) return false;
    if (!abstract->func.has_value() || !concrete->func.has_value()) return false;
    auto& af = abstract->func.value();
    auto& cf = concrete->func.value();
    if (af.params.size() != cf.params.size()) return false;
    for (size_t i = 0; i < af.params.size(); ++i) {
        if (!paramTypeEqualsForOverride(af.params[i], cf.params[i]))
            return false;
    }
    return returnTypeCovariant(af.returnTypes, cf.returnTypes);
}


std::string OverloadResolver::signatureToString(const ustring& name,
                                                const ptr<type::Type>&    funcType)
{
    std::ostringstream os;
    std::string n; name.toUTF8String(n);
    os << n << "(";
    if (funcType && funcType->func.has_value()) {
        auto& fn = funcType->func.value();
        for (size_t i = 0; i < fn.params.size(); ++i) {
            if (i) os << ", ";
            const auto& p = fn.params[i];
            if (!p.has_value()) { os << "?"; continue; }
            std::string pn; p->name.toUTF8String(pn);
            os << pn;
            if (p->variadic) os << "...";
            if (p->type.has_value() && p->type.value()) {
                os << " :" << type::to_string(p->type.value()->builtin);
            }
        }
    }
    os << ")";
    return os.str();
}


}  // namespace roxal
