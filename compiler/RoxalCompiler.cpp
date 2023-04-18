

#include <boost/algorithm/string/replace.hpp>

#include <core/common.h>

#include "Object.h"

#include "ASTGenerator.h"
#include "TypeDeducer.h"

#include "RoxalCompiler.h"

using namespace roxal;
using namespace roxal::ast;





// is ptr<P> p down-castable to ptr<C> where C is a subclass of P (or the same class)?
template<typename P, typename C>
bool isa(ptr<P> p) {
    if (p==nullptr) return false;
    return std::dynamic_pointer_cast<C>(p)!=nullptr;
}

template<typename C>
bool isa(ptr<AST> p) {
    if (p==nullptr) return false;
    return std::dynamic_pointer_cast<C>(p)!=nullptr;
}

// down-cast ptr<P> p to ptr<C> where C is a subclass of P (or the same class)
template<typename P, typename C>
ptr<C> as(ptr<P> p) {
    if (!isa<P,C>(p))
        throw std::runtime_error("Can't cast ptr<"+demangle(typeid(*p).name())+"> to ptr<"+demangle(typeid(C).name())+">");
    return std::dynamic_pointer_cast<C>(p);
}

template<typename C>
ptr<C> as(ptr<AST> p) {
    if (!isa<AST,C>(p))
        throw std::runtime_error("Can't cast ptr<"+demangle(typeid(*p).name())+"> to ptr<"+demangle(typeid(C).name())+">");
    return std::dynamic_pointer_cast<C>(p);
}


RoxalCompiler::RoxalCompiler() 
    : outputBytecodeDissasembly(false) 
{}



ObjFunction* RoxalCompiler::compile(std::istream& source, const std::string& name)
{
    ObjFunction* function { nullptr };

    ptr<ast::AST> ast {};
    try {
        ASTGenerator astGenerator {};
        ast = astGenerator.ast(source, name);
    } catch (std::exception& e) {
        std::cout << "Exception in parsing - " << std::string(e.what()) << std::endl; 
        return function;
    }

    if (!isa<File>(ast))
        throw std::runtime_error("ASTGenerator root node is not a File");

    try {
        TypeDeducer typeDeducer {};
        typeDeducer.visit(as<File>(ast));
    } catch (std::exception& e) {
        std::cout << "Exception during type inference - " << std::string(e.what()) << std::endl; 
        return function;
    }


    #if defined(DEBUG_OUTPUT_PARSE_TREE)
    std::cout << "== parse tree ==" << std::endl << ast << std::endl;
    #endif


    if (ast != nullptr) {

        auto moduleType { std::make_shared<type::Type>(BuiltinType::Func) };
        moduleType->func = type::Type::FuncType();

        funcScopes.push_back(FunctionScope(toUnicodeString(name), FunctionType::Module, moduleType));

        funcScope()->strict = false;

        try {
            auto file = as<File>(ast);

            file->accept(*this);
            
            function = funcScope()->function;

            if (outputBytecodeDissasembly)
                funcScope()->function->chunk->disassemble(funcScope()->function->name);

            //std::cout << "value:" << value->repr() << std::endl;
        } catch (std::logic_error& e) {
            funcScopes.pop_back();
            if (function != nullptr)
                delObj(function);
            std::cout << std::string("Compile error: ") << e.what() << std::endl;
            return nullptr;
        } catch (std::exception& e) {
            funcScopes.pop_back();
            if (function != nullptr)
                delObj(function);
            std::cout << std::string("Exception: ") << e.what() << std::endl;
            throw e;
        } 

        funcScopes.pop_back();
        
        //std::cout << "\n" << interpreter.stackAsString(false) << std::endl;
    }
    
    return function;
}


void RoxalCompiler::setOutputBytecodeDissasembly(bool outputBytecodeDissasembly)
{
    this->outputBytecodeDissasembly = outputBytecodeDissasembly;
}


ASTVisitor::TraversalOrder RoxalCompiler::traversalOrder() const
{
    // we don't want AST implemented pre- or post-order tree traversal.
    //  instead, we'll dictate the traversal order by explicitly calling visit() on children
    return TraversalOrder::VisitorDetermined;
}






void RoxalCompiler::visit(ptr<ast::File> ast)
{
    currentNode = ast;
    ast->acceptChildren(*this);
    emitReturn();
}


void RoxalCompiler::visit(ptr<ast::SingleInput> ast)
{
    currentNode = ast;
    ast->acceptChildren(*this);
}


void RoxalCompiler::visit(ptr<ast::Annotation> ast)
{
    currentNode = ast;
    // currently, we don't generate any code for annotations
    //ast->acceptChildren(*this);
}


void RoxalCompiler::visit(ptr<ast::TypeDecl> ast)
{
    currentNode = ast;

    bool isActor = ast->kind==TypeDecl::Actor;

    int16_t typeNameConstant = identifierConstant(ast->name);
    declareVariable(ast->name);    

    if (ast->implements.size()>2)
        throw std::runtime_error("Multiple implements types unimplemented.");

    emitBytes(isActor ? OpCode::ActorType : OpCode::ObjectType, typeNameConstant);
    defineVariable(typeNameConstant);

    namedVariable(ast->name, false); // make type accessible on the stack

    typeScopes.push_back(TypeScope());


    for(size_t i=0; i<ast->properties.size(); i++) {
        if (isActor) {
            // TODO: once we have private properties, allow those and allow them
            //  to be accessible by the actor's methods, but not from other threads
            error("Actors cannot declare shared properties");
            break;
        }

        // emit code to push type & initial value (if any) on stack, then OpCode::Property

        ptr<VarDecl> prop { ast->properties.at(i) }; 

        auto propName { prop->name };
        int16_t propNameConstant = identifierConstant(propName);
        if (propNameConstant >= 255) 
            error("Too many properties for one actor or object type.");
        
        // type
        if (prop->varType.has_value()) {
            auto varType { prop->varType.value() };

            if (std::holds_alternative<BuiltinType>(varType)) {
                auto builtinType { std::get<BuiltinType>(varType) };
                Value typeValue { typeSpecVal(builtinToValueType(builtinType)) };

                emitConstant(typeValue, "prop "+toUTF8StdString(propName)+" type");
            }
            else { // assume string names global (local?) type var
                // will emit GetLocal or GetGlobal (or GetUpValue)
                namedVariable( std::get<icu::UnicodeString>(varType), false );
            }

        }
        else {
            emitByte(OpCode::ConstNil, "prop "+toUTF8StdString(propName)+" (no type)"); // nil value will be interpreted as no type (or any type)
        }

        // initial value
        if (prop->initializer.has_value()) {
            prop->initializer.value()->accept(*this);
        }
        else { // no initializer
            bool declaredBuiltinType = prop->varType.has_value() && std::holds_alternative<BuiltinType>(prop->varType.value());
            if (declaredBuiltinType)
                emitConstant(defaultValue(builtinToValueType(std::get<BuiltinType>(prop->varType.value()))));
            else
                emitByte(OpCode::ConstNil);
        }

        emitBytes(OpCode::Property, uint8_t(propNameConstant), "property "+toUTF8StdString(propName));
    }

    for(size_t i=0; i<ast->methods.size(); i++) {

        auto func { ast->methods.at(i) };

        auto methodName { func->name };
        int16_t methodNameConstant = identifierConstant(methodName);
        if (methodNameConstant >= 255) 
            error("Too many methods for one actor or object type.");

        func->accept(*this);

        emitBytes(OpCode::Method, uint8_t(methodNameConstant), "method "+toUTF8StdString(methodName));
    }

    emitByte(OpCode::Pop, "type name");

    typeScopes.pop_back();
}


void RoxalCompiler::visit(ptr<ast::FuncDecl> ast)
{
    currentNode = ast;    

    auto name { as<Function>(ast->func)->name };

    declareVariable(name);

    uint16_t var { 0 };
    if (funcScope()->scopeDepth == 0) // global variable
        var = identifierConstant(name); // create constant table entry for name

    if (funcScope()->scopeDepth > 0) {
        // mark initialized
        funcScope()->locals.back().depth = funcScope()->scopeDepth;
    }

    ast->acceptChildren(*this);

    defineVariable(var);
}


void RoxalCompiler::visit(ptr<ast::VarDecl> ast)
{
    currentNode = ast;

    declareVariable(ast->name);
    uint16_t var { 0 };
    if (funcScope()->scopeDepth == 0) // global variable
        var = identifierConstant(ast->name); // create constant table entry for name

    // TODO: support type spec 

    if (ast->initializer.has_value()) {
        ast->initializer.value()->accept(*this);
    }
    else
        emitByte(OpCode::ConstNil);

    defineVariable(var);
}


void RoxalCompiler::visit(ptr<ast::Suite> ast)
{
    currentNode = ast;

    beginScope();
    ast->acceptChildren(*this);
    endScope();
}


void RoxalCompiler::visit(ptr<ast::ExpressionStatement> ast)
{
    currentNode = ast;
    ast->acceptChildren(*this);

    // expressions leave their value on the stack, but statements don't
    //  have a value, so discard it
    emitByte(OpCode::Pop, "expr_stmt value");
}


void RoxalCompiler::visit(ptr<ast::ReturnStatement> ast)
{
    currentNode = ast;

    ast->acceptChildren(*this);

    if (ast->expr.has_value()) {

        if (funcScope()->functionType == FunctionType::Initializer)
            error("A value cannot be returned from an 'init' method.");
        if (funcScope()->type->func.has_value() && funcScope()->type->func.value().isProc)
            error("A value cannot be returned from a proc method.");

        emitByte(OpCode::Return);
    }
    else        
        emitReturn();
}


void RoxalCompiler::visit(ptr<ast::IfStatement> ast)
{
    currentNode = ast;

    // (first) if condition 
    ast->conditionalSuites.at(0).first->accept(*this);

    auto jumpOverIf = emitJump(OpCode::JumpIfFalse);
    emitByte(OpCode::Pop, "if cond");

    beginScope();
    ast->conditionalSuites.at(0).second->accept(*this);
    endScope();

    auto jumpOverElse = emitJump(OpCode::Jump);

    patchJump(jumpOverIf);

    if (ast->conditionalSuites.size()>1) {
        throw std::runtime_error("elseif unimplemented");
        // for(int i=1; i<context->expression().size();i++) {
        //     visitExpression(context->expression().at(i));
        //     visitSuite(context->suite().at(i));
        // }
    }

    emitByte(OpCode::Pop, "if cond");
    if (ast->elseSuite.has_value()) {
        beginScope();
        ast->elseSuite.value()->accept(*this);
        endScope();
    }
    
    patchJump(jumpOverElse);
}


void RoxalCompiler::visit(ptr<ast::WhileStatement> ast)
{
    currentNode = ast;

    auto loopStart = currentChunk()->code.size();

    // while condition 
    ast->condition->accept(*this);

    auto jumpToExit = emitJump(OpCode::JumpIfFalse);
    emitByte(OpCode::Pop, "while cond");

    ast->body->accept(*this);

    emitLoop(loopStart);

    patchJump(jumpToExit);
    emitByte(OpCode::Pop, "while cond");
}


void RoxalCompiler::visit(ptr<ast::Function> ast)
{
    currentNode = ast;
    
    bool isProc = ast->isProc;
    bool isMethod = !typeScopes.empty();

    bool isInitializer = isMethod && (ast->name == "init");

    if (isInitializer && !isProc)
        error("object or actor type 'init' method must be a proc.");

    FunctionType ftype = isMethod ? 
                              (isInitializer ? FunctionType::Initializer : FunctionType::Method)
                            : FunctionType::Function;

    assert(ast->type.has_value());
    funcScopes.push_back(FunctionScope(ast->name, ftype, ast->type.value()));

    #ifdef DEBUG_BUILD
    emitByte(OpCode::Nop, "func "+toUTF8StdString(ast->name));
    #endif
    beginScope();

    funcScope()->function->arity = ast->params.size();
    if (funcScope()->function->arity > 255)
        error("Maximum of function or procedure 255 parameters exceeded.");

    ast->acceptChildren(*this);

    //endScope(); // state scope about to be discarded, not needed

    emitReturn();

    if (outputBytecodeDissasembly)
        funcScope()->function->chunk->disassemble(funcScope()->function->name);

    ObjFunction* function = funcScope()->function;
    auto functionScope { *funcScope() };

    funcScopes.pop_back(); // back to surrpounding function

//!!!
// std::cout << "Closure " << toUTF8StdString(function->name) << ": #" << function->upvalueCount << std::endl;//!!!
// std::cout << "   #" << functionState.upvalues.size() << std::endl;
//!!!
    emitBytes(OpCode::Closure, makeConstant(objVal(function)));
    for (int i = 0; i < function->upvalueCount; i++) {
        #ifdef DEBUG_BUILD
        if (i >= functionScope.upvalues.size())
            throw std::runtime_error("invalid upvalue index");
        #endif
//std::cout << "    - " << int(functionState.upvalues[i].index) << " " << std::string(functionState.upvalues[i].isLocal ?"local":"nonlocal") << std::endl;        
        emitByte(functionScope.upvalues[i].isLocal ? 1 : 0);
        emitByte(functionScope.upvalues[i].index);
    }
}


void RoxalCompiler::visit(ptr<ast::Parameter> ast)
{
    currentNode = ast;

    // TODO: handle optional type

    declareVariable(ast->name);
    uint16_t var = identifierConstant(ast->name); // create constant table entry for name

    defineVariable(var);

    // output code for evaluating default value (if any)
    if (ast->defaultValue.has_value()) {

        // treat like another func decl
        auto defFuncType { std::make_shared<type::Type>(BuiltinType::Func) };
        defFuncType->func = type::Type::FuncType();
        // TODO: specify return type? (necessary?)

        funcScopes.push_back(FunctionScope(ast->name, FunctionType::Function, defFuncType));

        #ifdef DEBUG_BUILD
        emitByte(OpCode::Nop, "param_def "+toUTF8StdString(ast->name));
        #endif
        beginScope();

        funcScope()->function->arity = 0;


        ast->acceptChildren(*this);

        //endScope(); // state scope about to be discarded, not needed

        // since this closure was called directly by being queued by OpCode::Call
        //  rather than through byte code pushing the callable/closure, we
        //  need get the return value copied into the placeholder arg slots in the parent frame
        //  rather than leaving it on the stack
        emitByte(OpCode::ReturnStore);

        if (outputBytecodeDissasembly)
            funcScope()->function->chunk->disassemble(funcScope()->function->name);

        ObjFunction* function = funcScope()->function;

        funcScopes.pop_back(); // back to surrpounding function

        // store the func that evaluates the default param value in the function
        //  for which it is a param
        funcScope()->function->paramDefaultFunc[ast->name.hashCode()] = function;
    }
}


void RoxalCompiler::visit(ptr<ast::Assignment> ast)
{
    currentNode = ast;
    
    if (isa<Variable>(ast->lhs)) {

        ast->rhs->accept(*this);

        // we don't visit the LHS Variable, since that will emit code to evaluate it

        auto name { as<Variable>(ast->lhs)->name };

        namedVariable(name, /*assign=*/true);
    }
    else if (isa<UnaryOp>(ast->lhs) && as<UnaryOp>(ast->lhs)->op==UnaryOp::Accessor) {
        auto accessor = as<UnaryOp>(ast->lhs);
        // visit the lhs of the accessor operator to generate code to evaluate it 
        //  (so we don't evaluate the access, since we want to set the member, not get it)
        accessor->arg->accept(*this);

        if (!accessor->member.has_value())
            throw std::runtime_error("accessor unary operator expects member name");
        int16_t propName = identifierConstant(accessor->member.value());

        ast->rhs->accept(*this);

        emitBytes(OpCode::SetProp, propName);
    }
    else
        throw std::runtime_error("Unimplemented kind of LHS for assignment");
}


void RoxalCompiler::visit(ptr<ast::BinaryOp> ast)
{
    currentNode = ast;

    // Logical And and Or operators have short-circuit semantics, so may not need to evaluate all
    //  children, so handle them differently
    if (ast->op == BinaryOp::Or) {
        ast->lhs->accept(*this);

        Chunk::size_type jumpToEnd = emitJump(OpCode::JumpIfTrue);
        emitByte(OpCode::Pop);

        ast->rhs->accept(*this);

        patchJump(jumpToEnd);
    }
    else if (ast->op == BinaryOp::And) {
        ast->lhs->accept(*this);
        Chunk::size_type jumpToEnd = emitJump(OpCode::JumpIfFalse);
        emitByte(OpCode::Pop);

        ast->rhs->accept(*this);

        patchJump(jumpToEnd);
    }
    else {
        ast->acceptChildren(*this);

        switch (ast->op) {
            case BinaryOp::Add: emitByte(OpCode::Add); break;
            case BinaryOp::Subtract: emitByte(OpCode::Subtract); break;
            case BinaryOp::Multiply: emitByte(OpCode::Multiply); break;
            case BinaryOp::Divide: emitByte(OpCode::Divide); break;
            case BinaryOp::Equal: emitByte(OpCode::Equal); break;
            case BinaryOp::NotEqual: emitByte(OpCode::Equal); emitByte(OpCode::Negate); break;
            case BinaryOp::Modulo: emitByte(OpCode::Modulo); break;
            case BinaryOp::LessThan: emitByte(OpCode::Less); break;
            case BinaryOp::GreaterThan: emitByte(OpCode::Greater); break;
            case BinaryOp::LessOrEqual: emitByte(OpCode::Greater); emitByte(OpCode::Negate); break;
            case BinaryOp::GreaterOrEqual: emitByte(OpCode::Less); emitByte(OpCode::Negate); break;
            case BinaryOp::FollowedBy: emitByte(OpCode::FollowedBy); break;
            default:
                throw std::runtime_error("unimplemented binary opertor:"+ast->opString());
        }
    }
}


void RoxalCompiler::visit(ptr<ast::UnaryOp> ast)
{
    currentNode = ast;
    ast->acceptChildren(*this);

    switch (ast->op) {
        case UnaryOp::Negate: emitByte(OpCode::Negate); break;
        case UnaryOp::Not: emitByte(OpCode::Negate); break;
        case UnaryOp::Accessor: {
            if (!ast->member.has_value())
                throw std::runtime_error("Accessor . required member name");

            int16_t identConstant = identifierConstant(ast->member.value());
            emitBytes(OpCode::GetProp, identConstant);
        } break;
        default:
            throw std::runtime_error("unimplemented unary opertor:"+ast->opString());
    }
}


void RoxalCompiler::visit(ptr<ast::Variable> ast)
{
    currentNode = ast;
    namedVariable(ast->name);
}


void RoxalCompiler::visit(ptr<ast::Call> ast)
{
    currentNode = ast;

    ast->acceptChildren(*this);

    auto argCount = ast->args.size();
    if (argCount > 127)
        error("Number of call parameters is limited to 127");

    //
    // create call param spec
    CallSpec callSpec {};

    if (!ast->namedArgs()) {
        // only positional arguments
        callSpec.allPositional = true;
        callSpec.argCount = ast->args.size();
    }
    else {
        #ifdef DEBUG_BUILD
        // keep track of hashes to check for collisions
        std::map<UnicodeString,uint16_t> hashes {};
        #endif
        // mix of positional & named args
        callSpec.allPositional = false;
        callSpec.argCount = ast->args.size();
        for(const auto& arg : ast->args) {
            CallSpec::ArgSpec aspec {};
            if (arg.first.isEmpty())
                aspec.positional = true;
            else {
                aspec.positional = false;
                // 15bits of param name string hash
                aspec.paramNameHash =0x8000 | (arg.first.hashCode() & 0x7fff);
                #ifdef DEBUG_BUILD
                hashes[arg.first] = aspec.paramNameHash; 
                #endif
            }
            callSpec.args.push_back(aspec);
        }
        #ifdef DEBUG_BUILD
        // if any hash collisions occurs, the size of the set of hashes
        //  won't match the arg count
        std::set<uint16_t> hashSet;
        for(auto const& hash: hashes)
            hashSet.insert(hash.second);
        if (hashSet.size() != hashes.size())
            throw std::runtime_error("Hash collision occured between two argument names");
        #endif
    }

    auto bytes = callSpec.toBytes();
    if (bytes.size()==1)
        emitBytes(OpCode::Call, bytes[0]);
    else {
        emitByte(OpCode::Call);
        for(auto i=0; i<bytes.size();i++)
            emitByte(bytes[i]);
    }
}


void RoxalCompiler::visit(ptr<ast::Index> ast)
{
    currentNode = ast;
    ast->acceptChildren(*this);

    auto argCount = ast->args.size();
    if (argCount > 255)
        error("Number of indices is limited to 255");
    emitBytes(OpCode::Index, argCount);
}


void RoxalCompiler::visit(ptr<ast::Literal> ast)
{
    currentNode = ast;
    // non-Nil typed literals handled by specialized visit methods
    if (ast->literalType==Literal::Nil)
        emitByte(OpCode::ConstNil);
    else
        throw std::runtime_error("Literal type unhandled");
}


void RoxalCompiler::visit(ptr<ast::Bool> ast)
{
    currentNode = ast;
    emitByte( ast->value ? OpCode::ConstTrue : OpCode::ConstFalse );
}


void RoxalCompiler::visit(ptr<ast::Str> ast)
{
    currentNode = ast;

    // new ObjString or existing one if exists in strings intern map
    auto objStr = stringVal(ast->str); 
    emitConstant(objVal(objStr));
}


void RoxalCompiler::visit(ptr<ast::Type> ast)
{
    currentNode = ast;
    ValueType type { builtinToValueType(ast->t) };

    emitConstant(typeVal(type));
}


void RoxalCompiler::visit(ptr<ast::Num> ast)
{
    currentNode = ast;

    if (std::holds_alternative<double>(ast->num)) {
        emitConstant(realVal(std::get<double>(ast->num)));
    }
    else if (std::holds_alternative<int32_t>(ast->num)) {
        emitConstant(intVal(std::get<int32_t>(ast->num)));
    }
    else 
        throw std::runtime_error("unhandled Num type");
}


void RoxalCompiler::visit(ptr<ast::List> ast)
{
    currentNode = ast;

    // generate code to eval each elements and leave on stack
    ast->acceptChildren(*this);

    if (ast->elements.size() > 255)
        error("Number of literal list elements is limited to 255");

    emitBytes(OpCode::NewList, ast->elements.size());
}


void RoxalCompiler::visit(ptr<ast::Dict> ast)
{
    currentNode = ast;

    // generate code to eval each key & value and leave on stack
    ast->acceptChildren(*this);

    if (ast->entries.size() > 255)
        error("Number of literal dict entries is limited to 255");

    // arg is entry count, so 2x as many stack values (key & value for each entry)
    emitBytes(OpCode::NewDict, ast->entries.size());
}




void RoxalCompiler::beginScope()
{
    funcScope()->scopeDepth++;
    //std::cout << "beginScope() depth=" << state()->scopeDepth << std::endl;
}

void RoxalCompiler::endScope()
{
    funcScope()->scopeDepth--;

    // count how many local variables in the current scope need to be poped
    //  from the stack and emit pop instruction(s)
    // int16_t count { 0 };
    // while (!state()->locals.empty()
    //        && state()->locals.back().depth > state()->scopeDepth) {
    //     count++;
    //     if (count == 255) {
    //         emitBytes(OpCode::PopN, 255);
    //         count=0;
    //     }
    //     state()->locals.pop_back();
    // }
    // if (count > 0) {
    //     if (count==1)
    //         emitByte(OpCode::Pop);
    //     else
    //         emitBytes(OpCode::PopN, uint8_t(count));
    // }

    auto& locals { funcScope()->locals };
// //!!!
// std::cout << "<endScope() depth=" << (state()->scopeDepth+1) << " local.size=" << locals.size() << ":" << std::endl;
// for(auto li=locals.begin(); li!=locals.end(); ++li) {
//     std::cout << "  " << int(&(*li) - &(*locals.begin())) << " " << toUTF8StdString(li->name) << " " << li->depth << " "
//      << (li->isCaptured ? "captured":"notcaptured") << std::endl;
// }
// std::cout << std::endl;
// //!!!
    while (!locals.empty()
           && locals.back().depth > funcScope()->scopeDepth) {

        std::string popComment { "local "+toUTF8StdString(locals.back().name)+" depth:"+std::to_string(locals.back().depth) };

        if (locals.back().isCaptured)
            emitByte(OpCode::CloseUpvalue, popComment);
        else
            emitByte(OpCode::Pop, popComment);

        locals.pop_back();           
    }
}

static std::string linePos(ptr<AST> node)
{
    return std::to_string(node->interval.first.line)+":"+std::to_string(node->interval.first.pos);
}


void RoxalCompiler::error(const std::string& message)
{
    throw std::logic_error(linePos(currentNode) + " - " + message);
}


ValueType RoxalCompiler::builtinToValueType(ast::BuiltinType bt)
{
    ValueType type {};
    switch(bt) {
        case ast::BuiltinType::Nil: type = ValueType::Nil; break;
        case ast::BuiltinType::Bool: type = ValueType::Bool; break;
        case ast::BuiltinType::Byte: type = ValueType::Byte; break;
        //case ast::BuiltinType::Number:  // not concrete
        case ast::BuiltinType::Int: type = ValueType::Int; break;
        case ast::BuiltinType::Real: type = ValueType::Real; break;
        case ast::BuiltinType::Decimal: type = ValueType::Decimal; break;
        case ast::BuiltinType::String: type = ValueType::String; break;
        case ast::BuiltinType::List: type = ValueType::List; break;
        case ast::BuiltinType::Dict: type = ValueType::Dict; break;
        case ast::BuiltinType::Vector: type = ValueType::Vector; break;
        case ast::BuiltinType::Matrix: type = ValueType::Matrix; break;
        case ast::BuiltinType::Tensor: type = ValueType::Tensor; break;
        case ast::BuiltinType::Orient: type = ValueType::Orient; break;
        case ast::BuiltinType::Stream: type = ValueType::Stream; break;
        default:
            throw std::runtime_error("unhandled builtin type "+ast::to_string(bt));
    }
    return type;
}


void RoxalCompiler::emitByte(uint8_t byte, const std::string& comment)
{
    currentChunk()->write(byte, currentNode->interval.first.line, comment);
}


void RoxalCompiler::emitByte(OpCode op, const std::string& comment)
{
    currentChunk()->write(asByte(op), currentNode->interval.first.line, comment);
}


void RoxalCompiler::emitBytes(uint8_t byte1, uint8_t byte2, const std::string& comment)
{
    currentChunk()->write(byte1, currentNode->interval.first.line, comment);
    currentChunk()->write(byte2, currentNode->interval.first.line);
}

void RoxalCompiler::emitBytes(OpCode op, uint8_t byte2, const std::string& comment)
{
    currentChunk()->write(op, currentNode->interval.first.line, comment);
    currentChunk()->write(byte2, currentNode->interval.first.line);
}


void RoxalCompiler::emitLoop(Chunk::size_type loopStart, const std::string& comment)
{
    emitByte(OpCode::Loop, comment);

    auto offset = currentChunk()->code.size() - loopStart + 2;
    if (offset > std::numeric_limits<uint16_t>::max())
        error("Loop body contains too many statements.");

    emitByte((uint16_t(offset) >> 8) & 0xff);
    emitByte(uint8_t(uint16_t(offset) & 0xff));
}


Chunk::size_type RoxalCompiler::emitJump(OpCode instruction, const std::string& comment)
{
    emitByte(instruction, comment);
    emitByte(0xff);
    emitByte(0xff);
    return currentChunk()->code.size() - 2;
}



void RoxalCompiler::emitReturn(const std::string& comment)
{
    if (funcScope()->functionType == FunctionType::Initializer)
        emitBytes(OpCode::GetLocal, 0);
    else 
        emitByte(OpCode::ConstNil, comment);

    emitByte(OpCode::Return);
}


void RoxalCompiler::emitConstant(const Value& value, const std::string& comment)
{
    uint16_t constant = makeConstant(value);
    if (constant <= 255)
        emitBytes(OpCode::Constant, uint8_t(constant), comment);
    else {
        emitByte(OpCode::Constant2);
        emitBytes( uint8_t(constant >> 8), uint8_t(constant & 0xff), comment );
    }
}


void RoxalCompiler::patchJump(Chunk::size_type jumpInstrOffset)
{
    int32_t jumpDist = (currentChunk()->code.size() - jumpInstrOffset) - 2;

    if (jumpDist > std::numeric_limits<uint16_t>::max()) {
        error("Too must code in conditional block");
    }

    currentChunk()->code[jumpInstrOffset] = (uint16_t(jumpDist) >> 8) & 0xff;
    currentChunk()->code[jumpInstrOffset+1] = uint8_t(uint16_t(jumpDist) & 0xff);
}


int16_t RoxalCompiler::makeConstant(const Value& value)
{
    size_t constant = currentChunk()->addConstant(value);
    if (constant >= std::numeric_limits<int16_t>::max()) {
        error("Too many constants in one chunk.");
        return 0;
    }
    return int16_t(constant);
}


int16_t RoxalCompiler::identifierConstant(const icu::UnicodeString& ident)
{
    // search for existing identifier string constant to re-use first
    bool found { false };
    int16_t constant {};
    for(auto identConst : funcScope()->identConsts) {
        if (asString(currentChunk()->constants.at(identConst))->s == ident) {
            constant = identConst;
            found = true;
            break;
        }
    }

    if (!found) {
        // not found, create new string constant
        //  (globals are late bound, so it may only be declared afterward)
        constant = makeConstant(objVal(stringVal(ident)));
        funcScope()->identConsts.push_back(constant);
    }
    return constant;
}


void RoxalCompiler::addLocal(const icu::UnicodeString& name)
{
    //std::cout << (&(*state()) - &(*states.begin())) << " addLocal(" << toUTF8StdString(name) << ")" << std::endl;//!!!
    if (funcScope()->locals.size() == 255) {
        error("Maximum of 255 local variables per function exceeded.");
        return;  
    }
    funcScope()->locals.push_back(Local(name, -1)); // scopeDepth=-1 --> uninitialized
    #ifdef DEBUG_BUILD
    auto index { funcScope()->locals.size()-1 };
    emitByte(OpCode::Nop, "local "+toUTF8StdString(name)+ "("+std::to_string(index)+") depth:"+std::to_string(funcScope()->scopeDepth));
    #endif

}


int16_t RoxalCompiler::resolveLocal(FunctionScopes::iterator scopeState, const icu::UnicodeString& name)
{
    //std::cout << (&(*scopeState) - &(*states.begin()))<< " resolveLocal(" << toUTF8StdString(name) << ")" << std::endl;//!!!
    if (!scopeState->locals.empty())
        for(int32_t i=scopeState->locals.size()-1; i>=0; i--) {
            #ifdef DEBUG_BUILD
                if (scopeState->locals.at(i).name == name) {
            #else
                if (scopeState->locals[i].name == name) {
            #endif
                    if (scopeState->locals[i].depth == -1)
                        error("Reference to local varaiable in initializer not allowed.");
                    return i;
                }
        }

    return -1;
}


int RoxalCompiler::addUpvalue(FunctionScopes::iterator scopeState, uint8_t index, bool isLocal)
{
    //std::cout << (&(*scopeState) - &(*states.begin())) << " addUpvalue(" << index << " " << (isLocal ? "local" : "notlocal") << ")" << std::endl;//!!!
    int upvalueCount = scopeState->function->upvalueCount;
    auto& upvalues { scopeState->upvalues };

    for (int i=0; i<upvalueCount; i++) {
        const Upvalue& upvalue = upvalues[i];
        if (upvalue.index == index && upvalue.isLocal == isLocal) 
            return i;        
    }

    if (upvalueCount == std::numeric_limits<uint8_t>::max()) {
        error("Maximum closure variables exceeded in function.");
        return 0;
    }

    upvalues.push_back(Upvalue(index, isLocal));
// //!!!
// std::cout << "Upvalues: ";
// for(int i=0; i<upvalues.size();i++) {
//     std:: cout << int(upvalues[i].index) << (upvalues[i].isLocal?"L":"n") << "  ";
// }
// std::cout << std::endl;
// std::cout << "  function.upvalueCount="+std::to_string(upvalueCount) << std::endl;
// //!!!
    return scopeState->function->upvalueCount++;
}


int16_t RoxalCompiler::resolveUpvalue(FunctionScopes::iterator scopeState, const icu::UnicodeString& name)
{
    //std::cout << (&(*scopeState) - &(*states.begin())) << " resolveUpvalue(" << toUTF8StdString(name) << ")" << std::endl;//!!!
    //std::string sname { toUTF8StdString(name) };//!!!

    if (scopeState == funcScopes.begin()) // no enclosing function scope
        return -1;

    int local = resolveLocal(enclosingFuncScope(scopeState), name);
    if (local != -1) {
        #ifdef DEBUG_BUILD
        enclosingFuncScope(scopeState)->locals.at(local).isCaptured = true;
        #else
        enclosingFuncScope(scopeState)->locals[local].isCaptured = true;
        #endif
        return addUpvalue(scopeState, uint8_t(local), true);
    }

    int upvalue = resolveUpvalue(enclosingFuncScope(scopeState), name);
    if (upvalue != -1)
        return addUpvalue(scopeState, uint8_t(upvalue), false);

    return -1;
}



void RoxalCompiler::declareVariable(const icu::UnicodeString& name)
{
    if (funcScope()->scopeDepth == 0)
        return;

    // check there is no variable with the same name in this scope (an error)
    for(auto li = funcScope()->locals.rbegin(); li != funcScope()->locals.rend(); ++li) {
        if ((li->depth != -1) && (li->depth < funcScope()->scopeDepth))
            break;

        if (li->name == name) {
            error("A variable with this name already exists in this scope.");
        }
    }

    addLocal(name);
}


void RoxalCompiler::defineVariable(uint16_t var)
{
    // local variables are already on the stack
    if (funcScope()->scopeDepth > 0) {
        // mark initialized
        funcScope()->locals.back().depth = funcScope()->scopeDepth;
        return;
    }

    // emit code to define named global variable at runtime
    if (var > 255) // TODO: remove when DefineGlobal2 supported
        throw std::runtime_error("Max of 255 global vars supported");

    emitBytes(OpCode::DefineGlobal, uint8_t(var));
}


bool RoxalCompiler::namedVariable(const icu::UnicodeString& name, bool assign)
{
    //std::cout << (&(*state()) - &(*states.begin())) << " namedVariable(" << toUTF8StdString(name) << ")" << std::endl;//!!!
    OpCode getOp, setOp;

    int16_t arg = resolveLocal(funcScope(),name);
    if (arg != -1) { // found
        getOp = OpCode::GetLocal;
        setOp = OpCode::SetLocal;
    }
    else if ((arg = resolveUpvalue(funcScope(),name)) != -1) {
        getOp = OpCode::GetUpvalue;
        setOp = OpCode::SetUpvalue;
    }
    else { // local, not found
        // assume global
        arg = identifierConstant(name);
        getOp = OpCode::GetGlobal;
        //  allow assigning without previously declaring, except within functions
        if (funcScope()->functionType != FunctionType::Module)
            setOp = OpCode::SetGlobal;
        else 
            setOp = OpCode::SetNewGlobal;
    }

    if (!assign)
        emitBytes(getOp, arg, toUTF8StdString(name));
    else
        emitBytes(setOp, arg, toUTF8StdString(name));

    return true;
}
