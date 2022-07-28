#include <stack>
#include <map>
#include <tuple>

#include <core/AST.h>

namespace roxal {

// generate graphviz/dot text format to rendering visualization of AST
class ASTGraphviz : public ast::ASTVisitor
{
public:
    ASTGraphviz();

    std::string generateGraphText(ptr<ast::AST> ast);

    virtual TraversalOrder traversalOrder() const;

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

protected:
    // during visit traversal, children of each pushes then popped to create links
    std::stack<std::string> stack; // stack of node names 
    void stackPush(const std::string& s);
    std::string stackPop();
    void addLink(const std::string& fromNodeName, const std::string& toNodeName, const std::string& linkLabel="");

    std::map<std::string,std::string> nodes; // node names -> graphviz declaration
    std::vector<std::tuple<std::string,std::string,std::string>> links; // from-node, to-node, link-name triples
};


}