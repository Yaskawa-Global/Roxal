#include "AstPrinter.h"
#include <core/common.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <set>
#include <sstream>
#include <stdexcept>
#include <typeinfo>

using namespace roxal;
using namespace roxal::ast;

namespace {

// precedence, matching the grammar chain (higher binds tighter)
constexpr int PREC_ASSIGN = 1;
constexpr int PREC_OR = 2;
constexpr int PREC_AND = 3;
constexpr int PREC_BITOR = 4;
constexpr int PREC_BITXOR = 5;
constexpr int PREC_BITAND = 6;
constexpr int PREC_EQUALITY = 7;
constexpr int PREC_COMPARISON = 8;
constexpr int PREC_TERM = 9;
constexpr int PREC_FACTOR = 10;
constexpr int PREC_UNARY = 11;
constexpr int PREC_CALL = 12;
constexpr int PREC_PRIMARY = 13;

// ASCII source token for a binary op (opString() uses display glyphs like ×)
const char* binOpToken(BinaryOp::Op op)
{
    switch (op) {
        case BinaryOp::Add: return "+";
        case BinaryOp::Subtract: return "-";
        case BinaryOp::Multiply: return "*";
        case BinaryOp::Divide: return "/";
        case BinaryOp::Modulo: return "rem";
        case BinaryOp::And: return "and";
        case BinaryOp::Or: return "or";
        case BinaryOp::BitAnd: return "&";
        case BinaryOp::BitOr: return "|";
        case BinaryOp::BitXor: return "^";
        case BinaryOp::Equal: return "==";
        case BinaryOp::NotEqual: return "!=";
        case BinaryOp::Is: return "is";
        case BinaryOp::In: return "in";
        case BinaryOp::NotIn: return "not in";
        case BinaryOp::LessThan: return "<";
        case BinaryOp::GreaterThan: return ">";
        case BinaryOp::LessOrEqual: return "<=";
        case BinaryOp::GreaterOrEqual: return ">=";
        default: throw std::runtime_error("AstPrinter: BinaryOp with op None");
    }
}

int binOpPrec(BinaryOp::Op op)
{
    switch (op) {
        case BinaryOp::Or: return PREC_OR;
        case BinaryOp::And: return PREC_AND;
        case BinaryOp::BitOr: return PREC_BITOR;
        case BinaryOp::BitXor: return PREC_BITXOR;
        case BinaryOp::BitAnd: return PREC_BITAND;
        case BinaryOp::Equal:
        case BinaryOp::NotEqual:
        case BinaryOp::Is: return PREC_EQUALITY;
        case BinaryOp::LessThan:
        case BinaryOp::GreaterThan:
        case BinaryOp::LessOrEqual:
        case BinaryOp::GreaterOrEqual:
        case BinaryOp::In:
        case BinaryOp::NotIn: return PREC_COMPARISON;
        case BinaryOp::Add:
        case BinaryOp::Subtract: return PREC_TERM;
        case BinaryOp::Multiply:
        case BinaryOp::Divide:
        case BinaryOp::Modulo: return PREC_FACTOR;
        default: return PREC_PRIMARY;
    }
}

template <class T>
const T* attrGet(const ast::AST& n, const char* key)
{
    auto it = n.attrs.find(key);
    if (it == n.attrs.end())
        return nullptr;
    return std::any_cast<T>(&it->second);
}

// grammar keywords: identifiers spelled like one must be backtick-quoted.
// 'this'/'super'/'true'/'false'/'nil' are deliberately absent — they parse as
// primaries and reappear as ordinary names in the AST.
bool isKeywordName(const std::string& s)
{
    static const std::set<std::string> kw = {
        "_", "actor", "and", "as", "becomes", "bool", "break", "by", "byte",
        "case", "changes", "const", "continue", "decimal", "default", "dict",
        "else", "elseif", "emit", "enum", "event", "except", "extends",
        "finally", "for", "func", "if", "implements", "import", "in", "int",
        "interface", "is", "jump", "list", "loperator", "match", "matrix",
        "mutable", "not", "number", "object", "occurs", "operator", "or",
        "orient", "private", "proc", "raise", "range", "real", "rem",
        "return", "roperator", "scope", "signal", "string", "tensor",
        "try", "type", "until", "var", "vector", "when", "where", "while",
        "with",
    };
    return kw.count(s) != 0;
}

// suffix as source text: bare for simple letter suffixes, braced otherwise
// (multi-word / compound suffixes like {u/t} or {my long unit})
std::string suffixText(const ustring& u)
{
    std::string s = toUTF8StdString(u);
    if (s == "%")
        return s;      // percent is a bare-only suffix: {%} does not lex
    bool simple = !s.empty();
    for (unsigned char c : s)
        if (!std::isalpha(c))
            simple = false;
    if (simple)
        return s;
    return "{" + s + "}";
}

// identifier as source text, backtick-quoted when it collides with a keyword
std::string identText(const ustring& u)
{
    std::string s = toUTF8StdString(u);
    if (isKeywordName(s))
        return "`" + s + "`";
    return s;
}

} // namespace


std::string AstPrinter::print(const ast::AST& n)
{
    out.clear();
    indent = 0;
    if (auto* f = dynamic_cast<const File*>(&n)) {
        file(*f);
        return out;
    }
    if (auto* e = dynamic_cast<const Expression*>(&n))
        return exprTop(*e);
    stmtWithTrivia(n);   // standalone statements render their decorations too
    return out;
}

void AstPrinter::emitLine(const std::string& s)
{
    if (s.empty()) {
        out += "\n";
        return;
    }
    std::string t = ind() + s;
    // a multi-line lambda that ends the statement leaves a trailing
    // indent-only tail — drop it rather than emitting a blank line
    size_t lastNl = t.rfind('\n');
    if (lastNl != std::string::npos &&
        t.find_first_not_of(' ', lastNl + 1) == std::string::npos)
        t.erase(lastNl);
    out += t + "\n";
}

// ---------------------------------------------------------------------------
// statement / declaration level
// ---------------------------------------------------------------------------

void AstPrinter::file(const File& n)
{
    for (auto& i : n.imports)
        if (i) stmtWithTrivia(*i);
    declsOrStmts(n.declsOrStmts);
    if (auto* end = attrGet<std::vector<std::string>>(n, "end_comments"))
        for (auto& c : *end)
            emitLine(c);
}

void AstPrinter::declsOrStmts(const std::vector<std::variant<ptr<Declaration>, ptr<Statement>>>& ds)
{
    for (auto& d : ds) {
        const ast::AST* p = nullptr;
        if (std::holds_alternative<ptr<Declaration>>(d))
            p = std::get<ptr<Declaration>>(d).get();
        else
            p = std::get<ptr<Statement>>(d).get();
        if (p)
            stmtWithTrivia(*p);
    }
}

void AstPrinter::stmtWithTrivia(const ast::AST& n)
{
    if (auto* blanks = attrGet<int64_t>(n, "blank_lines_before"))
        for (int64_t i = 0; i < *blanks; i++)
            out += "\n";
    if (auto* leading = attrGet<std::vector<std::string>>(n, "leading_comments"))
        for (auto& c : *leading)
            emitLine(c);

    size_t before = out.size();
    node(n);

    if (auto* trailing = attrGet<std::string>(n, "trailing_comment")) {
        // attach to the first rendered line
        size_t eol = out.find('\n', before);
        if (eol != std::string::npos)
            out.insert(eol, "   " + *trailing);
    }
}

void AstPrinter::body(const Suite& s)
{
    indent++;
    if (s.declsOrStmts.empty())
        emitLine("_");
    else
        declsOrStmts(s.declsOrStmts);
    indent--;
}

void AstPrinter::annotations(const ast::AST& n)
{
    for (auto& a : n.annotations) {
        if (!a) continue;
        std::string s = "@" + toUTF8StdString(a->name);
        if (!a->args.empty())
            s += "(" + args(a->args) + ")";
        emitLine(s);
    }
}

void AstPrinter::node(const ast::AST& n)
{
    const std::type_info& t = typeid(n);

    if (t == typeid(Import)) { importDecl(static_cast<const Import&>(n)); return; }
    if (t == typeid(VarDecl)) { varDecl(static_cast<const VarDecl&>(n), true); return; }
    if (t == typeid(FuncDecl)) {
        auto& fd = static_cast<const FuncDecl&>(n);
        // TypeDeducer propagates FuncDecl annotations onto the Function
        // (shared nodes) — print only the ones the Function doesn't carry
        for (auto& a : fd.annotations) {
            if (!a) continue;
            bool onFunc = false;
            if (fd.func)
                for (auto& fa : fd.func->annotations)
                    if (fa.get() == a.get()) { onFunc = true; break; }
            if (!onFunc) {
                std::string s = "@" + toUTF8StdString(a->name);
                if (!a->args.empty())
                    s += "(" + args(a->args) + ")";
                emitLine(s);
            }
        }
        if (fd.func) function(*fd.func);
        return;
    }
    if (t == typeid(TypeDecl)) { typeDecl(static_cast<const TypeDecl&>(n)); return; }
    if (t == typeid(Function)) { function(static_cast<const Function&>(n)); return; }
    if (t == typeid(PropertyAccessor)) { propertyAccessor(static_cast<const PropertyAccessor&>(n)); return; }

    if (t == typeid(Suite)) {
        // a Suite in statement position is a `scope:` block
        emitLine("scope:");
        body(static_cast<const Suite&>(n));
        return;
    }
    if (t == typeid(ExpressionStatement)) {
        auto& s = static_cast<const ExpressionStatement&>(n);
        std::string text = s.expr ? exprTop(*s.expr) : "";
        if (s.atHost.has_value() && s.atHost.value())
            text += " at " + expr(*s.atHost.value(), PREC_CALL);
        emitLine(text);
        return;
    }
    if (t == typeid(ReturnStatement)) {
        auto& s = static_cast<const ReturnStatement&>(n);
        emitLine(s.expr.has_value() && s.expr.value()
                     ? "return " + exprTop(*s.expr.value()) : "return");
        return;
    }
    if (t == typeid(BreakStatement)) { emitLine("break"); return; }
    if (t == typeid(ContinueStatement)) { emitLine("continue"); return; }
    if (t == typeid(JumpStatement)) {
        emitLine("jump " + toUTF8StdString(static_cast<const JumpStatement&>(n).name));
        return;
    }
    if (t == typeid(LabelStatement)) {
        emitLine("label " + toUTF8StdString(static_cast<const LabelStatement&>(n).name));
        return;
    }
    if (t == typeid(RaiseStatement)) {
        auto& s = static_cast<const RaiseStatement&>(n);
        emitLine(s.exception.has_value() && s.exception.value()
                     ? "raise " + exprTop(*s.exception.value()) : "raise");
        return;
    }
    if (t == typeid(IfStatement)) {
        auto& s = static_cast<const IfStatement&>(n);
        for (size_t i = 0; i < s.conditionalSuites.size(); i++) {
            auto& [cond, suite] = s.conditionalSuites[i];
            emitLine((i == 0 ? "if " : "elseif ") + (cond ? exprTop(*cond) : "") + ":");
            if (suite) body(*suite);
        }
        if (s.elseSuite.has_value() && s.elseSuite.value()) {
            emitLine("else:");
            body(*s.elseSuite.value());
        }
        return;
    }
    if (t == typeid(WhileStatement)) {
        auto& s = static_cast<const WhileStatement&>(n);
        emitLine("while " + (s.condition ? exprTop(*s.condition) : "") + ":");
        if (s.body) body(*s.body);
        return;
    }
    if (t == typeid(ForStatement)) {
        auto& s = static_cast<const ForStatement&>(n);
        std::string targets;
        for (size_t i = 0; i < s.targetList.size(); i++) {
            if (i) targets += ", ";
            auto& v = s.targetList[i];
            targets += identText(v->name);
            if (v->varType.has_value())
                targets += ":" + varTypeText(v->varType.value());
        }
        emitLine("for " + targets + " in " + (s.iterable ? exprTop(*s.iterable) : "") + ":");
        if (s.body) body(*s.body);
        return;
    }
    if (t == typeid(WhenStatement)) {
        auto& s = static_cast<const WhenStatement&>(n);
        std::string text = "when " + (s.trigger ? exprTop(*s.trigger) : "");
        if (s.becomes.has_value() && s.becomes.value())
            text += " becomes " + exprTop(*s.becomes.value());
        else if (s.requiresSignalChange)
            text += " changes";
        else
            text += " occurs";
        if (s.binding.has_value())
            text += " as " + identText(s.binding.value());
        if (s.targetFilter.has_value() && s.targetFilter.value()) {
            std::string binding = s.binding.has_value() ? toUTF8StdString(s.binding.value()) : "evt";
            text += " where " + binding + ".target == " + exprTop(*s.targetFilter.value());
        }
        emitLine(text + ":");
        if (s.body) body(*s.body);
        return;
    }
    if (t == typeid(UntilStatement)) {
        auto& s = static_cast<const UntilStatement&>(n);
        emitLine((s.stmt ? inlineStmt(*s.stmt) : "") + " until " +
                 (s.condition ? exprTop(*s.condition) : ""));
        return;
    }
    if (t == typeid(AdheringIfStatement)) {
        auto& s = static_cast<const AdheringIfStatement&>(n);
        emitLine((s.stmt ? inlineStmt(*s.stmt) : "") + " if " +
                 (s.condition ? exprTop(*s.condition) : ""));
        return;
    }
    if (t == typeid(TryStatement)) {
        auto& s = static_cast<const TryStatement&>(n);
        emitLine("try:");
        if (s.body) body(*s.body);
        for (auto& c : s.exceptClauses) {
            std::string text = "except";
            if (c.name.has_value())
                text += " " + toUTF8StdString(c.name.value());
            if (c.type.has_value() && c.type.value())
                text += " :" + exprTop(*c.type.value()) + ":";
            else
                text += ":";
            emitLine(text);
            if (c.body) body(*c.body);
        }
        if (s.finallySuite.has_value() && s.finallySuite.value()) {
            emitLine("finally:");
            body(*s.finallySuite.value());
        }
        return;
    }
    if (t == typeid(MatchStatement)) {
        auto& s = static_cast<const MatchStatement&>(n);
        emitLine("match " + (s.matchExpr ? exprTop(*s.matchExpr) : "") + ":");
        indent++;
        for (auto& [patterns, suite] : s.cases) {
            std::string pats;
            for (size_t i = 0; i < patterns.size(); i++) {
                if (i) pats += ", ";
                if (auto* r = dynamic_cast<const Range*>(patterns[i].get()))
                    pats += rangeText(*r, true);
                else
                    pats += exprTop(*patterns[i]);
            }
            emitLine("case " + pats + ":");
            if (suite) body(*suite);
        }
        if (s.defaultCase.has_value() && s.defaultCase.value()) {
            emitLine("default:");
            body(*s.defaultCase.value());
        }
        indent--;
        return;
    }
    if (t == typeid(WithStatement)) {
        auto& s = static_cast<const WithStatement&>(n);
        emitLine("with " + (s.contextExpr ? exprTop(*s.contextExpr) : "") + ":");
        if (s.body) body(*s.body);
        return;
    }
    if (t == typeid(SingleInput)) {
        auto& s = static_cast<const SingleInput&>(n);
        if (s.stmt) node(*s.stmt);
        return;
    }

    throw std::runtime_error(std::string("AstPrinter: unhandled statement node: ") + t.name());
}

std::string AstPrinter::inlineStmt(const Statement& n)
{
    const std::type_info& t = typeid(n);
    if (t == typeid(ExpressionStatement)) {
        auto& s = static_cast<const ExpressionStatement&>(n);
        std::string text = s.expr ? exprTop(*s.expr) : "";
        if (s.atHost.has_value() && s.atHost.value())
            text += " at " + expr(*s.atHost.value(), PREC_CALL);
        return text;
    }
    if (t == typeid(ReturnStatement)) {
        auto& s = static_cast<const ReturnStatement&>(n);
        return s.expr.has_value() && s.expr.value()
                   ? "return " + exprTop(*s.expr.value()) : "return";
    }
    if (t == typeid(BreakStatement)) return "break";
    if (t == typeid(ContinueStatement)) return "continue";
    if (t == typeid(JumpStatement))
        return "jump " + toUTF8StdString(static_cast<const JumpStatement&>(n).name);
    if (t == typeid(RaiseStatement)) {
        auto& s = static_cast<const RaiseStatement&>(n);
        return s.exception.has_value() && s.exception.value()
                   ? "raise " + exprTop(*s.exception.value()) : "raise";
    }
    throw std::runtime_error(std::string("AstPrinter: statement kind cannot render inline: ") + t.name());
}

// ---------------------------------------------------------------------------
// declarations
// ---------------------------------------------------------------------------

void AstPrinter::importDecl(const Import& n)
{
    std::string s = "import ";
    for (size_t i = 0; i < n.packages.size(); i++) {
        if (i) s += ".";
        s += toUTF8StdString(n.packages[i]);
    }
    if (n.symbols.size() == 1 && toUTF8StdString(n.symbols[0]) == "*") {
        s += ".*";
    } else if (!n.symbols.empty()) {
        s += ".[";
        for (size_t i = 0; i < n.symbols.size(); i++) {
            if (i) s += ", ";
            s += toUTF8StdString(n.symbols[i]);
        }
        s += "]";
    }
    emitLine(s);
}

void AstPrinter::varDecl(const VarDecl& n, bool asStatement)
{
    (void)asStatement;
    annotations(n);
    std::string s;
    if (n.access == Access::Private)
        s += "private ";
    s += n.isConst ? "const " : "var ";
    s += identText(n.name);
    if (n.varType.has_value()) {
        s += ":";
        if (n.isTypeConst) s += "const ";
        if (n.isTypeMutable) s += "mutable ";
        s += varTypeText(n.varType.value());
    }
    if (n.initializer.has_value() && n.initializer.value()) {
        s += " = " + exprTop(*n.initializer.value());
        if (n.atHost.has_value() && n.atHost.value())
            s += " at " + expr(*n.atHost.value(), PREC_CALL);
    }
    emitLine(s);
}

std::string AstPrinter::parameter(const Parameter& n)
{
    if (n.isStar)
        return "*";
    std::string s;
    if (n.variadic)
        s += "...";
    s += identText(n.name);
    if (n.type.has_value()) {
        s += ":";
        if (n.isConst) s += "const ";
        if (n.isMutable) s += "mutable ";
        s += varTypeText(n.type.value());
    }
    if (n.defaultValue.has_value() && n.defaultValue.value())
        s += "=" + exprTop(*n.defaultValue.value());
    return s;
}

std::string AstPrinter::funcSignature(const Function& n)
{
    std::string s;
    if (n.access == Access::Private)
        s += "private ";
    if (hasModifier(n.methodModifiers, MethodModifier::Implicit))
        s += "implicit ";
    if (hasModifier(n.methodModifiers, MethodModifier::StatementAction))
        s += "statement action ";
    s += n.isProc ? "proc" : "func";
    if (n.name.has_value()) {
        s += " ";
        std::string name = toUTF8StdString(n.name.value());
        // conversion operators are stored as "operator-><type>" but written
        // as "operator <type>"
        if (name.rfind("operator->", 0) == 0)
            name = "operator " + name.substr(10);
        else if (name.rfind("operator", 0) != 0 && name.rfind("loperator", 0) != 0 &&
                 name.rfind("roperator", 0) != 0 && isKeywordName(name))
            name = "`" + name + "`";
        s += name;
    }
    s += "(";
    for (size_t i = 0; i < n.params.size(); i++) {
        if (i) s += ", ";
        if (n.params[i]) s += parameter(*n.params[i]);
    }
    s += ")";
    if (n.returnTypes.has_value() && !n.returnTypes.value().empty()) {
        auto& rts = n.returnTypes.value();
        auto one = [&](size_t i) {
            std::string r;
            if (i < n.returnTypeConst.size() && n.returnTypeConst[i]) r += "const ";
            return r + varTypeText(rts[i]);
        };
        s += " -> ";
        if (rts.size() == 1) {
            s += one(0);
        } else {
            s += "[";
            for (size_t i = 0; i < rts.size(); i++) {
                if (i) s += ", ";
                s += one(i);
            }
            s += "]";
        }
    }
    return s;
}

void AstPrinter::function(const Function& n)
{
    annotations(n);
    std::string sig = funcSignature(n);

    if (std::holds_alternative<std::monostate>(n.body)) {
        emitLine(sig);   // abstract: signature only, no colon
        return;
    }
    if (std::holds_alternative<ptr<Suite>>(n.body)) {
        emitLine(sig + ":");
        auto& suite = std::get<ptr<Suite>>(n.body);
        if (suite) body(*suite);
        return;
    }
    // expression body (only lambdas normally, but render it anyway)
    emitLine(sig + ": " + exprTop(*std::get<ptr<Expression>>(n.body)));
}

void AstPrinter::propertyAccessor(const PropertyAccessor& n)
{
    annotations(n);
    std::string s;
    if (n.access == Access::Private)
        s += "private ";
    s += n.isConst ? "const " : "var ";
    s += identText(n.name);
    s += ":" + varTypeText(n.propType);
    if (n.initializer.has_value() && n.initializer.value())
        s += " = " + exprTop(*n.initializer.value());
    emitLine(s + ":");

    indent++;
    auto accessor = [this](const char* kw,
                           const std::optional<std::variant<ptr<Suite>, ptr<Statement>, std::monostate>>& acc) {
        if (!acc.has_value())
            return;
        auto& va = acc.value();
        if (std::holds_alternative<std::monostate>(va)) {
            emitLine(kw);   // abstract accessor
        } else if (std::holds_alternative<ptr<Suite>>(va)) {
            emitLine(std::string(kw) + ":");
            body(*std::get<ptr<Suite>>(va));
        } else {
            emitLine(std::string(kw) + ": " + inlineStmt(*std::get<ptr<Statement>>(va)));
        }
    };
    accessor("get", n.getter);
    accessor("set", n.setter);
    indent--;
}

void AstPrinter::typeDecl(const TypeDecl& n)
{
    annotations(n);
    std::string s;
    if (n.access == Access::Private)
        s += "private ";
    s += "type " + toUTF8StdString(n.name) + " ";
    switch (n.kind) {
        case TypeDecl::Object: s += "object"; break;
        case TypeDecl::Actor: s += "actor"; break;
        case TypeDecl::Interface: s += "interface"; break;
        case TypeDecl::Enumeration: s += "enum"; break;
        case TypeDecl::Event: s += "event"; break;
    }
    if (n.extends.has_value())
        s += " extends " + toUTF8StdString(joinTypeName(n.extends.value()));
    if (!n.implements.empty()) {
        s += " implements ";
        for (size_t i = 0; i < n.implements.size(); i++) {
            if (i) s += ", ";
            s += toUTF8StdString(joinTypeName(n.implements[i]));
        }
    }

    bool empty = n.properties.empty() && n.propertyAccessors.empty() &&
                 n.methods.empty() && n.nestedTypes.empty() && n.enumLabels.empty();
    if (empty) {
        emitLine(s);
        return;
    }

    emitLine(s + ":");
    indent++;
    if (n.kind == TypeDecl::Enumeration) {
        for (auto& [labelName, labelExpr] : n.enumLabels) {
            std::string l = toUTF8StdString(labelName);
            if (labelExpr)
                l += " = " + exprTop(*labelExpr);
            emitLine(l);
        }
    } else {
        // note: original member interleaving is not stored in the AST —
        // render properties, accessors, methods, then nested types
        for (auto& p : n.properties)
            if (p) stmtWithTrivia(*p);
        for (auto& a : n.propertyAccessors)
            if (a) stmtWithTrivia(*a);
        for (auto& m : n.methods)
            if (m) stmtWithTrivia(*m);
        for (auto& nt : n.nestedTypes)
            if (nt) stmtWithTrivia(*nt);
    }
    indent--;
}

// ---------------------------------------------------------------------------
// expressions
// ---------------------------------------------------------------------------

std::string AstPrinter::varTypeText(const ast::VarType& vt)
{
    if (std::holds_alternative<type::BuiltinType>(vt))
        return type::to_string(std::get<type::BuiltinType>(vt));
    return toUTF8StdString(joinTypeName(std::get<ast::TypeName>(vt)));
}

std::string AstPrinter::numText(const std::variant<int32_t, int64_t, double>& num)
{
    if (std::holds_alternative<int32_t>(num))
        return std::to_string(std::get<int32_t>(num));
    if (std::holds_alternative<int64_t>(num))
        return std::to_string(std::get<int64_t>(num));

    double d = std::get<double>(num);
    char buf[40];
    for (int prec = 6; prec <= 17; prec++) {
        snprintf(buf, sizeof(buf), "%.*g", prec, d);
        if (std::strtod(buf, nullptr) == d)
            break;
    }
    std::string s(buf);
    if (s.find_first_of(".eE") == std::string::npos &&
        s.find("inf") == std::string::npos && s.find("nan") == std::string::npos)
        s += ".0";
    return s;
}

std::string AstPrinter::strLiteral(const ustring& u)
{
    std::string raw = toUTF8StdString(u);
    std::string s = "'";
    for (char c : raw) {
        switch (c) {
            case '\\': s += "\\\\"; break;
            case '\'': s += "\\'"; break;
            case '\n': s += "\\n"; break;
            case '\r': s += "\\r"; break;
            case '\t': s += "\\t"; break;
            default: s += c;
        }
    }
    return s + "'";
}

std::string AstPrinter::args(const std::vector<ArgNameExpr>& a)
{
    std::string s;
    for (size_t i = 0; i < a.size(); i++) {
        if (i) s += ", ";
        if (a[i].first.length() > 0)
            s += identText(a[i].first) + "=";
        if (a[i].second)
            s += exprTop(*a[i].second);
    }
    return s;
}

std::string AstPrinter::rangeText(const Range& r, bool bare)
{
    std::string s;
    if (r.start) s += exprTop(*r.start);
    s += r.closed ? ".." : "..<";
    if (r.stop) {
        std::string stop = exprTop(*r.stop);
        if (!r.closed && !stop.empty() && stop[0] == '-')
            s += " ";     // '..<-1' would lex as '..' '<-' (copy-into)
        s += stop;
    }
    if (r.step) s += " by " + exprTop(*r.step);
    if (bare)
        return s;
    return "range(" + s + ")";
}

std::string AstPrinter::expr(const Expression& e, int parentPrec, bool isRight)
{
    const std::type_info& t = typeid(e);
    std::string s;
    int myPrec = PREC_PRIMARY;

    if (t == typeid(Assignment)) {
        auto& a = static_cast<const Assignment&>(e);
        myPrec = PREC_ASSIGN;
        s = (a.lhs ? expr(*a.lhs, PREC_ASSIGN) : "") +
            (a.op == Assignment::CopyInto ? " <- " : " = ") +
            (a.rhs ? expr(*a.rhs, 0) : "");
        if (a.atHost.has_value() && a.atHost.value())
            s += " at " + expr(*a.atHost.value(), PREC_CALL);
    }
    else if (t == typeid(BinaryOp)) {
        auto& b = static_cast<const BinaryOp&>(e);
        myPrec = binOpPrec(b.op);
        s = (b.lhs ? expr(*b.lhs, myPrec, false) : "") +
            " " + binOpToken(b.op) + " " +
            (b.rhs ? expr(*b.rhs, myPrec, true) : "");
    }
    else if (t == typeid(UnaryOp)) {
        auto& u = static_cast<const UnaryOp&>(e);
        if (u.op == UnaryOp::Accessor) {
            myPrec = PREC_CALL;
            std::string member = u.member.has_value() ? toUTF8StdString(u.member.value()) : "";
            // the grammar allows when/emit/match bare after '.'
            if (isKeywordName(member) && member != "when" && member != "emit" && member != "match")
                member = "`" + member + "`";
            s = (u.arg ? expr(*u.arg, PREC_CALL) : "") + "." + member;
        } else {
            myPrec = PREC_UNARY;
            const char* tok = u.op == UnaryOp::Negate ? "-"
                             : u.op == UnaryOp::Not ? "not " : "~";
            s = tok + (u.arg ? expr(*u.arg, PREC_UNARY) : "");
        }
    }
    else if (t == typeid(Variable)) {
        std::string name = toUTF8StdString(static_cast<const Variable&>(e).name);
        // 'this' and 'super' parse as primaries, not identifiers — keep bare
        s = (name == "this" || name == "super") ? name : identText(static_cast<const Variable&>(e).name);
    }
    else if (t == typeid(Call)) {
        auto& c = static_cast<const Call&>(e);
        myPrec = PREC_CALL;
        // range(a..b): the grammar's RANGE '(' range ')' primary parses to a
        // Call around a Range node — render the inner range bare so the
        // wrapper isn't doubled
        const ast::Range* bareRange = nullptr;
        if (c.callable && typeid(*c.callable) == typeid(ast::Type) &&
            static_cast<const ast::Type&>(*c.callable).t == type::BuiltinType::Range &&
            c.args.size() == 1 && c.args[0].second &&
            typeid(*c.args[0].second) == typeid(ast::Range))
            bareRange = static_cast<const ast::Range*>(c.args[0].second.get());
        if (bareRange)
            s = "range(" + rangeText(*bareRange, true) + ")";
        else
            s = (c.callable ? expr(*c.callable, PREC_CALL) : "") + "(" + args(c.args) + ")";
    }
    else if (t == typeid(Index)) {
        auto& ix = static_cast<const Index&>(e);
        myPrec = PREC_CALL;
        s = (ix.indexable ? expr(*ix.indexable, PREC_CALL) : "") + "[";
        for (size_t i = 0; i < ix.args.size(); i++) {
            if (i) s += ", ";
            if (auto* r = dynamic_cast<const Range*>(ix.args[i].get()))
                s += rangeText(*r, true);
            else
                s += exprTop(*ix.args[i]);
        }
        s += "]";
    }
    else if (t == typeid(Range)) {
        s = rangeText(static_cast<const Range&>(e), false);
    }
    else if (t == typeid(LambdaFunc)) {
        auto& l = static_cast<const LambdaFunc&>(e);
        myPrec = PREC_ASSIGN;   // parenthesize inside operators
        if (!l.func) {
            s = "";
        } else if (std::holds_alternative<ptr<Expression>>(l.func->body)) {
            s = funcSignature(*l.func) + ": " +
                exprTop(*std::get<ptr<Expression>>(l.func->body));
        } else if (std::holds_alternative<ptr<Suite>>(l.func->body)) {
            // multi-line lambda: valid only where the lambda is the last
            // thing on its line; whatever follows (e.g. a closing paren)
            // lands on a fresh line at the enclosing indent, after DEDENT
            std::string sig = funcSignature(*l.func) + ":";
            AstPrinter sub;
            sub.indent = indent + 2;
            sub.body(*std::get<ptr<Suite>>(l.func->body));
            s = sig + "\n" + sub.out + ind();
        } else {
            s = funcSignature(*l.func);
        }
    }
    else if (t == typeid(Literal)) {
        s = "nil";
    }
    else if (t == typeid(ast::Bool)) {
        s = static_cast<const ast::Bool&>(e).value ? "true" : "false";
    }
    else if (t == typeid(Num)) {
        auto& num = static_cast<const Num&>(e).num;
        s = numText(num);
        // a negative literal used as an operand needs parens in tight spots
        if (!s.empty() && s[0] == '-')
            myPrec = PREC_UNARY;
    }
    else if (t == typeid(Str)) {
        s = strLiteral(static_cast<const Str&>(e).str);
    }
    else if (t == typeid(StrInterp)) {
        auto& si = static_cast<const StrInterp&>(e);
        std::string bodyText;
        bool needTriple = false;
        for (auto& p : si.parts) {
            if (p.isLiteral()) {
                for (char c : toUTF8StdString(p.text)) {
                    switch (c) {
                        case '\\': bodyText += "\\\\"; break;
                        case '"': bodyText += "\\\""; break;
                        case '{': bodyText += "\\{"; break;
                        case '\n': bodyText += "\\n"; break;
                        case '\r': bodyText += "\\r"; break;
                        case '\t': bodyText += "\\t"; break;
                        default: bodyText += c;
                    }
                }
            } else if (p.expr) {
                std::string hole = exprTop(*p.expr);
                if (hole.find('"') != std::string::npos)
                    needTriple = true;   // nested double quotes need """..."""
                if (!hole.empty() && hole[0] == '{')
                    hole = " " + hole;   // '{{' would read as an escaped brace
                bodyText += "{" + hole + "}";
            }
        }
        const char* q = needTriple ? "\"\"\"" : "\"";
        s = q + bodyText + q;
        if (si.suffix.length() > 0)
            s += suffixText(si.suffix);
    }
    else if (t == typeid(SuffixedNum)) {
        auto& sn = static_cast<const SuffixedNum&>(e);
        s = numText(sn.num) + suffixText(sn.suffix);
        if (!s.empty() && s[0] == '-')
            myPrec = PREC_UNARY;
    }
    else if (t == typeid(SuffixedStr)) {
        auto& ss = static_cast<const SuffixedStr&>(e);
        s = strLiteral(ss.str) + suffixText(ss.suffix);
    }
    else if (t == typeid(ast::Type)) {
        s = type::to_string(static_cast<const ast::Type&>(e).t);
    }
    else if (t == typeid(List)) {
        auto& l = static_cast<const List&>(e);
        s = "[";
        for (size_t i = 0; i < l.elements.size(); i++) {
            if (i) s += ", ";
            if (!l.elements[i]) continue;
            // "[a - b]" is ambiguous with a vector literal — parenthesize
            // operator expressions
            if (typeid(*l.elements[i]) == typeid(BinaryOp))
                s += "(" + exprTop(*l.elements[i]) + ")";
            else
                s += exprTop(*l.elements[i]);
        }
        s += "]";
    }
    else if (t == typeid(Dict)) {
        auto& d = static_cast<const Dict&>(e);
        s = "{";
        for (size_t i = 0; i < d.entries.size(); i++) {
            if (i) s += ", ";
            if (d.entries[i].first) s += exprTop(*d.entries[i].first);
            s += ": ";
            if (d.entries[i].second) s += exprTop(*d.entries[i].second);
        }
        s += "}";
    }
    else if (t == typeid(ast::Vector)) {
        auto& v = static_cast<const ast::Vector&>(e);
        s = "[";
        for (size_t i = 0; i < v.elements.size(); i++) {
            if (i) s += " ";
            if (!v.elements[i]) continue;
            // vec_elem is a signed number or a parenthesized expression; the
            // parser rejects bare interior negatives ("[1 -2]") as ambiguous
            // with subtraction, so negatives are always parenthesized
            auto& el = *v.elements[i];
            std::string elText;
            if (typeid(el) == typeid(Num) || typeid(el) == typeid(SuffixedNum) ||
                (typeid(el) == typeid(UnaryOp) &&
                 static_cast<const UnaryOp&>(el).op == UnaryOp::Negate &&
                 static_cast<const UnaryOp&>(el).arg &&
                 typeid(*static_cast<const UnaryOp&>(el).arg) == typeid(Num)))
                elText = exprTop(el);
            if (!elText.empty() && elText[0] != '-')
                s += elText;
            else if (!elText.empty())
                s += "(" + elText + ")";
            else
                s += "(" + exprTop(el) + ")";
        }
        s += "]";
    }
    else if (t == typeid(Matrix)) {
        auto& m = static_cast<const Matrix&>(e);
        s = "[";
        for (size_t r = 0; r < m.rows.size(); r++) {
            if (r) s += "; ";
            if (!m.rows[r]) continue;
            std::string row = expr(*m.rows[r], 0);
            // strip the vector's own brackets
            if (row.size() >= 2 && row.front() == '[' && row.back() == ']')
                row = row.substr(1, row.size() - 2);
            s += row;
        }
        s += "]";
    }
    else {
        throw std::runtime_error(std::string("AstPrinter: unhandled expression node: ") + t.name());
    }

    bool needParens = myPrec < parentPrec || (myPrec == parentPrec && isRight &&
                                              myPrec >= PREC_OR && myPrec <= PREC_FACTOR);
    if (needParens)
        return "(" + s + ")";
    return s;
}
