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
    virtual std::any visit(ptr<ast::Function> ast);
    virtual std::any visit(ptr<ast::Parameter> ast);
    virtual std::any visit(ptr<ast::Assignment> ast);
    virtual std::any visit(ptr<ast::BinaryOp> ast);
    virtual std::any visit(ptr<ast::UnaryOp> ast);
    virtual std::any visit(ptr<ast::Variable> ast);
    virtual std::any visit(ptr<ast::Call> ast);
    virtual std::any visit(ptr<ast::Range> ast);
    virtual std::any visit(ptr<ast::Index> ast);
    virtual std::any visit(ptr<ast::Literal> ast);
    virtual std::any visit(ptr<ast::Bool> ast);
    virtual std::any visit(ptr<ast::Str> ast);
    virtual std::any visit(ptr<ast::Type> ast);
    virtual std::any visit(ptr<ast::Num> ast);
    virtual std::any visit(ptr<ast::List> ast);
    virtual std::any visit(ptr<ast::Dict> ast);

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
