

#include "TypeDeducer.h"


using namespace roxal;

using roxal::type::BuiltinType;
using roxal::type::Type;


ast::ASTVisitor::TraversalOrder TypeDeducer::traversalOrder() const
{
    return TraversalOrder::VisitorDetermined;
}


void TypeDeducer::visit(ptr<ast::File> ast)
{
    ast->acceptChildren(*this);
}


void TypeDeducer::visit(ptr<ast::SingleInput> ast)
{
    ast->acceptChildren(*this);
}


void TypeDeducer::visit(ptr<ast::TypeDecl> ast)
{
    ast->acceptChildren(*this);
}


void TypeDeducer::visit(ptr<ast::FuncDecl> ast)
{
    ast->acceptChildren(*this);

    // for completeness/convenience, set function type here too
    ast->type = ast->func->type;
}


void TypeDeducer::visit(ptr<ast::VarDecl> ast)
{
    ast->acceptChildren(*this);
}


void TypeDeducer::visit(ptr<ast::Suite> ast)
{
    ast->acceptChildren(*this);

}


void TypeDeducer::visit(ptr<ast::ExpressionStatement> ast)
{
    ast->acceptChildren(*this);

}


void TypeDeducer::visit(ptr<ast::ReturnStatement> ast)
{
    ast->acceptChildren(*this);

}


void TypeDeducer::visit(ptr<ast::IfStatement> ast)
{
    ast->acceptChildren(*this);

}


void TypeDeducer::visit(ptr<ast::WhileStatement> ast)
{
    ast->acceptChildren(*this);

}


void TypeDeducer::visit(ptr<ast::Function> ast)
{
    ast->acceptChildren(*this);

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
    }

    ast->type = type;
}


void TypeDeducer::visit(ptr<ast::Parameter> ast)
{
    ast->acceptChildren(*this);

}


void TypeDeducer::visit(ptr<ast::Assignment> ast)
{
    ast->acceptChildren(*this);

}


void TypeDeducer::visit(ptr<ast::BinaryOp> ast)
{
    ast->acceptChildren(*this);

}


void TypeDeducer::visit(ptr<ast::UnaryOp> ast)
{
    ast->acceptChildren(*this);

}


void TypeDeducer::visit(ptr<ast::Variable> ast)
{
}


void TypeDeducer::visit(ptr<ast::Call> ast)
{
    ast->acceptChildren(*this);

}


void TypeDeducer::visit(ptr<ast::Index> ast)
{
    ast->acceptChildren(*this);

    // if the indexable is a string, indexing yields a string also
    if (ast->indexable->type.has_value()) {
        if (ast->indexable->type.value()->builtin == BuiltinType::String)
            ast->type = std::make_shared<Type>(BuiltinType::String);
    }

}


void TypeDeducer::visit(ptr<ast::Literal> ast)
{
    // non-Nil typed literals handled by specialized visit methods
    if (ast->literalType == ast::Literal::Nil)
        ast->type = std::make_shared<Type>(BuiltinType::Nil);
}


void TypeDeducer::visit(ptr<ast::Bool> ast)
{
    ast->type = std::make_shared<Type>(BuiltinType::Bool);
}


void TypeDeducer::visit(ptr<ast::Str> ast)
{
    ast->type = std::make_shared<Type>(BuiltinType::String);
}


void TypeDeducer::visit(ptr<ast::Type> ast)
{
    ast->type = std::make_shared<Type>(BuiltinType::Type);
}


void TypeDeducer::visit(ptr<ast::Num> ast)
{
    if (std::holds_alternative<int32_t>(ast->num)) 
        ast->type = std::make_shared<Type>(BuiltinType::Int);
    else if (std::holds_alternative<double>(ast->num)) 
        ast->type = std::make_shared<Type>(BuiltinType::Real);
    else
        throw std::runtime_error("Unhandled Num literal type");
}


void TypeDeducer::visit(ptr<ast::List> ast)
{
    ast->acceptChildren(*this);
    ast->type = std::make_shared<Type>(BuiltinType::List);
}


void TypeDeducer::visit(ptr<ast::Dict> ast)
{
    ast->acceptChildren(*this);
    ast->type = std::make_shared<Type>(BuiltinType::Dict);
}

