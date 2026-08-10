#!/usr/bin/env python3
"""Generator for the `inspect` builtin module's AST mirror binding.

Reads the node definitions in core/AST.h and emits:
  1. the GENERATED section of modules/inspect.rox  (pure-Roxal mirror classes,
     one per AST node kind, plus the node_fields() schema table)
  2. compiler/InspectAstConv.inc  (C++ -> Roxal converter functions, the
     typeid dispatch, and forEachChildOf)

Design: the node/field inventory lives HERE as a reviewed declarative spec
(NODE_SPEC below).  A strict verifier re-parses core/AST.h structurally and
hard-errors if the header has any struct/member the spec doesn't account for
(or vice versa), so drift is caught loudly at generation time.  This was chosen
over libclang because AST.h's include chain (ICU, project headers) makes an
out-of-build libclang parse fragile, while the header itself is small, stable
and extremely regular.  Precedent: modules/opencv/shim/generate.py.

Usage:  python3 tools/inspect-gen/generate.py [--check]
  --check: verify + regenerate to memory and diff against the checked-in
           outputs; exit nonzero on any difference (no files written).

Both outputs are checked in; run this manually after editing core/AST.h.
"""

import os
import re
import sys

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
AST_H = os.path.join(REPO, "core", "AST.h")
ROX_PATH = os.path.join(REPO, "modules", "inspect.rox")
INC_PATH = os.path.join(REPO, "compiler", "InspectAstConv.inc")

BEGIN_MARK = "# ==== BEGIN GENERATED (tools/inspect-gen/generate.py) — do not edit by hand ===="
END_MARK = "# ==== END GENERATED ===="

# ---------------------------------------------------------------------------
# Field kinds
#
#   ustr             ustring                        -> string
#   opt_ustr         optional<ustring>              -> string | nil
#   str_list         vector<ustring>                -> list of string
#   bool             bool                           -> bool
#   bool_list        vector<bool>                   -> list of bool
#   num_variant      variant<int32,int64,double>    -> int | real
#   node             ptr<T>  (may be null)          -> node | nil
#   opt_node         optional<ptr<T>>               -> node | nil
#   node_list        vector<ptr<T>>                 -> list of node
#   decl_stmt_list   vector<variant<Decl,Stmt>>     -> list of node
#   body_variant     variant<Suite,Expression,mono> -> node | nil
#   accessor_variant optional<variant<Suite,Statement,mono>>
#                                                   -> node | nil (+ _abstract bool)
#   arg_list         vector<pair<ustring,ptr<Expression>>> -> list of Arg
#   cond_suite_list  vector<pair<Expr,Suite>>       -> list of [cond, suite]
#   case_list        vector<pair<vector<Expr>,Suite>> -> list of [[patterns], suite]
#   pair_expr_list   vector<pair<Expr,Expr>>        -> list of [key, value]
#   interp_parts     vector<StrInterpPart>          -> list of string | node
#   except_clauses   vector<ExceptClause>           -> list of ExceptClause
#   var_targets      vector<VarDecl::Target>        -> list of VarTarget
#   var_type         VarType                        -> string
#   opt_var_type     optional<VarType>              -> string | nil
#   opt_var_type_list optional<vector<VarType>>     -> list of string | nil
#   opt_type_name    optional<TypeName>             -> string | nil
#   type_name_list   vector<TypeName>               -> list of string
#   access           Access                         -> 'public' | 'private'
#   typedecl_kind    TypeDecl::Kind                 -> 'object' | 'actor' | ...
#   op_string        BinaryOp/UnaryOp/Assignment Op -> opString()
#   modifiers        MethodModifiers bitset         -> list of string
#   builtin_type     type::BuiltinType              -> string
# ---------------------------------------------------------------------------

def F(cpp, ctype, kind, rox=None, cls=None, extra=None):
    return dict(cpp=cpp, ctype=ctype, kind=kind,
                rox=rox or snake(cpp), cls=cls, extra=extra)

def snake(name):
    return re.sub(r"(?<!^)(?=[A-Z])", "_", name).lower()

# (ClassName, RoxBase, [fields], {options})
# Field order == declaration order in AST.h == children() order.
# 'skip' lists members that exist in the header but are deliberately not
# mirrored (discriminators and deduction-time metadata).
NODE_SPEC = [
    ("Declaration", "Node", [], {"skip": ["declType"], "abstract": True}),
    ("Statement", "Node", [], {"skip": ["stmtType"], "abstract": True}),
    ("Expression", "Node", [], {"skip": ["exprType"], "abstract": True}),

    ("File", "Node", [
        F("imports", "std::vector<ptr<Import>>", "node_list", cls="Import"),
        F("declsOrStmts", "std::vector<std::variant<ptr<Declaration>, ptr<Statement>>>", "decl_stmt_list"),
    ], {"extra_rox_fields": [("end_comments", "list of string — comments after the last statement", "[]")]}),

    ("SingleInput", "Node", [
        F("stmt", "ptr<Statement>", "node", cls="Statement"),
    ], {}),

    ("Annotation", "Node", [
        F("name", "ustring", "ustr"),
        F("args", "std::vector<ArgNameExpr>", "arg_list"),
    ], {}),

    ("Import", "Node", [
        F("packages", "std::vector<ustring>", "str_list"),
        F("symbols", "std::vector<ustring>", "str_list"),
    ], {}),

    ("Suite", "Statement", [
        F("declsOrStmts", "std::vector<std::variant<ptr<Declaration>, ptr<Statement>>>", "decl_stmt_list"),
    ], {}),

    ("ExpressionStatement", "Statement", [
        F("expr", "ptr<ast::Expression>", "node", cls="Expression"),
        F("atHost", "std::optional<ptr<ast::Expression>>", "opt_node", cls="Expression"),
    ], {}),

    ("ReturnStatement", "Statement", [
        F("expr", "std::optional<ptr<ast::Expression>>", "opt_node", cls="Expression"),
    ], {}),

    ("BreakStatement", "Statement", [], {}),
    ("ContinueStatement", "Statement", [], {}),

    ("JumpStatement", "Statement", [
        F("name", "ustring", "ustr"),
    ], {}),

    ("LabelStatement", "Statement", [
        F("name", "ustring", "ustr"),
    ], {}),

    ("IfStatement", "Statement", [
        F("conditionalSuites", "std::vector<std::pair<ptr<ast::Expression>, ptr<ast::Suite>>>", "cond_suite_list"),
        F("elseSuite", "std::optional<ptr<ast::Suite>>", "opt_node", cls="Suite"),
    ], {}),

    ("WhileStatement", "Statement", [
        F("condition", "ptr<ast::Expression>", "node", cls="Expression"),
        F("body", "ptr<ast::Suite>", "node", cls="Suite"),
    ], {}),

    ("ForStatement", "Statement", [
        F("targetList", "std::vector<ptr<VarDecl>>", "node_list", cls="VarDecl"),
        F("iterable", "ptr<ast::Expression>", "node", cls="Expression"),
        F("body", "ptr<ast::Suite>", "node", cls="Suite"),
    ], {}),

    ("WhenStatement", "Statement", [
        F("trigger", "ptr<ast::Expression>", "node", cls="Expression"),
        F("binding", "std::optional<ustring>", "opt_ustr"),
        # 'becomes' is a grammar keyword — property access n.becomes would not parse
        F("becomes", "std::optional<ptr<ast::Expression>>", "opt_node", cls="Expression",
          rox="becomes_expr"),
        F("targetFilter", "std::optional<ptr<ast::Expression>>", "opt_node", cls="Expression"),
        F("matchesBecomes", "bool", "bool"),
        F("requiresSignalChange", "bool", "bool"),
        F("body", "ptr<ast::Suite>", "node", cls="Suite"),
    ], {}),

    ("UntilStatement", "Statement", [
        F("stmt", "ptr<ast::Statement>", "node", cls="Statement"),
        F("condition", "ptr<ast::Expression>", "node", cls="Expression"),
    ], {}),

    ("AdheringIfStatement", "Statement", [
        F("stmt", "ptr<ast::Statement>", "node", cls="Statement"),
        F("condition", "ptr<ast::Expression>", "node", cls="Expression"),
    ], {}),

    ("RaiseStatement", "Statement", [
        F("exception", "std::optional<ptr<ast::Expression>>", "opt_node", cls="Expression"),
    ], {}),

    ("TryStatement", "Statement", [
        F("body", "ptr<ast::Suite>", "node", cls="Suite"),
        F("exceptClauses", "std::vector<ExceptClause>", "except_clauses"),
        F("finallySuite", "std::optional<ptr<ast::Suite>>", "opt_node", cls="Suite"),
    ], {}),

    ("MatchStatement", "Statement", [
        F("matchExpr", "ptr<ast::Expression>", "node", cls="Expression"),
        F("cases", "std::vector<std::pair<std::vector<ptr<ast::Expression>>, ptr<ast::Suite>>>", "case_list"),
        F("defaultCase", "std::optional<ptr<ast::Suite>>", "opt_node", cls="Suite"),
    ], {"skip": ["isEnumMatch", "isIntegralMatch", "hasRangeCase", "enumTypeId"]}),

    ("WithStatement", "Statement", [
        F("contextExpr", "ptr<ast::Expression>", "node", cls="Expression"),
        F("body", "ptr<ast::Suite>", "node", cls="Suite"),
    ], {"skip": ["contextKind", "contextType"]}),

    ("VarDecl", "Declaration", [
        F("name", "ustring", "ustr"),
        F("initializer", "std::optional<ptr<Expression>>", "opt_node", cls="Expression"),
        F("varType", "std::optional<VarType>", "opt_var_type"),
        F("access", "Access", "access"),
        F("isConst", "bool", "bool"),
        F("isTypeConst", "bool", "bool"),
        F("isTypeMutable", "bool", "bool"),
        F("atHost", "std::optional<ptr<Expression>>", "opt_node", cls="Expression"),
        F("targets", "std::vector<Target>", "var_targets"),
    ], {}),

    ("PropertyAccessor", "Node", [
        F("name", "ustring", "ustr"),
        F("propType", "VarType", "var_type"),
        F("initializer", "std::optional<ptr<Expression>>", "opt_node", cls="Expression"),
        F("access", "Access", "access"),
        F("isConst", "bool", "bool"),
        F("getter", "std::optional<std::variant<ptr<Suite>, ptr<Statement>, std::monostate>>",
          "accessor_variant", extra="getter_abstract"),
        F("setter", "std::optional<std::variant<ptr<Suite>, ptr<Statement>, std::monostate>>",
          "accessor_variant", extra="setter_abstract"),
    ], {}),

    ("FuncDecl", "Declaration", [
        # 'func' is a grammar keyword — property access n.func would not parse
        F("func", "ptr<Function>", "node", cls="Function", rox="function"),
    ], {}),

    ("Function", "Node", [
        F("isProc", "bool", "bool"),
        F("name", "std::optional<ustring>", "opt_ustr"),
        F("params", "std::vector<ptr<Parameter>>", "node_list", cls="Parameter"),
        F("returnTypes", "std::optional<std::vector<VarType>>", "opt_var_type_list"),
        F("returnTypeConst", "std::vector<bool>", "bool_list"),
        F("body", "std::variant<ptr<Suite>, ptr<Expression>, std::monostate>", "body_variant"),
        F("access", "Access", "access"),
        F("methodModifiers", "MethodModifiers", "modifiers", rox="modifiers"),
    ], {}),

    ("Parameter", "Node", [
        F("name", "ustring", "ustr"),
        F("type", "std::optional<VarType>", "opt_var_type", rox="param_type"),
        F("defaultValue", "std::optional<ptr<Expression>>", "opt_node", cls="Expression"),
        F("variadic", "bool", "bool"),
        F("isConst", "bool", "bool"),
        F("isMutable", "bool", "bool"),
        F("isStar", "bool", "bool"),
    ], {}),

    ("TypeDecl", "Declaration", [
        F("kind", "Kind", "typedecl_kind", rox="type_kind"),
        F("name", "ustring", "ustr"),
        F("access", "Access", "access"),
        F("extends", "std::optional<TypeName>", "opt_type_name", rox="extends_type"),
        F("implements", "std::vector<TypeName>", "type_name_list", rox="implements_types"),
        F("methods", "std::vector<ptr<Function>>", "node_list", cls="Function"),
        F("properties", "std::vector<ptr<VarDecl>>", "node_list", cls="VarDecl"),
        F("propertyAccessors", "std::vector<ptr<PropertyAccessor>>", "node_list", cls="PropertyAccessor"),
        F("enumLabels", "std::vector<std::pair<ustring, ptr<Expression>>>", "arg_list"),
        F("nestedTypes", "std::vector<ptr<TypeDecl>>", "node_list", cls="TypeDecl"),
    ], {}),

    ("Assignment", "Expression", [
        F("op", "Op", "op_string"),
        F("lhs", "ptr<Expression>", "node", cls="Expression"),
        F("rhs", "ptr<Expression>", "node", cls="Expression"),
        F("atHost", "std::optional<ptr<Expression>>", "opt_node", cls="Expression"),
    ], {}),

    ("BinaryOp", "Expression", [
        F("op", "Op", "op_string"),
        F("lhs", "ptr<Expression>", "node", cls="Expression"),
        F("rhs", "ptr<Expression>", "node", cls="Expression"),
    ], {}),

    ("UnaryOp", "Expression", [
        F("op", "Op", "op_string"),
        F("member", "std::optional<ustring>", "opt_ustr"),
        F("arg", "ptr<Expression>", "node", cls="Expression"),
    ], {}),

    ("Variable", "Expression", [
        F("name", "ustring", "ustr"),
    ], {}),

    ("Call", "Expression", [
        F("callable", "ptr<Expression>", "node", cls="Expression"),
        F("args", "std::vector<ArgNameExpr>", "arg_list"),
    ], {}),

    ("Range", "Expression", [
        F("start", "ptr<Expression>", "node", cls="Expression"),
        F("stop", "ptr<Expression>", "node", cls="Expression"),
        F("step", "ptr<Expression>", "node", cls="Expression"),
        F("closed", "bool", "bool"),
    ], {}),

    ("Index", "Expression", [
        F("indexable", "ptr<Expression>", "node", cls="Expression"),
        F("args", "std::vector<ptr<Expression>>", "node_list", cls="Expression"),
    ], {}),

    ("LambdaFunc", "Expression", [
        # 'func' is a grammar keyword — property access n.func would not parse
        F("func", "ptr<Function>", "node", cls="Function", rox="function"),
    ], {}),

    ("Literal", "Expression", [], {"skip": ["literalType"]}),

    ("Bool", "Literal", [
        F("value", "bool", "bool"),
    ], {}),

    ("Num", "Literal", [
        F("num", "std::variant<int32_t,int64_t,double>", "num_variant", rox="value"),
    ], {}),

    ("Str", "Literal", [
        F("str", "ustring", "ustr", rox="value"),
    ], {}),

    ("StrInterp", "Literal", [
        F("parts", "std::vector<StrInterpPart>", "interp_parts"),
        F("suffix", "ustring", "ustr"),
    ], {}),

    ("SuffixedNum", "Literal", [
        F("num", "std::variant<int32_t,int64_t,double>", "num_variant", rox="value"),
        F("suffix", "ustring", "ustr"),
    ], {}),

    ("SuffixedStr", "Literal", [
        F("str", "ustring", "ustr", rox="value"),
        F("suffix", "ustring", "ustr"),
    ], {}),

    ("Type", "Literal", [
        F("t", "BuiltinType", "builtin_type", rox="value"),
    ], {}),

    ("List", "Literal", [
        F("elements", "std::vector<ptr<Expression>>", "node_list", cls="Expression"),
    ], {}),

    ("Vector", "Literal", [
        F("elements", "std::vector<ptr<ast::Expression>>", "node_list", cls="Expression"),
    ], {}),

    ("Matrix", "Literal", [
        F("rows", "std::vector<ptr<ast::Vector>>", "node_list", cls="Vector"),
    ], {}),

    ("Dict", "Literal", [
        F("entries", "std::vector<std::pair<ptr<Expression>,ptr<Expression>>>", "pair_expr_list"),
    ], {}),
]

# The nested TryStatement::ExceptClause struct, verified separately.
EXCEPT_CLAUSE_FIELDS = [
    ("type", "std::optional<ptr<ast::Expression>>"),
    ("name", "std::optional<ustring>"),
    ("body", "ptr<ast::Suite>"),
]

# The nested VarDecl::Target struct ('var [a, b] = ...'), verified separately.
VAR_TARGET_FIELDS = [
    ("name", "ustring"),
    ("varType", "std::optional<VarType>"),
    ("isTypeConst", "bool"),
    ("isTypeMutable", "bool"),
]

CONCRETE = [c for (c, b, f, o) in NODE_SPEC if not o.get("abstract")]


# ---------------------------------------------------------------------------
# Verifier: structural re-parse of AST.h, diffed against NODE_SPEC.
# ---------------------------------------------------------------------------

def normalize_type(t):
    t = re.sub(r"\s+", "", t)
    t = t.replace("ast::", "")
    return t


def parse_header(path):
    """Return {structName: (base, [(member, ctype)])} for depth-1 data members."""
    src = open(path, encoding="utf-8").read()
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    src = re.sub(r"//[^\n]*", "", src)

    structs = {}
    for m in re.finditer(r"\bstruct\s+(\w+)\s*(?::\s*public\s+([\w:<>]+))?\s*\{", src):
        name, base = m.group(1), m.group(2)
        # walk to the matching closing brace
        depth = 1
        i = m.end()
        body_start = i
        while i < len(src) and depth > 0:
            if src[i] == "{":
                depth += 1
            elif src[i] == "}":
                depth -= 1
            i += 1
        body = src[body_start:i - 1]
        members = extract_members(body)
        structs[name] = (base, members)
    return structs


def extract_members(body):
    """Depth-1 simple data members of a struct body."""
    members = []
    depth = 0
    # split into statements at depth 0 (top level of the struct body)
    stmt = ""
    for ch in body:
        if ch == "{":
            depth += 1
            stmt += ch
        elif ch == "}":
            depth -= 1
            stmt += ch
            # a ctor/method body ends at its closing brace without a ';' —
            # flush it so it doesn't swallow the next member.  Member
            # default-init braces ("Access access { Access::Public };") have
            # no parens and keep accumulating until their ';'.
            if depth == 0 and "(" in stmt:
                stmt = ""
        elif ch == ";" and depth == 0:
            handle_stmt(stmt, members)
            stmt = ""
        else:
            stmt += ch
    return members


def handle_stmt(stmt, members):
    s = " ".join(stmt.split())
    if not s:
        return
    first = s.split()[0]
    if first in ("virtual", "static", "typedef", "using", "friend", "enum",
                 "struct", "class", "public:", "private:", "protected:"):
        return
    # drop default-member-init and in-class bodies
    s = re.sub(r"\{[^{}]*\}$", "", s).strip()
    s = re.sub(r"=\s*[^=]+$", "", s).strip()
    if "(" in s or ")" in s or "~" in s or not s:
        return  # function decl / ctor / dtor
    mm = re.match(r"^(.*?)\s*(\w+)$", s)
    if not mm:
        return
    ctype, name = mm.group(1), mm.group(2)
    if not ctype:
        return
    members.append((name, ctype))


AST_BASE_MEMBERS = {"source", "interval", "fullSource", "type", "annotations", "attrs"}
GLOBAL_SKIP = {"declType", "stmtType", "exprType", "literalType"}


def verify(structs):
    errors = []
    spec_by_name = {c: (b, f, o) for (c, b, f, o) in NODE_SPEC}

    header_nodes = {}
    for name, (base, members) in structs.items():
        if name in ("AST", "LinePos", "StrInterpPart", "ExceptClause", "Target"):
            continue
        header_nodes[name] = (base, members)

    for name in spec_by_name:
        if name not in header_nodes:
            errors.append(f"spec class {name} not found in AST.h")
    for name in header_nodes:
        if name not in spec_by_name:
            errors.append(f"AST.h struct {name} missing from NODE_SPEC")

    for name, (base, members) in header_nodes.items():
        if name not in spec_by_name:
            continue
        _, fields, opts = spec_by_name[name]
        skips = set(opts.get("skip", [])) | GLOBAL_SKIP
        spec_fields = {f["cpp"]: f for f in fields}
        for (mname, mtype) in members:
            if mname in AST_BASE_MEMBERS or mname in skips:
                continue
            if mname not in spec_fields:
                errors.append(f"{name}::{mname} ({mtype}) not in NODE_SPEC — header drift")
            elif normalize_type(mtype) != normalize_type(spec_fields[mname]["ctype"]):
                errors.append(f"{name}::{mname} type changed: header '{mtype}' vs spec "
                              f"'{spec_fields[mname]['ctype']}'")
        header_names = {mname for (mname, _) in members}
        for fname in spec_fields:
            if fname not in header_names:
                errors.append(f"NODE_SPEC field {name}::{fname} no longer in AST.h")

    # nested ExceptClause
    if "ExceptClause" in structs:
        _, ec_members = structs["ExceptClause"]
        got = [(n, normalize_type(t)) for (n, t) in ec_members]
        want = [(n, normalize_type(t)) for (n, t) in EXCEPT_CLAUSE_FIELDS]
        if got != want:
            errors.append(f"TryStatement::ExceptClause drifted: header {got} vs spec {want}")
    else:
        errors.append("TryStatement::ExceptClause not found in AST.h")

    # nested VarDecl::Target
    if "Target" in structs:
        _, vt_members = structs["Target"]
        got = [(n, normalize_type(t)) for (n, t) in vt_members]
        want = [(n, normalize_type(t)) for (n, t) in VAR_TARGET_FIELDS]
        if got != want:
            errors.append(f"VarDecl::Target drifted: header {got} vs spec {want}")
    else:
        errors.append("VarDecl::Target not found in AST.h")

    if errors:
        sys.stderr.write("core/AST.h does not match NODE_SPEC:\n")
        for e in errors:
            sys.stderr.write("  " + e + "\n")
        sys.stderr.write("Update NODE_SPEC in tools/inspect-gen/generate.py accordingly.\n")
        sys.exit(1)


# ---------------------------------------------------------------------------
# Roxal mirror-class emitter
# ---------------------------------------------------------------------------

ROX_DEFAULTS = {
    "ustr": ("var {n}:string = ''", "string"),
    "opt_ustr": ("var {n}", "string"),
    "str_list": ("var {n} = []", "list(string)"),
    "bool": ("var {n}:bool = false", "bool"),
    "bool_list": ("var {n} = []", "list(bool)"),
    # default nil, NOT 0: property assignment is change-suppressed on numeric
    # equality, so a real 0.0 written over an initial int 0 would silently
    # keep the int (drifting 0.0 literals to 0 through a round trip)
    "num_variant": ("var {n}", "value"),
    "node": ("var {n}", "node"),
    "opt_node": ("var {n}", "node"),
    "node_list": ("var {n} = []", "list(node)"),
    "decl_stmt_list": ("var {n} = []", "list(node)"),
    "body_variant": ("var {n}", "node"),
    "accessor_variant": ("var {n}", "node"),
    "arg_list": ("var {n} = []", "list(arg)"),
    "cond_suite_list": ("var {n} = []", "list(pair)"),
    "case_list": ("var {n} = []", "list(case)"),
    "pair_expr_list": ("var {n} = []", "list(pair)"),
    "interp_parts": ("var {n} = []", "list(part)"),
    "except_clauses": ("var {n} = []", "list(except_clause)"),
    "var_targets": ("var {n} = []", "list(var_target)"),
    "var_type": ("var {n}:string = ''", "string"),
    "opt_var_type": ("var {n}", "string"),
    "opt_var_type_list": ("var {n}", "list(string)"),
    "opt_type_name": ("var {n}", "string"),
    "type_name_list": ("var {n} = []", "list(string)"),
    "access": ("var {n}:string = 'public'", "string"),
    "typedecl_kind": ("var {n}:string = 'object'", "string"),
    "op_string": ("var {n}:string = ''", "string"),
    "modifiers": ("var {n} = []", "list(string)"),
    "builtin_type": ("var {n}:string = ''", "string"),
}


def emit_rox():
    out = []
    w = out.append
    w(BEGIN_MARK)
    w("")
    for (cls, base, fields, opts) in NODE_SPEC:
        w(f"type {cls} object extends {base}:")
        body_lines = []
        for f in fields:
            decl, _ = ROX_DEFAULTS[f["kind"]]
            body_lines.append("  " + decl.format(n=f["rox"]))
            if f["kind"] == "accessor_variant":
                body_lines.append(f"  var {f['extra']}:bool = false")
        for (name, doc, default) in opts.get("extra_rox_fields", []):
            body_lines.append(f"  var {name} = {default}   # {doc}")
        body_lines.append("")
        body_lines.append("  proc init(*):")
        body_lines.append(f'    """construct a {cls} node, properties as named arguments"""')
        body_lines.append("")
        body_lines.append("  func kind() -> string:")
        body_lines.append(f"    return '{cls}'")
        child_lines = emit_rox_children(fields)
        body_lines.append("")
        body_lines.append("  func children() -> list:")
        body_lines.extend("  " + l for l in child_lines)
        out.extend(body_lines)
        w("")
        w("")
    emit_rox_schema(out)
    w(END_MARK)
    return "\n".join(out) + "\n"


def emit_rox_children(fields):
    ls = []
    ls.append("var cs = []")
    ls.append("for a in annotations:")
    ls.append("  cs.append(a)")
    for f in fields:
        n, k = f["rox"], f["kind"]
        if k in ("node", "opt_node", "body_variant", "accessor_variant"):
            ls.append(f"if {n} != nil:")
            ls.append(f"  cs.append({n})")
        elif k in ("node_list", "decl_stmt_list", "arg_list", "except_clauses"):
            ls.append(f"for x in {n}:")
            ls.append("  cs.append(x)")
        elif k in ("cond_suite_list", "pair_expr_list"):
            ls.append(f"for p in {n}:")
            ls.append("  cs.append(p[0])")
            ls.append("  cs.append(p[1])")
        elif k == "case_list":
            ls.append(f"for c in {n}:")
            ls.append("  for pat in c[0]:")
            ls.append("    cs.append(pat)")
            ls.append("  cs.append(c[1])")
        elif k == "interp_parts":
            ls.append(f"for p in {n}:")
            ls.append("  if not (p is string):")
            ls.append("    cs.append(p)")
    ls.append("return cs")
    return ["  " + l for l in ls]


def emit_rox_schema(out):
    w = out.append
    w("# Machine-readable node schemas for structural editors:")
    w("#   node_fields(kind) -> list of {'name':..., 'kind':..., 'class':...}")
    w("var _node_fields = {}")
    for (cls, base, fields, opts) in NODE_SPEC:
        entries = []
        for f in fields:
            _, sk = ROX_DEFAULTS[f["kind"]]
            e = f"{{'name': '{f['rox']}', 'kind': '{sk}'"
            if f["cls"]:
                e += f", 'class': '{f['cls']}'"
            e += "}"
            entries.append(e)
            if f["kind"] == "accessor_variant":
                entries.append(f"{{'name': '{f['extra']}', 'kind': 'bool'}}")
        for (name, doc, default) in opts.get("extra_rox_fields", []):
            entries.append(f"{{'name': '{name}', 'kind': 'list(string)'}}")
        w(f"_node_fields['{cls}'] = [" + ", ".join(entries) + "]")
    w("")
    w("func node_fields(kind:string) -> list:")
    w('  """field schema for the given node kind: name, kind, and node class where applicable"""')
    w("  return _node_fields[kind]")
    w("")


# ---------------------------------------------------------------------------
# C++ converter emitter (InspectAstConv.inc)
# ---------------------------------------------------------------------------

CPP_SETTERS = {
    "ustr": 'cx.setUStr(v, "{r}", n.{c});',
    "opt_ustr": 'cx.setOptUStr(v, "{r}", n.{c});',
    "str_list": 'cx.setUStrList(v, "{r}", n.{c});',
    "bool": 'cx.setBool(v, "{r}", n.{c});',
    "bool_list": 'cx.setBoolList(v, "{r}", n.{c});',
    "num_variant": 'cx.setNumVariant(v, "{r}", n.{c});',
    "node": 'cx.setNode(v, "{r}", n.{c});',
    "opt_node": 'cx.setOptNode(v, "{r}", n.{c});',
    "node_list": 'cx.setNodeList(v, "{r}", n.{c});',
    "decl_stmt_list": 'cx.setDeclStmtList(v, "{r}", n.{c});',
    "body_variant": 'cx.setBodyVariant(v, "{r}", n.{c});',
    "accessor_variant": 'cx.setAccessorVariant(v, "{r}", "{x}", n.{c});',
    "arg_list": 'cx.setArgList(v, "{r}", n.{c});',
    "cond_suite_list": 'cx.setCondSuiteList(v, "{r}", n.{c});',
    "case_list": 'cx.setCaseList(v, "{r}", n.{c});',
    "pair_expr_list": 'cx.setPairExprList(v, "{r}", n.{c});',
    "interp_parts": 'cx.setInterpParts(v, "{r}", n.{c});',
    "except_clauses": 'cx.setExceptClauses(v, "{r}", n.{c});',
    "var_targets": 'cx.setVarTargets(v, "{r}", n.{c});',
    "var_type": 'cx.setVarType(v, "{r}", n.{c});',
    "opt_var_type": 'cx.setOptVarType(v, "{r}", n.{c});',
    "opt_var_type_list": 'cx.setOptVarTypeList(v, "{r}", n.{c});',
    "opt_type_name": 'cx.setOptTypeName(v, "{r}", n.{c});',
    "type_name_list": 'cx.setTypeNameList(v, "{r}", n.{c});',
    "access": 'cx.setAccess(v, "{r}", n.{c});',
    "typedecl_kind": 'cx.setTypeDeclKind(v, "{r}", n.{c});',
    "op_string": 'cx.setStr(v, "{r}", n.opString());',
    "modifiers": 'cx.setModifiers(v, "{r}", n.{c});',
    "builtin_type": 'cx.setStr(v, "{r}", roxal::type::to_string(n.{c}));',
}

# Reverse builders (Roxal mirror -> C++ AST) for unparse/compile.
# Each returns the field-assignment lines for buildXxx(); UnaryOp's op goes in
# the constructor (no default ctor), so its op_string line is skipped there.
CPP_BUILDERS = {
    "ustr": 'n->{c} = bx.getUStr(inst, "{r}");',
    "opt_ustr": 'n->{c} = bx.getOptUStr(inst, "{r}");',
    "str_list": 'n->{c} = bx.getUStrList(inst, "{r}");',
    "bool": 'n->{c} = bx.getBool(inst, "{r}");',
    "bool_list": 'n->{c} = bx.getBoolList(inst, "{r}");',
    "num_variant": 'n->{c} = bx.getNumVariant(inst, "{r}");',
    "node": 'n->{c} = bx.getNodeAs<ast::{cls}>(inst, "{r}");',
    "opt_node": 'n->{c} = bx.getOptNodeAs<ast::{cls}>(inst, "{r}");',
    "node_list": 'n->{c} = bx.getNodeListAs<ast::{cls}>(inst, "{r}");',
    "decl_stmt_list": 'n->{c} = bx.getDeclStmtList(inst, "{r}");',
    "body_variant": 'n->{c} = bx.getBodyVariant(inst, "{r}");',
    "accessor_variant": 'n->{c} = bx.getAccessorVariant(inst, "{r}", "{x}");',
    "arg_list": 'n->{c} = bx.getArgList(inst, "{r}");',
    "cond_suite_list": 'n->{c} = bx.getCondSuiteList(inst, "{r}");',
    "case_list": 'n->{c} = bx.getCaseList(inst, "{r}");',
    "pair_expr_list": 'n->{c} = bx.getPairExprList(inst, "{r}");',
    "interp_parts": 'n->{c} = bx.getInterpParts(inst, "{r}");',
    "except_clauses": 'n->{c} = bx.getExceptClauses(inst, "{r}");',
    "var_targets": 'n->{c} = bx.getVarTargets(inst, "{r}");',
    "var_type": 'n->{c} = bx.getVarType(inst, "{r}");',
    "opt_var_type": 'n->{c} = bx.getOptVarType(inst, "{r}");',
    "opt_var_type_list": 'n->{c} = bx.getOptVarTypeList(inst, "{r}");',
    "opt_type_name": 'n->{c} = bx.getOptTypeName(inst, "{r}");',
    "type_name_list": 'n->{c} = bx.getTypeNameList(inst, "{r}");',
    "access": 'n->{c} = bx.getAccess(inst, "{r}");',
    "typedecl_kind": 'n->{c} = bx.getTypeDeclKind(inst, "{r}");',
    "op_string": None,   # per-class, see emit_builder
    "modifiers": 'n->{c} = bx.getModifiers(inst, "{r}");',
    "builtin_type": 'n->{c} = bx.getBuiltinType(inst, "{r}");',
}

OP_FROM_STR = {
    "BinaryOp": "binOpFromStr",
    "UnaryOp": "unOpFromStr",
    "Assignment": "assignOpFromStr",
}


def emit_builder(w, cls, fields, opts):
    w(f"static ptr<ast::{cls}> build{cls}(InspectBuild& bx, ObjectInstance* inst)")
    w("{")
    if cls == "UnaryOp":
        w('    auto n = roxal::make_ptr<ast::UnaryOp>(bx.unOpFromStr(bx.getStr(inst, "op")));')
    else:
        w(f"    auto n = roxal::make_ptr<ast::{cls}>();")
    for f in fields:
        if f["kind"] == "op_string":
            if cls == "UnaryOp":
                continue    # already in the constructor
            w(f'    n->{f["cpp"]} = bx.{OP_FROM_STR[cls]}(bx.getStr(inst, "{f["rox"]}"));')
            continue
        line = CPP_BUILDERS[f["kind"]].format(c=f["cpp"], r=f["rox"],
                                              cls=f["cls"] or "", x=f.get("extra") or "")
        w("    " + line)
    for (name, doc, default) in opts.get("extra_rox_fields", []):
        w(f'    bx.putStrListAttr(*n, inst, "{name}");')
    w("    return n;")
    w("}")
    w("")


# forEachChildOf emission per kind (child enumeration over the C++ tree)
def cpp_children(fields):
    ls = []
    for f in fields:
        c, k = f["cpp"], f["kind"]
        if k == "node":
            ls.append(f"if (n.{c}) f(n.{c}.get());")
        elif k == "opt_node":
            ls.append(f"if (n.{c}.has_value() && n.{c}.value()) f(n.{c}.value().get());")
        elif k == "node_list":
            ls.append(f"for (auto& e : n.{c}) if (e) f(e.get());")
        elif k == "decl_stmt_list":
            ls.append(f"for (auto& e : n.{c}) {{ if (std::holds_alternative<ptr<ast::Declaration>>(e)) {{ auto& p = std::get<ptr<ast::Declaration>>(e); if (p) f(p.get()); }} else {{ auto& p = std::get<ptr<ast::Statement>>(e); if (p) f(p.get()); }} }}")
        elif k == "body_variant":
            ls.append(f"if (std::holds_alternative<ptr<ast::Suite>>(n.{c})) {{ auto& p = std::get<ptr<ast::Suite>>(n.{c}); if (p) f(p.get()); }} else if (std::holds_alternative<ptr<ast::Expression>>(n.{c})) {{ auto& p = std::get<ptr<ast::Expression>>(n.{c}); if (p) f(p.get()); }}")
        elif k == "accessor_variant":
            ls.append(f"if (n.{c}.has_value()) {{ auto& va = n.{c}.value(); if (std::holds_alternative<ptr<ast::Suite>>(va)) {{ auto& p = std::get<ptr<ast::Suite>>(va); if (p) f(p.get()); }} else if (std::holds_alternative<ptr<ast::Statement>>(va)) {{ auto& p = std::get<ptr<ast::Statement>>(va); if (p) f(p.get()); }} }}")
        elif k == "arg_list":
            ls.append(f"for (auto& e : n.{c}) if (e.second) f(e.second.get());")
        elif k == "cond_suite_list":
            ls.append(f"for (auto& e : n.{c}) {{ if (e.first) f(e.first.get()); if (e.second) f(e.second.get()); }}")
        elif k == "case_list":
            ls.append(f"for (auto& e : n.{c}) {{ for (auto& p : e.first) if (p) f(p.get()); if (e.second) f(e.second.get()); }}")
        elif k == "pair_expr_list":
            ls.append(f"for (auto& e : n.{c}) {{ if (e.first) f(e.first.get()); if (e.second) f(e.second.get()); }}")
        elif k == "interp_parts":
            ls.append(f"for (auto& p : n.{c}) if (p.expr) f(p.expr.get());")
        elif k == "except_clauses":
            ls.append(f"for (auto& e : n.{c}) {{ if (e.type.has_value() && e.type.value()) f(e.type.value().get()); if (e.body) f(e.body.get()); }}")
    return ls


def emit_inc():
    out = []
    w = out.append
    w("// GENERATED by tools/inspect-gen/generate.py — do not edit by hand.")
    w("// C++ AST -> Roxal mirror-object converters for the inspect module.")
    w("// Regenerate after changing core/AST.h:  python3 tools/inspect-gen/generate.py")
    w("")
    for cls in CONCRETE:
        w(f"static Value conv{cls}(InspectConv& cx, const ast::{cls}& n);")
    w("")
    for (cls, base, fields, opts) in NODE_SPEC:
        if opts.get("abstract"):
            continue
        w(f"static Value conv{cls}(InspectConv& cx, const ast::{cls}& n)")
        w("{")
        w(f'    Value v = cx.newNode("{cls}", n);')
        for f in fields:
            line = CPP_SETTERS[f["kind"]].format(r=f["rox"], c=f["cpp"], x=f.get("extra") or "")
            w("    " + line)
        w("    return v;")
        w("}")
        w("")
    # dispatch
    w("Value InspectConv::node(const ast::AST* n)")
    w("{")
    w("    if (!n) return Value::nilVal();")
    w("    auto it = mirrorOf.find(n);")
    w("    if (it != mirrorOf.end()) return it->second;")
    w("    const std::type_info& t = typeid(*n);")
    for cls in CONCRETE:
        w(f"    if (t == typeid(ast::{cls})) return conv{cls}(*this, static_cast<const ast::{cls}&>(*n));")
    w('    throw std::runtime_error(std::string("inspect: unmapped AST node type: ") + t.name());')
    w("}")
    w("")
    # reverse builders (Roxal mirror -> C++ AST)
    for (cls, base, fields, opts) in NODE_SPEC:
        if opts.get("abstract"):
            continue
        emit_builder(w, cls, fields, opts)
    w("ptr<ast::AST> InspectBuild::build(const Value& v)")
    w("{")
    w("    if (v.isNil()) return nullptr;")
    w("    if (!isObjectInstance(v))")
    w('        throw std::runtime_error("inspect: expected an AST mirror node, got " + v.typeName());')
    w("    ObjectInstance* inst = asObjectInstance(v);")
    w("    auto mit = memo.find(inst);")
    w("    if (mit != memo.end()) return mit->second;")
    w("    std::string kind = toUTF8StdString(asObjectType(inst->instanceType)->name);")
    for cls in CONCRETE:
        w(f'    if (kind == "{cls}") return memo[inst] = finish(build{cls}(*this, inst), inst);')
    w('    throw std::runtime_error("inspect: \'" + kind + "\' is not a standalone AST node kind");')
    w("}")
    w("")
    # forEachChildOf
    w("void forEachChildOf(const ast::AST& node, const std::function<void(const ast::AST*)>& f)")
    w("{")
    w("    for (auto& a : node.annotations) if (a) f(a.get());")
    w("    const std::type_info& t = typeid(node);")
    for (cls, base, fields, opts) in NODE_SPEC:
        if opts.get("abstract"):
            continue
        body = cpp_children(fields)
        if not body:
            continue
        w(f"    if (t == typeid(ast::{cls})) {{")
        w(f"        auto& n = static_cast<const ast::{cls}&>(node); (void)n;")
        for l in body:
            w("        " + l)
        w("        return;")
        w("    }")
    w("}")
    w("")
    return "\n".join(out) + "\n"


# ---------------------------------------------------------------------------

def splice_rox(existing, generated):
    b = existing.find(BEGIN_MARK)
    e = existing.find(END_MARK)
    if b < 0 or e < 0:
        sys.stderr.write(f"markers not found in {ROX_PATH}\n")
        sys.exit(1)
    return existing[:b] + generated + existing[e + len(END_MARK):].lstrip("\n")


def main():
    check = "--check" in sys.argv
    structs = parse_header(AST_H)
    verify(structs)

    rox_gen = emit_rox()
    inc_gen = emit_inc()

    existing_rox = open(ROX_PATH, encoding="utf-8").read()
    new_rox = splice_rox(existing_rox, rox_gen)

    if check:
        ok = True
        if new_rox != existing_rox:
            sys.stderr.write(f"{ROX_PATH} is out of date\n")
            ok = False
        if not os.path.exists(INC_PATH) or open(INC_PATH, encoding="utf-8").read() != inc_gen:
            sys.stderr.write(f"{INC_PATH} is out of date\n")
            ok = False
        sys.exit(0 if ok else 1)

    open(ROX_PATH, "w", encoding="utf-8").write(new_rox)
    open(INC_PATH, "w", encoding="utf-8").write(inc_gen)
    print(f"wrote {ROX_PATH} (generated section) and {INC_PATH}")
    print(f"{len(CONCRETE)} concrete node classes, "
          f"{sum(len(f) for (_, _, f, _) in NODE_SPEC)} fields")


if __name__ == "__main__":
    main()
