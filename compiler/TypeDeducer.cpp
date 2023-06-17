

#include "TypeDeducer.h"


using namespace roxal;

using roxal::type::BuiltinType;
using roxal::type::Type;


ast::ASTVisitor::TraversalOrder TypeDeducer::traversalOrder() const
{
    return TraversalOrder::VisitorDetermined;
}


std::any TypeDeducer::visit(ptr<ast::File> ast)
{
    ast::Anys results {};
    ast->acceptChildren(*this, results);
    return results;
}


std::any TypeDeducer::visit(ptr<ast::SingleInput> ast)
{
    ast::Anys results {};
    ast->acceptChildren(*this, results);
    return results;
}


std::any TypeDeducer::visit(ptr<ast::Annotation> ast)
{
    ast::Anys results {};
    ast->acceptChildren(*this, results);
    return results;
}


std::any TypeDeducer::visit(ptr<ast::TypeDecl> ast)
{
    ast::Anys results {};
    ast->acceptChildren(*this, results);
    return results;
}


std::any TypeDeducer::visit(ptr<ast::FuncDecl> ast)
{
    ast::Anys results {};
    ast->acceptChildren(*this, results);

    // for completeness/convenience, set function type here too
    ast->type = ast->func->type;
    return results;
}


std::any TypeDeducer::visit(ptr<ast::VarDecl> ast)
{
    ast::Anys results {};
    ast->acceptChildren(*this, results);
    if (ast->varType.has_value()) {
        if (std::holds_alternative<BuiltinType>(ast->varType.value())) {
            ast->type = std::make_shared<type::Type>(std::get<BuiltinType>(ast->varType.value()));
        }
        else {
            // lookup name..
        }
    }
    else { // type wasn't explicitly specified, so type will be that of initializer
        if (ast->initializer.has_value()) { // if given
            if (ast->initializer.value()->type.has_value()) { // and type known
                ast->type = ast->initializer.value()->type.value();
            }
        }

    }
    return results;
}


std::any TypeDeducer::visit(ptr<ast::Suite> ast)
{
    ast::Anys results {};
    ast->acceptChildren(*this, results);
    return results;
}


std::any TypeDeducer::visit(ptr<ast::ExpressionStatement> ast)
{
    ast::Anys results {};
    ast->acceptChildren(*this, results);
    return results;
}


std::any TypeDeducer::visit(ptr<ast::ReturnStatement> ast)
{
    ast::Anys results {};
    ast->acceptChildren(*this, results);
    return results;
}


std::any TypeDeducer::visit(ptr<ast::IfStatement> ast)
{
    ast::Anys results {};
    ast->acceptChildren(*this, results);
    return results;
}


std::any TypeDeducer::visit(ptr<ast::WhileStatement> ast)
{
    ast::Anys results {};
    ast->acceptChildren(*this, results);
    return results;
}   


std::any TypeDeducer::visit(ptr<ast::Function> ast)
{
    ast::Anys results {};
    ast->acceptChildren(*this, results);

    auto type = std::make_shared<Type>();
    type->builtin = BuiltinType::Func;
    type->func = Type::FuncType();
    type->func->isProc = ast->isProc;
    if (ast->returnType.has_value()) {
        if (std::holds_alternative<BuiltinType>(ast->returnType.value())) {
            type->func->returnType = std::make_shared<Type>(std::get<BuiltinType>(ast->returnType.value()));
        }
        else if (std::holds_alternative<icu::UnicodeString>(ast->returnType.value())) {
            // lookup name
            //...
        }
    }

    type->func->params.resize(ast->params.size());
    for(size_t i=0; i<ast->params.size();i++) {
        auto param { ast->params[i] };
        if (param->type.has_value()) {
            if (std::holds_alternative<BuiltinType>(param->type.value())) {
                Type::FuncType::ParamType paramType {};
                paramType.name = param->name;
                paramType.nameHashCode = param->name.hashCode();
                paramType.type = std::make_shared<Type>(std::get<BuiltinType>(param->type.value()));
                paramType.hasDefault = param->defaultValue.has_value();
                type->func.value().params[i] = paramType;
            }
            else if (std::holds_alternative<icu::UnicodeString>(param->type.value())) {
                // lookup name
                //...
            }
        }
        else {
            Type::FuncType::ParamType paramType {};
            paramType.name = param->name;
            paramType.nameHashCode = param->name.hashCode();
            paramType.hasDefault = param->defaultValue.has_value();
            type->func.value().params[i] = paramType;
        }
    }

    ast->type = type;
    return results;
}


std::any TypeDeducer::visit(ptr<ast::Parameter> ast)
{
    ast::Anys results {};
    ast->acceptChildren(*this, results);
    return results;
}


std::any TypeDeducer::visit(ptr<ast::Assignment> ast)
{
    ast::Anys results {};
    ast->acceptChildren(*this, results);

    // assignment has type of rhs

    //  TODO: when assigning to lhs vars of explicitly declared type,
    //  rhs will be converted to lhs type
    if (ast->rhs->type.has_value())
        ast->type = ast->rhs->type.value();

    return results;
}


std::any TypeDeducer::visit(ptr<ast::BinaryOp> ast)
{
    ast::Anys results {};
    ast->acceptChildren(*this, results);
    return results;
}


std::any TypeDeducer::visit(ptr<ast::UnaryOp> ast)
{
    ast::Anys results {};
    ast->acceptChildren(*this, results);
    return results;
}


std::any TypeDeducer::visit(ptr<ast::Variable> ast)
{
    // TODO: lookup name
    return {};
}


std::any TypeDeducer::visit(ptr<ast::Call> ast)
{
    ast::Anys results {};
    ast->acceptChildren(*this, results);
    return results;
}


std::any TypeDeducer::visit(ptr<ast::Range> ast)
{
    ast::Anys results {};
    ast->acceptChildren(*this, results);

    ast->type = std::make_shared<Type>(BuiltinType::Range);

    return results;
}


std::any TypeDeducer::visit(ptr<ast::Index> ast)
{
    ast::Anys results {};
    ast->acceptChildren(*this, results);

    // if the indexable is a string, indexing yields a string also
    if (ast->indexable->type.has_value()) {
        if (ast->indexable->type.value()->builtin == BuiltinType::String)
            ast->type = std::make_shared<Type>(BuiltinType::String);
    }
    return results;
}


std::any TypeDeducer::visit(ptr<ast::Literal> ast)
{
    // non-Nil typed literals handled by specialized visit methods
    if (ast->literalType == ast::Literal::Nil)
        ast->type = std::make_shared<Type>(BuiltinType::Nil);
    return {};
}


std::any TypeDeducer::visit(ptr<ast::Bool> ast)
{
    ast->type = std::make_shared<Type>(BuiltinType::Bool);
    return {};
}


std::any TypeDeducer::visit(ptr<ast::Str> ast)
{
    ast->type = std::make_shared<Type>(BuiltinType::String);
    return {};
}


std::any TypeDeducer::visit(ptr<ast::Type> ast)
{
    ast->type = std::make_shared<Type>(BuiltinType::Type);
    return {};
}


std::any TypeDeducer::visit(ptr<ast::Num> ast)
{
    if (std::holds_alternative<int32_t>(ast->num)) 
        ast->type = std::make_shared<Type>(BuiltinType::Int);
    else if (std::holds_alternative<double>(ast->num)) 
        ast->type = std::make_shared<Type>(BuiltinType::Real);
    else
        throw std::runtime_error("Unhandled Num literal type");
    return {};
}


std::any TypeDeducer::visit(ptr<ast::List> ast)
{
    ast::Anys results {};
    ast->acceptChildren(*this, results);
    ast->type = std::make_shared<Type>(BuiltinType::List);
    return results;
}


std::any TypeDeducer::visit(ptr<ast::Dict> ast)
{
    ast::Anys results {};
    ast->acceptChildren(*this, results);
    ast->type = std::make_shared<Type>(BuiltinType::Dict);
    return results;
}

