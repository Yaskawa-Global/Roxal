#pragma once

#include <cassert>

#include <core/AST.h>
#include <core/types.h>

namespace roxal {


class TypeDeducer : public ast::ASTVisitor
{
public:
    TypeDeducer() {}

    virtual TraversalOrder traversalOrder() const;

    virtual void visit(ptr<ast::File> ast);
    virtual void visit(ptr<ast::SingleInput> ast);
    virtual void visit(ptr<ast::TypeDecl> ast);
    virtual void visit(ptr<ast::FuncDecl> ast);
    virtual void visit(ptr<ast::VarDecl> ast);
    virtual void visit(ptr<ast::Suite> ast);
    virtual void visit(ptr<ast::ExpressionStatement> ast);
    virtual void visit(ptr<ast::ReturnStatement> ast);
    virtual void visit(ptr<ast::IfStatement> ast);
    virtual void visit(ptr<ast::WhileStatement> ast);
    virtual void visit(ptr<ast::Function> ast);
    virtual void visit(ptr<ast::Parameter> ast);
    virtual void visit(ptr<ast::Assignment> ast);
    virtual void visit(ptr<ast::BinaryOp> ast);
    virtual void visit(ptr<ast::UnaryOp> ast);
    virtual void visit(ptr<ast::Variable> ast);
    virtual void visit(ptr<ast::Call> ast);
    virtual void visit(ptr<ast::Index> ast);
    virtual void visit(ptr<ast::Literal> ast);
    virtual void visit(ptr<ast::Bool> ast);
    virtual void visit(ptr<ast::Str> ast);
    virtual void visit(ptr<ast::Type> ast);
    virtual void visit(ptr<ast::Num> ast);
    virtual void visit(ptr<ast::List> ast);
    virtual void visit(ptr<ast::Dict> ast);

    bool hasType(ptr<ast::AST> ast) const { return ast->existsAttr("type"); }

    ptr<type::Type> typeAttr(ptr<ast::AST> ast) {
        #ifdef DEBUG_BUILD
        assert(hasType(ast));
        #endif
        return std::any_cast<ptr<type::Type>>(ast->attrs["type"]);
    }
};


}
