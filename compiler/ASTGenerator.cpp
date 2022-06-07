
#include <typeinfo>
#include <vector>

#include "ASTGenerator.h"

#include <boost/algorithm/string/replace.hpp>

#include <core/common.h>
#include <core/AST.h>
#include "RoxalIndentationLexer.h"


using namespace roxal;
using namespace roxal::ast;

using icu::UnicodeString;


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



void ASTGenerator::setSourceInfo(ptr<AST> ast, antlr4::ParserRuleContext* context)
{
    ast->source = source;

    LinePos start { context->start->getLine(), context->start->getCharPositionInLine() };
    LinePos end { context->stop->getLine(),  context->stop->getCharPositionInLine() };

    ast->interval = std::make_pair(start, end);

    #ifdef DEBUG_BUILD
    ast->fullSource = stringInterval(*source,ast->interval.first.line, ast->interval.first.pos, ast->interval.second.line, ast->interval.second.pos);
    #endif
}

void ASTGenerator::setSourceInfo(ptr<AST> ast, antlr4::tree::TerminalNode* terminal)
{
    ast->source = source;

    auto symbol = terminal->getSymbol();
    auto symbolLength = symbol->getStopIndex() - symbol->getStartIndex();

    LinePos start { size_t(symbol->getLine()), size_t(symbol->getCharPositionInLine()) };

    ast->interval = std::make_pair(start,
                                   LinePos(start.line,  start.pos + symbolLength) );

    #ifdef DEBUG_BUILD
    ast->fullSource = stringInterval(*source,ast->interval.first.line, ast->interval.first.pos, ast->interval.second.line, ast->interval.second.pos);
    #endif
}



// is ptr<P> p down-castable to ptr<C> where C is a subclass of P (or the same class)?
template<typename P, typename C>
bool isa(ptr<P> p) {
    if (p==nullptr) return false;
    return std::dynamic_pointer_cast<C>(p)!=nullptr;
}



// The AST tree nodes all inherit from class AST 
//  However, the ANTLR visitor return type is Any (or std::any in ANTLR 4.10).   
//  Unfortunately, the any classes don't allow access to the address of the contained type so there
//   is no way to use dynamic_cast to test for runtime type information to determine which
//   AST subclass is contained at runtime
// Hence, we always return an instance of TypeValue instead (a pair of the typeid of the subclass and
//   the ptr<AST> value)

struct TypeValue {
    TypeValue(const std::type_info& _tid, ptr<AST> _v) : tid(_tid), v(_v) {}
    const std::type_info& tid;
    ptr<AST> v;

    std::string typeName() const { return demangle(tid.name()); }

    template<typename T>
    bool is() const { return typeid(T) == tid; }

    template<typename T>
    ptr<T> as() const { return std::dynamic_pointer_cast<T>(v); }

    template<typename T>
    bool isa() const { return is<T>() || as<T>()!=nullptr; }

    template<typename T>
    void assertType() const {
        if (!isa<T>())
            throw std::runtime_error(std::string("expected type ")+demangle(typeid(T).name())+" not type "+demangle(tid.name()));
    }
};

// construct TypeValue from ptr<T> where T is AST subclass
template<typename T>
TypeValue typeValue(ptr<T> v) {
    return TypeValue(typeid(*v), v);
}

std::string typeName(const antlrcpp::Any& a) {
    if (a.isNull()) return "null-any";
    if (a.is<TypeValue>())
        return a.as<TypeValue>().typeName();
    return demangle(typeid(a).name());
}

std::string astTypeName(ptr<AST> a) {
    if (a==nullptr) return "null";
    return demangle(typeid(*a).name());
}



template<typename T>
void assertType(const antlrcpp::Any& a) {
    #ifdef DEBUG_BUILD
    if (!(a.isNotNull() && a.is<TypeValue>()))
        throw std::runtime_error("Assert expected type "+demangle(typeid(T).name())+" but have (non-TypeValue) "+typeName(a));
    if (!a.as<TypeValue>().isa<T>())
        throw std::runtime_error("Assert expected type "+demangle(typeid(T).name())+" but have "+typeName(a));
    #endif
    assert(a.isNotNull() && a.is<TypeValue>());
    a.as<TypeValue>().assertType<T>(); // will throw
}

// is a an Any TypeValue wrapping a ptr<T> (exactly T)
template<typename T>
bool is(const antlrcpp::Any& a) {
    if (a.isNull() || !a.is<TypeValue>()) 
        throw std::runtime_error("is<T> requires Any a is a TypeValue");
    return a.as<TypeValue>().is<T>();
}

// is a an Any TypeValue wrapping a ptr<T> or a superclass of T?
template<typename T>
bool isa(const antlrcpp::Any& a) {
    if (a.isNull() || !a.is<TypeValue>()) 
        throw std::runtime_error("isa<T> requires Any a is a TypeValue");
    if (a.is<TypeValue>()) 
        return a.as<TypeValue>().isa<T>();
    return false;
}

// convert from Any wrapping TypeValue to ptr<T>
template<typename T>
ptr<T> as(const antlrcpp::Any& a) {
    if (a.isNull() || !a.is<TypeValue>()) 
        throw std::runtime_error("as<T> requires Any a is a TypeValue");
    #ifdef DEBUG_BUILD
    if (!isa<T>(a))
        throw std::runtime_error("Can't cast as<"+demangle(typeid(T).name())+">("+typeName(a)+")");
    #endif
    return a.as<TypeValue>().as<T>();
}




    
ptr<AST> ASTGenerator::ast(std::istream& source, const std::string& name)
{
    // store entire source string
    this->source = std::make_shared<std::string>(std::string(std::istreambuf_iterator<char>(source), {}));
    source.seekg(0);
    this->sourceName = name;

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

    ptr<File> ast = nullptr;

    if (parser.getNumberOfSyntaxErrors() == 0) {

        try {
            auto file = visitFile_input(tree);
            assertType<File>(file);
            ast = as<File>(file);
            
        } catch (std::logic_error& e) {
            std::cout << std::string("Compile error: ") << e.what() << std::endl;
            return nullptr;
        } catch (std::exception& e) {
            std::cout << std::string("Exception: ") << e.what() << std::endl;
            throw e;
        }
    } 

    this->source = nullptr;
    this->sourceName.clear();

    return ast;
}


// macros to wrap visitor method code and catch exceptions (as development aid)
#define visitStart() \
    ParseTracer pt(__func__, context); \
    currentToken = context->start; \
    try {

#ifdef DEBUG_BUILD
    #define visitEnd() \
        } catch (std::exception& ve) { \
            std::cerr << "Exception in " << __func__ << " - " << ve.what() << std::endl; \
            throw; \
        } \
        return antlrcpp::Any();
#else
    #define visitEnd() \
        } catch (std::exception& ve) { \
            throw std::runtime_error((std::string("Exception in ") + __func__ + " - " + std::string(ve.what())).c_str()); \
        } \
        return antlrcpp::Any();
#endif




antlrcpp::Any ASTGenerator::visitFile_input(RoxalParser::File_inputContext *context)
{
    visitStart();

    auto file = std::make_shared<File>();
    setSourceInfo(file, context);

    for(auto& declaration : context->declaration()) {
        auto declOrStmt { visitDeclaration(declaration) };

        if (isa<Declaration>(declOrStmt))
            file->declsOrStmts.push_back( as<Declaration>(declOrStmt) );
        else if (isa<Statement>(declOrStmt))
            file->declsOrStmts.push_back( as<Statement>(declOrStmt) );
        else
            throw std::runtime_error("expected Declaration or Statement");
    }

    // add a return stmt
    // auto retStmt = std::make_shared<ReturnStatement>();
    // decltype(file->declsOrStmts)::value_type declOrStmt;
    // declOrStmt = retStmt;
    // file->declsOrStmts.push_back(retStmt);

    return typeValue(file);

    visitEnd();
}



antlrcpp::Any ASTGenerator::visitSingle_input(RoxalParser::Single_inputContext *context)
{
    visitStart();

    return typeValue(std::make_shared<AST>());

    visitEnd();
}




antlrcpp::Any ASTGenerator::visitDeclaration(RoxalParser::DeclarationContext *context)
{
    visitStart();

    ptr<Declaration> decl { nullptr };

    if (context->type_decl())
        decl = as<TypeDecl>(visitType_decl(context->type_decl()));
    else if (context->func_decl())
        decl = as<FuncDecl>(visitFunc_decl(context->func_decl()));
    else if (context->var_decl())
        decl = as<VarDecl>(visitVar_decl(context->var_decl()));
    else if (context->statement()) {
        auto stmt = visitStatement(context->statement());
        assertType<Statement>(stmt);
        return stmt;
    }
    else
        throw std::runtime_error("unimplemented declaration type");

    setSourceInfo(decl, context);
    return typeValue(decl);

    visitEnd();
}



antlrcpp::Any ASTGenerator::visitStatement(RoxalParser::StatementContext *context)
{
    visitStart();

    ptr<Statement> stmt { nullptr };

    if (context->expr_stmt()) {
        auto expr = visitExpr_stmt(context->expr_stmt());
        assertType<Expression>(expr);
        auto exprStmt = std::make_shared<ExpressionStatement>();
        exprStmt->expr = as<Expression>(expr);
        stmt = exprStmt;
    }
    //if (context->simple_stmt())
    //    visitSimple_stmt(context->simple_stmt());
    else if (context->compound_stmt()) {
        auto compound = visitCompound_stmt(context->compound_stmt());
        if (is<PrintStatement>(compound)) {
            stmt = as<PrintStatement>(compound);
        }
        else if (is<ReturnStatement>(compound)) {
            stmt = as<ReturnStatement>(compound);
        }
        else if (is<WhileStatement>(compound)) {
            stmt = as<WhileStatement>(compound);
        }
        else if (is<IfStatement>(compound)) {
            stmt = as<IfStatement>(compound);
        }
        else if (is<Suite>(compound)) {
            stmt = as<Suite>(compound);
        }
        else
            throw std::runtime_error(std::string("unimplemented compound statement type:")+typeName(compound));

    }
    else
        throw std::runtime_error("unhandled statement parse alternative");

    setSourceInfo(stmt, context);
    return typeValue(stmt);

    visitEnd();
}



antlrcpp::Any ASTGenerator::visitExpr_stmt(RoxalParser::Expr_stmtContext *context)
{
    visitStart();

    auto expr = visitExpression(context->expression());
    setSourceInfo(as<Expression>(expr), context);

    return expr;

    visitEnd();
}



antlrcpp::Any ASTGenerator::visitExpression(RoxalParser::ExpressionContext *context)
{
    visitStart();
    auto assignment = visitAssignment(context->assignment());
    setSourceInfo(as<AST>(assignment), context);
    return assignment;
    visitEnd();
}



antlrcpp::Any ASTGenerator::visitCompound_stmt(RoxalParser::Compound_stmtContext *context)
{
    visitStart();

    if (context->print_stmt())
        return visitPrint_stmt(context->print_stmt());
    else if (context->return_stmt())
        return visitReturn_stmt(context->return_stmt());
    else if (context->block_stmt())
        return visitBlock_stmt(context->block_stmt());
    else if (context->if_stmt())
        return visitIf_stmt(context->if_stmt());
    else if (context->while_stmt())
        return visitWhile_stmt(context->while_stmt());
    else
        throw std::runtime_error("unimplemented compound statement alternative");

    visitEnd();
}



antlrcpp::Any ASTGenerator::visitBlock_stmt(RoxalParser::Block_stmtContext *context)
{
    visitStart();
    return visitSuite(context->suite());
    visitEnd();
}



antlrcpp::Any ASTGenerator::visitPrint_stmt(RoxalParser::Print_stmtContext *context)
{
    visitStart();

    auto printStmt = std::make_shared<PrintStatement>();

    auto expr = visitExpression(context->expression());
    assertType<Expression>(expr);

    printStmt->expr = as<Expression>(expr);

    return typeValue(printStmt);
    visitEnd();
}



antlrcpp::Any ASTGenerator::visitReturn_stmt(RoxalParser::Return_stmtContext *context)
{
    visitStart();

    auto returnStmt = std::make_shared<ReturnStatement>();
    setSourceInfo(returnStmt, context);
    if (context->expression())
        returnStmt->expr = as<Expression>(visitExpression(context->expression()));
        
    return typeValue(returnStmt);
    visitEnd();
}



antlrcpp::Any ASTGenerator::visitIf_stmt(RoxalParser::If_stmtContext *context)
{
    visitStart();

    auto ifStmt = std::make_shared<IfStatement>();
    setSourceInfo(ifStmt,context);

    // at least one condition & body required
    auto cond = as<Expression>(visitExpression(context->expression().at(0)));
    auto body = as<Suite>(visitSuite(context->suite().at(0)));

    ifStmt->conditionalSuites.push_back(std::make_pair(cond,body));

    if (context->ELSEIF().size()>0) {
        for(int i=1; i<context->expression().size();i++) {
            cond = as<Expression>(visitExpression(context->expression().at(i)));
            body = as<Suite>(visitSuite(context->suite().at(i)));
            ifStmt->conditionalSuites.push_back(std::make_pair(cond,body));
        }
    }

    if (context->ELSE()) {
        body = as<Suite>(visitSuite(context->suite().at(context->suite().size()-1)));
        ifStmt->elseSuite = body;
    }
    
    return typeValue(ifStmt);
    visitEnd();
}



antlrcpp::Any ASTGenerator::visitWhile_stmt(RoxalParser::While_stmtContext *context)
{
    visitStart();

    auto condition = visitExpression(context->expression());
    auto body = visitSuite(context->suite());

    auto whileStmt = std::make_shared<WhileStatement>();
    setSourceInfo(whileStmt, context);
    whileStmt->condition = as<Expression>(condition);
    whileStmt->body = as<Suite>(body);

    return typeValue(whileStmt);
    visitEnd();
}



antlrcpp::Any ASTGenerator::visitVar_decl(RoxalParser::Var_declContext *context)
{
    visitStart();

    UnicodeString ident { UnicodeString::fromUTF8(context->IDENTIFIER()->getText()) };

    auto vardecl = std::make_shared<VarDecl>();
    setSourceInfo(vardecl,context);
    vardecl->name = ident;

    if (context->EQUALS()) 
        vardecl->initializer = as<Expression>(visitExpression(context->expression()));

    return typeValue(vardecl);
    visitEnd();
}



antlrcpp::Any ASTGenerator::visitFunc_decl(RoxalParser::Func_declContext *context)
{
    visitStart();

    auto ident { UnicodeString::fromUTF8(context->function()->IDENTIFIER()->getText()) };

    auto funcdecl = std::make_shared<FuncDecl>();
    setSourceInfo(funcdecl,context);
    auto func = visitFunction(context->function());
    funcdecl->func = as<Function>(func);

    return typeValue(funcdecl);
    visitEnd();
}



antlrcpp::Any ASTGenerator::visitFunction(RoxalParser::FunctionContext *context)
{
    visitStart();

    auto ident { UnicodeString::fromUTF8(context->IDENTIFIER()->getText()) };

    auto func = std::make_shared<Function>();
    setSourceInfo(func,context);
    func->isProc = (context->PROC() != nullptr);
    func->name = ident;

    if (context->parameters()) {
        auto params = visitParameters(context->parameters());
        func->params = *params.as<ptr<std::vector<ptr<Parameter>>>>();
    }

    auto body = visitSuite(context->suite());
    func->body = as<Suite>(body);

    return typeValue(func);
    visitEnd();
}


// returns an Any(ptr<std::vector<ptr<Parameter>>>)
antlrcpp::Any ASTGenerator::visitParameters(RoxalParser::ParametersContext *context)
{
    visitStart();
    auto params = std::make_shared<std::vector<ptr<Parameter>>>();
    for(size_t i=0; i<context->parameter().size(); i++) {

        auto param = visitParameter(context->parameter().at(i));
        params->push_back( as<Parameter>(param) );
    }
    return params;
    visitEnd();
}



antlrcpp::Any ASTGenerator::visitParameter(RoxalParser::ParameterContext *context)
{
    visitStart();
    auto ident { UnicodeString::fromUTF8(context->IDENTIFIER().at(0)->getText()) };

    auto param = std::make_shared<Parameter>();
    setSourceInfo(param,context->IDENTIFIER().at(0));
    param->name = ident;

    if (context->builtin_type()) {
        auto builtinType = visitBuiltin_type(context->builtin_type()).as<BuiltinType>();
        param->type = builtinType;
    }
    else if (context->IDENTIFIER().size()>1) {
        auto typeIdent { UnicodeString::fromUTF8(context->IDENTIFIER().at(1)->getText()) };
        param->type = typeIdent;
    }
    else {} // type is optional

    return typeValue(param);
    visitEnd();
}



antlrcpp::Any ASTGenerator::visitSuite(RoxalParser::SuiteContext *context)
{
    visitStart();

    auto suite = std::make_shared<Suite>();
    setSourceInfo(suite,context);
    for(int i=0; i<context->declaration().size();i++) {
        auto declOrStmt = visitDeclaration(context->declaration().at(i));
        if (isa<Declaration>(declOrStmt))
            suite->declsOrStmts.push_back( as<Declaration>(declOrStmt));
        else if (isa<Statement>(declOrStmt))
            suite->declsOrStmts.push_back( as<Statement>(declOrStmt));
        else
            throw std::runtime_error("suite expected Declaration or Stamement from declaration()");
    }

    return typeValue(suite);
    visitEnd();
}



antlrcpp::Any ASTGenerator::visitType_decl(RoxalParser::Type_declContext *context)
{
    visitStart();

    auto typedecl = std::make_shared<TypeDecl>();
    setSourceInfo(typedecl,context);

    bool isActor = (context->ACTOR() != nullptr);
    typedecl->kind = isActor ? TypeDecl::Actor : TypeDecl::Object;

    size_t identIndex = 0;
    typedecl->name = UnicodeString::fromUTF8(context->IDENTIFIER().at(identIndex++)->getText());

    if (context->EXTENDS())
        typedecl->extends = UnicodeString::fromUTF8(context->IDENTIFIER().at(identIndex++)->getText());

    while(identIndex < context->IDENTIFIER().size())
        typedecl->implements.push_back(UnicodeString::fromUTF8(context->IDENTIFIER().at(identIndex++)->getText()));

    if (context->function().size() > 255) // TODO: revise
        throw std::runtime_error("Too many methods for one actor or object type.");

    for(size_t i=0; i<context->function().size(); i++) {

        auto funcContext { context->function().at(i) };

        auto func = visitFunction(funcContext);
        if (!isa<Function>(func))
            throw std::runtime_error("Expected Function for methods of object or actor type");
        typedecl->methods.push_back(as<Function>(func));
    }

    return typeValue(typedecl);
    visitEnd();
}



antlrcpp::Any ASTGenerator::visitAssignment(RoxalParser::AssignmentContext *context)
{
    visitStart();

    if (context->logic_or()) {
        return visitLogic_or(context->logic_or());
    }
    else if (context->EQUALS()) { // assignment
        icu::UnicodeString ident { icu::UnicodeString::fromUTF8(context->IDENTIFIER()->getText()) };

        auto assign = std::make_shared<Assignment>();
        setSourceInfo(assign, context);

        if (context->DOT()) { // property set
            
            auto callable = visitCall(context->call());

            auto access = std::make_shared<UnaryOp>(UnaryOp::Accessor);
            setSourceInfo(access,context->DOT());
            access->arg = as<Expression>(callable);
            access->member = ident;

            assign->lhs = access;
        }
        else { // variable set
            assign->lhs = std::make_shared<Variable>(ident);
            setSourceInfo(assign->lhs,context->IDENTIFIER());
        }

        assign->rhs = as<Expression>(visitAssignment(context->assignment()));

        return typeValue(assign);
    }
    else
        throw std::runtime_error("unhandled assignment alternative");

    visitEnd();
}



antlrcpp::Any ASTGenerator::visitLogic_or(RoxalParser::Logic_orContext *context)
{
    visitStart();

    auto logicAnd = visitLogic_and(context->logic_and().at(0));
    if (context->OR().size()==0) // just passing through
        return logicAnd;

    auto lhs = as<Expression>(logicAnd);

    ptr<BinaryOp> orOp;

    if (context->logic_and().size() > 1) {

        for(auto i=1; i<context->logic_and().size(); i++) {
            orOp = std::make_shared<BinaryOp>(BinaryOp::Or);
            setSourceInfo(orOp,context);//!!!
            orOp->lhs = lhs;

            auto rhs = visitLogic_and(context->logic_and().at(i));
            orOp->rhs = as<Expression>(rhs);

            lhs = orOp;
        }
    }

    return typeValue(orOp);
    visitEnd();
}



antlrcpp::Any ASTGenerator::visitLogic_and(RoxalParser::Logic_andContext *context)
{
    visitStart();

    auto equality = visitEquality(context->equality().at(0));
    if (context->AND().size()==0) // just passing through
        return equality;

    auto lhs = as<Expression>(equality);

    ptr<BinaryOp> andOp;

    if (context->equality().size() > 1) {

        for(auto i=1; i<context->equality().size(); i++) {
            andOp = std::make_shared<BinaryOp>(BinaryOp::And);
            setSourceInfo(andOp,context);//!!!
            andOp->lhs = lhs;

            auto rhs = visitEquality(context->equality().at(i));
            andOp->rhs = as<Expression>(rhs);

            lhs = andOp;
        }
    }

    return typeValue(andOp);
    visitEnd();
}



antlrcpp::Any ASTGenerator::visitEquality(RoxalParser::EqualityContext *context)
{
    visitStart();

    auto comparison = visitComparison(context->comparison());
    if (context->equalnotequal().size()==0)
        return comparison;

    auto lhs = as<Expression>(comparison);

    for(auto i=0; i<context->equalnotequal().size();i++) {
        auto compOp = visitEqualnotequal(context->equalnotequal().at(i));
        as<BinaryOp>(compOp)->lhs = as<Expression>(comparison);
        lhs = as<Expression>(compOp);
    }

    return typeValue(lhs);
    visitEnd();
}


// returns a BinaryOp(Equal|NotEqual) with rhs set and lhs null
antlrcpp::Any ASTGenerator::visitEqualnotequal(RoxalParser::EqualnotequalContext *context)
{
    visitStart();
    auto compOp = std::make_shared<BinaryOp>( context->ISEQUAL() ? BinaryOp::Equal : BinaryOp::NotEqual);
    setSourceInfo(compOp,context);

    auto comparison = visitComparison(context->comparison());
    compOp->rhs = as<Expression>(comparison);

    return typeValue(compOp);
    visitEnd();
}



antlrcpp::Any ASTGenerator::visitComparison(RoxalParser::ComparisonContext *context)
{
    visitStart();

    auto term = visitTerm(context->term().at(0));
    if (context->term().size()==1)
        return term;

    auto op = (   context->GREATER_THAN() ? BinaryOp::GreaterThan
              : ( context->GT_EQ() ? BinaryOp::GreaterOrEqual
              : ( context->LESS_THAN() ? BinaryOp::LessThan
              : ( context->LT_EQ() ? BinaryOp::LessOrEqual
              : BinaryOp::None
              ))));

    auto compOp = std::make_shared<BinaryOp>(op);
    setSourceInfo(compOp, context);
    compOp->lhs = as<Expression>(term);
    compOp->rhs = as<Expression>(visitTerm(context->term().at(1)));

    return typeValue(compOp);
    visitEnd();
}



antlrcpp::Any ASTGenerator::visitTerm(RoxalParser::TermContext *context)
{
    visitStart();

    auto factor = visitFactor(context->factor().at(0));
    if (context->factor().size()==1) // just passing through
        return factor;

    auto lhs = as<Expression>(factor);

    // in grammar there is only (optional) "+" to seperate terms, since
    //  a source string like "2-1" is actually (term + (negate term))
    ptr<BinaryOp> plusOp;

    for(auto i=1; i<context->factor().size(); i++) {
        plusOp = std::make_shared<BinaryOp>(BinaryOp::Add);
        setSourceInfo(plusOp,context);
        plusOp->lhs = lhs;

        auto rhs = visitFactor(context->factor().at(i));
        assertType<Expression>(rhs);
        plusOp->rhs = as<Expression>(rhs);

        // TODO: if the rhs is just (UniaryOp::Negate -> Term, 
        //  delete the UnaryOp node and conver plusOp to subtractOp)

        lhs = plusOp;
    }

    return typeValue(plusOp);
    visitEnd();
}



antlrcpp::Any ASTGenerator::visitFactor(RoxalParser::FactorContext *context)
{
    visitStart();

    auto unary = visitUnary(context->unary());
    if (context->multdiv().size() == 0)
        return unary;

    auto lhs = as<Expression>(unary);

    ptr<BinaryOp> op;

    for(auto i=0; i<context->multdiv().size(); i++) {

        auto multdiv = visitMultdiv(context->multdiv().at(i));
        assertType<BinaryOp>(multdiv);
        op = as<BinaryOp>(multdiv);
        op->lhs = lhs;

        lhs = op;
    }

    return typeValue(op);
    visitEnd();
}



// returns a BinaryOp with rhs set and lhs null
antlrcpp::Any ASTGenerator::visitMultdiv(RoxalParser::MultdivContext *context)
{
    visitStart();
    auto op = std::make_shared<BinaryOp>();
    setSourceInfo(op,context);
    if (context->MULT() || context->STAR())
        op->op = BinaryOp::Multiply;
    else if (context->DIV())
        op->op = BinaryOp::Divide;
    else if (context->MOD())
        op->op = BinaryOp::Modulo;
    else
        throw std::runtime_error("unhandled multidiv alternative");

    auto unary = visitUnary(context->unary());
    op->rhs = as<Expression>(unary);

    return typeValue(op);
    visitEnd();
}



antlrcpp::Any ASTGenerator::visitUnary(RoxalParser::UnaryContext *context)
{
    visitStart();

    if (context->unary()) {

        auto arg = visitUnary(context->unary());

        // as special case, if the arg is a single bool or num literal, just
        //  modify the literal to negate it and return that instead of creating 
        //  a negation UnaryOp
        if (context->NOT() && isa<Bool>(arg)) {
            as<Bool>(arg)->value = !as<Bool>(arg)->value;
            return arg;
        }
        else if (context->MINUS() && isa<Num>(arg)) {
            auto numarg = as<Num>(arg);
            if (std::holds_alternative<int32_t>(numarg->num))
                numarg->num = -std::get<int32_t>(numarg->num);
            else if (std::holds_alternative<double>(numarg->num))
                numarg->num = -std::get<double>(numarg->num);
            else
                throw std::runtime_error("unhandled Num type");
            return typeValue(numarg);
        }

        auto op = std::make_shared<UnaryOp>(context->NOT() ? 
                                              UnaryOp::Not
                                              : (context->MINUS() ? UnaryOp::Negate
                                              : UnaryOp::None )
                                           );

        setSourceInfo(op, context);
        op->arg = as<Expression>(arg);
        return typeValue(op);
    }
    else if (context->call()) {
        return visitCall(context->call());
    }
    else if (context->index()) {
        return visitIndex(context->index());
    }
    else
        throw std::runtime_error("unimplemented unary alternative");

    visitEnd();
}


// info to represent cases:
//  .access - accessor only (accessor==true)
//  .access(arg*) - accessor and call (with 0 or more args) (accessor=true && call==true)
//  (arg*) - call only with 0 or more args - (call==true)
//  [arg+] - indexer only with 0 or more args - (indexer==true)
struct ArgsOrAccessorInfo {
    bool call;
    bool accessor;
    bool indexer;
    UnicodeString accessed;
    ptr<std::vector<ptr<Expression>>> args;
};

// return will be either ptr<Call> or ptr<UnaryOp(Accessor)>
antlrcpp::Any ASTGenerator::visitCall(RoxalParser::CallContext *context)
{
    visitStart();

    auto primary = visitPrimary(context->primary());
    if (context->args_or_accessor().size() == 0)
        return primary;

    auto callable = as<Expression>(primary);

    for(int i=0; i<context->args_or_accessor().size();i++) {
        auto argsOrAccessorInfo = visitArgs_or_accessor(context->args_or_accessor().at(i)).as<ptr<ArgsOrAccessorInfo>>();
        if (argsOrAccessorInfo->accessor) {
            auto accessOp = std::make_shared<UnaryOp>(UnaryOp::Accessor);
            accessOp->arg = callable;
            accessOp->member = argsOrAccessorInfo->accessed;
            callable = accessOp;
        }
        if (argsOrAccessorInfo->call) {
            auto call = std::make_shared<Call>();
            call->callable = callable;
            call->args = *argsOrAccessorInfo->args;
            callable = call;
        }
        if (argsOrAccessorInfo->indexer) {
            auto call = std::make_shared<Call>();
            call->callable = callable;
            call->args = *argsOrAccessorInfo->args;
            callable = call;
        }
    }

    return typeValue(callable);
    visitEnd();
}


// return ptr<ArgsOrAccessorInfo> 
antlrcpp::Any ASTGenerator::visitArgs_or_accessor(RoxalParser::Args_or_accessorContext *context)
{
    visitStart();

    auto info = std::make_shared<ArgsOrAccessorInfo>();
    info->accessor = info->indexer = info->call = false;

    if (context->DOT()) { // accessor, possibly call or indexer
        info->accessor = true;
        UnicodeString ident = context->IDENTIFIER() ? UnicodeString::fromUTF8(context->IDENTIFIER()->getText()) : "";
        info->accessed = ident;

        if (context->OPEN_PAREN()) {
            info->call = true;
            if (context->arguments()) {
                auto args = visitArguments(context->arguments()).as<ptr<std::vector<ptr<Expression>>>>();
                info->args = args;
            }
            else
                info->args = std::make_shared<std::vector<ptr<Expression>>>();
        }
    }
    else if (context->OPEN_PAREN()) { // call
        info->call = true;
        if (context->arguments()) {
            auto args = visitArguments(context->arguments()).as<ptr<std::vector<ptr<Expression>>>>();
            info->args = args;
        }
        else
            info->args = std::make_shared<std::vector<ptr<Expression>>>();
    }
    else
        throw std::runtime_error("unimplemented args_or_accessor alternative");

    return info;
    visitEnd();
}


antlrcpp::Any ASTGenerator::visitIndex(RoxalParser::IndexContext *context)
{
    visitStart();

    auto indexable = as<Expression>(visitPrimary(context->primary()));

    auto index = std::make_shared<Index>();
    setSourceInfo(index,context);
    index->indexable = indexable;
    
    auto args = visitArguments(context->arguments()).as<ptr<std::vector<ptr<Expression>>>>();
    index->args = *args;

    return typeValue(index);
    visitEnd();
}



// returns ptr<std::vector<ptr<Expression>>> of argument expressions
antlrcpp::Any ASTGenerator::visitArguments(RoxalParser::ArgumentsContext *context)
{
    visitStart();

    auto argexprs = std::make_shared<std::vector<ptr<Expression>>>();
    for(int i=0; i<context->expression().size(); i++)
        argexprs->push_back(as<Expression>(visitExpression(context->expression().at(i))));

    return argexprs;
    visitEnd();
}



antlrcpp::Any ASTGenerator::visitPrimary(RoxalParser::PrimaryContext *context)
{
    visitStart();

    if (context->LTRUE()) 
        return typeValue(std::make_shared<Bool>(true));    
    else if (context->LFALSE())
        return typeValue(std::make_shared<Bool>(false));    
    else if (context->LNIL())
        return typeValue(std::make_shared<Literal>());
    else if (context->THIS()) {
        auto var = std::make_shared<Variable>("this");
        setSourceInfo(var,context);
        return typeValue(var);
    }
    else if (context->IDENTIFIER()) {
        UnicodeString ident { UnicodeString::fromUTF8(context->IDENTIFIER()->getText()) };
        auto var = std::make_shared<Variable>(ident);
        setSourceInfo(var,context);
        return typeValue(var);
    }
    else if (context->OPEN_PAREN())
        return visitExpression(context->expression());
    else if (context->SUPER()) {
        auto var = std::make_shared<Variable>("super");
        setSourceInfo(var,context);
        return typeValue(var);
    }
    else if (context->num())
        return visitNum(context->num());
    else if (context->str())
        return visitStr(context->str());
    else
        throw std::runtime_error("unimplemented primary alternative");

    visitEnd();
}


// returns BuiltinType enum value
antlrcpp::Any ASTGenerator::visitBuiltin_type(RoxalParser::Builtin_typeContext *context)
{
    visitStart();

    BuiltinType type { BuiltinType::Nil };
    if (context->LNIL())
        type = BuiltinType::Nil;
    else if (context->BOOL())
        type = BuiltinType::Bool;
    else if (context->BYTE())
        type = BuiltinType::Byte;
    else if (context->NUMBER())
        type = BuiltinType::Number;
    else if (context->INT())
        type = BuiltinType::Int;
    else if (context->REAL())
        type = BuiltinType::Real;
    else if (context->DECIMAL())
        type = BuiltinType::Decimal;
    else if (context->STRING())
        type = BuiltinType::String;
    else if (context->LIST())
        type = BuiltinType::List;
    else if (context->DICT())
        type = BuiltinType::Dict;
    else if (context->VECTOR())
        type = BuiltinType::Vector;
    else if (context->MATRIX())
        type = BuiltinType::Matrix;
    else if (context->TENSOR())
        type = BuiltinType::Tensor;
    else if (context->ORIENT())
        type = BuiltinType::Orient;
    else if (context->STREAM())
        type = BuiltinType::Stream;
    else
        throw std::runtime_error("unhandled BuiltinType alternative");

    return type;    
    visitEnd();
}



antlrcpp::Any ASTGenerator::visitStr(RoxalParser::StrContext *context)
{
    visitStart();
    auto text = context->STRING_LITERAL()->getText();

    // drop enclosing quotes
    text = text.substr(1,text.size()-2);

    auto str = std::make_shared<Str>();
    setSourceInfo(str,context);
    // convert to UnicodeString assuming UTF-8 encoding and unescape escape sequences
    //  (see ICU UnicodeString::unescape() docs for details)
    str->str = toUnicodeString(text).unescape();
    return typeValue(str);
    visitEnd();
}



antlrcpp::Any ASTGenerator::visitNum(RoxalParser::NumContext *context)
{
    visitStart();

    if (context->integer())
        return visitInteger(context->integer());

    // real/float
    // TODO: do we need to consider Unicode here?
    std::string realStr = context->FLOAT_NUMBER()->getText();
    double real {0.0};
    try {
        real = std::stod(realStr);
    } catch (std::invalid_argument&) {
        throw std::runtime_error("invalid number \""+realStr+"\"");
    }
    auto num = std::make_shared<Num>();
    setSourceInfo(num,context->FLOAT_NUMBER());
    num->num = real;
    return typeValue(num);
    visitEnd();
}



antlrcpp::Any ASTGenerator::visitInteger(RoxalParser::IntegerContext *context)
{
    visitStart();

    auto num = std::make_shared<Num>();

    int32_t integer {0};
    if (context->DECIMAL_INTEGER()) {
        try {
            integer = std::stoll(context->getText());
        } catch (...) {
            throw std::runtime_error("Invalid integer literal");
        }
        num->num = integer;
        setSourceInfo(num,context->DECIMAL_INTEGER());
    }
    else if (context->HEX_INTEGER()) {
        char *p_end;
        integer = std::strtoll(context->getText().c_str()+2, &p_end, 16);
        if (errno == ERANGE)
            throw std::runtime_error("Invalid hexadecimal integer literal");
        num->num = integer;
        setSourceInfo(num,context->HEX_INTEGER());
    }
    else if (context->OCT_INTEGER()) {
        char *p_end;
        integer = std::strtoll(context->getText().c_str()+2, &p_end, 8);
        if (errno == ERANGE)
            throw std::runtime_error("Invalid octal integer literal");
        num->num = integer;
        setSourceInfo(num,context->OCT_INTEGER());
    }
    else if (context->BIN_INTEGER()) {
        char *p_end;
        integer = std::strtoll(context->getText().c_str()+2, &p_end, 2);
        if (errno == ERANGE)
            throw std::runtime_error("Invalid binary integer literal");
        num->num = integer;
        setSourceInfo(num,context->BIN_INTEGER());
    }
    else
        throw std::runtime_error("unimplemented integer literal:"+context->getText());

    return typeValue(num);

    visitEnd();
}

