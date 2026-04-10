

#include "TypeDeducer.h"


using namespace roxal;

using roxal::type::BuiltinType;
using roxal::type::Type;
using roxal::type::to_string;
using roxal::ast::TypeName;
using roxal::ast::joinTypeName;

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

    // In REPL mode, maintain persistent scope across lines
    if (replMode) {
        if (!replScopeInitialized) {
            pushScope(strictContext);
            replScopeInitialized = true;
        }
        ast->acceptChildren(*this, results);
        // Don't pop scope in REPL mode - it persists across lines
    } else {
        pushScope(strictContext); // module scope strictness can be overridden
        ast->acceptChildren(*this, results);
        popScope();
    }
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
    typeKindStack.push_back(ast->kind);
    ast->acceptChildren(*this, results);
    typeKindStack.pop_back();

    // if (ast->type.has_value())
    //     std::cout << toUTF8StdString(ast->name) << " : " <<  ast->type.value()->toString() << std::endl;

    if (ast->kind == ast::TypeDecl::Kind::Enumeration) {

        ptr<type::Type> underlyingType;
        if (ast->extends.has_value()) {
            auto extendsStr = toUTF8StdString(joinTypeName(ast->extends.value()));
            if (extendsStr == to_string(BuiltinType::Byte))
                underlyingType = make_ptr<type::Type>(BuiltinType::Byte);
            else if (extendsStr == to_string(BuiltinType::Int))
                underlyingType = make_ptr<type::Type>(BuiltinType::Int);
            else // TODO: consider allowing enums to extens other enums
                throw std::runtime_error("Enum(eration) "+toUTF8StdString(ast->name)+" cannot extend type " + extendsStr);
        }
        else // default to int
            underlyingType = make_ptr<type::Type>(BuiltinType::Int);

        // iterate over each enum label and
        //   * look at the type of it's expression (check it matches (or can match) the enum type)
        //   * if it has no expression, create an appropriate literal (e.g. incremented int)

        // TODO: since this is manipulating the AST, it should be done in a separate pass

        int32_t nextValue = 0;
        bool isByteEnum = underlyingType->builtin == BuiltinType::Byte;

        std::vector<std::pair<icu::UnicodeString, int32_t>> enumValues;

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
                    auto literalTtype = dynamic_ptr_cast<ast::Literal>(labelExpr)->literalType;
                    if (literalTtype != ast::Literal::LiteralType::Num) // TODO: check it isn't a real/double
                        throw std::runtime_error("Enum(eration)"+toUTF8StdString(ast->name)
                                +" label "+toUTF8StdString(enumLabel.first)+" must be a literal (byte, int)");

                    auto labeltype = labelExpr->type.value();
                    //std::cout << "type: " << labeltype->toString() << std::endl;

                    // set nextValue to the expression literal value
                    ptr<ast::Num> numExpr = dynamic_ptr_cast<ast::Num>(enumLabel.second);
                    nextValue = std::get<int>(numExpr->num); // incremented below
                }

            }
            else { // no expression supplied for enum label, assign next incremental according to type

                if (isByteEnum) { // values are 0-255
                    if (nextValue == 256)
                        throw std::runtime_error("Enum(eration)"+toUTF8StdString(ast->name)
                                +" value for "+toUTF8StdString(enumLabel.first)+" is out of range (>255 for byte enum)");
                }
                ptr<ast::Num> numExpr = make_ptr<ast::Num>();
                numExpr->num = nextValue;
                numExpr->type = make_ptr<type::Type>(BuiltinType::Int);
                enumLabel.second = numExpr;
            }
            enumValues.push_back({enumLabel.first, nextValue});
            nextValue++;
        }

        // Create the enum type with full information
        ptr<type::Type> enumType = make_ptr<type::Type>(BuiltinType::Enum);
        enumType->enumer = type::Type::EnumType{};
        enumType->enumer->name = ast->name;
        enumType->enumer->extends = underlyingType;
        enumType->enumer->values = enumValues;

        ast->type = enumType;

        // Register the enum type in the scope so 'with' statements can find it
        declareVar(ast->name, enumType, true);

    }
    else if (ast->kind == ast::TypeDecl::Kind::Object || ast->kind == ast::TypeDecl::Kind::Actor) {
        // Create object/actor type with full information
        BuiltinType typeKind = (ast->kind == ast::TypeDecl::Kind::Object) ? BuiltinType::Object : BuiltinType::Actor;
        ptr<type::Type> objType = make_ptr<type::Type>(typeKind);
        objType->obj = type::Type::ObjectType{};
        objType->obj->name = ast->name;

        // Handle extends
        if (ast->extends.has_value()) {
            // For now, create a placeholder - full type resolution would require lookups
            ptr<type::Type> extendsType = make_ptr<type::Type>(BuiltinType::Object);
            objType->obj->extends = extendsType;
        }

        // Register properties
        for (const auto& prop : ast->properties) {
            type::Type::ObjectType::PropType propType;
            propType.name = prop->name;
            propType.nameHashCode = prop->name.hashCode();
            // Set type if available
            if (prop->varType.has_value() && std::holds_alternative<BuiltinType>(prop->varType.value())) {
                propType.type = make_ptr<type::Type>(std::get<BuiltinType>(prop->varType.value()));
            }
            propType.hasDefault = prop->initializer.has_value();
            objType->obj->properties.push_back(propType);
        }

        // Register property accessors (Phase 5)
        for (const auto& propAccessor : ast->propertyAccessors) {
            type::Type::ObjectType::PropType propType;
            propType.name = propAccessor->name;
            propType.nameHashCode = propAccessor->name.hashCode();

            // Set type if available
            if (std::holds_alternative<BuiltinType>(propAccessor->propType)) {
                propType.type = make_ptr<type::Type>(std::get<BuiltinType>(propAccessor->propType));
            }
            // TODO: handle custom type identifiers (non-builtin types)

            propType.hasDefault = propAccessor->initializer.has_value();

            // Set accessor flags
            propType.hasGetter = propAccessor->getter.has_value();
            propType.hasSetter = propAccessor->setter.has_value();

            // Validate at least one accessor is present
            if (!propType.hasGetter && !propType.hasSetter) {
                throw std::runtime_error("Property accessor '" + toUTF8StdString(propAccessor->name) +
                                       "' must have at least one get or set block");
            }

            objType->obj->properties.push_back(propType);
        }

        // Register methods
        for (const auto& method : ast->methods) {
            if (method->name.has_value()) {
                // Create a basic function type for the method
                ptr<type::Type::FuncType> methodType = make_ptr<type::Type::FuncType>();
                methodType->isProc = method->isProc;
                objType->obj->methods.emplace_back(method->name.value(), methodType);
            }
        }

        ast->type = objType;

        // Register the object/actor type in the scope so 'with' statements can find it
        declareVar(ast->name, objType, true);
    }

    return results;
}


std::any TypeDeducer::visit(ptr<ast::FuncDecl> ast)
{
    ast::Anys results {};
    // propagate annotations to the underlying Function node so visitors
    // like visit(Function) can inspect them. Don't discard any annotations
    // that may already be attached to the Function (e.g. a docstring
    // converted during AST generation).
    ast->func->annotations.insert(ast->func->annotations.end(),
                                  ast->annotations.begin(),
                                  ast->annotations.end());
    ast->acceptChildren(*this, results);

    // for completeness/convenience, set function type here too
    ast->type = ast->func->type;
    return results;
}


std::any TypeDeducer::visit(ptr<ast::VarDecl> ast)
{
    ast::Anys results {};
    ast->acceptChildren(*this, results);
    if (ast->isConst && !ast->initializer.has_value()) {
        throw std::logic_error(linePos(ast) + " - const declarations require an initializer");
    }
    if (ast->varType.has_value()) {
        if (std::holds_alternative<BuiltinType>(ast->varType.value())) {
            ast->type = make_ptr<type::Type>(std::get<BuiltinType>(ast->varType.value()));
        } else if (std::holds_alternative<TypeName>(ast->varType.value())) {
            // Custom type (like Widget, MyClass, etc.) or runtime type variable
            auto typeName = joinTypeName(std::get<TypeName>(ast->varType.value()));
            auto typeInfo = lookupVar(typeName);
            if (typeInfo.has_value() && typeInfo->type != nullptr) {
                // Only use as compile-time type if it's not a runtime type variable
                // (runtime type variables have type BuiltinType::Type)
                if (typeInfo->type->builtin != BuiltinType::Type) {
                    ast->type = typeInfo->type;
                }
            }
            // If not found or is a runtime type variable, type remains unset
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

std::any TypeDeducer::visit(ptr<ast::PropertyAccessor> ast)
{
    ast::Anys results {};

    // Visit children (initializer, getter body, setter body) for type deduction
    ast->acceptChildren(*this, results);

    // Set the type of the PropertyAccessor itself based on propType
    if (std::holds_alternative<BuiltinType>(ast->propType)) {
        ast->type = make_ptr<type::Type>(std::get<BuiltinType>(ast->propType));
    }
    // TODO: handle custom type identifiers

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

std::any TypeDeducer::visit(ptr<ast::WhenStatement> ast)
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

std::any TypeDeducer::visit(ptr<ast::MatchStatement> ast)
{
    ast::Anys results {};
    ast->acceptChildren(*this, results);
    // TODO: Add type checking for match patterns and exhaustiveness checking for enums
    return results;
}

std::any TypeDeducer::visit(ptr<ast::WithStatement> ast)
{
    ast::Anys results {};

    // Visit the context expression first to deduce its type
    results.push_back(ast->contextExpr->accept(*this));

    // Determine the kind of with context based on the expression type
    if (ast->contextExpr->type.has_value()) {
        auto exprType = ast->contextExpr->type.value();

        switch (exprType->builtin) {
            case BuiltinType::Enum:
                // Enum type - brings enum labels into scope
                ast->contextKind = ast::WithStatement::EnumType;
                ast->contextType = exprType;
                break;

            case BuiltinType::Object:
                // Object instance - brings object members into scope
                ast->contextKind = ast::WithStatement::ObjectType;
                ast->contextType = exprType;
                break;

            case BuiltinType::Actor:
                // Actor instance - brings actor members into scope
                ast->contextKind = ast::WithStatement::ActorType;
                ast->contextType = exprType;
                break;

            default:
                // Other types don't support with statement
                throw std::logic_error(linePos(ast) +
                    " - with statement requires enum type or object/actor instance, got " +
                    to_string(exprType->builtin));
        }
    } else {
        // Cannot determine type at compile time
        throw std::logic_error(linePos(ast) +
            " - with statement context type cannot be determined at compile time");
    }

    // Visit the body with the context available
    results.push_back(ast->body->accept(*this));

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
            ptr<Type> ptype = make_ptr<Type>(std::get<BuiltinType>(param->type.value()));
            declareVar(param->name, ptype, /*explicit*/true);
        }
    }

    ast->acceptChildren(*this, results);

    ptr<Type> type = make_ptr<Type>();
    type->builtin = BuiltinType::Func;
    type->func = Type::FuncType();
    type->func->isProc = ast->isProc;
    if (ast->returnTypes.has_value()) {
        auto& returnTypes = ast->returnTypes.value();
        for (size_t ri = 0; ri < returnTypes.size(); ri++) {
            ptr<Type> retType;
            if (std::holds_alternative<BuiltinType>(returnTypes[ri])) {
                retType = make_ptr<Type>(std::get<BuiltinType>(returnTypes[ri]));
            }
            else if (std::holds_alternative<TypeName>(returnTypes[ri])) {
                // lookup name - for now create a placeholder
                // TODO: implement proper name lookup
                retType = make_ptr<Type>(BuiltinType::Object);
            }
            // Mark return type as const if explicitly qualified (-> const T).
            // Default is mutable (isConst=false). Only -> const T freezes at actor boundary.
            if (retType && ri < ast->returnTypeConst.size() && ast->returnTypeConst[ri])
                retType->isConst = true;
            type->func->returnTypes.push_back(retType);
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
                paramType.type = make_ptr<Type>(std::get<BuiltinType>(param->type.value()));
                if (param->isConst || (inActorScope() && !param->isMutable))
                    paramType.type.value()->isConst = true;
                paramType.hasDefault = param->defaultValue.has_value();
                paramType.variadic = param->variadic;
                type->func.value().params[i] = paramType;
            }
            else if (std::holds_alternative<TypeName>(param->type.value())) {
                // Named type (user-defined object/actor) — use Object as placeholder
                // builtin type with the type name stored in obj for runtime resolution.
                Type::FuncType::ParamType paramType {};
                paramType.name = param->name;
                paramType.nameHashCode = param->name.hashCode();
                paramType.type = make_ptr<Type>(BuiltinType::Object);
                paramType.type.value()->obj = Type::ObjectType{};
                paramType.type.value()->obj->name = joinTypeName(std::get<TypeName>(param->type.value()));
                if (param->isConst || (inActorScope() && !param->isMutable))
                    paramType.type.value()->isConst = true;
                paramType.hasDefault = param->defaultValue.has_value();
                paramType.variadic = param->variadic;
                type->func.value().params[i] = paramType;
            }
        }
        else {
            Type::FuncType::ParamType paramType {};
            paramType.name = param->name;
            paramType.nameHashCode = param->name.hashCode();
            paramType.hasDefault = param->defaultValue.has_value();
            paramType.variadic = param->variadic;
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
        static_cast<ast::AST*>(ast.get())->type = make_ptr<Type>(std::get<BuiltinType>(ast->type.value()));
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
    if (dynamic_ptr_cast<ast::Variable>(ast->lhs) != nullptr) {
        auto vname = dynamic_ptr_cast<ast::Variable>(ast->lhs)->name;
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

        bool signalArg = lhsType == BuiltinType::Signal || rhsType == BuiltinType::Signal;
        bool supportsSignal = false;

        switch(ast->op) {
            case ast::BinaryOp::Add:
            case ast::BinaryOp::Subtract:
            case ast::BinaryOp::Multiply:
            case ast::BinaryOp::Divide:
            case ast::BinaryOp::Modulo:
            case ast::BinaryOp::And:
            case ast::BinaryOp::Or:
            case ast::BinaryOp::BitAnd:
            case ast::BinaryOp::BitOr:
            case ast::BinaryOp::BitXor:
            case ast::BinaryOp::Equal:
            case ast::BinaryOp::NotEqual:
            case ast::BinaryOp::LessThan:
            case ast::BinaryOp::GreaterThan:
            case ast::BinaryOp::LessOrEqual:
            case ast::BinaryOp::GreaterOrEqual:
                supportsSignal = true;
                break;
            default:
                break;
        }

        if (supportsSignal && signalArg) {
            ast->type = make_ptr<Type>(BuiltinType::Signal);
            return results;
        }

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
                    ast->type = make_ptr<Type>(BuiltinType::String);
                }
                else if (isNumericOrBool(lhsType) && isNumericOrBool(rhsType)) {
                    ast->type = make_ptr<Type>(numericResultType(lhsType, rhsType));
                }
                break;
            case ast::BinaryOp::Modulo:
                if (isNumericOrBool(lhsType) && isNumericOrBool(rhsType))
                    ast->type = make_ptr<Type>(BuiltinType::Int);
                break;
            case ast::BinaryOp::And:
            case ast::BinaryOp::Or:
                if (lhsType==BuiltinType::Bool && rhsType==BuiltinType::Bool)
                    ast->type = make_ptr<Type>(BuiltinType::Bool);
                break;
            case ast::BinaryOp::In:
            case ast::BinaryOp::NotIn:
                ast->type = make_ptr<Type>(BuiltinType::Bool);
                break;
            case ast::BinaryOp::Equal:
            case ast::BinaryOp::NotEqual:
            case ast::BinaryOp::LessThan:
            case ast::BinaryOp::GreaterThan:
            case ast::BinaryOp::LessOrEqual:
            case ast::BinaryOp::GreaterOrEqual:
                // Tensor comparisons return a tensor; scalar comparisons return bool
                if (lhsType == BuiltinType::Tensor || rhsType == BuiltinType::Tensor)
                    ast->type = make_ptr<Type>(BuiltinType::Tensor);
                else
                    ast->type = make_ptr<Type>(BuiltinType::Bool);
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

        bool isSignalArg = argType == BuiltinType::Signal;
        bool supportsSignal = false;

        switch(ast->op) {
            case ast::UnaryOp::Negate:
            case ast::UnaryOp::Not:
            case ast::UnaryOp::BitNot:
                supportsSignal = true;
                break;
            default:
                break;
        }

        if (supportsSignal && isSignalArg) {
            ast->type = make_ptr<Type>(BuiltinType::Signal);
        } else {
            switch(ast->op) {
                case ast::UnaryOp::Not:
                    ast->type = make_ptr<Type>(BuiltinType::Bool);
                    break;
                case ast::UnaryOp::Negate:
                    if (argType==BuiltinType::Int || argType==BuiltinType::Byte)
                        ast->type = make_ptr<Type>(BuiltinType::Int);
                    else if (argType==BuiltinType::Real)
                        ast->type = make_ptr<Type>(BuiltinType::Real);
                    else if (argType==BuiltinType::Bool)
                        ast->type = make_ptr<Type>(BuiltinType::Bool);
                    break;
                case ast::UnaryOp::BitNot:
                    if (argType==BuiltinType::Bool)
                        ast->type = make_ptr<Type>(BuiltinType::Bool);
                    else if (argType==BuiltinType::Byte || argType==BuiltinType::Int)
                        ast->type = make_ptr<Type>(BuiltinType::Int);
                    break;
                case ast::UnaryOp::Accessor:
                    break;
                default:
                    break;
            }
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
            auto typeLit = dynamic_ptr_cast<ast::Type>(ast->callable);
            if (typeLit != nullptr)
                ast->type = make_ptr<Type>(typeLit->t);
        }
        else if (ctype->builtin == BuiltinType::Object || ctype->builtin == BuiltinType::Actor) {
            // Object/Actor constructor call - return an instance of that type
            ast->type = ctype;
        }
        else if (ctype->builtin == BuiltinType::Enum) {
            // Enum constructor call - return an instance of that enum
            ast->type = ctype;
        }
    }

    return results;
}


std::any TypeDeducer::visit(ptr<ast::Range> ast)
{
    ast::Anys results {};
    ast->acceptChildren(*this, results);

    ast->type = make_ptr<Type>(BuiltinType::Range);

    return results;
}


std::any TypeDeducer::visit(ptr<ast::Index> ast)
{
    ast::Anys results {};
    ast->acceptChildren(*this, results);

    // if the indexable is a string, indexing yields a string also
    if (ast->indexable->type.has_value()) {
        if (ast->indexable->type.value()->builtin == BuiltinType::String)
            ast->type = make_ptr<Type>(BuiltinType::String);
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
        ast->type = make_ptr<Type>(BuiltinType::Nil);
    return {};
}


std::any TypeDeducer::visit(ptr<ast::Bool> ast)
{
    ast->type = make_ptr<Type>(BuiltinType::Bool);
    return {};
}


std::any TypeDeducer::visit(ptr<ast::Str> ast)
{
    ast->type = make_ptr<Type>(BuiltinType::String);
    return {};
}


std::any TypeDeducer::visit(ptr<ast::Type> ast)
{
    ast->type = make_ptr<Type>(BuiltinType::Type);
    return {};
}


std::any TypeDeducer::visit(ptr<ast::Num> ast)
{
    if (std::holds_alternative<int32_t>(ast->num))
        ast->type = make_ptr<Type>(BuiltinType::Int);
    else if (std::holds_alternative<int64_t>(ast->num))
        ast->type = make_ptr<Type>(BuiltinType::Int);
    else if (std::holds_alternative<double>(ast->num))
        ast->type = make_ptr<Type>(BuiltinType::Real);
    else
        throw std::runtime_error("Unhandled Num literal type");
    return {};
}


std::any TypeDeducer::visit(ptr<ast::SuffixedNum> ast)
{
    // Type depends on suffix function return type; resolved by compiler
    // For now, leave type unset
    return {};
}

std::any TypeDeducer::visit(ptr<ast::SuffixedStr> ast)
{
    // Type depends on suffix function return type; resolved by compiler
    return {};
}


std::any TypeDeducer::visit(ptr<ast::List> ast)
{
    ast::Anys results {};
    ast->acceptChildren(*this, results);
    ast->type = make_ptr<Type>(BuiltinType::List);
    return results;
}


std::any TypeDeducer::visit(ptr<ast::Vector> ast)
{
    ast::Anys results {};
    ast->acceptChildren(*this, results);
    ast->type = make_ptr<Type>(BuiltinType::Vector);
    return results;
}


std::any TypeDeducer::visit(ptr<ast::Matrix> ast)
{
    ast::Anys results {};
    ast->acceptChildren(*this, results);
    ast->type = make_ptr<Type>(BuiltinType::Matrix);
    return results;
}


std::any TypeDeducer::visit(ptr<ast::Dict> ast)
{
    ast::Anys results {};
    ast->acceptChildren(*this, results);
    ast->type = make_ptr<Type>(BuiltinType::Dict);
    return results;
}
