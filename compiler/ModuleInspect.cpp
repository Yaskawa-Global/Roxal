#include "ModuleInspect.h"
#include "VM.h"
#include "Object.h"
#include "ASTGenerator.h"
#include "AstPrinter.h"
#include "TypeDeducer.h"
#include "RoxalCompiler.h"
#include "SimpleMarkSweepGC.h"
#include "OverloadResolver.h"
#include "Error.h"
#include <core/AST.h>
#include <core/types.h>
#include <dataflow/DataflowEngine.h>
#include <dataflow/Signal.h>
#include <dataflow/FuncNode.h>

#include <sstream>
#include <fstream>
#include <filesystem>
#include <functional>
#include <typeinfo>
#include <unordered_map>
#include <algorithm>
#include <stdexcept>
#include <cctype>

using namespace roxal;

namespace roxal {
namespace {

// ---------------------------------------------------------------------------
// InspectConv: conversion context for one parse.  Builds mirror ObjectInstances
// (classes declared in modules/inspect.rox) from the C++ AST.  All allocation
// happens inside a NativeFn (i.e. inside execute()), so no GC cover is needed
// and C++ locals are safe: safepoints only occur in the dispatch loop.
// ---------------------------------------------------------------------------
struct InspectConv {
    Value moduleTypeVal;
    const std::string* src = nullptr;   // shared source text of the parse
    std::unordered_map<const ast::AST*, Value> mirrorOf;
    std::unordered_map<std::string, Value> typeCache;

    Value typeFor(const std::string& kind)
    {
        auto it = typeCache.find(kind);
        if (it != typeCache.end())
            return it->second;
        auto tv = asModuleType(moduleTypeVal)->vars.load(toUnicodeString(kind));
        if (!tv.has_value() || !isObjectType(tv.value()))
            throw std::runtime_error("inspect: mirror type '" + kind + "' not found in module");
        typeCache[kind] = tv.value();
        return tv.value();
    }

    // instance for a helper class with no backing AST node (Arg, ExceptClause)
    Value newHelper(const char* kind)
    {
        return Value::objVal(newObjectInstance(typeFor(kind)));
    }

    Value newNode(const char* kind, const ast::AST& n)
    {
        Value v = Value::objVal(newObjectInstance(typeFor(kind)));
        mirrorOf[&n] = v;
        ObjectInstance* inst = asObjectInstance(v);
        inst->setProperty("start_line", Value::intVal(int64_t(n.interval.first.line)));
        inst->setProperty("start_col", Value::intVal(int64_t(n.interval.first.pos)));
        inst->setProperty("end_line", Value::intVal(int64_t(n.interval.second.line)));
        inst->setProperty("end_col", Value::intVal(int64_t(n.interval.second.pos)));
        if (n.type.has_value() && n.type.value())
            inst->setProperty("deduced_type",
                              Value::stringVal(toUnicodeString(n.type.value()->toString())));
        if (!n.annotations.empty()) {
            Value lst = Value::listVal();
            for (auto& a : n.annotations) {
                Value av = node(a.get());
                setParent(av, v);
                asList(lst)->append(av);
            }
            inst->setProperty("annotations", lst);
        }
        return v;
    }

    void set(const Value& v, const char* name, const Value& val)
    {
        asObjectInstance(v)->setProperty(name, val);
    }

    void setParent(const Value& child, const Value& parent)
    {
        if (child.isNil())
            return;
        asObjectInstance(child)->setProperty("parent", parent);
    }

    // ---- scalar setters ---------------------------------------------------

    void setStr(const Value& v, const char* name, const std::string& s)
    {
        set(v, name, Value::stringVal(toUnicodeString(s)));
    }

    void setUStr(const Value& v, const char* name, const ustring& s)
    {
        set(v, name, Value::stringVal(s));
    }

    void setOptUStr(const Value& v, const char* name, const std::optional<ustring>& s)
    {
        if (s.has_value())
            set(v, name, Value::stringVal(s.value()));
    }

    void setUStrList(const Value& v, const char* name, const std::vector<ustring>& ss)
    {
        Value lst = Value::listVal();
        for (auto& s : ss)
            asList(lst)->append(Value::stringVal(s));
        set(v, name, lst);
    }

    void setBool(const Value& v, const char* name, bool b)
    {
        set(v, name, b ? Value::trueVal() : Value::falseVal());
    }

    void setBoolList(const Value& v, const char* name, const std::vector<bool>& bs)
    {
        Value lst = Value::listVal();
        for (bool b : bs)
            asList(lst)->append(b ? Value::trueVal() : Value::falseVal());
        set(v, name, lst);
    }

    void setNumVariant(const Value& v, const char* name,
                       const std::variant<int32_t, int64_t, double>& num)
    {
        if (std::holds_alternative<int32_t>(num))
            set(v, name, Value::intVal(std::get<int32_t>(num)));
        else if (std::holds_alternative<int64_t>(num))
            set(v, name, Value::intVal(std::get<int64_t>(num)));
        else
            set(v, name, Value::realVal(std::get<double>(num)));
    }

    // ---- type-reference setters -------------------------------------------

    static std::string varTypeToString(const ast::VarType& vt)
    {
        if (std::holds_alternative<type::BuiltinType>(vt))
            return type::to_string(std::get<type::BuiltinType>(vt));
        return toUTF8StdString(ast::joinTypeName(std::get<ast::TypeName>(vt)));
    }

    void setVarType(const Value& v, const char* name, const ast::VarType& vt)
    {
        setStr(v, name, varTypeToString(vt));
    }

    void setOptVarType(const Value& v, const char* name, const std::optional<ast::VarType>& vt)
    {
        if (vt.has_value())
            setVarType(v, name, vt.value());
    }

    void setOptVarTypeList(const Value& v, const char* name,
                           const std::optional<std::vector<ast::VarType>>& vts)
    {
        if (!vts.has_value())
            return;
        Value lst = Value::listVal();
        for (auto& vt : vts.value())
            asList(lst)->append(Value::stringVal(toUnicodeString(varTypeToString(vt))));
        set(v, name, lst);
    }

    void setOptTypeName(const Value& v, const char* name, const std::optional<ast::TypeName>& tn)
    {
        if (tn.has_value())
            set(v, name, Value::stringVal(ast::joinTypeName(tn.value())));
    }

    void setTypeNameList(const Value& v, const char* name, const std::vector<ast::TypeName>& tns)
    {
        Value lst = Value::listVal();
        for (auto& tn : tns)
            asList(lst)->append(Value::stringVal(ast::joinTypeName(tn)));
        set(v, name, lst);
    }

    // ---- enum setters -----------------------------------------------------

    void setAccess(const Value& v, const char* name, ast::Access a)
    {
        setStr(v, name, a == ast::Access::Private ? "private" : "public");
    }

    void setTypeDeclKind(const Value& v, const char* name, ast::TypeDecl::Kind k)
    {
        const char* s = "object";
        switch (k) {
            case ast::TypeDecl::Object:      s = "object"; break;
            case ast::TypeDecl::Actor:       s = "actor"; break;
            case ast::TypeDecl::Interface:   s = "interface"; break;
            case ast::TypeDecl::Enumeration: s = "enumeration"; break;
            case ast::TypeDecl::Event:       s = "event"; break;
        }
        setStr(v, name, s);
    }

    void setModifiers(const Value& v, const char* name, ast::MethodModifiers m)
    {
        Value lst = Value::listVal();
        if (ast::hasModifier(m, ast::MethodModifier::Implicit))
            asList(lst)->append(Value::stringVal(toUnicodeString("implicit")));
        if (ast::hasModifier(m, ast::MethodModifier::StatementAction))
            asList(lst)->append(Value::stringVal(toUnicodeString("statement_action")));
        if (ast::hasModifier(m, ast::MethodModifier::Abstract))
            asList(lst)->append(Value::stringVal(toUnicodeString("abstract")));
        set(v, name, lst);
    }

    // ---- node setters -----------------------------------------------------

    Value node(const ast::AST* n);  // dispatch — defined in InspectAstConv.inc

    template <class T>
    void setNode(const Value& v, const char* name, const ptr<T>& child)
    {
        if (!child)
            return;
        Value cv = node(child.get());
        set(v, name, cv);
        setParent(cv, v);
    }

    template <class T>
    void setOptNode(const Value& v, const char* name, const std::optional<ptr<T>>& child)
    {
        if (child.has_value())
            setNode(v, name, child.value());
    }

    template <class T>
    void setNodeList(const Value& v, const char* name, const std::vector<ptr<T>>& children)
    {
        Value lst = Value::listVal();
        for (auto& c : children) {
            Value cv = node(c.get());
            setParent(cv, v);
            asList(lst)->append(cv);
        }
        set(v, name, lst);
    }

    void setDeclStmtList(const Value& v, const char* name,
                         const std::vector<std::variant<ptr<ast::Declaration>, ptr<ast::Statement>>>& children)
    {
        Value lst = Value::listVal();
        for (auto& c : children) {
            const ast::AST* p = nullptr;
            if (std::holds_alternative<ptr<ast::Declaration>>(c))
                p = std::get<ptr<ast::Declaration>>(c).get();
            else
                p = std::get<ptr<ast::Statement>>(c).get();
            Value cv = node(p);
            setParent(cv, v);
            asList(lst)->append(cv);
        }
        set(v, name, lst);
    }

    void setBodyVariant(const Value& v, const char* name,
                        const std::variant<ptr<ast::Suite>, ptr<ast::Expression>, std::monostate>& body)
    {
        if (std::holds_alternative<ptr<ast::Suite>>(body))
            setNode(v, name, std::get<ptr<ast::Suite>>(body));
        else if (std::holds_alternative<ptr<ast::Expression>>(body))
            setNode(v, name, std::get<ptr<ast::Expression>>(body));
        // monostate (abstract): leave nil
    }

    void setAccessorVariant(const Value& v, const char* name, const char* abstractName,
                            const std::optional<std::variant<ptr<ast::Suite>, ptr<ast::Statement>, std::monostate>>& acc)
    {
        if (!acc.has_value())
            return;     // accessor not declared: node nil, abstract false
        auto& va = acc.value();
        if (std::holds_alternative<ptr<ast::Suite>>(va))
            setNode(v, name, std::get<ptr<ast::Suite>>(va));
        else if (std::holds_alternative<ptr<ast::Statement>>(va))
            setNode(v, name, std::get<ptr<ast::Statement>>(va));
        else
            setBool(v, abstractName, true);   // declared abstract
    }

    void setArgList(const Value& v, const char* name,
                    const std::vector<std::pair<ustring, ptr<ast::Expression>>>& args)
    {
        Value lst = Value::listVal();
        for (auto& a : args) {
            Value argV = newHelper("Arg");
            if (a.first.length() > 0)
                set(argV, "name", Value::stringVal(a.first));
            if (a.second) {
                Value ev = node(a.second.get());
                set(argV, "value", ev);
                setParent(ev, argV);
            }
            setParent(argV, v);
            asList(lst)->append(argV);
        }
        set(v, name, lst);
    }

    void setCondSuiteList(const Value& v, const char* name,
                          const std::vector<std::pair<ptr<ast::Expression>, ptr<ast::Suite>>>& pairs)
    {
        Value lst = Value::listVal();
        for (auto& p : pairs) {
            Value condV = node(p.first.get());
            Value suiteV = node(p.second.get());
            setParent(condV, v);
            setParent(suiteV, v);
            Value pairLst = Value::listVal();
            asList(pairLst)->append(condV);
            asList(pairLst)->append(suiteV);
            asList(lst)->append(pairLst);
        }
        set(v, name, lst);
    }

    void setCaseList(const Value& v, const char* name,
                     const std::vector<std::pair<std::vector<ptr<ast::Expression>>, ptr<ast::Suite>>>& cases)
    {
        Value lst = Value::listVal();
        for (auto& c : cases) {
            Value patterns = Value::listVal();
            for (auto& p : c.first) {
                Value pv = node(p.get());
                setParent(pv, v);
                asList(patterns)->append(pv);
            }
            Value suiteV = node(c.second.get());
            setParent(suiteV, v);
            Value caseLst = Value::listVal();
            asList(caseLst)->append(patterns);
            asList(caseLst)->append(suiteV);
            asList(lst)->append(caseLst);
        }
        set(v, name, lst);
    }

    void setPairExprList(const Value& v, const char* name,
                         const std::vector<std::pair<ptr<ast::Expression>, ptr<ast::Expression>>>& pairs)
    {
        Value lst = Value::listVal();
        for (auto& p : pairs) {
            Value kv = node(p.first.get());
            Value vv = node(p.second.get());
            setParent(kv, v);
            setParent(vv, v);
            Value pairLst = Value::listVal();
            asList(pairLst)->append(kv);
            asList(pairLst)->append(vv);
            asList(lst)->append(pairLst);
        }
        set(v, name, lst);
    }

    void setInterpParts(const Value& v, const char* name,
                        const std::vector<ast::StrInterpPart>& parts)
    {
        Value lst = Value::listVal();
        for (auto& p : parts) {
            if (p.isLiteral()) {
                asList(lst)->append(Value::stringVal(p.text));
            } else {
                Value ev = node(p.expr.get());
                setParent(ev, v);
                asList(lst)->append(ev);
            }
        }
        set(v, name, lst);
    }

    void setVarTargets(const Value& v, const char* name,
                       const std::vector<ast::VarDecl::Target>& targets)
    {
        Value lst = Value::listVal();
        for (auto& t : targets) {
            Value tv = newHelper("VarTarget");
            set(tv, "name", Value::stringVal(t.name));
            setOptVarType(tv, "var_type", t.varType);
            setBool(tv, "is_type_const", t.isTypeConst);
            setBool(tv, "is_type_mutable", t.isTypeMutable);
            setParent(tv, v);
            asList(lst)->append(tv);
        }
        set(v, name, lst);
    }

    void setExceptClauses(const Value& v, const char* name,
                          const std::vector<ast::TryStatement::ExceptClause>& clauses)
    {
        Value lst = Value::listVal();
        for (auto& c : clauses) {
            Value cl = newHelper("ExceptClause");
            if (c.type.has_value() && c.type.value()) {
                Value tv = node(c.type.value().get());
                set(cl, "type_expr", tv);
                setParent(tv, cl);
            }
            if (c.name.has_value())
                set(cl, "name", Value::stringVal(c.name.value()));
            if (c.body) {
                Value bv = node(c.body.get());
                set(cl, "body", bv);
                setParent(bv, cl);
            }
            setParent(cl, v);
            asList(lst)->append(cl);
        }
        set(v, name, lst);
    }
};

// ---------------------------------------------------------------------------
// InspectBuild: reverse conversion (Roxal mirror tree -> C++ AST) for
// unparse/compile.  Lenient on absent optionals (nil -> null/empty), strict on
// wrong types (throws with the offending field named).  Trivia decorations are
// carried into AST::attrs for AstPrinter to re-emit.
// ---------------------------------------------------------------------------
struct InspectBuild {
    ptr<ast::AST> build(const Value& v);   // generated dispatch (InspectAstConv.inc)

    // The mirror tree can be a DAG (e.g. TypeDeducer shares annotation nodes
    // between a FuncDecl and its Function) — rebuild each mirror object once
    // so sharing survives the round trip.
    std::unordered_map<Obj*, ptr<ast::AST>> memo;

    [[noreturn]] static void fail(const char* field, const std::string& msg)
    {
        throw std::runtime_error(std::string("inspect: field '") + field + "': " + msg);
    }

    static Value prop(ObjectInstance* inst, const char* name)
    {
        return inst->getProperty(name);
    }

    // ---- scalars ----------------------------------------------------------

    std::string getStr(ObjectInstance* inst, const char* name)
    {
        Value v = prop(inst, name);
        if (v.isNil()) return "";
        if (!isString(v)) fail(name, "expected a string, got " + v.typeName());
        return toUTF8StdString(asStringObj(v)->s);
    }

    ustring getUStr(ObjectInstance* inst, const char* name)
    {
        Value v = prop(inst, name);
        if (v.isNil()) return {};
        if (!isString(v)) fail(name, "expected a string, got " + v.typeName());
        return asStringObj(v)->s;
    }

    std::optional<ustring> getOptUStr(ObjectInstance* inst, const char* name)
    {
        Value v = prop(inst, name);
        if (v.isNil()) return std::nullopt;
        if (!isString(v)) fail(name, "expected a string or nil, got " + v.typeName());
        return asStringObj(v)->s;
    }

    std::vector<ustring> getUStrList(ObjectInstance* inst, const char* name)
    {
        std::vector<ustring> out;
        Value v = prop(inst, name);
        if (v.isNil()) return out;
        if (!isList(v)) fail(name, "expected a list of strings, got " + v.typeName());
        ObjList* l = asList(v);
        for (int i = 0; i < l->length(); i++) {
            Value e = l->getElement(i);
            if (!isString(e)) fail(name, "expected a list of strings");
            out.push_back(asStringObj(e)->s);
        }
        return out;
    }

    bool getBool(ObjectInstance* inst, const char* name)
    {
        Value v = prop(inst, name);
        if (v.isNil()) return false;
        if (!v.isBool()) fail(name, "expected a bool, got " + v.typeName());
        return v.asBool();
    }

    std::vector<bool> getBoolList(ObjectInstance* inst, const char* name)
    {
        std::vector<bool> out;
        Value v = prop(inst, name);
        if (v.isNil()) return out;
        if (!isList(v)) fail(name, "expected a list of bools, got " + v.typeName());
        ObjList* l = asList(v);
        for (int i = 0; i < l->length(); i++)
            out.push_back(l->getElement(i).isBool() && l->getElement(i).asBool());
        return out;
    }

    std::variant<int32_t, int64_t, double> getNumVariant(ObjectInstance* inst, const char* name)
    {
        Value v = prop(inst, name);
        if (v.isNil()) return int32_t(0);
        if (v.type() == ValueType::Int) {
            int64_t i = v.asInt();
            if (i >= INT32_MIN && i <= INT32_MAX)
                return int32_t(i);
            return i;
        }
        if (v.type() == ValueType::Real)
            return v.asReal();
        fail(name, "expected a number, got " + v.typeName());
    }

    // ---- nodes ------------------------------------------------------------

    template <class T>
    ptr<T> castAs(ptr<ast::AST> n, const char* field)
    {
        if (!n) return nullptr;
        auto typed = dynamic_ptr_cast<T>(n);
        if (!typed) fail(field, "node has the wrong grammatical class");
        return typed;
    }

    template <class T>
    ptr<T> getNodeAs(ObjectInstance* inst, const char* name)
    {
        Value v = prop(inst, name);
        if (v.isNil()) return nullptr;
        return castAs<T>(build(v), name);
    }

    template <class T>
    std::optional<ptr<T>> getOptNodeAs(ObjectInstance* inst, const char* name)
    {
        Value v = prop(inst, name);
        if (v.isNil()) return std::nullopt;
        return castAs<T>(build(v), name);
    }

    template <class T>
    std::vector<ptr<T>> getNodeListAs(ObjectInstance* inst, const char* name)
    {
        std::vector<ptr<T>> out;
        Value v = prop(inst, name);
        if (v.isNil()) return out;
        if (!isList(v)) fail(name, "expected a list of nodes, got " + v.typeName());
        ObjList* l = asList(v);
        for (int i = 0; i < l->length(); i++)
            out.push_back(castAs<T>(build(l->getElement(i)), name));
        return out;
    }

    std::vector<std::variant<ptr<ast::Declaration>, ptr<ast::Statement>>>
    getDeclStmtList(ObjectInstance* inst, const char* name)
    {
        std::vector<std::variant<ptr<ast::Declaration>, ptr<ast::Statement>>> out;
        Value v = prop(inst, name);
        if (v.isNil()) return out;
        if (!isList(v)) fail(name, "expected a list of nodes, got " + v.typeName());
        ObjList* l = asList(v);
        for (int i = 0; i < l->length(); i++) {
            auto n = build(l->getElement(i));
            if (auto d = roxal::dynamic_ptr_cast<ast::Declaration>(n))
                out.push_back(d);
            else if (auto s = roxal::dynamic_ptr_cast<ast::Statement>(n))
                out.push_back(s);
            else
                fail(name, "element is neither a declaration nor a statement");
        }
        return out;
    }

    std::variant<ptr<ast::Suite>, ptr<ast::Expression>, std::monostate>
    getBodyVariant(ObjectInstance* inst, const char* name)
    {
        Value v = prop(inst, name);
        if (v.isNil()) return std::monostate{};
        auto n = build(v);
        if (auto s = roxal::dynamic_ptr_cast<ast::Suite>(n))
            return s;
        if (auto e = roxal::dynamic_ptr_cast<ast::Expression>(n))
            return e;
        fail(name, "body must be a Suite or an Expression");
    }

    std::optional<std::variant<ptr<ast::Suite>, ptr<ast::Statement>, std::monostate>>
    getAccessorVariant(ObjectInstance* inst, const char* name, const char* abstractName)
    {
        if (getBool(inst, abstractName))
            return std::variant<ptr<ast::Suite>, ptr<ast::Statement>, std::monostate>(std::monostate{});
        Value v = prop(inst, name);
        if (v.isNil()) return std::nullopt;
        auto n = build(v);
        if (auto s = roxal::dynamic_ptr_cast<ast::Suite>(n))
            return std::variant<ptr<ast::Suite>, ptr<ast::Statement>, std::monostate>(s);
        if (auto st = roxal::dynamic_ptr_cast<ast::Statement>(n))
            return std::variant<ptr<ast::Suite>, ptr<ast::Statement>, std::monostate>(st);
        fail(name, "accessor must be a Suite or a Statement");
    }

    std::vector<ast::ArgNameExpr> getArgList(ObjectInstance* inst, const char* name)
    {
        std::vector<ast::ArgNameExpr> out;
        Value v = prop(inst, name);
        if (v.isNil()) return out;
        if (!isList(v)) fail(name, "expected a list of Arg nodes, got " + v.typeName());
        ObjList* l = asList(v);
        for (int i = 0; i < l->length(); i++) {
            Value av = l->getElement(i);
            if (!isObjectInstance(av)) fail(name, "expected Arg instances");
            ObjectInstance* arg = asObjectInstance(av);
            ustring argName = getUStr(arg, "name");
            Value ev = prop(arg, "value");
            ptr<ast::Expression> expr;
            if (!ev.isNil())
                expr = castAs<ast::Expression>(build(ev), name);
            out.emplace_back(argName, expr);
        }
        return out;
    }

    std::vector<std::pair<ptr<ast::Expression>, ptr<ast::Suite>>>
    getCondSuiteList(ObjectInstance* inst, const char* name)
    {
        std::vector<std::pair<ptr<ast::Expression>, ptr<ast::Suite>>> out;
        Value v = prop(inst, name);
        if (v.isNil()) return out;
        if (!isList(v)) fail(name, "expected a list of [condition, suite] pairs");
        ObjList* l = asList(v);
        for (int i = 0; i < l->length(); i++) {
            Value pv = l->getElement(i);
            if (!isList(pv) || asList(pv)->length() != 2)
                fail(name, "expected [condition, suite] pairs");
            out.emplace_back(castAs<ast::Expression>(build(asList(pv)->getElement(0)), name),
                             castAs<ast::Suite>(build(asList(pv)->getElement(1)), name));
        }
        return out;
    }

    std::vector<std::pair<std::vector<ptr<ast::Expression>>, ptr<ast::Suite>>>
    getCaseList(ObjectInstance* inst, const char* name)
    {
        std::vector<std::pair<std::vector<ptr<ast::Expression>>, ptr<ast::Suite>>> out;
        Value v = prop(inst, name);
        if (v.isNil()) return out;
        if (!isList(v)) fail(name, "expected a list of [[patterns], suite] pairs");
        ObjList* l = asList(v);
        for (int i = 0; i < l->length(); i++) {
            Value cv = l->getElement(i);
            if (!isList(cv) || asList(cv)->length() != 2)
                fail(name, "expected [[patterns], suite] pairs");
            Value patsV = asList(cv)->getElement(0);
            if (!isList(patsV)) fail(name, "patterns must be a list");
            std::vector<ptr<ast::Expression>> pats;
            ObjList* pl = asList(patsV);
            for (int j = 0; j < pl->length(); j++)
                pats.push_back(castAs<ast::Expression>(build(pl->getElement(j)), name));
            out.emplace_back(std::move(pats),
                             castAs<ast::Suite>(build(asList(cv)->getElement(1)), name));
        }
        return out;
    }

    std::vector<std::pair<ptr<ast::Expression>, ptr<ast::Expression>>>
    getPairExprList(ObjectInstance* inst, const char* name)
    {
        std::vector<std::pair<ptr<ast::Expression>, ptr<ast::Expression>>> out;
        Value v = prop(inst, name);
        if (v.isNil()) return out;
        if (!isList(v)) fail(name, "expected a list of [key, value] pairs");
        ObjList* l = asList(v);
        for (int i = 0; i < l->length(); i++) {
            Value pv = l->getElement(i);
            if (!isList(pv) || asList(pv)->length() != 2)
                fail(name, "expected [key, value] pairs");
            out.emplace_back(castAs<ast::Expression>(build(asList(pv)->getElement(0)), name),
                             castAs<ast::Expression>(build(asList(pv)->getElement(1)), name));
        }
        return out;
    }

    std::vector<ast::StrInterpPart> getInterpParts(ObjectInstance* inst, const char* name)
    {
        std::vector<ast::StrInterpPart> out;
        Value v = prop(inst, name);
        if (v.isNil()) return out;
        if (!isList(v)) fail(name, "expected a list of strings and expression nodes");
        ObjList* l = asList(v);
        for (int i = 0; i < l->length(); i++) {
            Value e = l->getElement(i);
            if (isString(e))
                out.emplace_back(asStringObj(e)->s);
            else
                out.emplace_back(castAs<ast::Expression>(build(e), name));
        }
        return out;
    }

    std::vector<ast::VarDecl::Target>
    getVarTargets(ObjectInstance* inst, const char* name)
    {
        std::vector<ast::VarDecl::Target> out;
        Value v = prop(inst, name);
        if (v.isNil()) return out;
        if (!isList(v)) fail(name, "expected a list of VarTarget nodes");
        ObjList* l = asList(v);
        for (int i = 0; i < l->length(); i++) {
            Value tv = l->getElement(i);
            if (!isObjectInstance(tv)) fail(name, "expected VarTarget instances");
            ObjectInstance* t = asObjectInstance(tv);
            ast::VarDecl::Target target;
            auto nameOpt = getOptUStr(t, "name");
            if (nameOpt.has_value())
                target.name = nameOpt.value();
            target.varType = getOptVarType(t, "var_type");
            target.isTypeConst = getBool(t, "is_type_const");
            target.isTypeMutable = getBool(t, "is_type_mutable");
            out.push_back(std::move(target));
        }
        return out;
    }

    std::vector<ast::TryStatement::ExceptClause>
    getExceptClauses(ObjectInstance* inst, const char* name)
    {
        std::vector<ast::TryStatement::ExceptClause> out;
        Value v = prop(inst, name);
        if (v.isNil()) return out;
        if (!isList(v)) fail(name, "expected a list of ExceptClause nodes");
        ObjList* l = asList(v);
        for (int i = 0; i < l->length(); i++) {
            Value cv = l->getElement(i);
            if (!isObjectInstance(cv)) fail(name, "expected ExceptClause instances");
            ObjectInstance* c = asObjectInstance(cv);
            ast::TryStatement::ExceptClause clause;
            Value tv = prop(c, "type_expr");
            if (!tv.isNil())
                clause.type = castAs<ast::Expression>(build(tv), name);
            clause.name = getOptUStr(c, "name");
            Value bv = prop(c, "body");
            if (!bv.isNil())
                clause.body = castAs<ast::Suite>(build(bv), name);
            out.push_back(std::move(clause));
        }
        return out;
    }

    // ---- type references --------------------------------------------------

    static ast::TypeName typeNameFromString(const std::string& s)
    {
        ast::TypeName out;
        size_t start = 0;
        for (size_t i = 0; i <= s.size(); i++) {
            if (i == s.size() || s[i] == '.') {
                out.push_back(toUnicodeString(s.substr(start, i - start)));
                start = i + 1;
            }
        }
        return out;
    }

    static ast::VarType varTypeFromString(const char* field, const std::string& s)
    {
        if (s.empty())
            fail(field, "empty type name");
        if (auto bt = type::builtinTypeFromName(s))
            return bt.value();
        return typeNameFromString(s);
    }

    ast::VarType getVarType(ObjectInstance* inst, const char* name)
    {
        return varTypeFromString(name, getStr(inst, name));
    }

    std::optional<ast::VarType> getOptVarType(ObjectInstance* inst, const char* name)
    {
        Value v = prop(inst, name);
        if (v.isNil()) return std::nullopt;
        if (!isString(v)) fail(name, "expected a type name string or nil");
        return varTypeFromString(name, toUTF8StdString(asStringObj(v)->s));
    }

    std::optional<std::vector<ast::VarType>> getOptVarTypeList(ObjectInstance* inst, const char* name)
    {
        Value v = prop(inst, name);
        if (v.isNil()) return std::nullopt;
        if (!isList(v)) fail(name, "expected a list of type name strings or nil");
        std::vector<ast::VarType> out;
        ObjList* l = asList(v);
        for (int i = 0; i < l->length(); i++) {
            Value e = l->getElement(i);
            if (!isString(e)) fail(name, "expected type name strings");
            out.push_back(varTypeFromString(name, toUTF8StdString(asStringObj(e)->s)));
        }
        return out;
    }

    std::optional<ast::TypeName> getOptTypeName(ObjectInstance* inst, const char* name)
    {
        Value v = prop(inst, name);
        if (v.isNil()) return std::nullopt;
        if (!isString(v)) fail(name, "expected a type name string or nil");
        return typeNameFromString(toUTF8StdString(asStringObj(v)->s));
    }

    std::vector<ast::TypeName> getTypeNameList(ObjectInstance* inst, const char* name)
    {
        std::vector<ast::TypeName> out;
        for (auto& s : getUStrList(inst, name))
            out.push_back(typeNameFromString(toUTF8StdString(s)));
        return out;
    }

    // ---- enums ------------------------------------------------------------

    ast::Access getAccess(ObjectInstance* inst, const char* name)
    {
        std::string s = getStr(inst, name);
        if (s.empty() || s == "public") return ast::Access::Public;
        if (s == "private") return ast::Access::Private;
        fail(name, "expected 'public' or 'private', got '" + s + "'");
    }

    ast::TypeDecl::Kind getTypeDeclKind(ObjectInstance* inst, const char* name)
    {
        std::string s = getStr(inst, name);
        if (s.empty() || s == "object") return ast::TypeDecl::Object;
        if (s == "actor") return ast::TypeDecl::Actor;
        if (s == "interface") return ast::TypeDecl::Interface;
        if (s == "enumeration" || s == "enum") return ast::TypeDecl::Enumeration;
        if (s == "event") return ast::TypeDecl::Event;
        fail(name, "unknown type kind '" + s + "'");
    }

    ast::MethodModifiers getModifiers(ObjectInstance* inst, const char* name)
    {
        ast::MethodModifiers m = 0;
        for (auto& u : getUStrList(inst, name)) {
            std::string s = toUTF8StdString(u);
            if (s == "implicit") ast::setModifier(m, ast::MethodModifier::Implicit);
            else if (s == "statement_action") ast::setModifier(m, ast::MethodModifier::StatementAction);
            else if (s == "abstract") ast::setModifier(m, ast::MethodModifier::Abstract);
            else fail(name, "unknown modifier '" + s + "'");
        }
        return m;
    }

    type::BuiltinType getBuiltinType(ObjectInstance* inst, const char* name)
    {
        std::string s = getStr(inst, name);
        if (auto bt = type::builtinTypeFromName(s))
            return bt.value();
        fail(name, "unknown builtin type '" + s + "'");
    }

    // ---- op mappings (accept both source tokens and opString glyphs) ------

    ast::BinaryOp::Op binOpFromStr(const std::string& s)
    {
        if (s == "+") return ast::BinaryOp::Add;
        if (s == "-") return ast::BinaryOp::Subtract;
        if (s == "*" || s == "×") return ast::BinaryOp::Multiply;
        if (s == "/") return ast::BinaryOp::Divide;
        if (s == "rem" || s == "%") return ast::BinaryOp::Modulo;
        if (s == "and") return ast::BinaryOp::And;
        if (s == "or") return ast::BinaryOp::Or;
        if (s == "&") return ast::BinaryOp::BitAnd;
        if (s == "|") return ast::BinaryOp::BitOr;
        if (s == "^") return ast::BinaryOp::BitXor;
        if (s == "==" || s == "≟") return ast::BinaryOp::Equal;
        if (s == "!=" || s == "<>" || s == "≠") return ast::BinaryOp::NotEqual;
        if (s == "is") return ast::BinaryOp::Is;
        if (s == "in") return ast::BinaryOp::In;
        if (s == "not in") return ast::BinaryOp::NotIn;
        if (s == "<") return ast::BinaryOp::LessThan;
        if (s == ">") return ast::BinaryOp::GreaterThan;
        if (s == "<=" || s == "≤" || s == "⩽") return ast::BinaryOp::LessOrEqual;
        if (s == ">=" || s == "≥" || s == "⩾") return ast::BinaryOp::GreaterOrEqual;
        fail("op", "unknown binary operator '" + s + "'");
    }

    ast::UnaryOp::Op unOpFromStr(const std::string& s)
    {
        if (s == "-") return ast::UnaryOp::Negate;
        if (s == "not") return ast::UnaryOp::Not;
        if (s == "~") return ast::UnaryOp::BitNot;
        if (s == ".") return ast::UnaryOp::Accessor;
        fail("op", "unknown unary operator '" + s + "'");
    }

    ast::Assignment::Op assignOpFromStr(const std::string& s)
    {
        if (s.empty() || s == "=") return ast::Assignment::Assign;
        if (s == "<-" || s == "←") return ast::Assignment::CopyInto;
        fail("op", "unknown assignment operator '" + s + "'");
    }

    // ---- base fields & trivia --------------------------------------------

    void putStrListAttr(ast::AST& n, ObjectInstance* inst, const char* name)
    {
        auto ss = getUStrList(inst, name);
        if (ss.empty()) return;
        std::vector<std::string> out;
        for (auto& s : ss)
            out.push_back(toUTF8StdString(s));
        n.attrs[name] = std::move(out);
    }

    ptr<ast::AST> finish(ptr<ast::AST> n, ObjectInstance* inst)
    {
        if (!n) return n;
        n->annotations = getNodeListAs<ast::Annotation>(inst, "annotations");
        putStrListAttr(*n, inst, "leading_comments");
        Value tc = prop(inst, "trailing_comment");
        if (isString(tc))
            n->attrs["trailing_comment"] = toUTF8StdString(asStringObj(tc)->s);
        Value bl = prop(inst, "blank_lines_before");
        if (bl.type() == ValueType::Int && bl.asInt() > 0)
            n->attrs["blank_lines_before"] = int64_t(bl.asInt());
        return n;
    }
};

// generic child enumeration over the C++ tree (also generated)
void forEachChildOf(const ast::AST& node, const std::function<void(const ast::AST*)>& f);

#include "InspectAstConv.inc"

// ---------------------------------------------------------------------------
// Trivia attachment: assign harvested comments and blank-line counts to
// statement/declaration-granularity mirror nodes.
// ---------------------------------------------------------------------------

// Nodes that own comment decorations: direct children of File and Suite
// (statements and declarations), and TypeDecl members.
void collectGranularity(const ast::AST& n, std::vector<const ast::AST*>& out)
{
    const std::type_info& t = typeid(n);
    auto addDeclsOrStmts =
        [&out](const std::vector<std::variant<ptr<ast::Declaration>, ptr<ast::Statement>>>& ds) {
            for (auto& d : ds) {
                if (std::holds_alternative<ptr<ast::Declaration>>(d)) {
                    if (auto& p = std::get<ptr<ast::Declaration>>(d)) out.push_back(p.get());
                } else {
                    if (auto& p = std::get<ptr<ast::Statement>>(d)) out.push_back(p.get());
                }
            }
        };
    if (t == typeid(ast::File)) {
        auto& f = static_cast<const ast::File&>(n);
        for (auto& i : f.imports) if (i) out.push_back(i.get());
        addDeclsOrStmts(f.declsOrStmts);
    } else if (t == typeid(ast::Suite)) {
        addDeclsOrStmts(static_cast<const ast::Suite&>(n).declsOrStmts);
    } else if (t == typeid(ast::TypeDecl)) {
        auto& td = static_cast<const ast::TypeDecl&>(n);
        for (auto& m : td.methods) if (m) out.push_back(m.get());
        for (auto& p : td.properties) if (p) out.push_back(p.get());
        for (auto& a : td.propertyAccessors) if (a) out.push_back(a.get());
        for (auto& nt : td.nestedTypes) if (nt) out.push_back(nt.get());
    }
    forEachChildOf(n, [&out](const ast::AST* c) {
        if (c) collectGranularity(*c, out);
    });
}

struct SourceLines {
    // byte offset of the start of each (1-based) line
    std::vector<size_t> starts;
    const std::string* src;

    explicit SourceLines(const std::string& s) : src(&s)
    {
        starts.push_back(0);   // dummy for line 0
        starts.push_back(0);   // line 1
        for (size_t i = 0; i < s.size(); i++)
            if (s[i] == '\n')
                starts.push_back(i + 1);
    }

    bool valid(size_t line) const { return line >= 1 && line < starts.size(); }

    // text of the line up to (not including) byte column `col` is all whitespace?
    bool blankBefore(size_t line, size_t col) const
    {
        if (!valid(line))
            return true;
        size_t s = starts[line];
        for (size_t i = s; i < src->size() && (*src)[i] != '\n' && i - s < col; i++)
            if (!std::isspace(static_cast<unsigned char>((*src)[i])))
                return false;
        return true;
    }

    bool lineBlank(size_t line) const
    {
        if (!valid(line))
            return false;
        for (size_t i = starts[line]; i < src->size() && (*src)[i] != '\n'; i++)
            if (!std::isspace(static_cast<unsigned char>((*src)[i])))
                return false;
        return true;
    }
};

void attachTrivia(InspectConv& cx, const ast::AST& root, const Value& rootV,
                  const std::vector<ASTGenerator::CommentTok>& comments)
{
    if (!cx.src)
        return;
    SourceLines lines(*cx.src);

    std::vector<const ast::AST*> gran;
    // fragment roots are themselves statement/declaration granularity
    if (typeid(root) != typeid(ast::File))
        gran.push_back(&root);
    collectGranularity(root, gran);
    std::sort(gran.begin(), gran.end(), [](const ast::AST* a, const ast::AST* b) {
        if (a->interval.first.line != b->interval.first.line)
            return a->interval.first.line < b->interval.first.line;
        return a->interval.first.pos < b->interval.first.pos;
    });

    std::unordered_map<const ast::AST*, std::vector<std::string>> leading;
    std::unordered_map<const ast::AST*, std::string> trailing;
    std::unordered_map<const ast::AST*, size_t> firstOwnedLine;
    std::vector<std::string> endComments;

    for (auto* g : gran)
        firstOwnedLine[g] = g->interval.first.line;

    for (auto& c : comments) {
        if (!lines.blankBefore(c.line, c.pos)) {
            // trailing: deepest granularity node whose line range contains it
            const ast::AST* best = nullptr;
            for (auto* g : gran) {
                if (g->interval.first.line <= c.line && c.line <= g->interval.second.line)
                    best = g;   // sorted by start: later match == deeper/closer
                if (g->interval.first.line > c.line)
                    break;
            }
            if (best)
                trailing[best] = c.text;
        } else {
            // leading: next granularity node below the comment
            const ast::AST* next = nullptr;
            for (auto* g : gran) {
                if (g->interval.first.line > c.line) {
                    next = g;
                    break;
                }
            }
            if (next) {
                leading[next].push_back(c.text);
                auto it = firstOwnedLine.find(next);
                if (it != firstOwnedLine.end() && c.line < it->second)
                    it->second = c.line;
            } else {
                endComments.push_back(c.text);
            }
        }
    }

    for (auto* g : gran) {
        auto mit = cx.mirrorOf.find(g);
        if (mit == cx.mirrorOf.end())
            continue;
        const Value& mv = mit->second;

        auto lit = leading.find(g);
        if (lit != leading.end()) {
            Value lst = Value::listVal();
            for (auto& s : lit->second)
                asList(lst)->append(Value::stringVal(toUnicodeString(s)));
            cx.set(mv, "leading_comments", lst);
        }
        auto tit = trailing.find(g);
        if (tit != trailing.end())
            cx.setStr(mv, "trailing_comment", tit->second);

        size_t first = firstOwnedLine[g];
        size_t blanks = 0;
        while (first > 1 && lines.lineBlank(first - 1)) {
            blanks++;
            first--;
        }
        if (blanks > 0)
            cx.set(mv, "blank_lines_before", Value::intVal(int64_t(blanks)));
    }

    if (!endComments.empty() && typeid(root) == typeid(ast::File)) {
        Value lst = Value::listVal();
        for (auto& s : endComments)
            asList(lst)->append(Value::stringVal(toUnicodeString(s)));
        cx.set(rootV, "end_comments", lst);
    }
}

} // anonymous namespace
} // namespace roxal

// ---------------------------------------------------------------------------
// ModuleInspect
// ---------------------------------------------------------------------------

ModuleInspect::ModuleInspect()
{
    moduleTypeValue = Value::objVal(newModuleTypeObj(toUnicodeString("inspect")));
    ObjModuleType::allModules.push_back(moduleTypeValue);
}

ModuleInspect::~ModuleInspect()
{
    destroyModuleType(moduleTypeValue);
}

void ModuleInspect::registerBuiltins(VM& vm)
{
    setVM(vm);

    link("parse", [this](VM&, ArgsView a) { return inspect_parse_builtin(a); });
    link("parse_file", [this](VM&, ArgsView a) { return inspect_parse_file_builtin(a); });

    link("networks", [this](VM&, ArgsView a) { return inspect_networks_builtin(a); });
    link("signals", [this](VM&, ArgsView a) { return inspect_signals_builtin(a); });

    link("unparse", [this](VM&, ArgsView a) { return inspect_unparse_builtin(a); });
    link("compile", [this](VM&, ArgsView a) { return inspect_compile_builtin(a); });
    link("parse_expression", [this](VM&, ArgsView a) { return inspect_parse_expression_builtin(a); });
    link("parse_statement", [this](VM&, ArgsView a) { return inspect_parse_statement_builtin(a); });
    link("parse_declaration", [this](VM&, ArgsView a) { return inspect_parse_declaration_builtin(a); });

    link("members", [this](VM&, ArgsView a) { return inspect_members_builtin(a); });
    link("signatures", [this](VM&, ArgsView a) { return inspect_signatures_builtin(a); });
    link("call", [this](VM&, ArgsView a) { return inspect_call_builtin(a); });
    link("calling_module", [this](VM&, ArgsView a) { return inspect_calling_module_builtin(a); });
    link("main_module", [this](VM&, ArgsView a) { return inspect_main_module_builtin(a); });
}

void ModuleInspect::onModuleLoaded(VM& vm)
{
    // Per-signal queries must be SIGNAL METHODS: a plain function call with a
    // signal argument is lifted into a dataflow node by VM::callValue, so a
    // module-level network(sig) would never execute as a call.  Method
    // dispatch (bindMethod) does not lift.  args[0] is the receiver.
    vm.defineBuiltinMethod(ValueType::Signal, "network",
        [this](VM&, ArgsView a) { return inspect_network_builtin(a); });
    vm.defineBuiltinMethod(ValueType::Signal, "info",
        [this](VM&, ArgsView a) { return inspect_signal_info_builtin(a); });
}

// ---------------------------------------------------------------------------
// Dataflow-network introspection
// ---------------------------------------------------------------------------

namespace {

Value moduleClassInstance(const Value& moduleTypeValue, const char* typeName)
{
    auto tv = asModuleType(moduleTypeValue)->vars.load(toUnicodeString(typeName));
    if (!tv.has_value() || !isObjectType(tv.value()))
        throw std::runtime_error(std::string("inspect: type '") + typeName + "' not found in module");
    return Value::objVal(newObjectInstance(tv.value()));
}

void dictPut(const Value& d, const char* key, const Value& v)
{
    asDict(d)->store(Value::stringVal(toUnicodeString(key)), v);
}

Value networkValue(const Value& moduleTypeValue,
                   const df::DataflowEngine::NetworkSnapshot& ns)
{
    Value netV = moduleClassInstance(moduleTypeValue, "Network");
    ObjectInstance* net = asObjectInstance(netV);

    Value nodesV = Value::listVal();
    for (const auto& f : ns.funcs) {
        Value nodeV = moduleClassInstance(moduleTypeValue, "DataflowNode");
        ObjectInstance* node = asObjectInstance(nodeV);
        node->setProperty("id", Value::intVal(int64_t(f.id)));
        node->setProperty("name", Value::stringVal(toUnicodeString(f.name)));
        node->setProperty("period_us", Value::intVal(f.period.microSecs()));
        node->setProperty("src_name", Value::stringVal(toUnicodeString(f.srcName)));
        node->setProperty("src_line", Value::intVal(int64_t(f.srcLine)));
        node->setProperty("src_col", Value::intVal(int64_t(f.srcCol)));

        auto portList = [&moduleTypeValue](const std::vector<df::DataflowEngine::PortSnapshot>& ports) {
            Value lst = Value::listVal();
            for (const auto& p : ports) {
                Value portV = moduleClassInstance(moduleTypeValue, "Port");
                ObjectInstance* port = asObjectInstance(portV);
                port->setProperty("name", Value::stringVal(toUnicodeString(p.name)));
                port->setProperty("index", Value::intVal(int64_t(p.index)));
                port->setProperty("has_default", p.hasDefault ? Value::trueVal() : Value::falseVal());
                if (p.signal)
                    port->setProperty("sig", Value::signalRefVal(p.signal));
                asList(lst)->append(portV);
            }
            return lst;
        };
        node->setProperty("inputs", portList(f.inputs));
        node->setProperty("outputs", portList(f.outputs));
        asList(nodesV)->append(nodeV);
    }
    net->setProperty("nodes", nodesV);

    Value sigsV = Value::listVal();
    for (const auto& s : ns.signals)
        if (s)
            asList(sigsV)->append(Value::signalRefVal(s));
    net->setProperty("signals", sigsV);

    net->setProperty("tick_period_us", Value::intVal(ns.tickPeriod.microSecs()));
    net->setProperty("background", ns.background ? Value::trueVal() : Value::falseVal());
    return netV;
}

} // anonymous namespace

Value ModuleInspect::inspect_network_builtin(ArgsView args)
{
    if (args.size() < 1 || !isSignal(args[0]))
        throw std::invalid_argument("signal.network() expects a signal receiver");
    auto sig = asSignal(args[0])->signal;
    if (!sig)
        return Value::nilVal();

    auto engine = df::DataflowEngine::instance(false);
    if (!engine)
        return Value::nilVal();

    auto snapshot = engine->subnetworkContaining(sig);
    if (!snapshot.has_value())
        return Value::nilVal();
    return networkValue(moduleTypeValue, snapshot.value());
}

Value ModuleInspect::inspect_networks_builtin(ArgsView args)
{
    (void)args;
    Value lst = Value::listVal();
    auto engine = df::DataflowEngine::instance(false);
    if (!engine)
        return lst;
    for (const auto& ns : engine->allSubnetworks())
        asList(lst)->append(networkValue(moduleTypeValue, ns));
    return lst;
}

Value ModuleInspect::inspect_signals_builtin(ArgsView args)
{
    bool includeInternal = args.size() >= 1 && args[0].isBool() && args[0].asBool();
    Value lst = Value::listVal();
    auto engine = df::DataflowEngine::instance(false);
    if (!engine)
        return lst;
    for (const auto& s : engine->allSignals(includeInternal))
        asList(lst)->append(Value::signalRefVal(s));
    return lst;
}

Value ModuleInspect::inspect_signal_info_builtin(ArgsView args)
{
    if (args.size() < 1 || !isSignal(args[0]))
        throw std::invalid_argument("signal.info() expects a signal receiver");
    auto sig = asSignal(args[0])->signal;
    if (!sig)
        return Value::nilVal();

    Value d = Value::dictVal();
    dictPut(d, "id", Value::intVal(int64_t(sig->id())));
    dictPut(d, "name", Value::stringVal(toUnicodeString(sig->name())));
    dictPut(d, "freq", Value::realVal(sig->frequency()));
    dictPut(d, "domain", Value::stringVal(toUnicodeString(
        sig->domain() == df::Signal::Domain::Background ? "background" : "rt")));
    dictPut(d, "internal", sig->isInternal() ? Value::trueVal() : Value::falseVal());
    dictPut(d, "source", sig->isSourceSignal() ? Value::trueVal() : Value::falseVal());
    dictPut(d, "clock", sig->isClockSignal() ? Value::trueVal() : Value::falseVal());
    dictPut(d, "running", sig->isRunning() ? Value::trueVal() : Value::falseVal());
    dictPut(d, "event_driven", sig->isEventDriven() ? Value::trueVal() : Value::falseVal());
    dictPut(d, "derived", sig->derived() ? Value::trueVal() : Value::falseVal());
    auto base = sig->derivedBase();
    dictPut(d, "base", base ? Value::signalRefVal(base) : Value::nilVal());
    dictPut(d, "base_index", Value::intVal(int64_t(sig->derivedIndex())));
    dictPut(d, "src_name", Value::stringVal(toUnicodeString(sig->srcName())));
    dictPut(d, "src_line", Value::intVal(int64_t(sig->srcLine())));
    dictPut(d, "src_col", Value::intVal(int64_t(sig->srcCol())));
    return d;
}

Value ModuleInspect::parseToMirror(const std::string& source, const std::string& name,
                                   bool tolerant, bool deduceTypes)
{
    std::istringstream stream(source);
    ASTGenerator gen;
    std::vector<ASTGenerator::CommentTok> comments;
    std::vector<ASTGenerator::ParseErr> errors;

    ptr<ast::AST> tree;
    try {
        // partial trees only in tolerant mode: the non-tolerant path throws
        // on any error, so building a partial result would be wasted work
        // (and a recovered-tree visit followed by a throw in the same native
        // frame trips a wasm-exceptions unwinding quirk — see wasm/README.md)
        tree = gen.ast(stream, name, &comments, &errors, /*partialOnErrors=*/tolerant);
    } catch (std::exception& e) {
        clearCompileContext();
        throw std::runtime_error(std::string("inspect.parse: ") + e.what());
    }
    clearCompileContext();

    auto makeErrList = [&errors]() {
        Value errList = Value::listVal();
        for (auto& e : errors) {
            Value d = Value::dictVal();
            asDict(d)->store(Value::stringVal(toUnicodeString("line")),
                             Value::intVal(int64_t(e.line)));
            asDict(d)->store(Value::stringVal(toUnicodeString("col")),
                             Value::intVal(int64_t(e.pos)));
            asDict(d)->store(Value::stringVal(toUnicodeString("message")),
                             Value::stringVal(toUnicodeString(e.message)));
            asList(errList)->append(d);
        }
        return errList;
    };

    if (!tree) {
        if (errors.empty())
            errors.push_back({ 0, 0, "parse failed" });
        if (!tolerant) {
            auto& e = errors.front();
            throw std::runtime_error("inspect.parse: syntax error at " + std::to_string(e.line) +
                                     ":" + std::to_string(e.pos) + " - " + e.message);
        }
        Value result = Value::dictVal();
        asDict(result)->store(Value::stringVal(toUnicodeString("tree")), Value::nilVal());
        asDict(result)->store(Value::stringVal(toUnicodeString("errors")), makeErrList());
        return result;
    }
    if (!tolerant && !errors.empty()) {
        auto& e = errors.front();
        throw std::runtime_error("inspect.parse: syntax error at " + std::to_string(e.line) +
                                 ":" + std::to_string(e.pos) + " - " + e.message);
    }

    auto file = dynamic_ptr_cast<ast::File>(tree);
    if (!file)
        throw std::runtime_error("inspect.parse: internal error: root is not a File");

    if (deduceTypes) {
        // best-effort: fragments legitimately reference unknown symbols, so
        // deduction failures only mean some nodes stay untyped
        try {
            TypeDeducer typeDeducer {};
            typeDeducer.visit(file);
        } catch (std::exception&) {
        }
        clearCompileContext();
    }

    InspectConv cx;
    cx.moduleTypeVal = moduleTypeValue;
    cx.src = file->source.get();

    Value fileV = cx.node(file.get());
    attachTrivia(cx, *file, fileV, comments);

    if (tolerant) {
        Value result = Value::dictVal();
        asDict(result)->store(Value::stringVal(toUnicodeString("tree")), fileV);
        asDict(result)->store(Value::stringVal(toUnicodeString("errors")), makeErrList());
        return result;
    }
    return fileV;
}

Value ModuleInspect::inspect_parse_builtin(ArgsView args)
{
    if (args.size() < 1 || !isString(args[0]))
        throw std::invalid_argument("inspect.parse expects a source string");
    std::string source = toUTF8StdString(asStringObj(args[0])->s);

    std::string name = "fragment";
    if (args.size() >= 2 && isString(args[1]))
        name = toUTF8StdString(asStringObj(args[1])->s);

    bool tolerant = args.size() >= 3 && args[2].isBool() && args[2].asBool();

    return parseToMirror(source, name, tolerant, /*deduceTypes=*/true);
}

Value ModuleInspect::inspect_unparse_builtin(ArgsView args)
{
    if (args.size() < 1 || args[0].isNil())
        throw std::invalid_argument("inspect.unparse expects an AST mirror node");

    InspectBuild bx;
    ptr<ast::AST> tree = bx.build(args[0]);
    if (!tree)
        throw std::invalid_argument("inspect.unparse expects an AST mirror node");

    AstPrinter printer;
    return Value::stringVal(toUnicodeString(printer.print(*tree)));
}

Value ModuleInspect::inspect_compile_builtin(ArgsView args)
{
    if (args.size() < 1 || args[0].isNil())
        throw std::invalid_argument("inspect.compile expects a File mirror node");

    // Mirror trees carry no source text, so the unparsed render IS the
    // tree's canonical source: compile that.  Compile errors and runtime
    // positions then reference text the caller can reproduce exactly with
    // inspect.unparse.
    InspectBuild bx;
    ptr<ast::AST> tree = bx.build(args[0]);
    auto file = roxal::dynamic_ptr_cast<ast::File>(tree);
    if (!file)
        throw std::invalid_argument("inspect.compile expects a File mirror node");

    AstPrinter printer;
    std::string source = printer.print(*file);

    std::string name = "anonymous";
    if (args.size() >= 2 && isString(args[1]))
        name = toUTF8StdString(asStringObj(args[1])->s);

    std::istringstream stream(source);
    RoxalCompiler compiler {};
    Value function;
    try {
        function = compiler.compile(stream, name);
    } catch (std::exception& e) {
        throw std::runtime_error(std::string("inspect.compile: ") + e.what());
    }
    if (function.isNil())
        throw std::runtime_error("inspect.compile: compile failed (see errors above)");
    return Value::closureVal(function);
}

// Convert a fragment-parse result to a mirror node.
Value ModuleInspect::fragmentToMirror(const ptr<ast::AST>& node,
                                      const std::vector<ASTGenerator::ParseErr>& errors,
                                      const char* what,
                                      const std::vector<ASTGenerator::CommentTok>* comments)
{
    clearCompileContext();
    if (!node) {
        if (!errors.empty()) {
            auto& e = errors.front();
            throw std::runtime_error(std::string(what) + ": syntax error at " +
                                     std::to_string(e.line) + ":" + std::to_string(e.pos) +
                                     " - " + e.message);
        }
        throw std::runtime_error(std::string(what) + ": parse failed");
    }
    InspectConv cx;
    cx.moduleTypeVal = moduleTypeValue;
    cx.src = node->source.get();
    Value v = cx.node(node.get());
    if (comments && !comments->empty())
        attachTrivia(cx, *node, v, *comments);
    return v;
}

Value ModuleInspect::inspect_parse_expression_builtin(ArgsView args)
{
    if (args.size() < 1 || !isString(args[0]))
        throw std::invalid_argument("inspect.parse_expression expects a source string");
    ASTGenerator gen;
    std::vector<ASTGenerator::ParseErr> errors;
    auto node = gen.parseExpressionFragment(toUTF8StdString(asStringObj(args[0])->s), &errors);
    return fragmentToMirror(node, errors, "inspect.parse_expression");
}

Value ModuleInspect::inspect_parse_statement_builtin(ArgsView args)
{
    if (args.size() < 1 || !isString(args[0]))
        throw std::invalid_argument("inspect.parse_statement expects a source string");
    ASTGenerator gen;
    std::vector<ASTGenerator::ParseErr> errors;
    std::vector<ASTGenerator::CommentTok> comments;
    auto node = gen.parseStatementFragment(toUTF8StdString(asStringObj(args[0])->s),
                                           &errors, &comments);
    return fragmentToMirror(node, errors, "inspect.parse_statement", &comments);
}

Value ModuleInspect::inspect_parse_declaration_builtin(ArgsView args)
{
    if (args.size() < 1 || !isString(args[0]))
        throw std::invalid_argument("inspect.parse_declaration expects a source string");
    ASTGenerator gen;
    std::vector<ASTGenerator::ParseErr> errors;
    std::vector<ASTGenerator::CommentTok> comments;
    auto node = gen.parseDeclarationFragment(toUTF8StdString(asStringObj(args[0])->s),
                                             &errors, &comments);
    return fragmentToMirror(node, errors, "inspect.parse_declaration", &comments);
}

Value ModuleInspect::inspect_parse_file_builtin(ArgsView args)
{
    if (args.size() < 1 || !isString(args[0]))
        throw std::invalid_argument("inspect.parse_file expects a path string");
    std::string path = toUTF8StdString(asStringObj(args[0])->s);

    std::ifstream in(path);
    if (!in)
        throw std::runtime_error("inspect.parse_file: cannot read '" + path + "'");
    std::string source { std::istreambuf_iterator<char>(in), {} };

    std::filesystem::path p(path);
    return parseToMirror(source, p.stem().string(), /*tolerant=*/false, /*deduceTypes=*/true);
}

// ---------------------------------------------------------------------------
// Runtime reflection
//
// Introspection of the live objects rather than of source text: what a module
// contains, what a callable's signature and annotations are, and a call whose
// argument list is only known at runtime.  Annotations are retained on
// ObjFunction (and survive .roc caching), but their arguments are stored as
// unevaluated AST expressions -- evalAnnotArg() below is the evaluator, and it
// accepts exactly the literal family that the .roc serializer can round-trip.
// ---------------------------------------------------------------------------

namespace {

Value numValue(const std::variant<int32_t,int64_t,double>& n)
{
    if (std::holds_alternative<int32_t>(n))
        return Value::intVal(int64_t(std::get<int32_t>(n)));
    if (std::holds_alternative<int64_t>(n))
        return Value::intVal(std::get<int64_t>(n));
    return Value::realVal(std::get<double>(n));
}

ObjModuleType* ownerModule(ObjFunction* fn)
{
    if (!fn)
        return nullptr;
    Value mv = fn->moduleType.strongRef();
    return (mv.isObj() && isModuleType(mv)) ? asModuleType(mv) : nullptr;
}

// Resolve a bare name in an annotation argument: the declaring module's
// variables first, then the globals (which is where the sys surface lives).
Value resolveName(const ustring& name, ObjFunction* fn, VM& vm)
{
    if (ObjModuleType* mod = ownerModule(fn)) {
        auto v = mod->vars.load(name.hashCode());
        if (v.has_value())
            return v.value();
    }
    auto g = vm.loadGlobal(name);
    if (g.has_value())
        return g.value();
    return Value::nilVal();
}

// Apply a literal suffix (2s, 100ms, 5kg) by calling the @suffix-registered
// function, exactly as a compiled suffixed literal would.  Registrations live
// on the module that declared them; sys's are visible everywhere.
Value applySuffix(const ustring& suffix, const Value& literal, ObjFunction* fn, VM& vm)
{
    ustring funcName;
    ObjModuleType* declaring = nullptr;
    if (ObjModuleType* mod = ownerModule(fn)) {
        auto it = mod->registeredSuffixes.find(suffix);
        if (it != mod->registeredSuffixes.end()) {
            funcName = it->second;
            declaring = mod;
        }
    }
    if (funcName.isEmpty()) {
        Value sysMod = vm.getBuiltinModuleType(toUnicodeString("sys"));
        if (sysMod.isObj() && isModuleType(sysMod)) {
            ObjModuleType* sys = asModuleType(sysMod);
            auto it = sys->registeredSuffixes.find(suffix);
            if (it != sys->registeredSuffixes.end()) {
                funcName = it->second;
                declaring = sys;
            }
        }
    }
    if (funcName.isEmpty() || !declaring)
        throw std::runtime_error("inspect: annotation uses unknown literal suffix '"
                                 + toUTF8StdString(suffix) + "'");

    auto fv = declaring->vars.load(funcName.hashCode());
    if (!fv.has_value() || !isClosure(fv.value()))
        throw std::runtime_error("inspect: suffix function '" + toUTF8StdString(funcName)
                                 + "' is not callable");
    auto result = vm.invokeClosure(asClosure(fv.value()), { literal });
    if (result.first != ExecutionStatus::OK)
        throw std::runtime_error("inspect: evaluating literal suffix '"
                                 + toUTF8StdString(suffix) + "' failed");
    return result.second;
}

Value evalAnnotArg(const ptr<ast::Expression>& e, ObjFunction* fn, VM& vm)
{
    using namespace ast;
    if (!e)
        return Value::nilVal();
    if (auto s = dynamic_ptr_cast<Str>(e))
        return Value::stringVal(s->str);
    if (auto n = dynamic_ptr_cast<Num>(e))
        return numValue(n->num);
    if (auto b = dynamic_ptr_cast<Bool>(e))
        return b->value ? Value::trueVal() : Value::falseVal();
    if (auto sn = dynamic_ptr_cast<SuffixedNum>(e))
        return applySuffix(sn->suffix, numValue(sn->num), fn, vm);
    if (auto ss = dynamic_ptr_cast<SuffixedStr>(e))
        return applySuffix(ss->suffix, Value::stringVal(ss->str), fn, vm);
    if (auto l = dynamic_ptr_cast<List>(e)) {
        Value lst = Value::listVal();
        for (const auto& el : l->elements)
            asList(lst)->append(evalAnnotArg(el, fn, vm));
        return lst;
    }
    if (auto d = dynamic_ptr_cast<Dict>(e)) {
        Value dict = Value::dictVal();
        for (const auto& entry : d->entries)
            asDict(dict)->store(evalAnnotArg(entry.first, fn, vm),
                                evalAnnotArg(entry.second, fn, vm));
        return dict;
    }
    if (auto u = dynamic_ptr_cast<UnaryOp>(e)) {
        if (u->op != UnaryOp::Negate)
            throw std::runtime_error("inspect: annotation argument is not a literal");
        return roxal::negate(evalAnnotArg(u->arg, fn, vm));
    }
    if (auto v = dynamic_ptr_cast<Variable>(e))
        return resolveName(v->name, fn, vm);
    // `nil` is a bare Literal rather than a node type of its own, so it has to
    // be recognised after the specific literal kinds above (which derive from it)
    if (auto lit = dynamic_ptr_cast<Literal>(e))
        if (lit->literalType == Literal::LiteralType::Nil)
            return Value::nilVal();
    // Anything else is rejected at compile time (see RoxalCompiler's annotation
    // validation), so reaching here means a node kind was added without
    // teaching this evaluator about it.
    throw std::runtime_error("inspect: annotation argument is not a literal");
}

// The declaration line of a callable: an annotation's line when there is one
// (it sits directly above the declaration), else the first line of its body.
int declLine(ObjFunction* fn)
{
    if (!fn)
        return 0;
    int best = 0;
    for (const auto& a : fn->annotations)
        if (a && (best == 0 || a->interval.first.line < best))
            best = a->interval.first.line;
    if (best > 0)
        return best;
    if (fn->chunk && !fn->chunk->code.empty())
        return fn->chunk->getLine(0);
    return 0;
}

// Every ObjFunction reachable from a callable value: one per overload.
std::vector<ObjFunction*> functionsOf(const Value& v)
{
    std::vector<ObjFunction*> out;
    if (isClosure(v))
        out.push_back(asFunction(asClosure(v)->function));
    else if (isFunction(v))
        out.push_back(asFunction(v));
    else if (isBoundMethod(v)) {
        Value m = asBoundMethod(v)->method;
        if (isClosure(m))
            out.push_back(asFunction(asClosure(m)->function));
        else if (isFunction(m))
            out.push_back(asFunction(m));
    }
    else if (isOverloadSet(v)) {
        for (const auto& c : asOverloadSet(v)->closures)
            if (isClosure(c))
                out.push_back(asFunction(asClosure(c)->function));
    }
    return out;
}

bool isCallableValue(const Value& v)
{
    return isClosure(v) || isFunction(v) || isBoundMethod(v)
        || isOverloadSet(v) || isNative(v) || isBoundNative(v);
}

} // anonymous namespace

Value ModuleInspect::signatureValue(ObjFunction* fn)
{
    // Evaluating a suffixed annotation argument (2s) re-enters the interpreter,
    // so a collection can be requested while the half-built mirror objects below
    // are reachable only from these C++ locals -- invisible to the mark phase.
    // Staying unparked makes the collection barrier wait us out instead.
    SimpleMarkSweepGC::GCNoParkScope nativeCover;

    Value sigV = moduleClassInstance(moduleTypeValue, "Signature");
    ObjectInstance* sig = asObjectInstance(sigV);
    sig->setProperty("name", Value::stringVal(fn->name));

    Value paramsV = Value::listVal();
    Value returnsV = Value::listVal();
    bool isProc = false;   // proc-ness lives only in the function type
    if (fn->funcType.has_value() && fn->funcType.value()
        && fn->funcType.value()->func.has_value()) {
        const auto& ft = fn->funcType.value()->func.value();
        isProc = ft.isProc;
        for (const auto& p : ft.params) {
            Value pV = moduleClassInstance(moduleTypeValue, "Param");
            ObjectInstance* param = asObjectInstance(pV);
            if (p.has_value()) {
                param->setProperty("name", Value::stringVal(p.value().name));
                param->setProperty("has_default",
                    p.value().hasDefault ? Value::trueVal() : Value::falseVal());
                param->setProperty("variadic",
                    p.value().variadic ? Value::trueVal() : Value::falseVal());
                if (p.value().type.has_value() && p.value().type.value())
                    param->setProperty("param_type", Value::stringVal(
                        toUnicodeString(p.value().type.value()->toString())));
            }
            asList(paramsV)->append(pV);
        }
        for (const auto& r : ft.returnTypes)
            if (r)
                asList(returnsV)->append(Value::stringVal(toUnicodeString(r->toString())));
    }
    sig->setProperty("params", paramsV);
    sig->setProperty("return_types", returnsV);
    sig->setProperty("is_proc", isProc ? Value::trueVal() : Value::falseVal());

    Value annotsV = Value::listVal();
    for (const auto& a : fn->annotations) {
        if (!a)
            continue;
        Value aV = moduleClassInstance(moduleTypeValue, "AnnotationInfo");
        ObjectInstance* ann = asObjectInstance(aV);
        ann->setProperty("name", Value::stringVal(a->name));
        Value posV = Value::listVal();
        Value namedV = Value::dictVal();
        for (const auto& arg : a->args) {
            Value av = evalAnnotArg(arg.second, fn, vm());
            if (arg.first.isEmpty())
                asList(posV)->append(av);
            else
                asDict(namedV)->store(Value::stringVal(arg.first), av);
        }
        ann->setProperty("args", posV);
        ann->setProperty("named", namedV);
        asList(annotsV)->append(aV);
    }
    sig->setProperty("annotations", annotsV);

    if (fn->chunk && !fn->chunk->sourceName.isEmpty())
        sig->setProperty("source_file", Value::stringVal(fn->chunk->sourceName));
    sig->setProperty("line", Value::intVal(int64_t(declLine(fn))));
    return sigV;
}

Value ModuleInspect::inspect_members_builtin(ArgsView args)
{
    if (args.size() < 1 || !isModuleType(args[0]))
        throw std::invalid_argument("inspect.members expects a module");
    ObjModuleType* mod = asModuleType(args[0]);

    struct Entry {
        ustring name;
        Value value;
        std::string kind;
        int line;
    };
    std::vector<Entry> entries;
    for (const auto& nv : mod->vars.snapshot()) {
        const ustring& name = nv.first;
        const Value& v = nv.second;
        std::string kind;
        int line = 0;
        if (isModuleType(v))
            kind = "module";
        else if (isObjectType(v) || isTypeSpec(v))
            kind = "type";
        else if (isCallableValue(v)) {
            auto fns = functionsOf(v);
            bool isProc = false;
            if (!fns.empty()) {
                line = declLine(fns.front());
                if (fns.front()->funcType.has_value() && fns.front()->funcType.value()
                    && fns.front()->funcType.value()->func.has_value())
                    isProc = fns.front()->funcType.value()->func.value().isProc;
            }
            kind = isProc ? "proc" : "func";
        }
        else
            kind = mod->constVars.count(name.hashCode()) ? "const" : "var";
        entries.push_back(Entry{ name, v, kind, line });
    }

    // Callables in declaration order (the order tests are written in matters to
    // a test runner); everything else after, by name, for a stable listing.
    std::stable_sort(entries.begin(), entries.end(),
        [](const Entry& a, const Entry& b) {
            bool ac = (a.kind == "func" || a.kind == "proc");
            bool bc = (b.kind == "func" || b.kind == "proc");
            if (ac != bc)
                return ac;
            if (ac && a.line != b.line)
                return a.line < b.line;
            return a.name < b.name;
        });

    Value lst = Value::listVal();
    for (const auto& e : entries) {
        Value mV = moduleClassInstance(moduleTypeValue, "Member");
        ObjectInstance* m = asObjectInstance(mV);
        m->setProperty("name", Value::stringVal(e.name));
        m->setProperty("value", e.value);
        m->setProperty("kind", Value::stringVal(toUnicodeString(e.kind)));
        asList(lst)->append(mV);
    }
    return lst;
}

Value ModuleInspect::inspect_signatures_builtin(ArgsView args)
{
    if (args.size() < 1 || !isCallableValue(args[0]))
        throw std::invalid_argument("inspect.signatures expects a callable");
    SimpleMarkSweepGC::GCNoParkScope nativeCover;   // `lst` outlives a re-entrant call
    Value lst = Value::listVal();
    for (ObjFunction* fn : functionsOf(args[0]))
        if (fn)
            asList(lst)->append(signatureValue(fn));
    return lst;
}

Value ModuleInspect::inspect_call_builtin(ArgsView args)
{
    if (args.size() < 1 || !isCallableValue(args[0]))
        throw std::invalid_argument("inspect.call expects a callable");

    std::vector<Value> callArgs;
    std::vector<ustring> callNames;
    if (args.size() >= 2 && !args[1].isNil()) {
        if (!isList(args[1]))
            throw std::invalid_argument("inspect.call: args must be a list");
        ObjList* lst = asList(args[1]);
        for (int32_t i = 0; i < lst->length(); ++i) {
            callArgs.push_back(lst->getElement(size_t(i)));
            callNames.push_back(ustring());
        }
    }
    if (args.size() >= 3 && !args[2].isNil()) {
        if (!isDict(args[2]))
            throw std::invalid_argument("inspect.call: named must be a dict");
        ObjDict* d = asDict(args[2]);
        for (const auto& k : d->keys()) {
            if (!isString(k))
                throw std::invalid_argument("inspect.call: named keys must be strings");
            callArgs.push_back(d->at(k));
            callNames.push_back(asStringObj(k)->s);
        }
    }

    Value callee = args[0];
    Value receiver = Value::nilVal();
    if (isBoundMethod(callee)) {
        receiver = asBoundMethod(callee)->receiver;
        callee = asBoundMethod(callee)->method;
    }
    else if (isOverloadSet(callee)) {
        // Choose the overload with the VM's own resolver rather than a private
        // rule, so a dynamic call ranks candidates exactly as a written-out one
        // does -- by argument type, subtyping and conversions, not merely by
        // how many arguments were supplied -- and reports the same diagnostics.
        auto* set = asOverloadSet(callee);
        std::vector<OverloadResolver::Candidate> cands;
        cands.reserve(set->closures.size());
        for (const auto& c : set->closures) {
            OverloadResolver::Candidate cand;
            if (c.isObj() && isClosure(c)) {
                auto* fn = asFunction(asClosure(c)->function);
                if (fn->funcType.has_value())
                    cand.funcType = fn->funcType.value();
            }
            cand.target = c;
            cand.isMethod = false;
            cands.push_back(cand);
        }

        std::vector<OverloadResolver::ArgInfo> argInfos;
        argInfos.reserve(callArgs.size());
        for (size_t i = 0; i < callArgs.size(); ++i) {
            OverloadResolver::ArgInfo info;
            info.type = valueRuntimeType(callArgs[i]);
            const ustring& name = i < callNames.size() ? callNames[i] : ustring();
            if (!name.isEmpty()) {
                info.isNamed = true;
                info.nameHash = name.hashCode();
            }
            argInfos.push_back(info);
        }

        OverloadResolver resolver(&vm());
        auto rr = resolver.resolve(cands, argInfos,
                                   /*staticDispatchAttempt=*/false,
                                   /*strictMode=*/true);
        if (rr.kind == OverloadResolver::ResolveResult::Ambiguous)
            throw std::runtime_error(
                toUTF8StdString(toUnicodeString("inspect.call: "))
                + resolver.ambiguityDiagnostic(set->name, cands, rr.tiedIndices, argInfos));
        if (rr.kind != OverloadResolver::ResolveResult::ResolvedUnique)
            throw std::runtime_error(
                std::string("inspect.call: ")
                + resolver.noMatchDiagnostic(set->name, cands, argInfos));
        callee = cands[rr.chosenIndex].target;
    }

    if (!isClosure(callee))
        throw std::invalid_argument("inspect.call: value is not callable dynamically");

    // Hand the call to the dispatch loop rather than re-entering execute():
    // a nested interpreter cannot yield (so it breaks runFor()), and an
    // exception raised inside one unwinds straight past the boundary.  As an
    // ordinary frame the callee behaves like any other call -- it can yield,
    // and its exceptions reach the caller's handlers.
    VM& vm_ = vm();
    auto& cont = vm_.thread->pushContinuation();
    // Leave the result slot unset: because this native pushes frames,
    // callNativeFn fills in the callee slot itself, from the stack depth and
    // argument count only it knows (our arguments may have been marshalled
    // into a buffer rather than left on the stack).
    cont.resultSlotIndex = -1;
    cont.onComplete = [](VM& vmRef, Value result) -> bool {
        vmRef.push(result);      // the call's value becomes inspect.call's value
        return true;
    };

    if (!vm_.pushContinuationCall(asClosure(callee), callArgs, callNames, receiver)) {
        vm_.clearContinuation();
        throw std::runtime_error("inspect.call: could not invoke the callable");
    }
    return Value::nilVal();
}

Value ModuleInspect::inspect_calling_module_builtin(ArgsView args)
{
    int64_t depth = (args.size() >= 1 && args[0].isNumber()) ? args[0].asInt() : 0;
    if (depth < 0 || !VM::thread)
        return Value::nilVal();
    // Natives do not push a bytecode frame, so frames.back() is our caller.
    const auto& frames = VM::thread->frames;
    if (frames.size() <= size_t(depth))
        return Value::nilVal();
    const CallFrame& frame = frames[frames.size() - 1 - size_t(depth)];
    ObjFunction* fn = asFunction(asClosure(frame.closure)->function);
    Value mv = fn->moduleType.strongRef();
    return (mv.isObj() && isModuleType(mv)) ? mv : Value::nilVal();
}

Value ModuleInspect::inspect_main_module_builtin(ArgsView args)
{
    (void)args;
    // The outermost frame of this thread: on the main thread that is the body
    // of the script the process was started with, which is exactly what a test
    // module wants in order to decide whether it is being run or imported.
    if (!VM::thread || VM::thread->frames.empty())
        return Value::nilVal();
    const CallFrame& frame = VM::thread->frames.front();
    ObjFunction* fn = asFunction(asClosure(frame.closure)->function);
    Value mv = fn->moduleType.strongRef();
    return (mv.isObj() && isModuleType(mv)) ? mv : Value::nilVal();
}
