

#include <boost/algorithm/string/replace.hpp>

#include "Object.h"

#include "RoxalIndentationLexer.h"

#include "RoxalCompiler.h"

using namespace roxal;
using antlrcpp::Any;



class ParseTracer {
public:
    ParseTracer(std::string method, antlr4::ParserRuleContext* context)
    {
        #if defined(DEBUG_TRACE_PARSE)
        auto pair { std::make_pair(method,context->getText()) };
        boost::replace_all(pair.second, "\n", "\\n");
        std::string spaces(parseStack.size(),' ');
        std::cout << spaces << std::to_string(context->start->getLine()) << ":" << pair.first << ":" << pair.second << std::endl;
        parseStack.push(pair);
        #endif
    }

    ~ParseTracer()
    {
        #if defined(DEBUG_TRACE_PARSE)
        parseStack.pop();
        #endif
    }

protected:
    static std::stack<std::pair<std::string,std::string>> parseStack;
};

std::stack<std::pair<std::string,std::string>> ParseTracer::parseStack {};


void traceContext(std::string method, antlr4::ParserRuleContext* context)
{
    std::cout << method << ":" << context->getText() << std::endl;
}



ObjFunction* RoxalCompiler::compile(std::istream& source, const std::string& name)
{
    using namespace antlr4;

    ANTLRInputStream input(source);
    RoxalIndentationLexer lexer(&input);
    CommonTokenStream tokens(&lexer);

    #if defined(DEBUG_OUTPUT_LEXER_TOKENS)
    std::cout << "== tokens ==" << std::endl;
    tokens.fill();
    auto vocab = lexer.getVocabulary();
    auto tokenvec { tokens.getTokens() };
    for(size_t i=0; i<tokenvec.size(); i++) {
        auto token { tokenvec[i] };
        std::cout << token->toString() << " " << vocab.getDisplayName(token->getType()) << std::endl;
    }
    std::cout << std::endl;
    #endif

    RoxalParser parser(&tokens);    
    
    auto tree = parser.file_input();

    #if defined(DEBUG_OUTPUT_PARSE_TREE)
    std::cout << "== parse tree ==" << std::endl << tree::Trees::toStringTree(tree,&parser,true) << std::endl;
    #endif

    // TODO: should we convert the parse tree into an AST before code generation?
    //   (to allow other passes to do rewriting, e.g. optimization)

    ObjFunction* function { nullptr };

    if (parser.getNumberOfSyntaxErrors() == 0) {

        funcScopes.push_back(FunctionScope(toUnicodeString(name), FunctionType::Module, false));

        funcScope()->strict = false;

        try {
            auto result = visitFile_input(tree);
            if (!result.isNull())
                throw std::runtime_error("visitFile_input returned non-Value resut in context "+tree->getText());
            
            function = funcScope()->function;

            #if defined(DEBUG_OUTPUT_CHUNK)
            funcScope()->function->chunk->disassemble(funcScope()->function->name);
            #endif
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





Any RoxalCompiler::visitFile_input(RoxalParser::File_inputContext *context) 
{
    ParseTracer pt(__func__, context);
    currentToken = context->start;

    for(auto& declaration : context->declaration())
        visitDeclaration(declaration);

    emitReturn();

    return Any();
}


Any RoxalCompiler::visitSingle_input(RoxalParser::Single_inputContext *context)
{
    ParseTracer pt(__func__, context);
    currentToken = context->start;

    if (context->statement())
        visitStatement(context->statement());
    else if (context->compound_stmt())
        visitCompound_stmt(context->compound_stmt());

    return Any();
}


Any RoxalCompiler::visitDeclaration(RoxalParser::DeclarationContext *context) 
{
    ParseTracer pt(__func__, context);
    currentToken = context->start;

    if (context->type_decl())
        visitType_decl(context->type_decl());
    else if (context->func_decl())
        visitFunc_decl(context->func_decl());
    else if (context->var_decl())
        visitVar_decl(context->var_decl());
    else if (context->statement())
        visitStatement(context->statement());
    else
        throw std::runtime_error("unimplemented declaration type");

    return Any();
}

Any RoxalCompiler::visitStatement(RoxalParser::StatementContext *context) 
{
    ParseTracer pt(__func__, context);
    currentToken = context->start;

    if (context->expr_stmt())
        visitExpr_stmt(context->expr_stmt());
    //if (context->simple_stmt())
    //    visitSimple_stmt(context->simple_stmt());
    else if (context->compound_stmt())
        visitCompound_stmt(context->compound_stmt());
    return Any();
}


Any RoxalCompiler::visitExpr_stmt(RoxalParser::Expr_stmtContext *context) 
{
    ParseTracer pt(__func__, context);
    currentToken = context->start;

    visitExpression(context->expression());

    // expressions leave their value on the stack, but statements don't
    //  have a value, so discard it
    emitByte(OpCode::Pop, "expr_stmt value");

    return Any();
}

Any RoxalCompiler::visitSuite(RoxalParser::SuiteContext *context) 
{
    ParseTracer pt(__func__, context);
    currentToken = context->start;

    for(int i=0; i<context->declaration().size(); i++)
        visitDeclaration(context->declaration().at(i));

    return Any();
}


Any RoxalCompiler::visitType_decl(RoxalParser::Type_declContext *context)
{
    ParseTracer pt(__func__, context);
    currentToken = context->start;

    bool isActor = (context->ACTOR() != nullptr);

    int nextIdentIndex { 0 };
    auto typeName { context->IDENTIFIER().at(nextIdentIndex++)->getText() };
    UnicodeString uTypeName { UnicodeString::fromUTF8(typeName) };

    int16_t typeNameConstant = identifierConstant(uTypeName);
    declareVariable(uTypeName);    

    if (context->IDENTIFIER().size()>2)
        throw std::runtime_error("Multiple implements types unimplemented.");

    emitBytes(isActor ? OpCode::ActorType : OpCode::ObjectType, typeNameConstant);
    defineVariable(typeNameConstant);

    namedVariable(uTypeName, false); // make type accessible on the stack

    typeScopes.push_back(TypeScope());
    auto& typeScope { typeScopes.back() };

    typeScope.hasSuperType = (context->EXTENDS() != nullptr);
    if (typeScope.hasSuperType) {
        auto superName { context->IDENTIFIER().at(nextIdentIndex++)->getText() };
        UnicodeString uSuperName { UnicodeString::fromUTF8(superName) };

        if (uSuperName == uTypeName) 
            error("An actor or object type cannot extend itself.");        

        namedVariable(uSuperName, false);

        beginScope();
        addLocal("super");
        defineVariable(0);

        namedVariable(uTypeName, false);
        emitByte(OpCode::Extend);
    }
    if (context->IMPLEMENTS()) {
        throw std::runtime_error("interface implements not implemented");
    }

    for(size_t i=0; i<context->function().size(); i++) {

        auto funcContext { context->function().at(i) };

        auto methodName { funcContext->IDENTIFIER()->getText() };
        UnicodeString uMethodName { UnicodeString::fromUTF8(methodName) };
        int16_t methodNameConstant = identifierConstant(uMethodName);
        if (methodNameConstant >= 255) 
            error("Too many methods for one actor or object type.");

        visitFunction(funcContext);

        emitBytes(OpCode::Method, uint8_t(methodNameConstant), "method "+methodName);
    }

    emitByte(OpCode::Pop, "type name");

    if (typeScope.hasSuperType)
        endScope();

    typeScopes.pop_back();

    return Any();
}



Any RoxalCompiler::visitExpression(RoxalParser::ExpressionContext *context) 
{
    ParseTracer pt(__func__, context);
    currentToken = context->start;

    visitAssignment(context->assignment());
    return Any();
}


Any RoxalCompiler::visitCompound_stmt(RoxalParser::Compound_stmtContext *context)
{
    ParseTracer pt(__func__, context);
    currentToken = context->start;

    if (context->print_stmt())
        visitPrint_stmt(context->print_stmt());
    else if (context->return_stmt())
        visitReturn_stmt(context->return_stmt());
    else if (context->block_stmt())
        visitBlock_stmt(context->block_stmt());
    else if (context->if_stmt())
        visitIf_stmt(context->if_stmt());
    else if (context->while_stmt())
        visitWhile_stmt(context->while_stmt());
    else
        throw std::runtime_error("unimplemented compound statement alternative");

    return Any();
}


Any RoxalCompiler::visitBlock_stmt(RoxalParser::Block_stmtContext *context)
{
    ParseTracer pt(__func__, context);
    currentToken = context->start;

    beginScope();
    visitSuite(context->suite());
    endScope();

    return Any();
}


Any RoxalCompiler::visitPrint_stmt(RoxalParser::Print_stmtContext *context)
{
    ParseTracer pt(__func__, context);
    currentToken = context->start;

    visitExpression(context->expression());
    emitByte(OpCode::Print);
    return Any();
}


Any RoxalCompiler::visitReturn_stmt(RoxalParser::Return_stmtContext *context)
{
    ParseTracer pt(__func__, context);
    currentToken = context->start;

    if (context->expression()) {

        if (funcScope()->functionType == FunctionType::Initializer)
            error("A value cannot be returned from an 'init' method.");
        if (funcScope()->isProc)
            error("A value cannot be returned from an proc method.");

        visitExpression(context->expression());

        // TODO: check the return type

        emitByte(OpCode::Return);

    }
    else
        emitReturn();

    return Any();
}



Any RoxalCompiler::visitIf_stmt(RoxalParser::If_stmtContext *context)
{
    ParseTracer pt(__func__, context);
    currentToken = context->start;

    // if condition 
    visitExpression(context->expression().at(0));

    auto jumpOverIf = emitJump(OpCode::JumpIfFalse);
    emitByte(OpCode::Pop, "if cond");

    beginScope();
    visitSuite(context->suite().at(0));
    endScope();

    auto jumpOverElse = emitJump(OpCode::Jump);

    patchJump(jumpOverIf);

    if (context->ELSEIF().size()>0) {
        throw std::runtime_error("elseif unimplemented");
        for(int i=1; i<context->expression().size();i++) {
            visitExpression(context->expression().at(i));
            visitSuite(context->suite().at(i));
        }
    }

    emitByte(OpCode::Pop, "if cond");
    if (context->ELSE()) {
        beginScope();
        visitSuite(context->suite().at(context->suite().size()-1));
        endScope();
    }
    
    patchJump(jumpOverElse);

    return Any();
}


Any RoxalCompiler::visitWhile_stmt(RoxalParser::While_stmtContext *context)
{
    ParseTracer pt(__func__, context);
    currentToken = context->start;

    auto loopStart = currentChunk()->code.size();

    // while condition 
    visitExpression(context->expression());

    auto jumpToExit = emitJump(OpCode::JumpIfFalse);
    emitByte(OpCode::Pop, "while cond");

    visitSuite(context->suite());

    emitLoop(loopStart);

    patchJump(jumpToExit);
    emitByte(OpCode::Pop, "while cond");

    return Any();
}


Any RoxalCompiler::visitVar_decl(RoxalParser::Var_declContext *context)
{
    ParseTracer pt(__func__, context);
    currentToken = context->start;

    auto identifier { context->IDENTIFIER()->getText() };
    UnicodeString uident { UnicodeString::fromUTF8(identifier) };

    declareVariable(uident);
    uint16_t var { 0 };
    if (funcScope()->scopeDepth == 0) // global variable
        var = identifierConstant(uident); // create constant table entry for name

    // TODO: support type spec and init with default for type

    if (context->EQUALS()) {
        visitExpression(context->expression());
    }
    else
        emitByte(OpCode::ConstNil);

    defineVariable(var);

    return Any();
}


Any RoxalCompiler::visitFunc_decl(RoxalParser::Func_declContext *context)
{
    ParseTracer pt(__func__, context);
    currentToken = context->start;

    auto identifier { context->function()->IDENTIFIER()->getText() };
    UnicodeString uident { UnicodeString::fromUTF8(identifier) };

    declareVariable(uident);
    uint16_t var { 0 };
    if (funcScope()->scopeDepth == 0) // global variable
        var = identifierConstant(uident); // create constant table entry for name

    if (funcScope()->scopeDepth > 0) {
        // mark initialized
        funcScope()->locals.back().depth = funcScope()->scopeDepth;
    }

    visitFunction(context->function());

    defineVariable(var);

    return Any();
}


Any RoxalCompiler::visitFunction(RoxalParser::FunctionContext *context)
{
    ParseTracer pt(__func__, context);
    currentToken = context->start;

    bool isProc = (context->PROC() != nullptr);
    bool isMethod = !typeScopes.empty();

    auto identifier { context->IDENTIFIER()->getText() };
    UnicodeString uident { UnicodeString::fromUTF8(identifier) };

    bool isInitializer = isMethod && (identifier == "init");

    if (isInitializer && !isProc)
        error("object or actor type 'init' method must be a proc.");

    FunctionType ftype = isMethod ? 
                              (isInitializer ? FunctionType::Initializer : FunctionType::Method)
                            : FunctionType::Function;
    

    funcScopes.push_back(FunctionScope(uident, ftype, isProc));

    #ifdef DEBUG_BUILD
    emitByte(OpCode::Nop, "func "+identifier);
    #endif
    beginScope();

    if (context->parameters()) 
        visitParameters(context->parameters());

    visitSuite(context->suite());

    //endScope(); // state scope about to be discarded, not needed

    emitReturn();

    #if defined(DEBUG_OUTPUT_CHUNK)
    funcScope()->function->chunk->disassemble(funcScope()->function->name);
    #endif

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

    return Any();
}


Any RoxalCompiler::visitParameters(RoxalParser::ParametersContext *context)
{
    ParseTracer pt(__func__, context);
    currentToken = context->start;

    for(size_t i=0; i<context->parameter().size(); i++) {

        if (++funcScope()->function->arity > 255)
            error("Maximum of function or procedure 255 parameters exceeded.");

        visitParameter(context->parameter().at(i));

    }

    return Any();
}


Any RoxalCompiler::visitParameter(RoxalParser::ParameterContext *context)
{
    ParseTracer pt(__func__, context);
    currentToken = context->start;

    auto name { context->IDENTIFIER().at(0)->getText() };
    UnicodeString uname { UnicodeString::fromUTF8(name) };

    // TODO: handle optional type

    declareVariable(uname);
    uint16_t var = identifierConstant(uname); // create constant table entry for name

    defineVariable(var);

    return Any();
}


Any RoxalCompiler::visitAssignment(RoxalParser::AssignmentContext *context) 
{
    ParseTracer pt(__func__, context);
    currentToken = context->start;

    if (context->EQUALS()) { // assignment

        UnicodeString ident { UnicodeString::fromUTF8(context->IDENTIFIER()->getText()) };

        if (context->DOT()) { // property set
            visitCall(context->call());

            int16_t propName = identifierConstant(ident);

            visitAssignment(context->assignment());

            emitBytes(OpCode::SetProp, propName);

        }
        else { // variable set
            visitAssignment(context->assignment());
            namedVariable(ident, /*assign=*/true);
        }
    }
    else
        visitLogic_or(context->logic_or());

    return Any();
}


Any RoxalCompiler::visitLogic_or(RoxalParser::Logic_orContext *context) 
{
    ParseTracer pt(__func__, context);
    currentToken = context->start;

    visitLogic_and(context->logic_and().at(0));

    if (context->OR().size()==0) // just passing through
        return Any();

    std::vector<Chunk::size_type> jumpsToEnd {};

    jumpsToEnd.push_back(emitJump(OpCode::JumpIfTrue));
    emitByte(OpCode::Pop);

    if (context->logic_and().size() > 1) {
        for(auto i=1; i<context->logic_and().size(); i++) {
            visitLogic_and(context->logic_and().at(i));

            if (i < context->logic_and().size()-1) {
                jumpsToEnd.push_back(emitJump(OpCode::JumpIfTrue));
                emitByte(OpCode::Pop);            
            }
        }
    }

    for(auto jumpToEnd : jumpsToEnd)
    patchJump(jumpToEnd);

    return Any();
}


Any RoxalCompiler::visitLogic_and(RoxalParser::Logic_andContext *context) 
{
    ParseTracer pt(__func__, context);
    currentToken = context->start;

    visitEquality(context->equality().at(0));

    if (context->AND().size()==0) // just passing through
        return Any();

    std::vector<Chunk::size_type> jumpsToEnd {};

    jumpsToEnd.push_back(emitJump(OpCode::JumpIfFalse));
    emitByte(OpCode::Pop);

    if (context->equality().size() > 1) {
        for(auto i=1; i<context->equality().size(); i++) {
            visitEquality(context->equality().at(i));

            if (i < context->equality().size()-1) {
                jumpsToEnd.push_back(emitJump(OpCode::JumpIfFalse));
                emitByte(OpCode::Pop);
            }
        }
    }

    for(auto jumpToEnd : jumpsToEnd)
        patchJump(jumpToEnd);

    return Any();
}


Any RoxalCompiler::visitEquality(RoxalParser::EqualityContext *context) 
{
    ParseTracer pt(__func__, context);
    currentToken = context->start;

    visitComparison(context->comparison());

    for(auto i=0; i<context->equalnotequal().size(); i++) 
        visitEqualnotequal(context->equalnotequal().at(i));            
        
    return Any();
}


Any RoxalCompiler::visitEqualnotequal(RoxalParser::EqualnotequalContext *context)
{
    ParseTracer pt(__func__, context);
    currentToken = context->start;

    visitComparison(context->comparison());
    if (context->ISEQUAL())
        emitByte(OpCode::Equal);
    else if (context->ISNOTEQUALS()) {
        emitByte(OpCode::Equal);
        emitByte(OpCode::Negate);
    }

    return Any();
}


Any RoxalCompiler::visitComparison(RoxalParser::ComparisonContext *context) 
{
    ParseTracer pt(__func__, context);
    currentToken = context->start;

    visitTerm(context->term().at(0));

    if (context->term().size() > 1) {
        visitTerm(context->term().at(1));

        if (context->GREATER_THAN())
            emitByte(OpCode::Greater);
        else if (context->GT_EQ()) {
            emitByte(OpCode::Less);
            emitByte(OpCode::Negate);
        }
        else if (context->LESS_THAN())
            emitByte(OpCode::Less);
        else if (context->LT_EQ()) {
            emitByte(OpCode::Greater);
            emitByte(OpCode::Negate);
        }
        else
            throw std::runtime_error("unimplemented comparison operator "+context->getText());

    }
    return Any();
}


Any RoxalCompiler::visitTerm(RoxalParser::TermContext *context) 
{
    ParseTracer pt(__func__, context);
    currentToken = context->start;

    visitFactor(context->factor().at(0));

    if (context->factor().size()>1) {
        for(int i=1; i<context->factor().size(); i++) {
            visitFactor(context->factor().at(i));
            emitByte(OpCode::Add);
            //visitAddsub(context->addsub(i));
        }
    }

    return Any();
}


Any RoxalCompiler::visitFactor(RoxalParser::FactorContext *context) 
{
    ParseTracer pt(__func__, context);
    currentToken = context->start;

    visitUnary(context->unary());

    for (int i=0; i<context->multdiv().size(); i++) {
        visitMultdiv(context->multdiv().at(i));
    }

    return Any();
}


antlrcpp::Any RoxalCompiler::visitMultdiv(RoxalParser::MultdivContext *context)
{
    ParseTracer pt(__func__, context);
    currentToken = context->start;

    visitUnary(context->unary());

    if (context->MULT() || context->STAR())
        emitByte(OpCode::Multiply);
    else if (context->DIV())
        emitByte(OpCode::Divide);
    else if (context->MOD())
        emitByte(OpCode::Modulo);

    return Any();
}



Any RoxalCompiler::visitUnary(RoxalParser::UnaryContext *context) 
{
    ParseTracer pt(__func__, context);
    currentToken = context->start;

    if (context->MINUS() || context->NOT()) {
        visitUnary(context->unary());
        emitByte(OpCode::Negate);
    }
    else
        visitCall(context->call()); 
    return Any();
}

Any RoxalCompiler::visitCall(RoxalParser::CallContext *context) 
{
    ParseTracer pt(__func__, context);
    currentToken = context->start;

    visitPrimary(context->primary());

    for(size_t i=0; i<context->args_or_accessor().size(); i++)
        visitArgs_or_accessor(context->args_or_accessor().at(i));

    return Any();
}


Any RoxalCompiler::visitArgs_or_accessor(RoxalParser::Args_or_accessorContext *context)
{
    ParseTracer pt(__func__, context);
    currentToken = context->start;

    if (context->DOT()) {

        UnicodeString ident { UnicodeString::fromUTF8(context->IDENTIFIER()->getText()) };
        int16_t identConstant = identifierConstant(ident);

        // since object.method() is so common, detect it as a special case
        //  to use optimized Invoke opcode
        if (context->OPEN_PAREN()) {

            int argCount { 0 };
            if (context->arguments())
                argCount = visitArguments(context->arguments()).as<int>();

            emitBytes(OpCode::Invoke, identConstant);
            emitByte(argCount);
        }
        else
            emitBytes(OpCode::GetProp, identConstant);
    }
    else {
        int argCount { 0 };
        if (context->arguments())
            argCount = visitArguments(context->arguments()).as<int>();

        emitBytes(OpCode::Call, argCount);
    }

    return Any();
}



Any RoxalCompiler::visitArguments(RoxalParser::ArgumentsContext *context)
{
    ParseTracer pt(__func__, context);
    currentToken = context->start;

    int argCount { 0 };
    for(size_t i=0; i<context->expression().size(); i++) {
        visitExpression(context->expression().at(i));
        argCount++;

        if (argCount == 255) 
            error("Maximum of 255 call arguments exceeded.");
        
    }

    return Any(argCount);
}


Any RoxalCompiler::visitPrimary(RoxalParser::PrimaryContext *context) 
{
    ParseTracer pt(__func__, context);
    currentToken = context->start;

    if (context->OPEN_PAREN()) 
        visitExpression(context->expression());
    else if (context->LTRUE())
        emitByte(OpCode::ConstTrue);        
    else if (context->LFALSE())
        emitByte(OpCode::ConstFalse);
    else if (context->THIS()) {
        if (typeScopes.empty()) 
            error("Can't reference 'this' outside of an actor or object method");        
        else
            namedVariable(toUnicodeString(context->THIS()->getText()),false);
    }
    else if (context->SUPER()) {
        UnicodeString ident { UnicodeString::fromUTF8(context->IDENTIFIER()->getText()) };
        int16_t identConstant = identifierConstant(ident);

        if (typeScopes.empty()) 
            error("Can't use 'super' outside of an actor or object.");
        else if (!typeScopes.back().hasSuperType) 
            error("Can't use 'super' in an actor or object that doesn't extend a super type.");

        namedVariable("this", false);
        namedVariable("super", false);
        emitBytes(OpCode::GetSuper, identConstant);
    }
    else if (context->IDENTIFIER()) {
        UnicodeString ident { UnicodeString::fromUTF8(context->IDENTIFIER()->getText()) };
        namedVariable(ident);
    }
    else if (context->num())
        visitNum(context->num());
    else if (context->LNIL())
        emitByte(OpCode::ConstNil);
    else if (context->str())
        visitStr(context->str());
    else
        throw std::runtime_error("unimplemented primary alterntive");

    return Any();
}


Any RoxalCompiler::visitBuiltin_type(RoxalParser::Builtin_typeContext *context)
{
    ParseTracer pt(__func__, context);
    currentToken = context->start;

    //...

    return Any();
}


Any RoxalCompiler::visitStr(RoxalParser::StrContext *context)
{
    ParseTracer pt(__func__, context);
    currentToken = context->start;

    auto str = context->STRING_LITERAL()->getText();
    // drop enclosing quotes (only one-char single or double quotes)
    str = str.substr(1,str.size()-2); 

    UnicodeString ustr { UnicodeString::fromUTF8(str) };

    // new ObjString or existing one if exists in strings intern map
    auto objStr = stringVal(ustr); 
    emitConstant(objVal(objStr));

    return Any();
}


Any RoxalCompiler::visitNum(RoxalParser::NumContext *context) 
{
    ParseTracer pt(__func__, context);
    currentToken = context->start;

    if (context->integer())
        return visitInteger(context->integer());
    
    // real/float
    // TODO: do we need to consider Unicoe here?
    std::string realStr = context->FLOAT_NUMBER()->getText();
    double real {0.0};
    try {
        real = std::stod(realStr);
    } catch (std::invalid_argument&) {
        error("Invalid real literal");
    }
    emitConstant(realVal(real));

    return Any();
}

Any RoxalCompiler::visitInteger(RoxalParser::IntegerContext *context) 
{
    ParseTracer pt(__func__, context);
    currentToken = context->start;

    int32_t integer {0};
    if (context->DECIMAL_INTEGER()) {
        try {
            integer = std::stoll(context->getText());
        } catch (...) {
            error("Invalid integer literal");
        }
        emitConstant(intVal(integer));
    }
    else if (context->HEX_INTEGER()) {
        char *p_end;
        integer = std::strtoll(context->getText().c_str()+2, &p_end, 16);
        if (errno == ERANGE)
            error("Invalid hexadecimal integer literal");
        emitConstant(intVal(integer));
    }
    else if (context->OCT_INTEGER()) {
        char *p_end;
        integer = std::strtoll(context->getText().c_str()+2, &p_end, 8);
        if (errno == ERANGE)
            error("Invalid octal integer literal");
        emitConstant(intVal(integer));
    }
    else if (context->BIN_INTEGER()) {
        char *p_end;
        integer = std::strtoll(context->getText().c_str()+2, &p_end, 2);
        if (errno == ERANGE)
            error("Invalid binary integer literal");
        emitConstant(intVal(integer));
    }
    else
        throw std::runtime_error("unimplemented integer literal:"+context->getText());

    return Any();
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
//!!!
std::cout << "<endScope() depth=" << (funcScope()->scopeDepth+1) << " local.size=" << locals.size() << ":" << std::endl;
for(auto li=locals.begin(); li!=locals.end(); ++li) {
    std::cout << "  " << int(&(*li) - &(*locals.begin())) << " " << toUTF8StdString(li->name) << " " << li->depth << " "
     << (li->isCaptured ? "captured":"notcaptured") << std::endl;
}
std::cout << std::endl;
//!!!
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


void RoxalCompiler::error(const std::string& message)
{
    throw std::logic_error(std::to_string(currentToken->getLine()) + ":" + message);
}



void RoxalCompiler::emitByte(uint8_t byte, const std::string& comment)
{
    currentChunk()->write(byte, currentToken->getLine(), comment);
}


void RoxalCompiler::emitByte(OpCode op, const std::string& comment)
{
    currentChunk()->write(asByte(op), currentToken->getLine(), comment);
}


void RoxalCompiler::emitBytes(uint8_t byte1, uint8_t byte2, const std::string& comment)
{
    currentChunk()->write(byte1, currentToken->getLine(), comment);
    currentChunk()->write(byte2, currentToken->getLine());
}

void RoxalCompiler::emitBytes(OpCode op, uint8_t byte2, const std::string& comment)
{
    currentChunk()->write(op, currentToken->getLine(), comment);
    currentChunk()->write(byte2, currentToken->getLine());
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
                        error("Reference to local variable in initializer not allowed.");
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
    std::cout << (&(*scopeState) - &(*funcScopes.begin())) << " resolveUpvalue(" << toUTF8StdString(name) << ")" << std::endl;//!!!
    std::string sname { toUTF8StdString(name) };//!!!

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
    for(auto li = funcScope()->locals.rbegin(); li != funcScope()->locals.rend(); li--) {
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
