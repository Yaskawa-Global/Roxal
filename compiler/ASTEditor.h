#pragma once
#include <stack>
#include <map>
#include <tuple>
#include <mutex>
#include <functional>
#include <core/AST.h>

namespace roxal {

// Edit the structure of the AST tree
class ASTEditor : public ast::ASTVisitor
{
public:
    //insert into suite
    void insertSubtreeAfter(ptr<roxal::ast::AST> tree, ptr<roxal::ast::AST> parent, ptr<roxal::ast::AST> sibling, ptr<roxal::ast::AST> toInsert);
    void insertSubtreeBefore(ptr<roxal::ast::AST> tree, ptr<roxal::ast::AST> parent, ptr<roxal::ast::AST> sibling, ptr<roxal::ast::AST> toInsert);
    void deleteSubtree(ptr<roxal::ast::AST> tree, ptr<roxal::ast::AST> parent, ptr<roxal::ast::AST> toRemove);
    void replaceSubtree(ptr<roxal::ast::AST> tree, ptr<roxal::ast::AST> parent, ptr<roxal::ast::AST> toRemove, ptr<roxal::ast::AST> toInsert);

    virtual TraversalOrder traversalOrder() const{ return TraversalOrder::VisitorDetermined; }

    virtual void visit(ptr<ast::File> ast);
    virtual void visit(ptr<ast::SingleInput> ast);
    virtual void visit(ptr<ast::Annotation> ast);
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
private:
    enum class AstOperation
    {
        InsertBefore,
        InsertAfter,
        Delete
    };

    AstOperation m_activeOperation;

    //helpers for casting and inserting
    template<typename T> bool isChildA();
    
    int findChildInSuite(std::vector<std::variant<ptr<roxal::ast::Declaration>, ptr<roxal::ast::Statement>>>& vec, ptr<roxal::ast::AST> toFind);
    void deleteFromSuite(std::vector<std::variant<ptr<roxal::ast::Declaration>, ptr<roxal::ast::Statement>>>& vec);
    void insertBeforeOrAfterIntoSuite(std::vector<std::variant<ptr<roxal::ast::Declaration>, ptr<roxal::ast::Statement>>>& vec, bool before);

    //fix the line interval (for file IO)
    void updateAstLinePositions(ptr<roxal::ast::AST> ast, int startingLine, int startingPos, int lineDelta, int posDelta);

    //update the inserted source to use the full ast's source
    void updateInsertedNodeSource();

    ptr<roxal::ast::AST> m_tree = nullptr; 
    ptr<roxal::ast::AST> m_parent = nullptr; 
    ptr<roxal::ast::AST> m_sibling = nullptr; 
    ptr<roxal::ast::AST> m_removed = nullptr;
    ptr<roxal::ast::AST> m_inserted = nullptr;
    std::mutex m_memberLock;
    
};

//helper to run the same method on every Ast node
class AstAllNodeCallback: public roxal::ast::ASTVisitor
{
public:
    virtual TraversalOrder traversalOrder() const
          { return TraversalOrder::Preorder; }

    void run(ptr<roxal::ast::AST> ast);
    void setCallback(std::function<void(ptr<roxal::ast::AST>)> f);
    void visit(ptr<roxal::ast::File> ast) override { if(m_f) m_f(ast); }
    void visit(ptr<roxal::ast::SingleInput> ast) override { if(m_f) m_f(ast); }
    void visit(ptr<roxal::ast::Annotation> ast) override { if(m_f) m_f(ast); }
    void visit(ptr<roxal::ast::TypeDecl> ast) override { if(m_f) m_f(ast); }
    void visit(ptr<roxal::ast::FuncDecl> ast) override { if(m_f) m_f(ast); }
    void visit(ptr<roxal::ast::VarDecl> ast) override { if(m_f) m_f(ast); }
    void visit(ptr<roxal::ast::Suite> ast) override { if(m_f) m_f(ast); }
    void visit(ptr<roxal::ast::ExpressionStatement> ast) override { if(m_f) m_f(ast); }
    void visit(ptr<roxal::ast::ReturnStatement> ast) override { if(m_f) m_f(ast); }
    void visit(ptr<roxal::ast::IfStatement> ast) override { if(m_f) m_f(ast); }
    void visit(ptr<roxal::ast::WhileStatement> ast) override { if(m_f) m_f(ast); }
    void visit(ptr<roxal::ast::Function> ast) override { if(m_f) m_f(ast); }
    void visit(ptr<roxal::ast::Parameter> ast) override { if(m_f) m_f(ast); }
    void visit(ptr<roxal::ast::Assignment> ast) override { if(m_f) m_f(ast); }
    void visit(ptr<roxal::ast::BinaryOp> ast) override { if(m_f) m_f(ast); }
    void visit(ptr<roxal::ast::UnaryOp> ast) override { if(m_f) m_f(ast); }
    void visit(ptr<roxal::ast::Variable> ast) override { if(m_f) m_f(ast); }
    void visit(ptr<roxal::ast::Call> ast) override { if(m_f) m_f(ast); }
    void visit(ptr<roxal::ast::Index> ast) override { if(m_f) m_f(ast); }
    void visit(ptr<roxal::ast::Literal> ast) override { if(m_f) m_f(ast); }
    void visit(ptr<roxal::ast::Bool> ast) override { if(m_f) m_f(ast); }
    void visit(ptr<roxal::ast::Type> ast) override { if(m_f) m_f(ast); }
    void visit(ptr<roxal::ast::Str> ast) override { if(m_f) m_f(ast); }
    void visit(ptr<roxal::ast::Num> ast) override { if(m_f) m_f(ast); }
    void visit(ptr<roxal::ast::List> ast) override { if(m_f) m_f(ast); }
    void visit(ptr<roxal::ast::Dict> ast) override { if(m_f) m_f(ast); }
private:
    std::function<void(ptr<roxal::ast::AST>)> m_f;

};
}
