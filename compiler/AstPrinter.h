#pragma once

#include <core/AST.h>
#include <string>

namespace roxal {

// Renders an AST (sub)tree back to canonical Roxal source text.
//
// Deterministic pretty-printer: 2-space indentation, ASCII operator tokens,
// single-quoted plain strings, conservative parenthesization (extra parens
// are structurally invisible to the parser).  Comment decorations are
// re-emitted from the AST's attrs extension map when present (the inspect
// module's Roxal->C++ converter stores them there):
//   "leading_comments"   std::vector<std::string>
//   "trailing_comment"   std::string
//   "blank_lines_before" int64_t
//   "end_comments"       std::vector<std::string>   (File only)
//
// parse(print(tree)) is structurally equal to tree; byte-exact text
// round-trip is a non-goal (formatting is normalized).
class AstPrinter {
public:
    // Render a whole File (statements, one per line) or any single node.
    // Expression nodes render as expression text without a trailing newline.
    std::string print(const ast::AST& node);

private:
    std::string out;
    int indent = 0;

    void emitLine(const std::string& s);
    std::string ind() const { return std::string(size_t(indent) * 2, ' '); }

    void node(const ast::AST& n);                 // statement/declaration level
    void stmtWithTrivia(const ast::AST& n);
    void body(const ast::Suite& s);               // suite at indent+1
    void declsOrStmts(const std::vector<std::variant<ptr<ast::Declaration>, ptr<ast::Statement>>>& ds);

    void file(const ast::File& n);
    void importDecl(const ast::Import& n);
    void varDecl(const ast::VarDecl& n, bool asStatement);
    void typeDecl(const ast::TypeDecl& n);
    void function(const ast::Function& n);
    void propertyAccessor(const ast::PropertyAccessor& n);
    void annotations(const ast::AST& n);

    std::string funcSignature(const ast::Function& n);
    std::string parameter(const ast::Parameter& n);
    std::string inlineStmt(const ast::Statement& n);  // single-line statement text

    // expression rendering; parentPrec/isRight drive parenthesization
    std::string expr(const ast::Expression& e, int parentPrec = 0, bool isRight = false);
    std::string exprTop(const ast::Expression& e) { return expr(e, 0, false); }
    std::string args(const std::vector<ast::ArgNameExpr>& a);
    std::string rangeText(const ast::Range& r, bool bare);
    std::string varTypeText(const ast::VarType& vt);
    std::string strLiteral(const ustring& s);
    std::string numText(const std::variant<int32_t, int64_t, double>& num);
};

} // namespace roxal
