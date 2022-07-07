

#include "TypeDeducer.h"


using namespace roxal;

using roxal::type::BuiltinType;
using roxal::type::Type;


ast::ASTVisitor::TraversalOrder TypeDeducer::traversalOrder() const
{
    // visit all children (to deduce their types) first
    return TraversalOrder::Postorder;
}


void TypeDeducer::visit(ptr<ast::File> ast)
{

}


void TypeDeducer::visit(ptr<ast::SingleInput> ast)
{

}


void TypeDeducer::visit(ptr<ast::TypeDecl> ast)
{

}


void TypeDeducer::visit(ptr<ast::FuncDecl> ast)
{

}


void TypeDeducer::visit(ptr<ast::VarDecl> ast)
{

}


void TypeDeducer::visit(ptr<ast::Suite> ast)
{

}


void TypeDeducer::visit(ptr<ast::ExpressionStatement> ast)
{

}


void TypeDeducer::visit(ptr<ast::ReturnStatement> ast)
{

}


void TypeDeducer::visit(ptr<ast::IfStatement> ast)
{

}


void TypeDeducer::visit(ptr<ast::WhileStatement> ast)
{

}


void TypeDeducer::visit(ptr<ast::Function> ast)
{

}


void TypeDeducer::visit(ptr<ast::Parameter> ast)
{

}


void TypeDeducer::visit(ptr<ast::Assignment> ast)
{

}


void TypeDeducer::visit(ptr<ast::BinaryOp> ast)
{

}


void TypeDeducer::visit(ptr<ast::UnaryOp> ast)
{

}


void TypeDeducer::visit(ptr<ast::Variable> ast)
{

}


void TypeDeducer::visit(ptr<ast::Call> ast)
{

}


void TypeDeducer::visit(ptr<ast::Index> ast)
{
    // if the indexable is a string, indexing yields a string also
    //if (ast->indexable->existsAttr("type")) {
    //    if (ast->indexable->attrs["type"].type == )
    //}

}


void TypeDeducer::visit(ptr<ast::Literal> ast)
{
    // non-Nil typed literals handled by specialized visit methods
    if (ast->type == ast::Literal::Nil)
        ast->attrs["type"] = std::make_shared<Type>(BuiltinType::Nil);
}


void TypeDeducer::visit(ptr<ast::Bool> ast)
{
    ast->attrs["type"] = std::make_shared<Type>(BuiltinType::Bool);
}


void TypeDeducer::visit(ptr<ast::Str> ast)
{
    ast->attrs["type"] = std::make_shared<Type>(BuiltinType::String);
}


void TypeDeducer::visit(ptr<ast::Type> ast)
{
    ast->attrs["type"] = std::make_shared<Type>(BuiltinType::Type);
}


void TypeDeducer::visit(ptr<ast::Num> ast)
{
    if (std::holds_alternative<int32_t>(ast->num)) 
        ast->attrs["type"] = std::make_shared<Type>(BuiltinType::Int);
    else if (std::holds_alternative<double>(ast->num)) 
        ast->attrs["type"] = std::make_shared<Type>(BuiltinType::Real);
    else
        throw std::runtime_error("Unhandled Num literal type");
}


void TypeDeducer::visit(ptr<ast::List> ast)
{
    ast->attrs["type"] = std::make_shared<Type>(BuiltinType::List);
}


void TypeDeducer::visit(ptr<ast::Dict> ast)
{
    ast->attrs["type"] = std::make_shared<Type>(BuiltinType::Dict);
}

