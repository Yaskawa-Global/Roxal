#include <sstream>
#include <iomanip>
#include <algorithm>

#include <core/common.h>

#include "ASTGraphviz.h"

using namespace roxal;
using namespace roxal::ast;


ASTGraphviz::ASTGraphviz()
{

}


// local helper functions
static std::string uname(ptr<AST> node)
{
    static uint32_t index = 0;
    return "n"+std::to_string(index++);
}

static std::string node(const std::string& name, const std::string type, const std::string& text="")
{
    if (text.empty())
        return name+" [label=\""+type+"\"]";
    else
        return name+" [label=\""+type+"\\n'"+text+"'\"]";
}


static int indent = 0;

#ifdef DEBUG_ASTGEN
    #define startVisit() \
    std::cout << spaces(indent++) << "entering " << demangle(typeid(*ast).name()) << std::endl;

    #define endVisit() \
    std::cout << spaces(--indent) << "exiting " << demangle(typeid(*ast).name()) << std::endl;
#else
    #define startVisit() {}
    #define endVisit() {}
#endif


void ASTGraphviz::stackPush(const std::string& s)
{
    stack.push(s);
    #ifdef DEBUG_ASTGEN
    std::cout << "push(" << nodes.at(s) << ")" << std::endl;
    #endif
}

std::string ASTGraphviz::stackPop()
{
    if (stack.empty())
        throw std::runtime_error("stack is empty");
    auto top = stack.top();
    stack.pop();
    #ifdef DEBUG_ASTGEN
    std::cout << "pop() -> " << nodes.at(top) << std::endl;
    #endif
    return top;
}

void ASTGraphviz::addLink(const std::string& fromNodeName, const std::string& toNodeName,const std::string& linkLabel)
{
    links.push_back(std::make_tuple(fromNodeName,toNodeName, linkLabel));
}


ASTVisitor::TraversalOrder ASTGraphviz::traversalOrder() const
{
    return TraversalOrder::Postorder;
}



std::string ASTGraphviz::generateGraphText(ptr<ast::AST> ast)
{
    nodes.clear();
    links.clear();
    stack = std::stack<std::string>();


    if (std::dynamic_pointer_cast<ast::File>(ast)!=nullptr) {
        auto file = std::dynamic_pointer_cast<ast::File>(ast);
        file->accept(*this);
    }
    else
        throw std::runtime_error("unsupported top-level AST node");

    std::ostringstream ss;
    ss << "digraph {" << std::endl;
    ss << "   ordering=\"out\"" << std::endl;
    for(const auto& node : nodes)
        ss << node.second << std::endl;
    // output in reverse order so that links visited in prefix ordering are defined top to bottom in the file
    //  and hence rendered left-right by graphviz (given the ordering=out above)
    std::for_each( links.rbegin(), 
                   links.rend(),
                   [&](const auto & link){
                      ss << "  " << std::get<0>(link) << " -> " << std::get<1>(link);
                      if (!std::get<2>(link).empty())
                        ss << " [headlabel=\"" << std::get<2>(link) << "\""
                           <<   " labeldistance=2.5 labelangle=0"
                        "]";
                      ss << std::endl;
                   }
                 );
    ss << "}" << std::endl;
    return ss.str();
}


std::any ASTGraphviz::visit(ptr<ast::File> ast)
{
    startVisit();

    auto name { uname(ast) };

    for(int i=0; i<ast->declsOrStmts.size();i++)
        addLink(name, stackPop());

    nodes[name] = node(name,"File");
    stackPush(name);
    endVisit()
    return {};
}


std::any ASTGraphviz::visit(ptr<ast::SingleInput> ast)
{
    startVisit();
    throw std::runtime_error(__PRETTY_FUNCTION__+std::string("unimplemented"));

    endVisit();
    return {};
}


std::any ASTGraphviz::visit(ptr<ast::Annotation> ast)
{
    startVisit();
    auto name { uname(ast) };

    for(int i=0; i<ast->args.size();i++) {
        size_t argIndex = ast->args.size()-i-1;
        std::string label = ast->args.at(argIndex).first.isEmpty() ? 
                                    std::to_string(argIndex)
                                  : toUTF8StdString(ast->args.at(argIndex).first)+"=";
        addLink(name, stackPop(), label);
    }

    addLink(name, stackPop());

    nodes[name] = node(name,"@");
    stackPush(name);

    endVisit();
    return {};
}


std::any ASTGraphviz::visit(ptr<ast::TypeDecl> ast)
{
    startVisit();

    auto name { uname(ast) };

    for(int i=0; i<ast->properties.size();i++)
        addLink(name, stackPop());

    for(int i=0; i<ast->methods.size();i++)
        addLink(name, stackPop());

    nodes[name] = node(name,
                       std::string("TypeDecl ")
                       +(ast->kind==TypeDecl::Object?"object":(ast->kind==TypeDecl::Actor?"actor":"?")),
                       toUTF8StdString(ast->name));
    stackPush(name);
    endVisit();
    return {};
}


std::any ASTGraphviz::visit(ptr<ast::FuncDecl> ast)
{
    startVisit();

    auto name { uname(ast) };

    addLink(name,stackPop());

    nodes[name] = node(name,"FuncDecl");
    stackPush(name);

    endVisit();
    return {};
}


std::any ASTGraphviz::visit(ptr<ast::VarDecl> ast)
{
    startVisit();
    auto name { uname(ast) };

    if (ast->initializer.has_value())
        addLink(name, stackPop());

    nodes[name] = node(name,"VarDecl",toUTF8StdString(ast->name));
    stackPush(name);
    endVisit();
    return {};
}


std::any ASTGraphviz::visit(ptr<ast::Suite> ast)
{
    startVisit();
    auto name { uname(ast) };

    if (stack.size()<ast->declsOrStmts.size()) {
        //while (!stack.empty())
        //    std::cout << nodes[stackPop()] << std::endl;
        throw std::runtime_error("visit(Suite) expected "+std::to_string(ast->declsOrStmts.size())+" on stack top, but only "+std::to_string(stack.size()));
    }

    for(auto i=0; i<ast->declsOrStmts.size();i++)
        addLink(name, stackPop());

    nodes[name] = node(name,"Suite");
    stackPush(name);

    endVisit();
    return {};
}


std::any ASTGraphviz::visit(ptr<ast::ExpressionStatement> ast)
{
    startVisit();
    auto name { uname(ast) };

    addLink(name, stackPop());

    nodes[name] = node(name,"ExprStmt");
    stackPush(name);
    endVisit();
    return {};
}


std::any ASTGraphviz::visit(ptr<ast::ReturnStatement> ast)
{
    startVisit();
    auto name { uname(ast) };

    if (ast->expr.has_value()) 
        addLink(name, stackPop());

    nodes[name] = node(name,"Return");
    stackPush(name);
    endVisit();
    return {};
}


std::any ASTGraphviz::visit(ptr<ast::IfStatement> ast)
{
    startVisit();
    auto name { uname(ast) };

    bool elseif = false;
    if (ast->elseSuite.has_value())
        addLink(name, stackPop(),"else"); // else body
    for(const auto& condBody : ast->conditionalSuites) {
        addLink(name, stackPop(),"then"); // body
        addLink(name, stackPop(),!elseif ? "if" : "elseif"); // condition
        elseif = true;
    }

    nodes[name] = node(name,"If");
    stackPush(name);
    endVisit();
    return {};
}


std::any ASTGraphviz::visit(ptr<ast::WhileStatement> ast)
{
    startVisit();
    auto name { uname(ast) };

    addLink(name, stackPop(), "body"); // body
    addLink(name, stackPop(), "cond"); // condition

    nodes[name] = node(name, "While");
    stackPush(name);

    endVisit();
    return {};
}


std::any ASTGraphviz::visit(ptr<ast::Function> ast)
{
    startVisit();
    auto name { uname(ast) };

    addLink(name, stackPop(), "body"); // body

    auto nameReturn = toUTF8StdString(ast->name);

    if (ast->returnType.has_value()) {
        if (std::holds_alternative<BuiltinType>(ast->returnType.value())) {
            nameReturn += " → "+to_string(std::get<BuiltinType>(ast->returnType.value()));
        }
        else if (std::holds_alternative<icu::UnicodeString>(ast->returnType.value())){
            nameReturn += " → "+toUTF8StdString(std::get<icu::UnicodeString>(ast->returnType.value()));
        }
     }

    auto n = ast->params.size();
    for(int i=0; i<n;i++)
        addLink(name, stackPop(), std::to_string(n-i-1));

    nodes[name] = node(name, "Function "+nameReturn);
    stackPush(name);
    endVisit();
    return {};
}


std::any ASTGraphviz::visit(ptr<ast::Parameter> ast)
{
    startVisit();
    auto name { uname(ast) };

    auto nametype = toUTF8StdString(ast->name);
    if (ast->type.has_value()) {
        if (std::holds_alternative<BuiltinType>(ast->type.value())) {
            nametype += " : "+to_string(std::get<BuiltinType>(ast->type.value()));
        }
        else if (std::holds_alternative<icu::UnicodeString>(ast->type.value())){
            nametype += " : "+toUTF8StdString(std::get<icu::UnicodeString>(ast->type.value()));
        }
     }

    if (ast->defaultValue.has_value())
        addLink(name, stackPop(), "=");

    nodes[name] = node(name, "Param "+nametype);
    stackPush(name);
    endVisit();
    return {};
}


std::any ASTGraphviz::visit(ptr<ast::Assignment> ast)
{
    startVisit();
    auto name { uname(ast) };

    addLink(name, stackPop());
    addLink(name, stackPop());

    nodes[name] = node(name,"Assign");
    stackPush(name);
    endVisit();
    return {};
}


std::any ASTGraphviz::visit(ptr<ast::BinaryOp> ast)
{
    startVisit();
    auto name { uname(ast) };

    addLink(name, stackPop());
    addLink(name, stackPop());

    nodes[name] = node(name,ast->opString());
    stackPush(name);
    endVisit();
    return {};
}


std::any ASTGraphviz::visit(ptr<ast::UnaryOp> ast)
{
    startVisit();
    auto name { uname(ast) };

    addLink(name, stackPop());

    nodes[name] = node(name,ast->opString() 
                   + ((ast->op == UnaryOp::Op::Accessor) && ast->member.has_value() ? toUTF8StdString(ast->member.value()) : "?"));
    stackPush(name);
    endVisit();
    return {};
}


std::any ASTGraphviz::visit(ptr<ast::Variable> ast)
{
    startVisit();
    auto name { uname(ast) };

    nodes[name] = node(name,"ident",toUTF8StdString(ast->name));
    stackPush(name);
    endVisit();
    return {};
}


std::any ASTGraphviz::visit(ptr<ast::Call> ast)
{
    startVisit();
    auto name { uname(ast) };

    for(int i=0; i<ast->args.size();i++) {
        size_t argIndex = ast->args.size()-i-1;
        std::string label = ast->args.at(argIndex).first.isEmpty() ? 
                                    std::to_string(argIndex)
                                  : toUTF8StdString(ast->args.at(argIndex).first)+"=";
        addLink(name, stackPop(), label);
    }

    addLink(name, stackPop());

    nodes[name] = node(name,"Call");
    stackPush(name);
    endVisit();
    return {};
}


std::any ASTGraphviz::visit(ptr<ast::Range> ast)
{
    startVisit();
    auto name { uname(ast) };

    if (ast->step != nullptr)
        addLink(name, stackPop(), "step");
    if (ast->stop != nullptr)
        addLink(name, stackPop(), "stop");
    if (ast->start != nullptr)
        addLink(name, stackPop(), "start");

    nodes[name] = node(name, std::string("range ")+(ast->closed ? "[]" : "[)") );
    stackPush(name);
    endVisit();
    return {};
}


std::any ASTGraphviz::visit(ptr<ast::Index> ast)
{
    startVisit();
    auto name { uname(ast) };

    for(int i=0; i<ast->args.size();i++)
        addLink(name, stackPop(), std::to_string(i));

    addLink(name, stackPop());

    nodes[name] = node(name,"[]");
    stackPush(name);
    endVisit();
    return {};
}


std::any ASTGraphviz::visit(ptr<ast::Literal> ast)
{
    startVisit();

    auto name { uname(ast) };
    nodes[name] = node(name,ast->literalType == Literal::Nil ? "nil" : "Literal",
                       ast->literalType == Literal::Nil ? "nil" : "?");
    stackPush(name);
    endVisit();
    return {};
}


std::any ASTGraphviz::visit(ptr<ast::Bool> ast)
{
    startVisit();
    auto name { uname(ast) };
    nodes[name] = node(name,"bool",ast->value ? "true" : "false");
    stackPush(name);
    endVisit();
    return {};
}


std::any ASTGraphviz::visit(ptr<ast::Str> ast)
{
    startVisit();
    auto name { uname(ast) };
    nodes[name] = node(name,"string",toUTF8StdString(ast->str));
    stackPush(name);
    endVisit();
    return {};
}


std::any ASTGraphviz::visit(ptr<ast::Type> ast)
{
    startVisit();
    auto name { uname(ast) };
    nodes[name] = node(name,"type",ast::to_string(ast->t));
    stackPush(name);
    endVisit();
    return {};
}


std::any ASTGraphviz::visit(ptr<ast::Num> ast)
{
    startVisit();
    auto name { uname(ast) };

    if (std::holds_alternative<double>(ast->num)) {
        std::stringstream ss;
        ss << std::fixed << std::setprecision(3) << std::get<double>(ast->num);
        nodes[name] = node(name,"real",ss.str());
    }
    else if (std::holds_alternative<int32_t>(ast->num)) {
        nodes[name] = node(name,"int",std::to_string(std::get<int32_t>(ast->num)));
    }
    else 
        throw std::runtime_error("unhandled Num type");

    stackPush(name);
    endVisit();
    return {};
}


std::any ASTGraphviz::visit(ptr<ast::List> ast)
{
    startVisit();
    auto name { uname(ast) };

    auto n = ast->elements.size();
    for(int i=0; i<n;i++)
        addLink(name, stackPop(), std::to_string(n-i-1));

    nodes[name] = node(name,"list");
    stackPush(name);
    endVisit();
    return {};
}


std::any ASTGraphviz::visit(ptr<ast::Dict> ast)
{
    startVisit();
    auto name { uname(ast) };

    auto n = ast->entries.size();
    for(int i=0; i<n;i+=2) {
        addLink(name, stackPop(), "k"+std::to_string(n-i-1));
        addLink(name, stackPop(), "v"+std::to_string(n-i-1));
    }

    nodes[name] = node(name,"dict");
    stackPush(name);
    endVisit();
    return {};
}
