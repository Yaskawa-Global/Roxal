

#include "TypeDeducer.h"


using namespace roxal;

using roxal::type::BuiltinType;
using roxal::type::Type;
using roxal::type::to_string;

static std::string linePos(ptr<ast::AST> node)
{
    return std::to_string(node->interval.first.line) + ":" +
           std::to_string(node->interval.first.pos);
}

void TypeDeducer::pushScope(bool strict)
{
    scopes.push_back(ScopeInfo{});
    scopes.back().strict = strict;
}

void TypeDeducer::popScope()
{
    if (!scopes.empty())
        scopes.pop_back();
}

bool TypeDeducer::currentStrict() const
{
    if (scopes.empty())
        return false;
    return scopes.back().strict;
}

void TypeDeducer::declareVar(const icu::UnicodeString& name, ptr<Type> type, bool explicitType)
{
    if (scopes.empty())
        return;
    scopes.back().symbols[name] = VarInfo{type, explicitType};
}

std::optional<VarInfo> TypeDeducer::lookupVar(const icu::UnicodeString& name) const
{
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
        auto vit = it->symbols.find(name);
        if (vit != it->symbols.end())
            return vit->second;
    }
    return std::nullopt;
}


ast::ASTVisitor::TraversalOrder TypeDeducer::traversalOrder() const
{
    return TraversalOrder::VisitorDetermined;
}


std::any TypeDeducer::visit(ptr<ast::File> ast)
{
    ast::Anys results {};

    bool strictContext = false;
    for (const auto& annot : ast->annotations) {
        if (annot->name == "strict")
            strictContext = true;
        else if (annot->name == "nonstrict")
            strictContext = false;
    }

    pushScope(strictContext); // module scope strictness can be overridden
    ast->acceptChildren(*this, results);
    popScope();
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


std::any TypeDeducer::visit(ptr<ast::Import> ast)
{
    ast::Anys results {};
    ast->acceptChildren(*this, results);
    return results;
}


std::any TypeDeducer::visit(ptr<ast::TypeDecl> ast)
{
    ast::Anys results {};
    ast->acceptChildren(*this, results);

    // if (ast->type.has_value())
    //     std::cout << toUTF8StdString(ast->name) << " : " <<  ast->type.value()->toString() << std::endl;

    if (ast->kind == ast::TypeDecl::Kind::Enumeration) {

        if (ast->extends.has_value()) {
            auto extendsStr = toUTF8StdString(ast->extends.value());
            if (extendsStr == to_string(BuiltinType::Byte))
                ast->type = std::make_shared<type::Type>(BuiltinType::Byte);
            else if (extendsStr == to_string(BuiltinType::Int))
                ast->type = std::make_shared<type::Type>(BuiltinType::Int);
            else // TODO: consider allowing enums to extens other enums
                throw std::runtime_error("Enum(eration) "+toUTF8StdString(ast->name)+" cannot extend type " + extendsStr);
        }
        else // default to int
            ast->type = std::make_shared<type::Type>(BuiltinType::Int);

        // iterate over each enum label and
        //   * look at the type of it's expression (check it matches (or can match) the enum type)
        //   * if it has no expression, create an appropriate literal (e.g. incremented int)

        // TODO: since this is manipulating the AST, it should be done in a separate pass

        int32_t nextValue = 0;
        bool isByteEnum = ast->type.value()->builtin == BuiltinType::Byte;

        for(auto& enumLabel : ast->enumLabels) {

            //std::cout << "enumLabel: " << toUTF8StdString(enumLabel.first) << std::endl;

            if (enumLabel.second != nullptr) {
                ptr<ast::Expression> labelExpr = enumLabel.second;
                if (labelExpr->type.has_value()) {

                    // TODO: until we have compile-time execution/evaluation-of-expresions, require enum values
                    //       to be literals
                    if (labelExpr->exprType != ast::Expression::ExprType::Literal)
                        throw std::runtime_error("Enum(eration)"+toUTF8StdString(ast->name)
                                +" label "+toUTF8StdString(enumLabel.first)+" must be a literal (byte, int)");
                    auto literalTtype = std::dynamic_pointer_cast<ast::Literal>(labelExpr)->literalType;
                    if (literalTtype != ast::Literal::LiteralType::Num) // TODO: check it isn't a real/double
                        throw std::runtime_error("Enum(eration)"+toUTF8StdString(ast->name)
                                +" label "+toUTF8StdString(enumLabel.first)+" must be a literal (byte, int)");

                    auto labeltype = labelExpr->type.value();
                    //std::cout << "type: " << labeltype->toString() << std::endl;

                    // set nextValue to the expression literal value
                    ptr<ast::Num> numExpr = std::dynamic_pointer_cast<ast::Num>(enumLabel.second);
                    nextValue = std::get<int>(numExpr->num); // incremented below
                }

            }
            else { // no expression supplied for enum label, assign next incremental according to type

                if (isByteEnum) { // values are 0-255
                    if (nextValue == 256)
                        throw std::runtime_error("Enum(eration)"+toUTF8StdString(ast->name)
                                +" value for "+toUTF8StdString(enumLabel.first)+" is out of range (>255 for byte enum)");
                }
                auto numExpr = std::make_shared<ast::Num>();
                numExpr->num = nextValue;
                numExpr->type = std::make_shared<type::Type>(BuiltinType::Int);
                enumLabel.second = numExpr;
            }
            nextValue++;
        }

    }

    return results;
}


std::any TypeDeducer::visit(ptr<ast::FuncDecl> ast)
{
    ast::Anys results {};
    // propagate annotations to the underlying Function node so visitors
    // like visit(Function) can inspect them
    ast->func->annotations = ast->annotations;
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
            ast->type = std::make_shared<Type>(std::get<BuiltinType>(ast->varType.value()));
        }
    } else if (ast->initializer.has_value() && ast->initializer.value()->type.has_value()) {
        ast->type = ast->initializer.value()->type.value();
    }

    // if variable has explicit type and initializer type known, check
    if (ast->varType.has_value() && std::holds_alternative<BuiltinType>(ast->varType.value()) &&
        ast->initializer.has_value() && ast->initializer.value()->type.has_value()) {
        auto lhsType = std::get<BuiltinType>(ast->varType.value());
        auto rhsType = ast->initializer.value()->type.value()->builtin;
        if (!type::convertibleTo(rhsType, lhsType, currentStrict())) {
            throw std::logic_error(linePos(ast) + " - unable to convert " + to_string(rhsType) + " to " + to_string(lhsType) + (currentStrict()?" in strict mode":""));
        }
    }

    if (ast->type.has_value())
        declareVar(ast->name, ast->type.value(), ast->varType.has_value());

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


std::any TypeDeducer::visit(ptr<ast::ForStatement> ast)
{
    ast::Anys results {};
    ast->acceptChildren(*this, results);
    return results;
}

std::any TypeDeducer::visit(ptr<ast::OnStatement> ast)
{
    ast::Anys results {};
    ast->acceptChildren(*this, results);
    return results;
}

std::any TypeDeducer::visit(ptr<ast::UntilStatement> ast)
{
    ast::Anys results {};
    ast->acceptChildren(*this, results);
    return results;
}

std::any TypeDeducer::visit(ptr<ast::TryStatement> ast)
{
    ast::Anys results {};
    ast->acceptChildren(*this, results);
    return results;
}

std::any TypeDeducer::visit(ptr<ast::RaiseStatement> ast)
{
    ast::Anys results {};
    ast->acceptChildren(*this, results);
    return results;
}


std::any TypeDeducer::visit(ptr<ast::Function> ast)
{
    ast::Anys results {};

    bool strictContext = true;
    for (const auto& annot : ast->annotations) {
        if (annot->name == "strict")
            strictContext = true;
        else if (annot->name == "nonstrict")
            strictContext = false;
    }

    pushScope(strictContext);
    // pre-register parameter types so body and defaults can reference them
    for (auto& param : ast->params) {
        if (param->type.has_value() && std::holds_alternative<BuiltinType>(param->type.value())) {
            auto ptype = std::make_shared<Type>(std::get<BuiltinType>(param->type.value()));
            declareVar(param->name, ptype, /*explicit*/true);
        }
    }

    ast->acceptChildren(*this, results);

    auto type = std::make_shared<Type>();
    type->builtin = BuiltinType::Func;
    type->func = Type::FuncType();
    type->func->isProc = ast->isProc;
    if (ast->returnTypes.has_value()) {
        auto& returnTypes = ast->returnTypes.value();
        for (const auto& returnType : returnTypes) {
            if (std::holds_alternative<BuiltinType>(returnType)) {
                type->func->returnTypes.push_back(std::make_shared<Type>(std::get<BuiltinType>(returnType)));
            }
            else if (std::holds_alternative<icu::UnicodeString>(returnType)) {
                // lookup name - for now create a placeholder
                // TODO: implement proper name lookup
                auto placeholderType = std::make_shared<Type>(BuiltinType::Object);
                type->func->returnTypes.push_back(placeholderType);
            }
        }
        
        if (returnTypes.size() > 1) {
            // Multiple return types - not yet fully supported in call type deduction
            // But we can still record them in the function type
            // TODO: implement proper multi-return call handling
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
    popScope();
    return results;
}


std::any TypeDeducer::visit(ptr<ast::Parameter> ast)
{
    ast::Anys results {};
    ast->acceptChildren(*this, results);
    if (ast->type.has_value() && std::holds_alternative<BuiltinType>(ast->type.value())) {
        static_cast<ast::AST*>(ast.get())->type = std::make_shared<Type>(std::get<BuiltinType>(ast->type.value()));
    }
    return results;
}


std::any TypeDeducer::visit(ptr<ast::Assignment> ast)
{
    ast::Anys results {};
    ast->acceptChildren(*this, results);

    // assignment has type of rhs
    if (ast->rhs->type.has_value())
        ast->type = ast->rhs->type.value();

    // static type check when lhs is a variable with known explicit type
    if (std::dynamic_pointer_cast<ast::Variable>(ast->lhs) != nullptr) {
        auto vname = std::dynamic_pointer_cast<ast::Variable>(ast->lhs)->name;
        auto info = lookupVar(vname);
        if (info.has_value() && info->explicitType && info->type != nullptr && ast->rhs->type.has_value()) {
            auto lhsType = info->type->builtin;
            auto rhsType = ast->rhs->type.value()->builtin;
            if (!type::convertibleTo(rhsType, lhsType, currentStrict())) {
                throw std::logic_error(linePos(ast) + " - unable to convert " + to_string(rhsType) + " to " + to_string(lhsType) + (currentStrict()?" in strict mode":""));
            }
        }
    }

    return results;
}


std::any TypeDeducer::visit(ptr<ast::BinaryOp> ast)
{
    ast::Anys results {};
    ast->acceptChildren(*this, results);

    if (ast->lhs->type.has_value() && ast->rhs->type.has_value()) {
        auto lhsType { ast->lhs->type.value()->builtin };
        auto rhsType { ast->rhs->type.value()->builtin };

        auto isNumericOrBool = [](BuiltinType t) {
            return t==BuiltinType::Bool || t==BuiltinType::Byte || t==BuiltinType::Int ||
                   t==BuiltinType::Real || t==BuiltinType::Decimal;
        };

        auto numericResultType = [&](BuiltinType a, BuiltinType b) -> BuiltinType {
            if (a == BuiltinType::Bool && b == BuiltinType::Bool)
                return BuiltinType::Bool;
            if (a == BuiltinType::Decimal || b == BuiltinType::Decimal)
                return BuiltinType::Decimal;
            if (a == BuiltinType::Real || b == BuiltinType::Real)
                return BuiltinType::Real;
            return BuiltinType::Int;
        };

        switch(ast->op) {
            case ast::BinaryOp::Add:
            case ast::BinaryOp::Subtract:
            case ast::BinaryOp::Multiply:
            case ast::BinaryOp::Divide:
                if (ast->op == ast::BinaryOp::Add &&
                        (lhsType == BuiltinType::String || rhsType == BuiltinType::String)) {
                    ast->type = std::make_shared<Type>(BuiltinType::String);
                }
                else if (isNumericOrBool(lhsType) && isNumericOrBool(rhsType)) {
                    ast->type = std::make_shared<Type>(numericResultType(lhsType, rhsType));
                }
                break;
            case ast::BinaryOp::Modulo:
                if (isNumericOrBool(lhsType) && isNumericOrBool(rhsType))
                    ast->type = std::make_shared<Type>(BuiltinType::Int);
                break;
            case ast::BinaryOp::And:
            case ast::BinaryOp::Or:
                if (lhsType==BuiltinType::Bool && rhsType==BuiltinType::Bool)
                    ast->type = std::make_shared<Type>(BuiltinType::Bool);
                break;
            case ast::BinaryOp::Equal:
            case ast::BinaryOp::NotEqual:
            case ast::BinaryOp::LessThan:
            case ast::BinaryOp::GreaterThan:
            case ast::BinaryOp::LessOrEqual:
            case ast::BinaryOp::GreaterOrEqual:
                ast->type = std::make_shared<Type>(BuiltinType::Bool);
                break;
            default: break;
        }
    }

    return results;
}


std::any TypeDeducer::visit(ptr<ast::UnaryOp> ast)
{
    ast::Anys results {};
    ast->acceptChildren(*this, results);

    if (ast->arg->type.has_value()) {
        auto argType { ast->arg->type.value()->builtin };
        switch(ast->op) {
            case ast::UnaryOp::Not:
                ast->type = std::make_shared<Type>(BuiltinType::Bool);
                break;
            case ast::UnaryOp::Negate:
                if (argType==BuiltinType::Int || argType==BuiltinType::Byte)
                    ast->type = std::make_shared<Type>(BuiltinType::Int);
                else if (argType==BuiltinType::Real)
                    ast->type = std::make_shared<Type>(BuiltinType::Real);
                else if (argType==BuiltinType::Bool)
                    ast->type = std::make_shared<Type>(BuiltinType::Bool);
                break;
            case ast::UnaryOp::Accessor:
                break;
            default:
                break;
        }
    }

    return results;
}


std::any TypeDeducer::visit(ptr<ast::Variable> ast)
{
    auto info = lookupVar(ast->name);
    if (info.has_value() && info->type != nullptr)
        ast->type = info->type;
    return {};
}


std::any TypeDeducer::visit(ptr<ast::Call> ast)
{
    ast::Anys results {};
    ast->acceptChildren(*this, results);

    if (ast->callable->type.has_value()) {
        auto ctype { ast->callable->type.value() };
        if (ctype->builtin == BuiltinType::Func && ctype->func.has_value()) {
            auto f = ctype->func.value();
            if (!f.returnTypes.empty()) {
                if (f.returnTypes.size() == 1) {
                    ast->type = f.returnTypes[0];
                } else {
                    // Multiple return types - for now just use the first one
                    // TODO: implement proper multi-return type handling
                    ast->type = f.returnTypes[0];
                }
            }
        }
        else if (ctype->builtin == BuiltinType::Type) {
            auto typeLit = std::dynamic_pointer_cast<ast::Type>(ast->callable);
            if (typeLit != nullptr)
                ast->type = std::make_shared<Type>(typeLit->t);
        }
    }

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


std::any TypeDeducer::visit(ptr<ast::LambdaFunc> ast)
{
    ast::Anys results {};
    ast->acceptChildren(*this, results);

    if (ast->func->type.has_value())
        ast->type = ast->func->type;

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


std::any TypeDeducer::visit(ptr<ast::Vector> ast)
{
    ast::Anys results {};
    ast->acceptChildren(*this, results);
    ast->type = std::make_shared<Type>(BuiltinType::Vector);
    return results;
}


std::any TypeDeducer::visit(ptr<ast::Matrix> ast)
{
    ast::Anys results {};
    ast->acceptChildren(*this, results);
    ast->type = std::make_shared<Type>(BuiltinType::Matrix);
    return results;
}


std::any TypeDeducer::visit(ptr<ast::Dict> ast)
{
    ast::Anys results {};
    ast->acceptChildren(*this, results);
    ast->type = std::make_shared<Type>(BuiltinType::Dict);
    return results;
}
