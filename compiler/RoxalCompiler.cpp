#include <filesystem>
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
    : outputBytecodeDisassembly(false)
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

        enterModuleScope("",toUnicodeString(name));

        auto module { asModuleScope(moduleScope()) };

        module->strict = false;

        try {
            auto file = as<File>(ast);

            file->accept(*this);

            function = module->function;

            assert(function != nullptr);

            if (outputBytecodeDisassembly)
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


void RoxalCompiler::setOutputBytecodeDisassembly(bool outputBytecodeDisassembly)
{
    this->outputBytecodeDisassembly = outputBytecodeDisassembly;
}

void RoxalCompiler::setModulePaths(const std::vector<std::string>& modulePaths)
{
    this->modulePaths = modulePaths;
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


std::any RoxalCompiler::visit(ptr<ast::Import> ast)
{

    currentNode = ast;

    // search the module paths (as package component roots)
    //  for the specified module
    ModuleInfo module = findImport(ast->packages);

    if (module.name.empty()) {
        error("import '"+toUTF8StdString(join(ast->packages,"."))+"' not found.");
        return {};
    }

    std::string absoluteModuleFilePath = std::filesystem::canonical(std::filesystem::absolute(module.modulePathRoot + "/" + module.packagePath + '/' + module.filename));

    // extra check the module file exists
    if (!std::filesystem::exists(std::filesystem::path(absoluteModuleFilePath))) {
        error("import file '"+module.packagePath + '/' + module.filename+"' not found.");
        return {};
    }

    // has this module already been imported?
    auto importedEntry = importedModules.find(module);
    bool imported = importedEntry != importedModules.end();

    //std::cout << module << std::endl;

    Value importedModuleType {};

    if (!imported) {  // import it

        // compile it, emit code to execute it
        //std::cout << "importing " << absoluteModuleFilePath << std::endl;

        // compile to generate code for imported module
        std::ifstream sourcestream(absoluteModuleFilePath);

        ObjFunction* function { nullptr };

        try {
            // compile to generate code for imported module
            function = compile(sourcestream, module.name);

            importedModuleType = function->moduleType;

            // emit code to place module's main chunk on stack as closure
            assert(function->upvalueCount == 0);
            emitBytes(OpCode::Closure, makeConstant(objVal(function)));

            // call it to have it executed (which will result in module vars being declared)
            CallSpec callSpec {};
            callSpec.allPositional = true;
            callSpec.argCount = 0;
            auto bytes = callSpec.toBytes();
            assert(bytes.size()==1);
            emitBytes(OpCode::Call, bytes[0]);

            importedModules[module] = importedModuleType;

        } catch (std::exception& e) {
            error(e.what());
            return {};
        }
    }
    else { // already previously imported
        //std::cout << absoluteModuleFilePath << " already imported" << std::endl;
        importedModuleType = importedEntry->second;
    }

    // define a variable in the importing module with the name of the imported module
    //  that has the value of the ObjModuleType
    //  (we can directly insert the var in the importing module since it is already existing static type)
    const auto& importingModuleType = asFuncScope(funcScope())->function->moduleType;
    auto& importingModuleVars = asModuleType(importingModuleType)->vars;
    icu::UnicodeString moduleName { toUnicodeString(module.name) };
    importingModuleVars.store(moduleName, importedModuleType);


    // if any (or all) symbols are explicitly imported into the importing module scope,
    //  create vars for those too
    if (!ast->symbols.empty()) {

        // convert AST symbols to a List of Values
        std::vector<Value> symbolsList {};
        for(const auto& symbol : ast->symbols)
            symbolsList.push_back(objVal(stringVal(symbol)));

        Value symbolsListVal { listVal() };
        asList(symbolsListVal)->elts = symbolsList;

        // Opcode::ImportModuleVars expects a list (of symbols) and the source module & target module
        emitConstant(symbolsListVal, "import vars "+toUTF8StdString(join(ast->symbols)));

        emitConstant(importedModuleType, "imported module type "+module.name);
        emitConstant(importingModuleType, "importing module type "+toUTF8StdString(asModuleType(importingModuleType)->name));

        emitByte(OpCode::ImportModuleVars);
    }


    return {};
}



std::any RoxalCompiler::visit(ptr<ast::TypeDecl> ast)
{
    currentNode = ast;

    bool isActor = ast->kind==TypeDecl::Actor;
    bool isInterface = ast->kind==TypeDecl::Interface;
    bool isEnumeration = ast->kind==TypeDecl::Enumeration;

    enterTypeScope(ast->name);

    // inherit property registry from super type if available
    if (ast->extends.has_value()) {
        auto superName = ast->extends.value();
        auto it = typePropertyRegistry.find(superName);
        if (it != typePropertyRegistry.end())
            asTypeScope(typeScope())->propertyNames.insert(it->second.begin(), it->second.end());
    }

    int16_t typeNameConstant = identifierConstant(ast->name);
    declareVariable(ast->name);

    if (ast->implements.size()>2)
        throw std::runtime_error("Multiple implements types unimplemented.");

    if (isInterface && (ast->implements.size() > 0))
        throw std::runtime_error("Interfaces can't implement (only extend)");

    if (isActor) emitBytes(OpCode::ActorType, typeNameConstant);
    else if (isInterface) emitBytes(OpCode::InterfaceType, typeNameConstant);
    else if (isEnumeration) emitBytes(OpCode::EnumerationType, typeNameConstant);
    else emitBytes(OpCode::ObjectType, typeNameConstant);
    defineVariable(typeNameConstant);


    // handle extension (inheritance)
    if (ast->extends.has_value() && !isEnumeration) {
        asTypeScope(typeScope())->hasSuperType = true;

        auto superTypeName = ast->extends.value();

        // can't inherit yourself
        if (superTypeName == ast->name)
            error("Type object, actor or interface '"+toUTF8StdString(ast->name)+"' can't extend itself.");

        namedVariable(superTypeName, /*assign=*/false); // parent (super)

        enterLocalScope();
        addLocal("super");
        defineVariable(0);

        namedVariable(ast->name, /*assign=*/false); // child (sub)
        emitByte(OpCode::Extend);
    }


    namedVariable(ast->name, false); // make type accessible on the stack

    for(size_t i=0; i<ast->properties.size(); i++) {

        if (isInterface) {
            error("Interfaces cannot declare properties");
            break;
        }

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

        // record property name for implicit access within methods
        asTypeScope(typeScope())->propertyNames.insert(propName);

        // type
        if (prop->varType.has_value()) {
            auto varType { prop->varType.value() };

            if (std::holds_alternative<BuiltinType>(varType)) {
                auto builtinType { std::get<BuiltinType>(varType) };
                Value typeValue { typeSpecVal(builtinToValueType(builtinType)) };

                emitConstant(typeValue, "prop "+toUTF8StdString(propName)+" type");
            }
            else { // assume string names module scope (local?) type var
                // will emit GetLocal or GetModuleVar (or GetUpValue)
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

    } // properties


    for(size_t i=0; i<ast->methods.size(); i++) {

        auto func { ast->methods.at(i) };

        assert(func->name.has_value()); // methods must have names
        auto methodName { func->name.value() };
        asTypeScope(typeScope())->propertyNames.insert(methodName);
        int16_t methodNameConstant = identifierConstant(methodName);
        if (methodNameConstant >= 255)
            error("Too many methods for one actor or object type.");

        func->accept(*this);

        emitBytes(OpCode::Method, uint8_t(methodNameConstant), "method "+toUTF8StdString(methodName));
    }


    if (isEnumeration) {

        for(size_t i=0; i<ast->enumLabels.size(); i++) {

            const auto& enumLabel { ast->enumLabels.at(i) };

            // TODO: TypeDeducer currenly adds values to enum labels if missing
            //  (but maybe this will be moved to another pass or to here)
            assert(enumLabel.second != nullptr);

            auto labelName { enumLabel.first };
            int16_t propNameConstant = identifierConstant(labelName);
            if (propNameConstant >= 255)
                error("Too many enum labels for one enum type.");

            assert(enumLabel.second->type.has_value());
            auto valType { enumLabel.second->type.value() };

            ptr<ast::Literal> literalExpr { std::dynamic_pointer_cast<ast::Literal>(enumLabel.second) };
            assert(literalExpr != nullptr); // currently expected to be a literal
            Value value {};
            if (literalExpr->literalType == ast::Literal::LiteralType::Num) {
                ptr<ast::Num> numExpr { std::dynamic_pointer_cast<ast::Num>(literalExpr) };
                if (valType->builtin == BuiltinType::Byte)
                    value = byteVal(std::get<int>(numExpr->num));
                else if (valType->builtin == BuiltinType::Int)
                    value = intVal(std::get<int>(numExpr->num));
                else
                    error("Unsupported literal type for enum label.");
            }
            else
                error("Unsupported literal type for enum label.");

            emitConstant(value);

            emitBytes(OpCode::EnumLabel, uint8_t(propNameConstant), "enum value "+toUTF8StdString(labelName));
        }
    }


    emitByte(OpCode::Pop, "type name");

    if (asTypeScope(typeScope())->hasSuperType)
        exitLocalScope();

    // record collected property names for this type for use by derived types
    typePropertyRegistry[ast->name] = asTypeScope(typeScope())->propertyNames;

    exitTypeScope();

    return {};
}


std::any RoxalCompiler::visit(ptr<ast::FuncDecl> ast)
{
    currentNode = ast;

    auto func {as<Function>(ast->func) };
    assert(func->name.has_value()); // func declarations must have names
    auto name { func->name.value() };

    declareVariable(name);

    uint16_t var { 0 };
    if (asFuncScope(funcScope())->scopeDepth == 0) // module variable
        var = identifierConstant(name); // create constant table entry for name

    if (asFuncScope(funcScope())->scopeDepth > 0) {
        // mark initialized
        asFuncScope(funcScope())->locals.back().depth = asFuncScope(funcScope())->scopeDepth;
    }

    Anys results {};
    ast->acceptChildren(*this, results);

    // unwrap ObjFunction* returned by visit(ptr<Function>)
    auto function = std::any_cast<ObjFunction*>(std::any_cast<Anys>(results.at(0)).at(0));

    // attached the FuncDecl annotations (which appear right before the func declaration)
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


std::any RoxalCompiler::visit(ptr<ast::ForStatement> ast)
{
    currentNode = ast;

    #ifdef DEBUG_BUILD
    emitByte(OpCode::Nop, "for scope");
    #endif
    enterLocalScope();

    // declare a local for the loop index
    icu::UnicodeString iname = "__index__";
    declareVariable(iname);
    emitByte(OpCode::ConstInt0);
    defineVariable();

    // declare local vars for each for target
    std::vector<icu::UnicodeString> targetVarNames {};

    uint8_t numTargets = ast->targetList.size();
    if (numTargets > 128) {
        error("Too many target variables in for statement.");
        return {};
    }
    for(auto i = 0; i < numTargets; i++) {
        assert(isa<VarDecl>(ast->targetList.at(i)));
        auto name = as<VarDecl>(ast->targetList.at(i))->name;
        // TODO: store type
        targetVarNames.push_back(name);
        declareVariable(name);
        emitByte(OpCode::ConstNil);
        defineVariable();
    }



    // evaluate the iterable
    ast->iterable->accept(*this);

    // special case for iterating over dicts:
    //  if single target, convert to list of keys
    //  otherwise, convert to list of key-value pairs (list of two elements)
    if (numTargets == 1)
        emitByte(OpCode::IfDictToKeys);
    else if (numTargets >= 2)
        emitByte(OpCode::IfDictToItems);

    // compute the length of the iterable

    // first find built-in global "len" function
    namedModuleVariable("len");

    // dup the iterable as arg for len
    emitByte(OpCode::DupBelow);

    // call it
    CallSpec lenCallSpec { 1 };
    auto lenCallSpecBytes = lenCallSpec.toBytes();
    assert(lenCallSpecBytes.size() == 1);
    emitBytes(OpCode::Call, lenCallSpecBytes[0]);

    // now we have stack: [iterable, len(iterable)]

    // check if len(iterable) == nil (e.g. for range, implies the range isn't definite)
    emitByte(OpCode::Dup); // len
    emitByte(OpCode::ConstNil);
    emitByte(OpCode::Equal);
    auto jumpToAbort = emitJump(OpCode::JumpIfTrue);
    emitByte(OpCode::Pop, "abort cond");

    auto loopStart = currentChunk()->code.size();
    #ifdef DEBUG_BUILD
    emitByte(OpCode::Nop, "for loop body");
    #endif

    // check condition iname < len(iterable)
    namedVariable(iname);
    emitByte(OpCode::DupBelow); // len
    emitByte(OpCode::Less);
    auto jumpToExit = emitJump(OpCode::JumpIfFalse);
    emitByte(OpCode::Pop, "exit cond");


    // index the iterable via the loop index
    emitByte(OpCode::DupBelow); // iterable
    namedVariable(iname);
    emitBytes(OpCode::Index, 1); // single index/arg indexing

    // if there is a single target, just assign the target the result of indexing the iterable (stack top)
    if (numTargets == 1) {
        namedVariable(targetVarNames.at(0),/*assign=*/true);
        emitByte(OpCode::Pop, "index result"); // discard index
    }
    else {
        // otherwise, index into the index result for the number of targets and assign each target
        for(auto i = 0; i < numTargets; i++) {
            emitByte(OpCode::Dup); // dup index result
            if (i==0)
                emitByte(OpCode::ConstInt0);
            else if (i==1)
                emitByte(OpCode::ConstInt1);
            else
                emitConstant(intVal(i));
            emitBytes(OpCode::Index, 1);

            // assign it to target
            namedVariable(targetVarNames.at(i),/*assign=*/true);

            emitByte(OpCode::Pop, "subindex result"); // discard index
        }
        emitByte(OpCode::Pop, "index result"); // discard index
    }

    // generate code for the body
    ast->body->accept(*this);

    // increment the loop index
    //  TODO: add Inc opcode (or IncLocal?)
    namedVariable(iname);
    emitByte(OpCode::ConstInt1);
    emitByte(OpCode::Add);
    namedVariable(iname, /*assign=*/true);
    emitByte(OpCode::Pop);

    emitLoop(loopStart);

    patchJump(jumpToExit);
    patchJump(jumpToAbort);
    emitByte(OpCode::Pop, "exit/abort cond");

    emitBytes(OpCode::PopN, 2, "iterable & len"); // discard the iterable & it's length (necessary?)

    exitLocalScope();

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

    auto enclosingModuleScope { asModuleScope(moduleScope()) };

    icu::UnicodeString funcName;
    if (ast->name.has_value())
        funcName = ast->name.value();
    else { // lambda func? create unique name using module name and source line position
        funcName = icu::UnicodeString::fromUTF8("__func_" + toUTF8StdString(enclosingModuleScope->moduleName)
                    +"_"+std::to_string(ast->interval.first.line)
                    +"_"+std::to_string(ast->interval.first.pos));
    }

    enterFuncScope(enclosingModuleScope->moduleType, funcName, ftype, ast->type.value());

    #ifdef DEBUG_BUILD
    emitByte(OpCode::Nop, "func "+toUTF8StdString(funcName));
    #endif
    enterLocalScope();

    asFuncScope(funcScope())->function->arity = ast->params.size();
    if (asFuncScope(funcScope())->function->arity > 255)
        error("Maximum of function or procedure 255 parameters exceeded.");

    Anys results {};
    ast->acceptChildren(*this, results);

    // if the body is an expression (e.g. lambda func), leaves the result on the stack, so return it
    if (std::holds_alternative<ptr<Expression>>(ast->body))
        emitByte(OpCode::Return);

    //exitLocalScope();

    if (lastByte() != uint8_t(OpCode::Return)) // if the code didn't conclude with a return, add one
        emitReturn();

    if (outputBytecodeDisassembly)
        asFuncScope(funcScope())->function->chunk->disassemble(asFuncScope(funcScope())->function->name);

    ObjFunction* function = asFuncScope(funcScope())->function;

    auto functionScope { *asFuncScope(funcScope()) };

    exitFuncScope(); // back to surrounding scope

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

        auto enclosingModuleScope { asModuleScope(moduleScope()) };

        enterFuncScope(enclosingModuleScope->moduleType, ast->name, FunctionType::Function, defFuncType);

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

        if (outputBytecodeDisassembly)
            asFuncScope(funcScope())->function->chunk->disassemble(asFuncScope(funcScope())->function->name);

        ObjFunction* function = asFuncScope(funcScope())->function;

        exitFuncScope(); // back to surrpounding scope

        // store the func that evaluates the default param value in the function
        //  for which it is a param
        function->incRef();
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


std::any RoxalCompiler::visit(ptr<ast::LambdaFunc> ast)
{
    currentNode = ast;

    auto func {as<Function>(ast->func) };

    Anys results {};
    ast->acceptChildren(*this, results);

    // unwrap ObjFunction* returned by visit(ptr<Function>)
    auto function = std::any_cast<ObjFunction*>(std::any_cast<Anys>(results.at(0)).at(0));

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



RoxalCompiler::ModuleInfo RoxalCompiler::findImport(const std::vector<icu::UnicodeString>& components) const
{
    // search the module paths (as package component roots)
    //  for the specified module
    std::vector<std::filesystem::path> candidatePaths; // paths that match the prefix, thus far
    for (const auto& modulePath : modulePaths)
        candidatePaths.push_back(std::filesystem::canonical(std::filesystem::absolute(modulePath)));

    //std::cout << "initial candidates:";
    //for (const auto& path : candidatePaths)
    //    std::cout << path << std::endl;

    size_t importComponentIndex = 0;
    // for each component of the import
    while (importComponentIndex < components.size()) {
        bool isLastComponent = (importComponentIndex == components.size()-1);

        // filter for the paths from the candidates thus far that match upto the current component
        std::vector<std::filesystem::path> newCandidatePaths {};
        for (const auto& modulePath : candidatePaths) {
            // list of folders and files in modulePath
            for (const auto& entry : std::filesystem::directory_iterator(modulePath)) {
                //std::cout << "considering " << entry.path() << " from module path" << modulePath << std::endl;
                auto entryName = toUnicodeString(entry.path().filename().string());
                if (entry.is_directory()) { // package
                    if (entryName == components.at(importComponentIndex))
                        newCandidatePaths.push_back(entry.path());
                }
                else if (isLastComponent && (entryName == components.at(importComponentIndex)+".rox")) {
                    //std::cout << "found: " << entry.path() << std::endl;
                    newCandidatePaths.push_back(entry.path());
                    break; // found the module, no need to search further
                }
            }
        }
        candidatePaths = newCandidatePaths;
        importComponentIndex++;
    }


    // debug - output candidate paths
    //std::cout << "final candidates:";
    //for (const auto& path : candidatePaths)
    //    std::cout << path << std::endl;

    if (candidatePaths.empty()) // not found
        return {};


    // found
    auto path { candidatePaths.at(0) }; // take first (if multiple)
    ModuleInfo module {};
    module.filename = path.filename().string();
    for (auto& modulePath : modulePaths) {
        auto absModulePath = std::filesystem::canonical(std::filesystem::absolute(modulePath));
        if (startsWith(path, absModulePath)) {
            module.modulePathRoot = modulePath;
            module.packagePath = std::filesystem::relative(path, absModulePath).parent_path().string();
            module.isPackage = std::filesystem::is_directory(path); // FIXME: handle package module file above
            module.filename = path.filename().string();
            module.name = path.stem().string();
            return module;
        }
    }

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



void RoxalCompiler::enterModuleScope(const icu::UnicodeString& packageName, const icu::UnicodeString& moduleName)
{
    auto moduleScope { std::make_shared<ModuleScope>(packageName, moduleName) };

    lexicalScopes.push_back(moduleScope);
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
    auto module { asModuleScope(moduleScope() };
    std::cout << "exitModuleScope("
              << (!module->packageName.isEmpty() ? toUTF8StdString(module->packageName)+"." : "")
              << toUTF8StdString(module->moduleName) << ")" << std::endl;
    #endif

    // store the ObjModuleType for this module in its associated objFunction, so the VM
    //  can access it at runtime to declare and access module variables
    auto modScope { asModuleScope(scope()) };
    #ifdef DEBUG_BUILD
    assert(!modScope->moduleType.isNil() && modScope->moduleType.isObj());
    assert(modScope->function != nullptr);
    #endif
    modScope->function->moduleType = modScope->moduleType;

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


void RoxalCompiler::enterFuncScope(Value moduleType, const icu::UnicodeString& funcName, FunctionType funcType, ptr<type::Type> type)
{
    // function scopes only valid in a module
    auto modScope { asModuleScope(moduleScope()) };

    auto funcScope {std::make_shared<FunctionScope>(modScope->packageName, modScope->moduleName,
                                                    funcName,funcType,type)};

    funcScope->function->moduleType = moduleType;

    lexicalScopes.push_back(funcScope);

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


bool RoxalCompiler::inModuleScope()
{
    for(auto i = lexicalScopes.rbegin(); i != lexicalScopes.rend(); ++i)
        if ((*i)->scopeType == LexicalScope::ScopeType::Module)
            return true;
    return false;
}

RoxalCompiler::Scope RoxalCompiler::moduleScope()
{
    auto s = scope();
    while ((*s)->scopeType != LexicalScope::ScopeType::Module)
        s = enclosingScope(s);
    return s;
}

RoxalCompiler::Scope RoxalCompiler::enclosingModuleScope(Scope s)
{
    auto es = enclosingScope(s);
    while ((*es)->scopeType != LexicalScope::ScopeType::Module)
        es = enclosingScope(es);
    return es;
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

uint8_t RoxalCompiler::lastByte()
{
    return currentChunk()->lastByte();
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
    //std::cout << (&(*state()) - &(*states.begin())) << " addLocal(" << toUTF8StdString(name) << ")" << std::endl;
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
    //std::cout << (&(*scopeState) - &(*states.begin()))<< " resolveLocal(" << toUTF8StdString(name) << ")" << std::endl;
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
    //std::cout << (&(*scopeState) - &(*states.begin())) << " addUpvalue(" << index << " " << (isLocal ? "local" : "notlocal") << ")" << std::endl;
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
    //std::cout << (&(*scopeState) - &(*states.begin())) << " resolveUpvalue(" << toUTF8StdString(name) << ")" << std::endl;
    //std::string sname { toUTF8StdString(name) };
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
        asFuncScope(enclosingFuncScope(scopeState))->locals[local].isCaptured = true;
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


void RoxalCompiler::defineVariable(uint16_t moduleVar)
{
    // local variables are already on the stack
    if (asFuncScope(funcScope())->scopeDepth > 0) {
        // mark initialized
        asFuncScope(funcScope())->locals.back().depth = asFuncScope(funcScope())->scopeDepth;
        return;
    }

    // emit code to define named module scope variable at runtime
    if (moduleVar > 255) // TODO: remove when DefineModuleVar2 supported
        throw std::runtime_error("Max of 255 module vars supported");

    emitBytes(OpCode::DefineModuleVar, uint8_t(moduleVar));
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

    if (!found) { // local or upvalue not found
        // try module scope first
        arg = identifierConstant(name);
        getOp = (arg<=255) ? OpCode::GetModuleVar : OpCode::GetModuleVar2;
        //  allow assigning without previously declaring, except within functions
        if (asFuncScope(funcScope())->functionType != FunctionType::Module)
            setOp = (arg<=255) ? OpCode::SetModuleVar : OpCode::SetModuleVar2;
        else
            setOp = (arg<=255) ? OpCode::SetNewModuleVar : OpCode::SetNewModuleVar2;

        // if module variable isn't found at runtime, the VM will raise an error.
        // to allow implicit property access, check for 'this' method context as fallback
        if (asFuncScope(funcScope())->functionType == FunctionType::Method ||
            asFuncScope(funcScope())->functionType == FunctionType::Initializer) {
            int16_t thisLocal = resolveLocal(funcScope(), UnicodeString("this"));
            if (thisLocal != -1 &&
                asTypeScope(typeScope())->propertyNames.count(name)>0) {
                // treat as property access
                if (!assign) {
                    namedVariable(UnicodeString("this"), false);
                    emitBytes(arg<=255 ? OpCode::GetProp : OpCode::GetProp2, arg);
                } else {
                    namedVariable(UnicodeString("this"), false);
                    emitByte(OpCode::Swap);
                    emitBytes(arg<=255 ? OpCode::SetProp : OpCode::SetProp2, arg);
                }
                return true;
            }
        }
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


void RoxalCompiler::namedModuleVariable(const icu::UnicodeString& name, bool assign)
{
    OpCode getOp, setOp;

    int16_t arg = identifierConstant(name);
    getOp = (arg<=255) ? OpCode::GetModuleVar  : OpCode::GetModuleVar2;
    //  allow assigning without previously declaring, except within functions
    if (asFuncScope(funcScope())->functionType != FunctionType::Module)
        setOp = (arg<=255) ? OpCode::SetModuleVar : OpCode::SetModuleVar2;
    else
        setOp = (arg<=255) ? OpCode::SetNewModuleVar : OpCode::SetNewModuleVar2;

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
}


std::ostream& roxal::operator<<(std::ostream& out, const RoxalCompiler::ModuleInfo& mi) {
    out << "ModuleInfo {"
        << "modulePathRoot: " << mi.modulePathRoot << ", "
        << "packagePath: " << mi.packagePath << ", "
        << "name: " << mi.name << ", "
        << "isPackage: " << (mi.isPackage ? "true" : "false") << ", "
        << "filename: " << mi.filename
        << "}";
    return out;
}
