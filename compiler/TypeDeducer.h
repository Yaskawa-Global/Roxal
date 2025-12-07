#pragma once

#include <cassert>

#include <core/AST.h>
#include <core/types.h>

namespace roxal {


struct VarInfo {
    ptr<type::Type> type;
    bool explicitType { false };
};

struct ScopeInfo {
    bool strict { false };
    std::map<icu::UnicodeString, VarInfo> symbols;
};

class TypeDeducer : public ast::ASTVisitor
{
public:
    TypeDeducer() {}

    void setReplMode(bool repl) { replMode = repl; }

    virtual TraversalOrder traversalOrder() const;

    virtual std::any visit(ptr<ast::File> ast);
    virtual std::any visit(ptr<ast::SingleInput> ast);
    virtual std::any visit(ptr<ast::Annotation> ast);
    virtual std::any visit(ptr<ast::Import> ast);
    virtual std::any visit(ptr<ast::TypeDecl> ast);
    virtual std::any visit(ptr<ast::FuncDecl> ast);
    virtual std::any visit(ptr<ast::VarDecl> ast);
    virtual std::any visit(ptr<ast::Suite> ast);
    virtual std::any visit(ptr<ast::ExpressionStatement> ast);
    virtual std::any visit(ptr<ast::ReturnStatement> ast);
    virtual std::any visit(ptr<ast::IfStatement> ast);
    virtual std::any visit(ptr<ast::WhileStatement> ast);
    virtual std::any visit(ptr<ast::ForStatement> ast);
    virtual std::any visit(ptr<ast::WhenStatement> ast);
    virtual std::any visit(ptr<ast::UntilStatement> ast);
    virtual std::any visit(ptr<ast::TryStatement> ast);
    virtual std::any visit(ptr<ast::MatchStatement> ast);
    virtual std::any visit(ptr<ast::WithStatement> ast);
    virtual std::any visit(ptr<ast::RaiseStatement> ast);
    virtual std::any visit(ptr<ast::Function> ast);
    virtual std::any visit(ptr<ast::Parameter> ast);
    virtual std::any visit(ptr<ast::Assignment> ast);
    virtual std::any visit(ptr<ast::BinaryOp> ast);
    virtual std::any visit(ptr<ast::UnaryOp> ast);
    virtual std::any visit(ptr<ast::Variable> ast);
    virtual std::any visit(ptr<ast::Call> ast);
    virtual std::any visit(ptr<ast::Range> ast);
    virtual std::any visit(ptr<ast::Index> ast);
    virtual std::any visit(ptr<ast::LambdaFunc> ast);
    virtual std::any visit(ptr<ast::Literal> ast);
    virtual std::any visit(ptr<ast::Bool> ast);
    virtual std::any visit(ptr<ast::Str> ast);
    virtual std::any visit(ptr<ast::Type> ast);
    virtual std::any visit(ptr<ast::Num> ast);
    virtual std::any visit(ptr<ast::List> ast);
    virtual std::any visit(ptr<ast::Vector> ast);
    virtual std::any visit(ptr<ast::Matrix> ast);
    virtual std::any visit(ptr<ast::Dict> ast);

    bool hasType(ptr<ast::AST> ast) const { return ast->existsAttr("type"); }

    ptr<type::Type> typeAttr(ptr<ast::AST> ast) {
        #ifdef DEBUG_BUILD
        assert(hasType(ast));
        #endif
        return std::any_cast<ptr<type::Type>>(ast->attrs["type"]);
    }

private:
    std::vector<ScopeInfo> scopes;
    bool replMode { false };
    bool replScopeInitialized { false };

    void pushScope(bool strict);
    void popScope();
    bool currentStrict() const;
    void declareVar(const icu::UnicodeString& name, ptr<type::Type> type, bool explicitType);
    std::optional<VarInfo> lookupVar(const icu::UnicodeString& name) const;
};


}
