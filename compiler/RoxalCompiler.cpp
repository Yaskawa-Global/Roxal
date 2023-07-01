
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

        enterModuleScope(toUnicodeString(name));

        auto module { asModuleScope(moduleScope()) };

        module->strict = false;

        try {
            auto file = as<File>(ast);

            file->accept(*this);
            
            function = module->function;

            assert(function != nullptr);

            if (outputBytecodeDissasembly)
                module->function->chunk->disassemble(module->function->name);

            //std::cout << "value:" << value->repr() << std::endl;
        } catch (std::logic_error& e) {
            std::cout << std::string("Compile error: ") << e.what() << std::endl;
            exitModuleScope();
            if (function != nullptr)
                delObj(function);
            return nullptr;
        } catch (std::exception& e) {
            std::cout << std::string("Exception: ") << e.what() << std::endl;
            exitModuleScope();
            if (function != nullptr)
                delObj(function);
            throw e;
        } 

        exitModuleScope();
        
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






std::any RoxalCompiler::visit(ptr<ast::File> ast)
{
    currentNode = ast;
    Anys results {};
    ast->acceptChildren(*this, results);
    emitReturn();
    return {};
}


std::any RoxalCompiler::visit(ptr<ast::SingleInput> ast)
{
    currentNode = ast;
    Anys results {};
    ast->acceptChildren(*this, results);
    return results;
}


std::any RoxalCompiler::visit(ptr<ast::Annotation> ast)
{
    currentNode = ast;
    // currently, we don't generate any code for annotations
    //ast->acceptChildren(*this);
    return {};
}


std::any RoxalCompiler::visit(ptr<ast::TypeDecl> ast)
{
    currentNode = ast;

    bool isActor = ast->kind==TypeDecl::Actor;

    enterTypeScope(ast->name);

    int16_t typeNameConstant = identifierConstant(ast->name);
    declareVariable(ast->name);    

    if (ast->implements.size()>2)
        throw std::runtime_error("Multiple implements types unimplemented.");

    emitBytes(isActor ? OpCode::ActorType : OpCode::ObjectType, typeNameConstant);
    defineVariable(typeNameConstant);


    // handle extension (inheritance)
    if (ast->extends.has_value()) {
        asTypeScope(typeScope())->hasSuperType = true;

        auto superTypeName = ast->extends.value();

        // can't inherit yourself
        if (superTypeName == ast->name)
            error("Type object or actor '"+toUTF8StdString(ast->name)+"' can't extend itself.");

        namedVariable(superTypeName, /*assign=*/false); // parent (super)

        enterLocalScope();
        addLocal("super");
        defineVariable(0);

        namedVariable(ast->name, /*assign=*/false); // child (sub)
        emitByte(OpCode::Extend);
    }


    namedVariable(ast->name, false); // make type accessible on the stack

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

    if (asTypeScope(typeScope())->hasSuperType)
        exitLocalScope();

    exitTypeScope();

    return {};
}


std::any RoxalCompiler::visit(ptr<ast::FuncDecl> ast)
{
    currentNode = ast;    

    auto name { as<Function>(ast->func)->name };

    declareVariable(name);

    uint16_t var { 0 };
    if (asFuncScope(funcScope())->scopeDepth == 0) // global variable
        var = identifierConstant(name); // create constant table entry for name

    if (asFuncScope(funcScope())->scopeDepth > 0) {
        // mark initialized
        asFuncScope(funcScope())->locals.back().depth = asFuncScope(funcScope())->scopeDepth;
    }

    Anys results {};
    ast->acceptChildren(*this, results);

    // unwrap ObjFunction* returned by visit(ptr<Function>)
    auto function = std::any_cast<ObjFunction*>(std::any_cast<Anys>(results.at(0)).at(0));

    // attached the FuncDecl annotations (which appear right before the func declatation)
    //  to the function object to make them available at runtime
    function->annotations = ast->annotations;

    defineVariable(var);

    return {};
}


std::any RoxalCompiler::visit(ptr<ast::VarDecl> ast)
{
    currentNode = ast;

    declareVariable(ast->name);
    uint16_t var { 0 };
    if (asFuncScope(funcScope())->scopeDepth == 0) // global variable
        var = identifierConstant(ast->name); // create constant table entry for name

    // TODO: support type spec 

    if (ast->initializer.has_value()) {
        ast->initializer.value()->accept(*this);
    }
    else
        emitByte(OpCode::ConstNil);

    defineVariable(var);

    return {};
}


std::any RoxalCompiler::visit(ptr<ast::Suite> ast)
{
    currentNode = ast;
    Anys results {};

    enterLocalScope();
    ast->acceptChildren(*this, results);
    exitLocalScope();
    return results;
}


std::any RoxalCompiler::visit(ptr<ast::ExpressionStatement> ast)
{
    currentNode = ast;
    ast::Anys results {};
    ast->acceptChildren(*this, results);

    // expressions leave their value on the stack, but statements don't
    //  have a value, so discard it
    emitByte(OpCode::Pop, "expr_stmt value");
    return results;
}


std::any RoxalCompiler::visit(ptr<ast::ReturnStatement> ast)
{
    currentNode = ast;
    ast::Anys results {};

    ast->acceptChildren(*this, results);

    if (ast->expr.has_value()) {

        if (asFuncScope(funcScope())->functionType == FunctionType::Initializer)
            error("A value cannot be returned from an 'init' method.");
        if (asFuncScope(funcScope())->type->func.has_value() && asFuncScope(funcScope())->type->func.value().isProc)
            error("A value cannot be returned from a proc method.");

        emitByte(OpCode::Return);
    }
    else        
        emitReturn();

    return results;
}


std::any RoxalCompiler::visit(ptr<ast::IfStatement> ast)
{
    currentNode = ast;

    // (first) if condition 
    ast->conditionalSuites.at(0).first->accept(*this);

    auto jumpOverIf = emitJump(OpCode::JumpIfFalse);
    emitByte(OpCode::Pop, "if cond");

    enterLocalScope();
    ast->conditionalSuites.at(0).second->accept(*this);
    exitLocalScope();

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
        enterLocalScope();
        ast->elseSuite.value()->accept(*this);
        exitLocalScope();
    }
    
    patchJump(jumpOverElse);

    return {};
}


std::any RoxalCompiler::visit(ptr<ast::WhileStatement> ast)
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

    return {};
}


std::any RoxalCompiler::visit(ptr<ast::Function> ast)
{
    currentNode = ast;
    
    bool isProc = ast->isProc;
    bool isMethod = inTypeScope() // methods can't be outside type decl
                     && (asFuncScope(funcScope())->functionType!=FunctionType::Method) // or directly inside another method
                     && (asFuncScope(funcScope())->functionType!=FunctionType::Initializer);
    // std::cout << " visit <Function> " << toUTF8StdString(ast->name) 
    //           << " current funcScope:" << toUTF8StdString(asFuncScope(funcScope())->function->name) 
    //            << "[type:" << toString(asFuncScope(funcScope())->functionType) << "]"
    //           << " isMethod?" << isMethod << std::endl;
    bool isInitializer = isMethod && (ast->name == "init");

    if (isInitializer && !isProc)
        error("object or actor type 'init' method must be a proc.");

    FunctionType ftype = isMethod ? 
                              (isInitializer ? FunctionType::Initializer : FunctionType::Method)
                            : FunctionType::Function;

    assert(ast->type.has_value());
    enterFuncScope(ast->name, ftype, ast->type.value());

    #ifdef DEBUG_BUILD
    emitByte(OpCode::Nop, "func "+toUTF8StdString(ast->name));
    #endif
    enterLocalScope();

    asFuncScope(funcScope())->function->arity = ast->params.size();
    if (asFuncScope(funcScope())->function->arity > 255)
        error("Maximum of function or procedure 255 parameters exceeded.");

    Anys results {};
    ast->acceptChildren(*this, results);

    //exitLocalScope(); 

    emitReturn();

    if (outputBytecodeDissasembly)
        asFuncScope(funcScope())->function->chunk->disassemble(asFuncScope(funcScope())->function->name);

    ObjFunction* function = asFuncScope(funcScope())->function;

    auto functionScope { *asFuncScope(funcScope()) };

    exitFuncScope(); // back to surrpounding scope

    // std::cout << "Closure " << toUTF8StdString(function->name) << ": #" << function->upvalueCount << std::endl;
    // std::cout << "   #" << functionState.upvalues.size() << std::endl;
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

    return function; // used by caller visit(ptr<FuncDecl>)
}


std::any RoxalCompiler::visit(ptr<ast::Parameter> ast)
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

        enterFuncScope(ast->name, FunctionType::Function, defFuncType);

        #ifdef DEBUG_BUILD
        emitByte(OpCode::Nop, "param_def "+toUTF8StdString(ast->name));
        #endif
        enterLocalScope();

        asFuncScope(funcScope())->function->arity = 0;

        Anys results;
        ast->acceptChildren(*this, results);

        exitLocalScope(); 

        // since this closure was called directly by being queued by OpCode::Call
        //  rather than through byte code pushing the callable/closure, we
        //  need get the return value copied into the placeholder arg slots in the parent frame
        //  rather than leaving it on the stack
        emitByte(OpCode::ReturnStore);

        if (outputBytecodeDissasembly)
            asFuncScope(funcScope())->function->chunk->disassemble(asFuncScope(funcScope())->function->name);

        ObjFunction* function = asFuncScope(funcScope())->function;

        exitFuncScope(); // back to surrpounding scope

        // store the func that evaluates the default param value in the function
        //  for which it is a param
        asFuncScope(funcScope())->function->paramDefaultFunc[ast->name.hashCode()] = function;
    }
    return {};
}


std::any RoxalCompiler::visit(ptr<ast::Assignment> ast)
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

        emitBytes(propName <= 255 ? OpCode::SetProp : OpCode::SetProp2, propName);
    }
    else if (isa<Index>(ast->lhs)) {

        // evaluate rhs
        ast->rhs->accept(*this);

        auto index { as<Index>(ast->lhs) };

        // value being indexed
        index->indexable->accept(*this);

        // index args
        for(auto& arg : index->args)
            arg->accept(*this);

        emitBytes(OpCode::SetIndex, index->args.size());
    }
    else if (isa<List>(ast->lhs)) {
        // binding assignment - assign east LHS element of list seperately from indexed element of RHS

        // evaluate rhs (expected to leave list on stack)
        //  TOOD: consider also supporting dict lhs so return components can be named(?)
        ast->rhs->accept(*this);

        auto lhsList = as<List>(ast->lhs);
        auto lhsSize = lhsList->elements.size();
        for(auto li=0; li<lhsSize; li++) {
            auto lhsElt = lhsList->elements.at(li);

            // first index the RHS list to get the RHS element to assign
            emitByte(OpCode::Dup); // duplicate the RHS (as Index will pop it)
            emitConstant(intVal(li));
            emitBytes(OpCode::Index, 1);

            if (isa<Variable>(lhsElt)) {
                auto varname { as<Variable>(lhsElt)->name };

                namedVariable(varname, /*assign=*/true);

            }
            else if (isa<UnaryOp>(lhsElt) && as<UnaryOp>(lhsElt)->op==UnaryOp::Accessor) {
                auto accessor = as<UnaryOp>(lhsElt);

                accessor->arg->accept(*this);

                if (!accessor->member.has_value())
                    throw std::runtime_error("accessor unary operator expects member name");
                int16_t propName = identifierConstant(accessor->member.value());

                emitByte(OpCode::Swap);

                emitBytes(propName <= 255 ? OpCode::SetProp : OpCode::SetProp2, propName);
            }
            else if (isa<Index>(lhsElt)) {

                auto index { as<Index>(lhsElt) };

                // value being indexed
                index->indexable->accept(*this);

                // index args
                for(auto& arg : index->args)
                    arg->accept(*this);

                emitBytes(OpCode::SetIndex, index->args.size());
            }
            else
                throw std::runtime_error("Elements of LHS list of binding assignment must be variables, property accessors or indexing");

            emitByte(OpCode::Pop,"RHS #"+std::to_string(li)); // discard RHS element, leaving RHS list on top
        }
    }
    else
        throw std::runtime_error("LHS of assignment must be a variable, property accessor or indexing");
    return {};
}


std::any RoxalCompiler::visit(ptr<ast::BinaryOp> ast)
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
        Anys results;
        ast->acceptChildren(*this, results);

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
    return {};
}


std::any RoxalCompiler::visit(ptr<ast::UnaryOp> ast)
{
    currentNode = ast;
    Anys results {};

    // special case for super.<member>
    if ((ast->op == UnaryOp::Accessor) && isa<Variable>(ast->arg)
        && as<Variable>(ast->arg)->name == "super") {

        if (!inTypeScope())
            error("Can't use 'super' outside of a type object or actor declaration.");

        if (!asTypeScope(typeScope())->hasSuperType)
            error("Can't use 'super' in a type object or actor that doesn't extend another type");

        if (!ast->member.has_value())
            throw std::runtime_error("super. accessor requires member name");

        int16_t identConstant = identifierConstant(ast->member.value());
        if (identConstant > 255)
            error("Too many constants in scope");

        namedVariable("this", false);
        namedVariable("super", false);
        emitBytes(OpCode::GetSuper, uint8_t(identConstant));
        return {};
    }

    ast->acceptChildren(*this, results);

    switch (ast->op) {
        case UnaryOp::Negate: emitByte(OpCode::Negate); break;
        case UnaryOp::Not: emitByte(OpCode::Negate); break;
        case UnaryOp::Accessor: {
            if (!ast->member.has_value())
                throw std::runtime_error("Accessor . requires member name");

            int16_t identConstant = identifierConstant(ast->member.value());
            if (identConstant > 255)
                error("Too many constants in scope");
            emitBytes(OpCode::GetProp, uint8_t(identConstant));
        } break;
        default:
            throw std::runtime_error("unimplemented unary opertor:"+ast->opString());
    }
    return {};
}


std::any RoxalCompiler::visit(ptr<ast::Variable> ast)
{
    currentNode = ast;
    namedVariable(ast->name);
    return {};
}


std::any RoxalCompiler::visit(ptr<ast::Call> ast)
{
    currentNode = ast;
    Anys results {};

    ast->acceptChildren(*this, results);

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
    return {};
}


std::any RoxalCompiler::visit(ptr<ast::Range> ast)
{
    currentNode = ast;
    Anys results {};

    // always push 3 values, nil for implicit

    // special case for range n:n - don't visit same
    //  expression twice and we just leave single expr on stack
    if ((ast->start != nullptr) && (ast->start == ast->stop)) {
        results.push_back( ast->start->accept(*this) );
    }
    else {
        if (ast->start != nullptr)
            results.push_back( ast->start->accept(*this) );
        else
            emitByte(OpCode::ConstNil);

        if (ast->stop != nullptr)
            results.push_back( ast->stop->accept(*this) );
        else
            emitByte(OpCode::ConstNil);

        if (ast->step != nullptr)
            results.push_back( ast->step->accept(*this) );
        else
            emitByte(OpCode::ConstNil);

        emitBytes(OpCode::NewRange, uint8_t(ast->closed ? 1 : 0));
    }

    return results;
}


std::any RoxalCompiler::visit(ptr<ast::Index> ast)
{
    currentNode = ast;
    Anys results {};
    ast->acceptChildren(*this, results);

    auto argCount = ast->args.size();
    if (argCount > 255)
        error("Number of indices is limited to 255");
    emitBytes(OpCode::Index, argCount);
    return {};
}


std::any RoxalCompiler::visit(ptr<ast::Literal> ast)
{
    currentNode = ast;
    // non-Nil typed literals handled by specialized visit methods
    if (ast->literalType==Literal::Nil)
        emitByte(OpCode::ConstNil);
    else
        throw std::runtime_error("Literal type unhandled");
    return {};
}


std::any RoxalCompiler::visit(ptr<ast::Bool> ast)
{
    currentNode = ast;
    emitByte( ast->value ? OpCode::ConstTrue : OpCode::ConstFalse );
    return {};
}


std::any RoxalCompiler::visit(ptr<ast::Str> ast)
{
    currentNode = ast;

    // new ObjString or existing one if exists in strings intern map
    auto objStr = stringVal(ast->str); 
    emitConstant(objVal(objStr));
    return {};
}


std::any RoxalCompiler::visit(ptr<ast::Type> ast)
{
    currentNode = ast;
    ValueType type { builtinToValueType(ast->t) };

    emitConstant(typeVal(type));
    return {};
}


std::any RoxalCompiler::visit(ptr<ast::Num> ast)
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
    return {};
}


std::any RoxalCompiler::visit(ptr<ast::List> ast)
{
    currentNode = ast;
    Anys results {};

    // generate code to eval each elements and leave on stack
    ast->acceptChildren(*this, results);

    if (ast->elements.size() > 255)
        error("Number of literal list elements is limited to 255");

    emitBytes(OpCode::NewList, ast->elements.size());
    return {};
}


std::any RoxalCompiler::visit(ptr<ast::Dict> ast)
{
    currentNode = ast;
    Anys results {};

    // generate code to eval each key & value and leave on stack
    ast->acceptChildren(*this, results);

    if (ast->entries.size() > 255)
        error("Number of literal dict entries is limited to 255");

    // arg is entry count, so 2x as many stack values (key & value for each entry)
    emitBytes(OpCode::NewDict, ast->entries.size());
    return {};
}


void RoxalCompiler::outputScopes()
{
    std::cout << "Scopes: ";
    for(auto s = lexicalScopes.cbegin(); s != lexicalScopes.cend(); ++s) {
        std::cout << toUTF8StdString((*s)->name) << "[" << (*s)->typeString() << "]";
        if (s+1 != lexicalScopes.cend())
            std::cout << " : ";
    }
    std::cout << std::endl;
}



void RoxalCompiler::enterGlobalScope()
{
    #ifdef DEBUG_TRACE_SCOPES
    std::cout << "enterGlobalScope()" << std::endl;
    outputScopes();
    #endif
}

void RoxalCompiler::exitGlobalScope()
{
    #ifdef DEBUG_TRACE_SCOPES
    std::cout << "exitGlobalScope()" << std::endl;
    outputScopes();
    #endif
}


void RoxalCompiler::enterModuleScope(const icu::UnicodeString& moduleName)
{
    lexicalScopes.push_back(std::make_shared<ModuleScope>(moduleName));
    #ifdef DEBUG_TRACE_SCOPES
    std::cout << "enterModuleScope(" << toUTF8StdString(moduleName) << ")" << std::endl;
    outputScopes();
    #endif
}

void RoxalCompiler::exitModuleScope()
{
    #ifdef DEBUG_BUILD
    if (lexicalScopes.empty())
        throw std::runtime_error("exitModuleScope() stack underflow");
    if ((*scope())->scopeType != LexicalScope::ScopeType::Module)
        throw std::runtime_error("exitModuleScope() - not in module scope");
    #endif
    #ifdef DEBUG_TRACE_SCOPES
    std::cout << "exitModuleScope(" << toUTF8StdString(asModuleScope(moduleScope())->function->name) << ")" << std::endl;
    #endif

    lexicalScopes.pop_back();

    #ifdef DEBUG_TRACE_SCOPES
    outputScopes();
    #endif
}


void RoxalCompiler::enterTypeScope(const icu::UnicodeString& typeName)
{
    lexicalScopes.push_back(std::make_shared<TypeScope>(typeName));

    #ifdef DEBUG_TRACE_SCOPES
    std::cout << "enterTypeScope(" << toUTF8StdString(typeName) << ")" << std::endl;
    outputScopes();
    #endif
}

void RoxalCompiler::exitTypeScope()
{
    #ifdef DEBUG_BUILD
    if (lexicalScopes.empty())
        throw std::runtime_error("exitTypeScope() stack underflow");
    if ((*scope())->scopeType != LexicalScope::ScopeType::Type)
        throw std::runtime_error("exitTypeScope() - not in type scope");
    #endif
    #ifdef DEBUG_TRACE_SCOPES
    std::cout << "exitTypeScope(" << toUTF8StdString(asTypeScope(typeScope())->name) << ")" << std::endl;
    #endif

    lexicalScopes.pop_back();

    #ifdef DEBUG_TRACE_SCOPES
    outputScopes();
    #endif
}


void RoxalCompiler::enterFuncScope(const icu::UnicodeString& funcName, FunctionType funcType, ptr<type::Type> type)
{
    lexicalScopes.push_back(std::make_shared<FunctionScope>(funcName,funcType,type));

    #ifdef DEBUG_TRACE_SCOPES
    std::cout << "enterFuncScope(" << toUTF8StdString(funcName) << ",funcType=" << toString(funcType) << ")" << std::endl;
    outputScopes();
    #endif
}

void RoxalCompiler::exitFuncScope()
{
    #ifdef DEBUG_BUILD
    if (lexicalScopes.empty())
        throw std::runtime_error("exitFuncScope() stack underflow");
    if ((*scope())->scopeType != LexicalScope::ScopeType::Func)
        throw std::runtime_error("exitFuncScope() - not in func scope");
    #endif
    #ifdef DEBUG_TRACE_SCOPES
    std::cout << "exitFuncScope(" << toUTF8StdString(asFuncScope(funcScope())->function->name) << ")" << std::endl;
    #endif

    lexicalScopes.pop_back();

    #ifdef DEBUG_TRACE_SCOPES
    outputScopes();
    #endif
}


void RoxalCompiler::enterLocalScope()
{
    asFuncScope(funcScope())->scopeDepth++;
    #ifdef DEBUG_TRACE_SCOPES
    std::cout << "enterLocalScope() depth:" << asFuncScope(funcScope())->scopeDepth << std::endl;
    outputScopes();
    #endif
}

void RoxalCompiler::exitLocalScope()
{
    #ifdef DEBUG_BUILD
    if (lexicalScopes.empty())
        throw std::runtime_error("exitLocalScope() stack underflow");
    if (!inFuncScope())
        throw std::runtime_error("exitLocalScope() - not in func scope");
    if (asFuncScope(funcScope())->scopeDepth == 0)
        throw std::runtime_error("exitLocalScope() depth underflow");
    #endif
    asFuncScope(funcScope())->scopeDepth--;

    auto& locals { asFuncScope(funcScope())->locals };

    while (!locals.empty()
           && locals.back().depth > asFuncScope(funcScope())->scopeDepth) {

        std::string popComment { "local "+toUTF8StdString(locals.back().name)+" depth:"+std::to_string(locals.back().depth) };

        if (locals.back().isCaptured)
            emitByte(OpCode::CloseUpvalue, popComment);
        else
            emitByte(OpCode::Pop, popComment);

        locals.pop_back();
    }

    #ifdef DEBUG_TRACE_SCOPES
    std::cout << "exitLexicalScope()" << std::endl;
    outputScopes();
    #endif
}


int RoxalCompiler::scopeDepth() const 
{ 
    return int(lexicalScopes.size());
}

RoxalCompiler::Scope RoxalCompiler::scope()
{
    #ifdef DEBUG_BUILD
    if (lexicalScopes.empty())
        throw std::runtime_error("scope() stack underflow");
    #endif
    return lexicalScopes.end()-1;
}

bool RoxalCompiler::hasEnclosingScope(Scope s)
{
    return (s != lexicalScopes.begin());
}

RoxalCompiler::Scope RoxalCompiler::enclosingScope(RoxalCompiler::Scope s)
{
    #ifdef DEBUG_BUILD
    if (s == lexicalScopes.begin())
        throw std::runtime_error("enclosingScope() stack underflow");
    #endif

    return --s;
}


bool RoxalCompiler::inFuncScope()
{
    for(auto i = lexicalScopes.rbegin(); i != lexicalScopes.rend(); ++i)
        if ((*i)->isFuncOrModule())
            return true;
    return false;
}

bool RoxalCompiler::inFuncScope(Scope s)
{
    // is this a func scope, or is enclosed by one?
    return (*s)->isFuncOrModule() || hasEnclosingFuncScope(s);
}


RoxalCompiler::Scope RoxalCompiler::funcScope()
{
    // find top-most func scope
    auto s = scope();
    while (!(*s)->isFuncOrModule())
        s = enclosingScope(s);
    return s;
}

bool RoxalCompiler::hasEnclosingFuncScope(Scope s)
{
    auto es = s;
    while (hasEnclosingScope(es)) {
        es = enclosingScope(es);
        if ((*es)->isFuncOrModule())
            return true;
    }

    return false;
}


RoxalCompiler::Scope RoxalCompiler::enclosingFuncScope(Scope s)
{
    auto es = enclosingScope(s);
    while (!(*es)->isFuncOrModule())
        es = enclosingScope(es);
    return es;
}

bool RoxalCompiler::inTypeScope()
{
    for(auto i = lexicalScopes.rbegin(); i != lexicalScopes.rend(); ++i)
        if ((*i)->scopeType == LexicalScope::ScopeType::Type)
            return true;
    return false;
}

RoxalCompiler::Scope RoxalCompiler::typeScope()
{
    auto s = scope();
    while ((*s)->scopeType != LexicalScope::ScopeType::Type)
        s = enclosingScope(s);
    return s;
}

RoxalCompiler::Scope RoxalCompiler::enclosingTypeScope(Scope s)
{
    auto es = enclosingScope(s);
    while ((*es)->scopeType != LexicalScope::ScopeType::Type)
        es = enclosingScope(es);
    return es;
}


RoxalCompiler::Scope RoxalCompiler::moduleScope()
{
    auto s = scope();
    while ((*s)->scopeType != LexicalScope::ScopeType::Module)
        s = enclosingScope(s);
    return s;
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
        case ast::BuiltinType::Range: type = ValueType::Range; break;
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

void RoxalCompiler::emitBytes(OpCode op, uint8_t byte2, uint8_t byte3, const std::string& comment)
{
    currentChunk()->write(op, currentNode->interval.first.line, comment);
    currentChunk()->write(byte2, currentNode->interval.first.line);
    currentChunk()->write(byte3, currentNode->interval.first.line);
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
    if (asFuncScope(funcScope())->functionType == FunctionType::Initializer)
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
    for(auto identConst : asFuncScope(funcScope())->identConsts) {
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
        asFuncScope(funcScope())->identConsts.push_back(constant);
    }
    return constant;
}


void RoxalCompiler::addLocal(const icu::UnicodeString& name)
{
    //std::cout << (&(*state()) - &(*states.begin())) << " addLocal(" << toUTF8StdString(name) << ")" << std::endl;//!!!
    if (asFuncScope(funcScope())->locals.size() == 255) {
        error("Maximum of 255 local variables per function exceeded.");
        return;  
    }
    #ifdef DEBUG_TRACE_NAME_RESOLUTION
    std::cout << "addLocal(" << toUTF8StdString(name) << ")" << std::endl;
    #endif

    asFuncScope(funcScope())->locals.push_back(Local(name, -1)); // scopeDepth=-1 --> uninitialized

    #ifdef DEBUG_BUILD
    auto index { asFuncScope(funcScope())->locals.size()-1 };
    emitByte(OpCode::Nop, "local "+toUTF8StdString(name)+ "("+std::to_string(index)+") depth:"+std::to_string(asFuncScope(funcScope())->scopeDepth));
    #endif

}


int16_t RoxalCompiler::resolveLocal(Scope scopeState, const icu::UnicodeString& name)
{
    #ifdef DEBUG_BUILD
    if (!(*scopeState)->isFuncOrModule())
        throw std::runtime_error("resolveLocal() scopeState is not a func/module scope");    
    #endif
    #ifdef DEBUG_TRACE_NAME_RESOLUTION
    std::cout << "resolveLocal(scope=" << toUTF8StdString((*scopeState)->name) << ", " << toUTF8StdString(name) << ")";
    #endif
    //std::cout << (&(*scopeState) - &(*states.begin()))<< " resolveLocal(" << toUTF8StdString(name) << ")" << std::endl;//!!!
    auto locals { asFuncScope(scopeState)->locals };
    if (!locals.empty())
        for(int32_t i=locals.size()-1; i>=0; i--) {
            #ifdef DEBUG_BUILD
                if (locals.at(i).name == name) {
            #else
                if (locals[i].name == name) {
            #endif
                    if (locals[i].depth == -1)
                        error("Reference to local varaiable in initializer not allowed.");
                    #ifdef DEBUG_TRACE_NAME_RESOLUTION
                    std::cout << " - found " << i << std::endl;
                    #endif
                    return i;
                }
        }

    #ifdef DEBUG_TRACE_NAME_RESOLUTION
    std::cout << " - not found" << std::endl;
    #endif
    return -1;
}


int RoxalCompiler::addUpvalue(Scope scopeState, uint8_t index, bool isLocal)
{
    //std::cout << (&(*scopeState) - &(*states.begin())) << " addUpvalue(" << index << " " << (isLocal ? "local" : "notlocal") << ")" << std::endl;//!!!
    int upvalueCount = asFuncScope(scopeState)->function->upvalueCount;
    auto& upvalues { asFuncScope(scopeState)->upvalues };

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

    // std::cout << "Upvalues: ";
    // for(int i=0; i<upvalues.size();i++) {
    //     std:: cout << int(upvalues[i].index) << (upvalues[i].isLocal?"L":"n") << "  ";
    // }
    // std::cout << std::endl;
    // std::cout << "  function.upvalueCount="+std::to_string(upvalueCount) << std::endl;
    return asFuncScope(scopeState)->function->upvalueCount++;
}


int16_t RoxalCompiler::resolveUpvalue(Scope scopeState, const icu::UnicodeString& name)
{
    //std::cout << (&(*scopeState) - &(*states.begin())) << " resolveUpvalue(" << toUTF8StdString(name) << ")" << std::endl;//!!!
    //std::string sname { toUTF8StdString(name) };//!!!
    #ifdef DEBUG_TRACE_NAME_RESOLUTION
    std::cout << "resolveUpvalue(scope=" << toUTF8StdString((*scopeState)->name) << ", " << toUTF8StdString(name) << ")";
    #endif

    if (!hasEnclosingFuncScope(scopeState)) { // no enclosing func scope
        #ifdef DEBUG_TRACE_NAME_RESOLUTION
        std::cout << " - not found" << std::endl;
            #ifdef DEBUG_TRACE_SCOPES
            outputScopes();
            #endif
        #endif
        return -1;
    }

    int local = resolveLocal(enclosingFuncScope(scopeState), name);
    if (local != -1) {
        #ifdef DEBUG_BUILD
        asFuncScope(enclosingFuncScope(scopeState))->locals.at(local).isCaptured = true;
        #else
        enclosingFuncScope(scopeState)->locals[local].isCaptured = true;
        #endif
        return addUpvalue(scopeState, uint8_t(local), true);
    }

    int upvalue = resolveUpvalue(enclosingFuncScope(scopeState), name);
    if (upvalue != -1) {
        #ifdef DEBUG_TRACE_NAME_RESOLUTION
        std::cout << " - found " << upvalue << std::endl;
        #endif
        return addUpvalue(scopeState, uint8_t(upvalue), false);
    }

    #ifdef DEBUG_TRACE_NAME_RESOLUTION
    std::cout << " - not found" << std::endl;
    #endif
    return -1;
}



void RoxalCompiler::declareVariable(const icu::UnicodeString& name)
{
    if (asFuncScope(funcScope())->scopeDepth == 0)
        return;

    // check there is no variable with the same name in this scope (an error)
    for(auto li = asFuncScope(funcScope())->locals.rbegin(); li != asFuncScope(funcScope())->locals.rend(); ++li) {
        if ((li->depth != -1) && (li->depth < asFuncScope(funcScope())->scopeDepth))
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
    if (asFuncScope(funcScope())->scopeDepth > 0) {
        // mark initialized
        asFuncScope(funcScope())->locals.back().depth = asFuncScope(funcScope())->scopeDepth;
        return;
    }

    // emit code to define named global variable at runtime
    if (var > 255) // TODO: remove when DefineGlobal2 supported
        throw std::runtime_error("Max of 255 global vars supported");

    emitBytes(OpCode::DefineGlobal, uint8_t(var));
}


bool RoxalCompiler::namedVariable(const icu::UnicodeString& name, bool assign)
{
    //std::cout << (&(*state()) - &(*states.begin())) << " namedVariable(" << toUTF8StdString(name) << ")" << std::endl;
    //std::cout << toUTF8StdString(funcScope()->function->name) << " namedVariable(" << toUTF8StdString(name) << ")" << std::endl;

    OpCode getOp, setOp;
    bool found = false;

    int16_t arg = resolveLocal(funcScope(),name);
    if (arg != -1) { // found
        found = true;
        getOp = (arg<=255) ? OpCode::GetLocal : OpCode::GetLocal2;
        setOp = (arg<=255) ? OpCode::SetLocal : OpCode::SetLocal2;
    }
    // else if ((funcScope()->functionType == FunctionType::Method ) && ((arg = resolveLocal(funcScope(),"this") != -1))) {
    //     // if we have a property name, allow access without 'this.' prefix
    //     if (!typeScopes.empty()) {
    //         // FIXME: statically check the property exists.. (otherwise we're blocking all outer scope local access..)
    //         //std::cout << funcScope() << std::endl;       
    //         arg = identifierConstant(name);
    //         namedVariable("this", false);
    //         //emitBytes(OpCode::GetProp, uint8_t(identConstant));
    //         getOp = (arg<=255) ? OpCode::GetProp : OpCode::GetProp2;
    //         setOp = (arg<=255) ? OpCode::SetProp : OpCode::SetProp2;
    //         found = true;
    //     }
    // }

    if (!found && ((arg = resolveUpvalue(funcScope(),name)) != -1)) {
        found = true;
        getOp = (arg<=255) ? OpCode::GetUpvalue : OpCode::GetUpvalue2;
        setOp = (arg<=255) ? OpCode::SetUpvalue : OpCode::SetUpvalue2;
    }

    if (!found) { // local, not found
        // assume global
        arg = identifierConstant(name);
        getOp = (arg<=255) ? OpCode::GetGlobal : OpCode::GetGlobal2;
        //  allow assigning without previously declaring, except within functions
        if (asFuncScope(funcScope())->functionType != FunctionType::Module)
            setOp = (arg<=255) ? OpCode::SetGlobal : OpCode::SetGlobal2;
        else 
            setOp = (arg<=255) ? OpCode::SetNewGlobal : OpCode::SetNewGlobal2;
    }

    if (!assign) {
        if (arg <= 255)
            emitBytes(getOp, arg, toUTF8StdString(name));
        else
            emitBytes(getOp, arg>>8, arg%256, toUTF8StdString(name));
    }
    else {
        if (arg <= 255)
            emitBytes(setOp, arg, toUTF8StdString(name));
        else
            emitBytes(setOp, arg>>8, arg%256, toUTF8StdString(name));
    }

    return true;
}
