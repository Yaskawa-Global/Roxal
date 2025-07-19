
#include <typeinfo>
#include <vector>

#include "ASTGenerator.h"

#include <boost/algorithm/string/replace.hpp>

#include <core/common.h>
#include <core/AST.h>
#include "RoxalIndentationLexer.h"
#include "Error.h"


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

std::stack<std::pair<std::string,std::string>> ParseTracer::parseStack {};



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


template<typename T>
bool anyis(const std::any& a) {
    return a.type() == typeid(T);
}

template<typename T>
T anyas(const std::any& a) {
    return std::any_cast<T>(a);
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

std::string typeName(const std::any& a) {
    if (!a.has_value()) return "null-any";
    if (anyis<TypeValue>(a))
        return anyas<TypeValue>(a).typeName();
    return demangle(typeid(a).name());
}

std::string astTypeName(ptr<AST> a) {
    if (a==nullptr) return "null";
    return demangle(typeid(*a).name());
}


template<typename T>
void assertType(const std::any& a) {
    #ifdef DEBUG_BUILD
    if (!(a.has_value() && anyis<TypeValue>(a)))
        throw std::runtime_error("Assert expected type "+demangle(typeid(T).name())+" but have (non-TypeValue) "+typeName(a));
    if (!anyas<TypeValue>(a).isa<T>())
        throw std::runtime_error("Assert expected type "+demangle(typeid(T).name())+" but have "+typeName(a));
    #endif
    assert(a.has_value() && anyis<TypeValue>(a));
    anyas<TypeValue>(a).assertType<T>(); // will throw
}

// is a an any TypeValue wrapping a ptr<T> (exactly T)
template<typename T>
bool is(const std::any& a) {
    if (!a.has_value() || !anyis<TypeValue>(a))
        throw std::runtime_error("is<T> requires any a is a TypeValue");
    return anyas<TypeValue>(a).is<T>();
}

// is a an any TypeValue wrapping a ptr<T> or a superclass of T?
template<typename T>
bool isa(const std::any& a) {
    if (!a.has_value() || !anyis<TypeValue>(a))
        throw std::runtime_error("isa<T> requires any a is a TypeValue");
    if (anyis<TypeValue>(a)) // redundant?
        return anyas<TypeValue>(a).isa<T>();
    return false;
}

// convert from any wrapping TypeValue to ptr<T>
template<typename T>
ptr<T> as(const std::any& a) {
    if (!a.has_value() || !anyis<TypeValue>(a))
        throw std::runtime_error("as<T> requires any a is a TypeValue");
    #ifdef DEBUG_BUILD
    if (!isa<T>(a))
        throw std::runtime_error("Can't cast as<"+demangle(typeid(T).name())+">("+typeName(a)+")");
    #endif
    return anyas<TypeValue>(a).as<T>();
}





ptr<AST> ASTGenerator::ast(std::istream& source, const std::string& name)
{
    // store entire source string
    this->source = std::make_shared<std::string>(std::string(std::istreambuf_iterator<char>(source), {}));
    source.seekg(0);
    this->sourceName = name;
    setCompileContext(this->source, this->sourceName);

    using namespace antlr4;

    ANTLRInputStream input(source);
    RoxalIndentationLexer lexer(&input);
    CommonTokenStream tokens(&lexer);

    class ParserErrorListener : public antlr4::BaseErrorListener {
    public:
        bool hadError = false;
        virtual void syntaxError(antlr4::Recognizer *recognizer,
                                 antlr4::Token *offendingSymbol,
                                 size_t line, size_t charPositionInLine,
                                 const std::string &msg, std::exception_ptr e) override
        {
            if (!hadError) {
                hadError = true;
                compileError(std::to_string(line) + ":" +
                             std::to_string(charPositionInLine) + " - " + msg);
            }
        }
    } errorListener;

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
    lexer.removeErrorListeners();
    parser.removeErrorListeners();
    lexer.addErrorListener(&errorListener);
    parser.addErrorListener(&errorListener);

    auto tree = parser.file_input();

    if (errorListener.hadError) {
        this->source = nullptr;
        this->sourceName.clear();
        return nullptr;
    }

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
            compileError(e.what());
            return nullptr;
        } catch (std::exception& e) {
            compileError(e.what());
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
        return std::any();
#else
    #define visitEnd() \
        } catch (std::exception& ve) { \
            throw std::runtime_error((std::string("Exception in ") + __func__ + " - " + std::string(ve.what())).c_str()); \
        } \
        return std::any();
#endif




// info to represent cases:
//  (arg*) - call only with 0 or more args - (call==true)
//  [arg+] - indexer only with 1 or more args - (indexer==true, arguments==true)
//  .access - accessor only (accessor==true)
//  .access(arg*) - accessor and call (with 0 or more args) (accessor=true && call==true)
struct ArgsOrAccessorInfo {
    bool call;
    bool accessor;
    bool indexer;
    bool arguments; // if indexer: arguments? (or slices)
    UnicodeString accessed; // for accessor (or annotation identifier)
    // calls can include param names, indexers cannot (empty string if no name given)
    typedef std::vector<ArgNameExpr> ArgNameExprVec;
    ptr<ArgNameExprVec> args;
};



std::any ASTGenerator::visitFile_input(RoxalParser::File_inputContext *context)
{
    visitStart();

    auto file = std::make_shared<File>();
    setSourceInfo(file, context);

    if (context->annotation().size() > 0) {

        for(size_t i=0; i < context->annotation().size();i++) {

            auto annotInfo = anyas<ptr<ArgsOrAccessorInfo>>(visitAnnotation(context->annotation().at(i)));

            auto annotation = std::make_shared<Annotation>();
            annotation->name = annotInfo->accessed;
            annotation->args = *annotInfo->args;

            file->annotations.push_back(annotation);
        }
    }





    if (context->import_stmt().size() > 0) {

        for(size_t i=0; i < context->import_stmt().size(); i++) {
            auto import = as<Import>(visitImport_stmt(context->import_stmt().at(i)));
            file->imports.push_back(import);
        }
    }


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



std::any ASTGenerator::visitSingle_input(RoxalParser::Single_inputContext *context)
{
    visitStart();

    return typeValue(std::make_shared<AST>());

    visitEnd();
}



std::any ASTGenerator::visitImport_stmt(RoxalParser::Import_stmtContext *context)
{
    visitStart();

    auto import = std::make_shared<Import>();
    setSourceInfo(import, context);

    for(auto i=0; i<context->IDENTIFIER().size(); i++) {
        auto component { UnicodeString::fromUTF8(context->IDENTIFIER().at(i)->getText()) };
        import->packages.push_back( component );
    }

    if (context->STAR())
        import->symbols.push_back("*");
    else if (context->identifier_list()) {
        auto symbols = anyas<std::vector<UnicodeString>>(visitIdentifier_list(context->identifier_list()));
        import->symbols = symbols;
    }

    return typeValue(import);
    visitEnd();
}


std::any ASTGenerator::visitIdentifier_list(RoxalParser::Identifier_listContext *context)
{
    visitStart();

    std::vector<UnicodeString> symbols {};
    for(auto i=0; i<context->IDENTIFIER().size(); i++) {
        auto component { UnicodeString::fromUTF8(context->IDENTIFIER().at(i)->getText()) };
        symbols.push_back(component);
    }

    return symbols;
    visitEnd();
}




std::any ASTGenerator::visitDeclaration(RoxalParser::DeclarationContext *context)
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



std::any ASTGenerator::visitStatement(RoxalParser::StatementContext *context)
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
    else if (context->compound_stmt()) {
        auto compound = visitCompound_stmt(context->compound_stmt());
        if (is<ReturnStatement>(compound)) {
            stmt = as<ReturnStatement>(compound);
        }
        else if (is<WhileStatement>(compound)) {
            stmt = as<WhileStatement>(compound);
        }
        else if (is<ForStatement>(compound)) {
            stmt = as<ForStatement>(compound);
        }
        else if (is<IfStatement>(compound)) {
            stmt = as<IfStatement>(compound);
        }
        else if (is<Suite>(compound)) {
            stmt = as<Suite>(compound);
        }
        else if (is<OnStatement>(compound)) {
            stmt = as<OnStatement>(compound);
        }
        else if (is<TryStatement>(compound)) {
            stmt = as<TryStatement>(compound);
        }
        else if (is<RaiseStatement>(compound)) {
            stmt = as<RaiseStatement>(compound);
        }
        else if (is<ExpressionStatement>(compound))
            stmt = as<ExpressionStatement>(compound);
        else
            throw std::runtime_error(std::string("unimplemented compound statement type: ")+typeName(compound));

    }
    else
        throw std::runtime_error("unhandled statement parse alternative");

    if (context->until_clause()) {
        auto ucExpr = as<Expression>(visitExpression(context->until_clause()->expression()));
        auto untilStmt = std::make_shared<UntilStatement>();
        setSourceInfo(untilStmt, context->until_clause());
        untilStmt->stmt = stmt;
        untilStmt->condition = ucExpr;
        stmt = untilStmt;
    }

    setSourceInfo(stmt, context);
    return typeValue(stmt);

    visitEnd();
}



std::any ASTGenerator::visitExpr_stmt(RoxalParser::Expr_stmtContext *context)
{
    visitStart();

    auto expr = visitExpression(context->expression());
    setSourceInfo(as<Expression>(expr), context);

    return expr;

    visitEnd();
}



std::any ASTGenerator::visitExpression(RoxalParser::ExpressionContext *context)
{
    visitStart();
    auto assignment = visitAssignment(context->assignment());
    setSourceInfo(as<AST>(assignment), context);
    return assignment;
    visitEnd();
}



std::any ASTGenerator::visitCompound_stmt(RoxalParser::Compound_stmtContext *context)
{
    visitStart();

    if (context->return_stmt())
        return visitReturn_stmt(context->return_stmt());
    else if (context->block_stmt())
        return visitBlock_stmt(context->block_stmt());
    else if (context->if_stmt())
        return visitIf_stmt(context->if_stmt());
    else if (context->while_stmt())
        return visitWhile_stmt(context->while_stmt());
    else if (context->for_stmt())
        return visitFor_stmt(context->for_stmt());
    else if (context->on_stmt())
        return visitOn_stmt(context->on_stmt());
    else if (context->emit_stmt())
        return visitEmit_stmt(context->emit_stmt());
    else if (context->try_stmt())
        return visitTry_stmt(context->try_stmt());
    else if (context->raise_stmt())
        return visitRaise_stmt(context->raise_stmt());
    else
        throw std::runtime_error("unimplemented compound statement alternative");

    visitEnd();
}



std::any ASTGenerator::visitBlock_stmt(RoxalParser::Block_stmtContext *context)
{
    visitStart();
    return visitSuite(context->suite());
    visitEnd();
}



std::any ASTGenerator::visitReturn_stmt(RoxalParser::Return_stmtContext *context)
{
    visitStart();

    auto returnStmt = std::make_shared<ReturnStatement>();
    setSourceInfo(returnStmt, context);
    if (context->expression())
        returnStmt->expr = as<Expression>(visitExpression(context->expression()));

    return typeValue(returnStmt);
    visitEnd();
}



std::any ASTGenerator::visitIf_stmt(RoxalParser::If_stmtContext *context)
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



std::any ASTGenerator::visitWhile_stmt(RoxalParser::While_stmtContext *context)
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


std::any ASTGenerator::visitFor_stmt(RoxalParser::For_stmtContext *context)
{
    visitStart();

    auto forStmt = std::make_shared<ForStatement>();
    setSourceInfo(forStmt, context);

    for(int i=0; i<context->ident_opt_type().size();i++) {
        auto ident_opt_type_any = visitIdent_opt_type(context->ident_opt_type().at(i));

        auto ident_opt_type = anyas<std::pair<icu::UnicodeString,std::variant<std::monostate,BuiltinType,icu::UnicodeString>>>(ident_opt_type_any);

        ptr<VarDecl> vardecl = std::make_shared<VarDecl>();
        vardecl->name = ident_opt_type.first;
        if (std::holds_alternative<BuiltinType>(ident_opt_type.second))
            vardecl->varType = std::get<BuiltinType>(ident_opt_type.second);
        if (std::holds_alternative<icu::UnicodeString>(ident_opt_type.second))
            vardecl->varType = std::get<icu::UnicodeString>(ident_opt_type.second);

        forStmt->targetList.push_back(vardecl);
    }

    auto iterable = visitExpression(context->expression());
    auto body = visitSuite(context->suite());

    forStmt->iterable = as<Expression>(iterable);
    forStmt->body = as<Suite>(body);

    return typeValue(forStmt);
    visitEnd();
}


std::any ASTGenerator::visitOn_stmt(RoxalParser::On_stmtContext *context)
{
    visitStart();

    auto onStmt = std::make_shared<OnStatement>();
    setSourceInfo(onStmt, context);

    onStmt->trigger = as<Expression>(visitExpression(context->expression()));
    onStmt->body = as<Suite>(visitSuite(context->suite()));

    return typeValue(onStmt);
    visitEnd();
}


std::any ASTGenerator::visitEmit_stmt(RoxalParser::Emit_stmtContext *context)
{
    visitStart();

    auto expr = as<Expression>(visitExpression(context->expression()));

    auto access = std::make_shared<UnaryOp>(UnaryOp::Accessor);
    setSourceInfo(access, context->EMIT());
    access->arg = expr;
    access->member = toUnicodeString("emit");

    auto call = std::make_shared<Call>();
    setSourceInfo(call, context);
    call->callable = access;
    call->args = {};

    auto exprStmt = std::make_shared<ExpressionStatement>();
    setSourceInfo(exprStmt, context);
    exprStmt->expr = call;

    return typeValue(exprStmt);
    visitEnd();
}

std::any ASTGenerator::visitTry_stmt(RoxalParser::Try_stmtContext *context)
{
    visitStart();

    auto tryStmt = std::make_shared<TryStatement>();
    setSourceInfo(tryStmt, context);

    tryStmt->body = as<Suite>(visitSuite(context->suite()));

    for (size_t i = 0; i < context->except_clause().size(); ++i) {
        auto excCtx = context->except_clause(i);
        TryStatement::ExceptClause ec;
        if (excCtx->expression())
            ec.type = as<Expression>(visitExpression(excCtx->expression()));
        if (excCtx->IDENTIFIER())
            ec.name = UnicodeString::fromUTF8(excCtx->IDENTIFIER()->getText());
        ec.body = as<Suite>(visitSuite(excCtx->suite()));
        tryStmt->exceptClauses.push_back(ec);
    }

    if (context->finally_clause())
        tryStmt->finallySuite = as<Suite>(visitSuite(context->finally_clause()->suite()));

    return typeValue(tryStmt);
    visitEnd();
}

std::any ASTGenerator::visitRaise_stmt(RoxalParser::Raise_stmtContext *context)
{
    visitStart();
    auto rs = std::make_shared<RaiseStatement>();
    setSourceInfo(rs, context);
    if (context->expression())
        rs->exception = as<Expression>(visitExpression(context->expression()));
    return typeValue(rs);
    visitEnd();
}

std::any ASTGenerator::visitExcept_clause(RoxalParser::Except_clauseContext *context)
{
    visitStart();
    visitEnd();
    return {};
}

std::any ASTGenerator::visitFinally_clause(RoxalParser::Finally_clauseContext *context)
{
    visitStart();
    visitEnd();
    return {};
}

std::any ASTGenerator::visitUntil_clause(RoxalParser::Until_clauseContext *context)
{
    visitStart();

    auto expr = visitExpression(context->expression());
    return expr;

    visitEnd();
}




std::any ASTGenerator::visitVar_decl(RoxalParser::Var_declContext *context)
{
    visitStart();

    UnicodeString ident { UnicodeString::fromUTF8(context->IDENTIFIER().at(0)->getText()) };

    auto vardecl = std::make_shared<VarDecl>();
    setSourceInfo(vardecl,context);
    vardecl->name = ident;

    if (context->annotation().size() > 0) {

        for(size_t i=0; i< context->annotation().size();i++) {

            auto annotInfo = anyas<ptr<ArgsOrAccessorInfo>>(visitAnnotation(context->annotation().at(i)));

            auto annotation = std::make_shared<Annotation>();
            annotation->name = annotInfo->accessed;
            annotation->args = *annotInfo->args;

            vardecl->annotations.push_back(annotation);
        }
    }

    if (context->COLON()) { // type specified
        if (context->builtin_type()) {
            auto builtinType = anyas<BuiltinType>(visitBuiltin_type(context->builtin_type()));
            vardecl->varType = builtinType;
        }
        else if (context->IDENTIFIER().size()>1) {
            auto typeIdent { UnicodeString::fromUTF8(context->IDENTIFIER().at(1)->getText()) };
            vardecl->varType = typeIdent;
        }
    }

    if (context->EQUALS())
        vardecl->initializer = as<Expression>(visitExpression(context->expression()));

    return typeValue(vardecl);
    visitEnd();
}


std::any ASTGenerator::visitIdent_opt_type(RoxalParser::Ident_opt_typeContext *context)
{
    visitStart();

    auto ident { UnicodeString::fromUTF8(context->IDENTIFIER().at(0)->getText()) };

    if (context->COLON()) { // type specified
        if (context->builtin_type())
            return std::make_pair(ident, std::variant<std::monostate,BuiltinType,icu::UnicodeString>(anyas<BuiltinType>(visitBuiltin_type(context->builtin_type()))));
        else {
            auto identType { UnicodeString::fromUTF8(context->IDENTIFIER().at(1)->getText()) };

            return std::make_pair(ident, std::variant<std::monostate,BuiltinType,icu::UnicodeString>(identType));
        }
    }
    else
        return std::make_pair(ident, std::variant<std::monostate,BuiltinType,icu::UnicodeString>(std::monostate{}));

    visitEnd();
}



std::any ASTGenerator::visitFunc_decl(RoxalParser::Func_declContext *context)
{
    visitStart();

    auto funcdecl = std::make_shared<FuncDecl>();
    setSourceInfo(funcdecl,context);

    if (context->annotation().size() > 0) {

        for(size_t i=0; i< context->annotation().size();i++) {

            auto annotInfo = anyas<ptr<ArgsOrAccessorInfo>>(visitAnnotation(context->annotation().at(i)));

            auto annotation = std::make_shared<Annotation>();
            annotation->name = annotInfo->accessed;
            annotation->args = *annotInfo->args;

            funcdecl->annotations.push_back(annotation);
        }
    }

    auto func = visitFunction(context->function());
    funcdecl->func = as<Function>(func);

    return typeValue(funcdecl);
    visitEnd();
}



std::any ASTGenerator::visitFunction(RoxalParser::FunctionContext *context)
{
    visitStart();

    auto func = as<Function>(visitFunc_sig(context->func_sig()));

    auto body = visitSuite(context->suite());
    auto suite = as<Suite>(body);

    if (!suite->declsOrStmts.empty()) {
        auto first = suite->declsOrStmts.front();
        if (std::holds_alternative<ptr<Statement>>(first)) {
            auto stmt = std::get<ptr<Statement>>(first);
            if (auto exprStmt = std::dynamic_pointer_cast<ExpressionStatement>(stmt)) {
                if (auto str = std::dynamic_pointer_cast<Str>(exprStmt->expr)) {
                    str->str = trim(str->str);
                    auto annot = std::make_shared<Annotation>();
                    annot->name = UnicodeString::fromUTF8("doc");
                    annot->args.emplace_back(UnicodeString(), str);
                    func->annotations.push_back(annot);
                    suite->declsOrStmts.erase(suite->declsOrStmts.begin());
                }
            }
        }
    }

    func->body = suite;

    return typeValue(func);
    visitEnd();
}


std::any ASTGenerator::visitFunc_sig(RoxalParser::Func_sigContext *context)
{
    visitStart();

    auto ident { UnicodeString::fromUTF8(context->IDENTIFIER()->getText()) };

    auto func = std::make_shared<Function>();
    setSourceInfo(func,context);
    func->isProc = (context->PROC() != nullptr);
    func->name = ident;

    if (context->parameters()) {
        auto params = visitParameters(context->parameters());
        func->params = *anyas<ptr<std::vector<ptr<Parameter>>>>(params);
    }

    // return types (optional)
    if (context->return_type()) {
        auto returnTypeVector = anyas<std::vector<std::variant<BuiltinType,icu::UnicodeString>>>(visitReturn_type(context->return_type()));
        func->returnTypes = returnTypeVector;
    }

    if (func->returnTypes.has_value() && func->isProc)
        throw std::runtime_error("Proc "+toUTF8StdString(ident)+" cannot specify return types.");

    return typeValue(func);
    visitEnd();
}


// returns an Any(ptr<std::vector<ptr<Parameter>>>)
std::any ASTGenerator::visitParameters(RoxalParser::ParametersContext *context)
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



std::any ASTGenerator::visitParameter(RoxalParser::ParameterContext *context)
{
    visitStart();
    auto ident { UnicodeString::fromUTF8(context->IDENTIFIER().at(0)->getText()) };

    auto param = std::make_shared<Parameter>();
    setSourceInfo(param,context->IDENTIFIER().at(0));
    param->name = ident;

   if (context->annotation().size() > 0) {

        for(size_t i=0; i< context->annotation().size();i++) {

            auto annotInfo = anyas<ptr<ArgsOrAccessorInfo>>(visitAnnotation(context->annotation().at(i)));

            auto annotation = std::make_shared<Annotation>();
            annotation->name = annotInfo->accessed;
            annotation->args = *annotInfo->args;

            param->annotations.push_back(annotation);
        }
    }

    if (context->builtin_type()) {
        auto builtinType = anyas<BuiltinType>(visitBuiltin_type(context->builtin_type()));
        param->type = builtinType;
    }
    else if (context->IDENTIFIER().size()>1) {
        auto typeIdent { UnicodeString::fromUTF8(context->IDENTIFIER().at(1)->getText()) };
        param->type = typeIdent;
    }
    else {} // type is optional

    if (context->expression()) {
        auto expr = visitExpression(context->expression());
        param->defaultValue = as<Expression>(expr);
    }

    return typeValue(param);
    visitEnd();
}



std::any ASTGenerator::visitSuite(RoxalParser::SuiteContext *context)
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



std::any ASTGenerator::visitType_decl(RoxalParser::Type_declContext *context)
{
    visitStart();

    auto typedecl = std::make_shared<TypeDecl>();
    setSourceInfo(typedecl,context);

    bool isInterface = (context->INTERFACE() != nullptr);
    bool isActor = (context->ACTOR() != nullptr);
    bool isEnumeration = (context->ENUM() != nullptr);
    bool isObject = (context->OBJECT() != nullptr);
    if (isActor)
        typedecl->kind = TypeDecl::Actor;
    else if (isInterface)
        typedecl->kind = TypeDecl::Interface;
    else if (isEnumeration)
        typedecl->kind = TypeDecl::Enumeration;
    else if (isObject)
        typedecl->kind = TypeDecl::Object;

    size_t identIndex = 0;
    typedecl->name = UnicodeString::fromUTF8(context->IDENTIFIER().at(identIndex++)->getText());

    if (isInterface && context->IMPLEMENTS() != nullptr)
        throw std::runtime_error("Interface "+toUTF8StdString(typedecl->name)+" cannot implement, use extends instead");

    if (isEnumeration && context->IMPLEMENTS() != nullptr)
        throw std::runtime_error("Enum "+toUTF8StdString(typedecl->name)+" cannot implement, use extends instead");

    if (context->annotation().size() > 0) {

        for(size_t i=0; i< context->annotation().size();i++) {

            auto annotInfo = anyas<ptr<ArgsOrAccessorInfo>>(visitAnnotation(context->annotation().at(i)));

            auto annotation = std::make_shared<Annotation>();
            annotation->name = annotInfo->accessed;
            annotation->args = *annotInfo->args;

        typedecl->annotations.push_back(annotation);
        }
    }

    if (context->str()) {
        auto strVal = as<Str>(visitStr(context->str()));
        strVal->str = trim(strVal->str);
        auto annotation = std::make_shared<Annotation>();
        annotation->name = UnicodeString::fromUTF8("doc");
        annotation->args.emplace_back(UnicodeString(), strVal);
        typedecl->annotations.push_back(annotation);
    }

    if (context->EXTENDS()) {
        typedecl->extends = UnicodeString::fromUTF8(context->IDENTIFIER().at(identIndex++)->getText());
        if (isEnumeration) {
            if (typedecl->extends != UnicodeString("byte") && typedecl->extends != UnicodeString("int"))
                throw std::runtime_error("Enum(eration) "+toUTF8StdString(typedecl->name)+" can only extend byte or int");
        }
    }

    while(identIndex < context->IDENTIFIER().size())
        typedecl->implements.push_back(UnicodeString::fromUTF8(context->IDENTIFIER().at(identIndex++)->getText()));

    if (context->method().size() > 255) // TODO: revise
        throw std::runtime_error("Too many methods for one actor or object type.");

    for(size_t i=0; i<context->method().size(); i++) {

        auto methodContext { context->method().at(i) };

        auto func = visitMethod(methodContext);

        typedecl->methods.push_back(as<Function>(func));
    }

    for(size_t i=0; i<context->property().size(); i++) {
        auto propertyContext { context->property().at(i) };

        auto varDecl = visitProperty(propertyContext);

        typedecl->properties.push_back(as<VarDecl>(varDecl));
    }

    for(size_t i=0; i<context->enum_label().size(); i++) {

        auto enumValueContext { context->enum_label().at(i) };

        auto enumLabelExpr = anyas<std::pair<icu::UnicodeString,ptr<Expression>>>(visitEnum_label(enumValueContext));

        typedecl->enumLabels.push_back(enumLabelExpr);
    }

    return typeValue(typedecl);
    visitEnd();
}



std::any ASTGenerator::visitMethod(RoxalParser::MethodContext *context)
{
    visitStart();

    auto func = visitFunc_sig(context->func_sig());
    if (!isa<Function>(func))
        throw std::runtime_error("Expected Function for methods of object or actor type");

    auto function = as<Function>(func);
    function->access = (context->PRIVATE()!=nullptr) ? Access::Private : Access::Public;

    // has body suite?
    if (context->COLON()) {
        auto body = visitSuite(context->suite());
        function->body = as<Suite>(body);
    }
    else // abstract method
        function->body = std::monostate();


    // TODO: should visitAnnotation before visitFunction?
    if (context->annotation().size() > 0) {

        for(size_t i=0; i< context->annotation().size();i++) {

            auto annotInfo = anyas<ptr<ArgsOrAccessorInfo>>(visitAnnotation(context->annotation().at(i)));

            auto annotation = std::make_shared<Annotation>();
            annotation->name = annotInfo->accessed;
            annotation->args = *annotInfo->args;

            function->annotations.push_back(annotation);
        }
    }

    return typeValue(function);
    visitEnd();
}



std::any ASTGenerator::visitProperty(RoxalParser::PropertyContext *context)
{
    visitStart();

    // a property looks like a variable declaration
    auto varDecl = std::make_shared<VarDecl>();

    varDecl->access = (context->PRIVATE()!=nullptr) ? Access::Private : Access::Public;

    varDecl->name = UnicodeString::fromUTF8(context->IDENTIFIER().at(0)->getText());
    if (context->annotation().size() > 0) {
        for(size_t i=0; i<context->annotation().size(); i++) {
            auto annotInfo = anyas<ptr<ArgsOrAccessorInfo>>(visitAnnotation(context->annotation().at(i)));
            auto annotation = std::make_shared<Annotation>();
            annotation->name = annotInfo->accessed;
            annotation->args = *annotInfo->args;
            varDecl->annotations.push_back(annotation);
        }
    }

    if (context->builtin_type()) {
        auto builtinType = anyas<BuiltinType>(visitBuiltin_type(context->builtin_type()));
        varDecl->varType = builtinType;
    }
    else if (context->IDENTIFIER().size()>1) {
        auto typeIdent { UnicodeString::fromUTF8(context->IDENTIFIER().at(1)->getText()) };
        varDecl->varType = typeIdent;
    }
    else {} // type is optional

    if (context->expression()) {
        auto expr = visitExpression(context->expression());
        varDecl->initializer = as<Expression>(expr);
    }

    return typeValue(varDecl);
    visitEnd();
}


std::any ASTGenerator::visitEnum_label(RoxalParser::Enum_labelContext *context)
{
    visitStart();

    UnicodeString labelName { UnicodeString::fromUTF8(context->IDENTIFIER()->getText()) };

    ptr<Expression> expr = nullptr;

    if (context->expression())
        expr = as<Expression>(visitExpression(context->expression()));

    return std::make_pair(labelName, expr);

    visitEnd();
}




// returns ptr<ArgsOrAccessorInfo>
std::any ASTGenerator::visitAnnotation(RoxalParser::AnnotationContext *context)
{
    visitStart();

    UnicodeString annotName { UnicodeString::fromUTF8(context->IDENTIFIER()->getText()) };

    auto info = std::make_shared<ArgsOrAccessorInfo>();
    info->accessor = info->indexer = false;
    info->call = true;
    info->accessed = annotName;

    auto argexprs = std::make_shared<ArgsOrAccessorInfo::ArgNameExprVec>();
    for(int i=0; i<context->annot_argument().size(); i++) {
        auto argNameExpr = anyas<ArgNameExpr>(visitAnnot_argument(context->annot_argument().at(i)));
        argexprs->push_back(argNameExpr);
    }

    info->args = argexprs;

    return info;
    visitEnd();
}



std::any ASTGenerator::visitAnnot_argument(RoxalParser::Annot_argumentContext *context)
{
    visitStart();

    UnicodeString argName { context->IDENTIFIER()? UnicodeString::fromUTF8(context->IDENTIFIER()->getText()) : UnicodeString() };
    ptr<Expression> expr = as<Expression>(visitExpression(context->expression()));

    return std::make_pair(argName, expr);
    visitEnd();
}


std::any ASTGenerator::visitLambda_func(RoxalParser::Lambda_funcContext *context)
{
    visitStart();

    auto func = std::make_shared<Function>();
    setSourceInfo(func,context);
    func->isProc = false;
    func->name.reset();

    if (context->parameters()) {
        auto params = visitParameters(context->parameters());
        func->params = *anyas<ptr<std::vector<ptr<Parameter>>>>(params);
    }

    // return types (optional)
    if (context->return_type()) {
        auto returnTypeVector = anyas<std::vector<std::variant<BuiltinType,icu::UnicodeString>>>(visitReturn_type(context->return_type()));
        func->returnTypes = returnTypeVector;
    }

    if (context->suite()) {
        auto suite = visitSuite(context->suite());
        func->body = as<Suite>(suite); // suite;
    }
    else if (context->expression()) {
        auto expr = visitExpression(context->expression());
        func->body = as<Expression>(expr);
    }
    else {
        // abstract
        func->body = std::monostate();
    }

    auto lambdaFunc = std::make_shared<LambdaFunc>();
    lambdaFunc->func = func;

    return typeValue(lambdaFunc);

    visitEnd();
}


std::any ASTGenerator::visitAssignment(RoxalParser::AssignmentContext *context)
{
    visitStart();

    if (context->EQUALS() || context->COPYINTO()) { // assignment or copy-into
        auto assign = std::make_shared<Assignment>();
        setSourceInfo(assign, context);
        assign->op = context->COPYINTO() ? Assignment::CopyInto : Assignment::Assign;

        if (context->IDENTIFIER()) {  // property or variable set

            icu::UnicodeString ident { icu::UnicodeString::fromUTF8(context->IDENTIFIER()->getText()) };

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
        }
        else { // lvalue set
            auto callable = visitCall(context->call());
            assign->lhs = as<Expression>(callable);
        }

        assign->rhs = as<Expression>(visitAssignment(context->assignment()));

        return typeValue(assign);
    }
    else {
        return visitLogic_or(context->logic_or());
    }

    visitEnd();
}





std::any ASTGenerator::visitLogic_or(RoxalParser::Logic_orContext *context)
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



std::any ASTGenerator::visitLogic_and(RoxalParser::Logic_andContext *context)
{
    visitStart();

    auto bw_or = visitBitwise_or(context->bitwise_or().at(0));
    if (context->AND().size()==0) // just passing through
        return bw_or;

    auto lhs = as<Expression>(bw_or);

    ptr<BinaryOp> andOp;

    if (context->bitwise_or().size() > 1) {

        for(auto i=1; i<context->bitwise_or().size(); i++) {
            andOp = std::make_shared<BinaryOp>(BinaryOp::And);
            setSourceInfo(andOp,context);//!!!
            andOp->lhs = lhs;

            auto rhs = visitBitwise_or(context->bitwise_or().at(i));
            andOp->rhs = as<Expression>(rhs);

            lhs = andOp;
        }
    }

    return typeValue(andOp);
    visitEnd();
}


std::any ASTGenerator::visitBitwise_or(RoxalParser::Bitwise_orContext *context)
{
    visitStart();

    auto xorExpr = visitBitwise_xor(context->bitwise_xor().at(0));
    if (context->BIT_OR().size()==0)
        return xorExpr;

    auto lhs = as<Expression>(xorExpr);
    ptr<BinaryOp> op;

    for (size_t i=1; i<context->bitwise_xor().size(); ++i) {
        op = std::make_shared<BinaryOp>(BinaryOp::BitOr);
        setSourceInfo(op, context);
        op->lhs = lhs;
        auto rhs = visitBitwise_xor(context->bitwise_xor().at(i));
        op->rhs = as<Expression>(rhs);
        lhs = op;
    }

    return typeValue(op);
    visitEnd();
}


std::any ASTGenerator::visitBitwise_xor(RoxalParser::Bitwise_xorContext *context)
{
    visitStart();

    auto andExpr = visitBitwise_and(context->bitwise_and().at(0));
    if (context->BIT_XOR().size()==0)
        return andExpr;

    auto lhs = as<Expression>(andExpr);
    ptr<BinaryOp> op;

    for (size_t i=1; i<context->bitwise_and().size(); ++i) {
        op = std::make_shared<BinaryOp>(BinaryOp::BitXor);
        setSourceInfo(op, context);
        op->lhs = lhs;
        auto rhs = visitBitwise_and(context->bitwise_and().at(i));
        op->rhs = as<Expression>(rhs);
        lhs = op;
    }

    return typeValue(op);
    visitEnd();
}


std::any ASTGenerator::visitBitwise_and(RoxalParser::Bitwise_andContext *context)
{
    visitStart();

    auto equality = visitEquality(context->equality().at(0));
    if (context->BIT_AND().size()==0)
        return equality;

    auto lhs = as<Expression>(equality);
    ptr<BinaryOp> op;

    for (size_t i=1; i<context->equality().size(); ++i) {
        op = std::make_shared<BinaryOp>(BinaryOp::BitAnd);
        setSourceInfo(op, context);
        op->lhs = lhs;
        auto rhs = visitEquality(context->equality().at(i));
        op->rhs = as<Expression>(rhs);
        lhs = op;
    }

    return typeValue(op);
    visitEnd();
}



std::any ASTGenerator::visitEquality(RoxalParser::EqualityContext *context)
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
std::any ASTGenerator::visitEqualnotequal(RoxalParser::EqualnotequalContext *context)
{
    visitStart();
    BinaryOp::Op op;
    if (context->ISEQUAL()) op = BinaryOp::Equal;
    else if (context->ISNOTEQUALS()) op = BinaryOp::NotEqual;
    else op = BinaryOp::Is;
    auto compOp = std::make_shared<BinaryOp>(op);
    setSourceInfo(compOp,context);

    auto comparison = visitComparison(context->comparison());
    compOp->rhs = as<Expression>(comparison);

    return typeValue(compOp);
    visitEnd();
}



std::any ASTGenerator::visitComparison(RoxalParser::ComparisonContext *context)
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



std::any ASTGenerator::visitTerm(RoxalParser::TermContext *context)
{
    visitStart();

    auto factor = visitFactor(context->factor());
    if (context->plusminus().size() == 0)
        return factor;

    auto lhs = as<Expression>(factor);

    ptr<BinaryOp> op;

    for(auto i=0; i<context->plusminus().size(); i++) {

        auto plusminus = visitPlusminus(context->plusminus().at(i));
        assertType<BinaryOp>(plusminus);
        op = as<BinaryOp>(plusminus);
        op->lhs = lhs;

        lhs = op;
    }

    return typeValue(op);
    visitEnd();
}


// returns a BinaryOp with rhs set and lhs null
std::any ASTGenerator::visitPlusminus(RoxalParser::PlusminusContext *context)
{
    visitStart();
    auto op = std::make_shared<BinaryOp>();
    setSourceInfo(op,context);
    if (context->PLUS())
        op->op = BinaryOp::Add;
    else if (context->MINUS())
        op->op = BinaryOp::Subtract;
    else
        throw std::runtime_error("unhandled plusminus alternative");

    auto factor = visitFactor(context->factor());
    op->rhs = as<Expression>(factor);

    return typeValue(op);
    visitEnd();
}



std::any ASTGenerator::visitFactor(RoxalParser::FactorContext *context)
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
std::any ASTGenerator::visitMultdiv(RoxalParser::MultdivContext *context)
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



std::any ASTGenerator::visitUnary(RoxalParser::UnaryContext *context)
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

        UnaryOp::Op uop = UnaryOp::None;
        if (context->NOT()) uop = UnaryOp::Not;
        else if (context->MINUS()) uop = UnaryOp::Negate;
        else if (context->BIT_NOT()) uop = UnaryOp::BitNot;

        auto op = std::make_shared<UnaryOp>(uop);

        setSourceInfo(op, context);
        op->arg = as<Expression>(arg);
        return typeValue(op);
    }
    else if (context->call()) {
        return visitCall(context->call());
    }
    else
        throw std::runtime_error("unimplemented unary alternative");

    visitEnd();
}



// return will be either ptr<Call> or ptr<UnaryOp(Accessor)>
std::any ASTGenerator::visitCall(RoxalParser::CallContext *context)
{
    visitStart();

    auto primary = visitPrimary(context->primary());
    if (context->args_or_index_or_accessor().size() == 0)
        return primary;

    auto callable_or_indexable = as<Expression>(primary);

    for(int i=0; i<context->args_or_index_or_accessor().size();i++) {
        auto argsOrAccessorInfo = anyas<ptr<ArgsOrAccessorInfo>>(visitArgs_or_index_or_accessor(context->args_or_index_or_accessor().at(i)));
        if (argsOrAccessorInfo->accessor) {
            auto accessOp = std::make_shared<UnaryOp>(UnaryOp::Accessor);
            setSourceInfo(accessOp,context->args_or_index_or_accessor().at(i));
            accessOp->arg = callable_or_indexable;
            accessOp->member = argsOrAccessorInfo->accessed;
            callable_or_indexable = accessOp;
        }
        if (argsOrAccessorInfo->call) {
            auto call = std::make_shared<Call>();
            setSourceInfo(call,context->args_or_index_or_accessor().at(i));
            call->callable = callable_or_indexable;
            call->args = *argsOrAccessorInfo->args;
            callable_or_indexable = call;
        }
        if (argsOrAccessorInfo->indexer) {
            auto index = std::make_shared<Index>();
            setSourceInfo(index,context->args_or_index_or_accessor().at(i));
            index->indexable = callable_or_indexable;

            std::vector<ptr<Expression>> args {};
            for(auto& namearg : *argsOrAccessorInfo->args) {
                auto& arg { namearg.second };
                args.push_back(arg);
            }
            index->args = args;

            callable_or_indexable = index;
        }
    }

    return typeValue(callable_or_indexable);
    visitEnd();
}


// return ptr<ArgsOrAccessorInfo>
std::any ASTGenerator::visitArgs_or_index_or_accessor(RoxalParser::Args_or_index_or_accessorContext *context)
{
    visitStart();

    auto info = std::make_shared<ArgsOrAccessorInfo>();
    info->accessor = info->indexer = info->call = false;

    if (context->DOT()) { // accessor, possibly call
        info->accessor = true;
        info->arguments = false;
        UnicodeString ident;
        if (context->IDENTIFIER())
            ident = UnicodeString::fromUTF8(context->IDENTIFIER()->getText());
        else if (context->ON())
            ident = UnicodeString("on");
        else if (context->EMIT())
            ident = UnicodeString("emit");
        info->accessed = ident;

        if (context->OPEN_PAREN()) {
            info->call = true;
            info->arguments = true; // (redundant)
            if (context->arguments()) {
                auto args = anyas<ptr<ArgsOrAccessorInfo::ArgNameExprVec>>(visitArguments(context->arguments()));
                info->args = args;
            }
            else
                info->args = std::make_shared<ArgsOrAccessorInfo::ArgNameExprVec>();
        }
    }
    else if (context->OPEN_BRACK()) { // indexing
        info->indexer = true;
        auto range_or_exprs = anyas<ptr<std::vector<ptr<Expression>>>>(visitRanges(context->ranges()));
        info->args = std::make_shared<ArgsOrAccessorInfo::ArgNameExprVec>();
        for(auto& range_or_expr : *range_or_exprs) {
            // special case: if the range is just [e..e], just use the
            //  expression instead of the range
            ptr<Expression> arg = range_or_expr;
            if (arg->exprType == Expression::ExprType::Range) {
                auto r = std::dynamic_pointer_cast<Range>(arg);
                if (r->closed && (r->start!=nullptr) && (r->start == r->stop) && (r->step==nullptr))
                    arg = r->start;
            }

            info->args->push_back(std::make_pair(icu::UnicodeString(),arg));
        }
        info->arguments = false;
    }
    else if (context->OPEN_PAREN()) { // call
        info->call = true;
        if (context->arguments()) {
            auto args = anyas<ptr<ArgsOrAccessorInfo::ArgNameExprVec>>(visitArguments(context->arguments()));
            info->args = args;
        }
        else
            info->args = std::make_shared<ArgsOrAccessorInfo::ArgNameExprVec>();
    }
    else
        throw std::runtime_error("unimplemented args_or_accessor alternative");

    return info;
    visitEnd();
}



std::any ASTGenerator::visitRanges(RoxalParser::RangesContext *context)
{
    visitStart();

    auto indices = std::make_shared<std::vector<ptr<Expression>>>();

    for(int i=0; i<context->range().size(); i++)
        indices->push_back(as<Expression>(visitRange(context->range().at(i))));

    return indices;
    visitEnd();
}


std::any ASTGenerator::visitRange(RoxalParser::RangeContext *context)
{
    visitStart();

    auto range = std::make_shared<ast::Range>();

    bool hasDots = (context->DOTDOT()!=nullptr);
    bool hasLess = (context->LESS_THAN()!=nullptr);
    bool hasColon = !context->COLON().empty();

    bool closedRange = hasDots && !hasLess;
    bool openRange = !closedRange && (hasDots || hasColon);
    bool expressionOnly = !closedRange && !openRange;

    if (openRange) { // half-open range [)
        range->closed = false;
        auto startOptExpr = visitOptional_expression(context->optional_expression().at(0));
        if (startOptExpr.has_value())
            range->start = as<Expression>(startOptExpr);

        auto stopOptExpr = visitOptional_expression(context->optional_expression().at(1));
        if (stopOptExpr.has_value())
            range->stop = as<Expression>(stopOptExpr);

        if (context->expression())
            range->step = as<Expression>(visitExpression(context->expression()));
    }
    else if (closedRange) { // closed range []
        range->closed = true;
        auto startOptExpr = visitOptional_expression(context->optional_expression().at(0));
        if (startOptExpr.has_value())
            range->start = as<Expression>(startOptExpr);

        auto stopOptExpr = visitOptional_expression(context->optional_expression().at(1));
        if (stopOptExpr.has_value())
            range->stop = as<Expression>(stopOptExpr);

        if (context->expression())
            range->step = as<Expression>(visitExpression(context->expression()));
    }
    else if (expressionOnly) { // simple single index expr
        // equivelent to range [n..n]
        range->closed = true;
        range->start = as<Expression>(visitExpression(context->expression()));
        range->stop = range->start;
    }
    else
        throw std::runtime_error("Unexpected range alternative");

    return typeValue(range);
    visitEnd();
}


std::any ASTGenerator::visitOptional_expression(RoxalParser::Optional_expressionContext *context)
{
    visitStart();
    if (context->expression())
        return visitExpression(context->expression());
    return {};
    visitEnd();
}


// returns ptr<std::vector<std::pair<UnicodeString,ptr<Expression>>> of argument expressions
//  string is param name or empty
std::any ASTGenerator::visitArguments(RoxalParser::ArgumentsContext *context)
{
    visitStart();

    auto argexprs = std::make_shared<ArgsOrAccessorInfo::ArgNameExprVec>();
    for(int i=0; i<context->argument().size(); i++) {
        auto argNameExpr = anyas<ArgNameExpr>(visitArgument(context->argument().at(i)));
        argexprs->push_back(argNameExpr);
    }

    return argexprs;
    visitEnd();
}



std::any ASTGenerator::visitArgument(RoxalParser::ArgumentContext *context)
{
    visitStart();

    UnicodeString argName { context->IDENTIFIER()? UnicodeString::fromUTF8(context->IDENTIFIER()->getText()) : UnicodeString() };
    ptr<Expression> expr = as<Expression>(visitExpression(context->expression()));

    return std::make_pair(argName, expr);
    visitEnd();
}




std::any ASTGenerator::visitPrimary(RoxalParser::PrimaryContext *context)
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
    else if (context->SUPER()) {
        auto supervar = std::make_shared<Variable>("super");
        setSourceInfo(supervar,context);

        UnicodeString ident { UnicodeString::fromUTF8(context->IDENTIFIER()->getText()) };

        auto access = std::make_shared<UnaryOp>(UnaryOp::Accessor);
        setSourceInfo(access, context->DOT());
        access->arg = supervar;
        access->member = ident;

        return typeValue(access);
    }
    else if (context->IDENTIFIER()) {
        UnicodeString ident { UnicodeString::fromUTF8(context->IDENTIFIER()->getText()) };
        auto var = std::make_shared<Variable>(ident);
        setSourceInfo(var,context);
        return typeValue(var);
    }
    else if (context->num())
        return visitNum(context->num());
    else if (context->str())
        return visitStr(context->str());
    else if (context->RANGE()) {
        // create constructor call to range type
        auto call = std::make_shared<ast::Call>();
        auto rangeType = std::make_shared<ast::Type>();
        rangeType->t = BuiltinType::Range;
        call->callable = rangeType;

        auto rangeExpr = as<Expression>(visitRange(context->range()));

        call->args.push_back(std::make_pair(icu::UnicodeString(), rangeExpr));

        return typeValue(call);
    }
    else if (context->OPEN_PAREN())
        return visitExpression(context->expression());
    else if (context->lambda_func())
        return visitLambda_func(context->lambda_func());
    else if (context->list())
        return visitList(context->list());
    else if (context->vector())
        return visitVector(context->vector());
    else if (context->matrix())
        return visitMatrix(context->matrix());
    else if (context->dict())
        return visitDict(context->dict());
    else if (context->builtin_type()) {
        auto builtinType = anyas<ast::BuiltinType>(visitBuiltin_type(context->builtin_type()));
        auto type = std::make_shared<Type>();
        setSourceInfo(type,context);
        type->t = builtinType;
        return typeValue(type);
    }
    else
        throw std::runtime_error("unimplemented primary alternative");

    visitEnd();
}


// returns BuiltinType enum value
std::any ASTGenerator::visitReturn_type(RoxalParser::Return_typeContext *context)
{
    visitStart();

    std::vector<std::variant<BuiltinType,icu::UnicodeString>> returnTypes;

    // Process all type_spec nodes in order
    for (auto* typeSpecCtx : context->type_spec()) {
        auto typeSpec = anyas<std::variant<BuiltinType,icu::UnicodeString>>(visitType_spec(typeSpecCtx));
        returnTypes.push_back(typeSpec);
    }

    if (returnTypes.empty()) {
        throw std::runtime_error("Invalid return type specification - no types found.");
    }

    return returnTypes;
    
    visitEnd();
}

std::any ASTGenerator::visitType_spec(RoxalParser::Type_specContext *context)
{
    visitStart();

    if (context->builtin_type()) {
        auto builtinType = anyas<BuiltinType>(visitBuiltin_type(context->builtin_type()));
        return std::variant<BuiltinType,icu::UnicodeString>(builtinType);
    }
    else if (context->IDENTIFIER()) {
        auto typeIdent = UnicodeString::fromUTF8(context->IDENTIFIER()->getText());
        return std::variant<BuiltinType,icu::UnicodeString>(typeIdent);
    }
    else {
        throw std::runtime_error("Invalid type specification.");
    }
    
    visitEnd();
}

std::any ASTGenerator::visitBuiltin_type(RoxalParser::Builtin_typeContext *context)
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
    else if (context->RANGE())
        type = BuiltinType::Range;
    else if (context->LIST())
        type = BuiltinType::List;
    else if (context->DICT())
        type = BuiltinType::Dict;
    else if (context->VECTOR())
        type = BuiltinType::Vector;
    else if (context->MATRIX())
        type = BuiltinType::Matrix;
    else if (context->SIGNAL())
        type = BuiltinType::Signal;
    else if (context->TENSOR())
        type = BuiltinType::Tensor;
    else if (context->ORIENT())
        type = BuiltinType::Orient;
    else if (context->EVENT())
        type = BuiltinType::Event;
    else
        throw std::runtime_error("unhandled BuiltinType alternative");

    return type;
    visitEnd();
}



std::any ASTGenerator::visitList(RoxalParser::ListContext *context)
{
    visitStart();

    auto list = std::make_shared<List>();
    setSourceInfo(list,context);
    for(int i=0; i<context->expression().size();i++)
        list->elements.push_back(as<Expression>(visitExpression(context->expression().at(i))));

    return typeValue(list);
    visitEnd();
}


std::any ASTGenerator::visitVector(RoxalParser::VectorContext *context)
{
    visitStart();

    auto vec = std::make_shared<Vector>();
    setSourceInfo(vec,context);
    for(int i=0; i<context->signed_num().size(); ++i)
        vec->elements.push_back(as<Num>(visitSigned_num(context->signed_num().at(i))));

    return typeValue(vec);
    visitEnd();
}

std::any ASTGenerator::visitMatrix(RoxalParser::MatrixContext *context)
{
    visitStart();

    auto mat = std::make_shared<Matrix>();
    setSourceInfo(mat,context);
    for(int i=0; i<context->row().size(); ++i)
        mat->rows.push_back(as<Vector>(visitRow(context->row().at(i))));

    return typeValue(mat);
    visitEnd();
}

std::any ASTGenerator::visitRow(RoxalParser::RowContext *context)
{
    visitStart();

    auto vec = std::make_shared<Vector>();
    setSourceInfo(vec,context);
    for(int i=0; i<context->signed_num().size(); ++i)
        vec->elements.push_back(as<Num>(visitSigned_num(context->signed_num().at(i))));

    return typeValue(vec);
    visitEnd();
}

std::any ASTGenerator::visitSigned_num(RoxalParser::Signed_numContext *context)
{
    visitStart();

    auto num = as<Num>(visitNum(context->num()));
    if (context->MINUS()) {
        if (std::holds_alternative<int32_t>(num->num))
            num->num = -std::get<int32_t>(num->num);
        else
            num->num = -std::get<double>(num->num);
    }

    return typeValue(num);
    visitEnd();
}


std::any ASTGenerator::visitDict(RoxalParser::DictContext *context)
{
    visitStart();

    auto dict = std::make_shared<Dict>();
    setSourceInfo(dict,context);
    for(int i=0; i<context->expression().size();i+=2) {
        auto keyVal = std::make_pair<ptr<Expression>,ptr<Expression>>(
            as<Expression>(visitExpression(context->expression().at(i))),
            as<Expression>(visitExpression(context->expression().at(i+1)))
        );
        dict->entries.push_back(keyVal);
    }

    return typeValue(dict);
    visitEnd();
}



std::any ASTGenerator::visitStr(RoxalParser::StrContext *context)
{
    visitStart();
    std::string text;
    bool isDouble = false;
    bool isTriple = false;
    if (context->SINGLE_STRING()) {
        text = context->SINGLE_STRING()->getText();
    } else if (context->DOUBLE_STRING()) {
        text = context->DOUBLE_STRING()->getText();
        isDouble = true;
    } else {
        text = context->TRIPLE_STRING()->getText();
        isTriple = true;
    }

    // drop enclosing quotes
    if (isTriple)
        text = text.substr(3, text.size()-6);
    else
        text = text.substr(1, text.size()-2);

    if (isDouble && text.find('{') != std::string::npos) {
        std::vector<ptr<Expression>> parts;
        size_t pos = 0;
        while (true) {
            size_t open = text.find('{', pos);
            if (open == std::string::npos) break;
            size_t close = text.find('}', open + 1);
            if (close == std::string::npos) break;
            if (open > pos) {
                auto s = std::make_shared<Str>();
                setSourceInfo(s, context);
                s->str = toUnicodeString(text.substr(pos, open - pos)).unescape();
                parts.push_back(s);
            }
            auto ident = text.substr(open + 1, close - open - 1);
            auto var = std::make_shared<Variable>(toUnicodeString(ident));
            setSourceInfo(var, context);
            parts.push_back(var);
            pos = close + 1;
        }
        if (pos < text.size()) {
            auto s = std::make_shared<Str>();
            setSourceInfo(s, context);
            s->str = toUnicodeString(text.substr(pos)).unescape();
            parts.push_back(s);
        }
        if (parts.empty()) {
            auto str = std::make_shared<Str>();
            setSourceInfo(str, context);
            str->str = toUnicodeString(text).unescape();
            return typeValue(str);
        }
        ptr<Expression> expr = parts[0];
        for (size_t i = 1; i < parts.size(); ++i) {
            auto add = std::make_shared<BinaryOp>(BinaryOp::Add);
            setSourceInfo(add, context);
            add->lhs = expr;
            add->rhs = parts[i];
            expr = add;
        }
        return typeValue(expr);
    } else {
            auto str = std::make_shared<Str>();
            setSourceInfo(str, context);
            str->str = toUnicodeString(text).unescape();
            return typeValue(str);
        }
    visitEnd();
}



std::any ASTGenerator::visitNum(RoxalParser::NumContext *context)
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



std::any ASTGenerator::visitInteger(RoxalParser::IntegerContext *context)
{
    visitStart();

    auto num = std::make_shared<Num>();

    long long integer {0};
    if (context->DECIMAL_INTEGER()) {
        try {
            integer = std::stoll(context->getText());
        } catch (...) {
            throw std::runtime_error("Invalid integer literal");
        }
        if ((integer > std::numeric_limits<int32_t>::max()) || (integer < std::numeric_limits<int32_t>::min()))
            throw std::runtime_error("Invalid integer literal (out-of-range)");
        num->num = int32_t(integer);
        setSourceInfo(num,context->DECIMAL_INTEGER());
    }
    else if (context->HEX_INTEGER()) {
        char *p_end;
        integer = std::strtoll(context->getText().c_str()+2, &p_end, 16);
        if (errno == ERANGE)
            throw std::runtime_error("Invalid hexadecimal integer literal");
        // TODO: accept unsigned range and convert to twos-compliment
        if ((integer > std::numeric_limits<int32_t>::max()) || (integer < std::numeric_limits<int32_t>::min()))
            throw std::runtime_error("Invalid hexadecimal integer literal (out-of-range)");
        num->num = int32_t(integer);
        setSourceInfo(num,context->HEX_INTEGER());
    }
    else if (context->OCT_INTEGER()) {
        char *p_end;
        integer = std::strtoll(context->getText().c_str()+2, &p_end, 8);
        if (errno == ERANGE)
            throw std::runtime_error("Invalid octal integer literal");
        // TODO: accept unsigned range and convert to twos-compliment
        if ((integer > std::numeric_limits<int32_t>::max()) || (integer < std::numeric_limits<int32_t>::min()))
            throw std::runtime_error("Invalid octal integer literal (out-of-range)");
        num->num = int32_t(integer);
        setSourceInfo(num,context->OCT_INTEGER());
    }
    else if (context->BIN_INTEGER()) {
        char *p_end;
        integer = std::strtoll(context->getText().c_str()+2, &p_end, 2);
        if (errno == ERANGE)
            throw std::runtime_error("Invalid binary integer literal");
        // TODO: accept unsigned range and convert to twos-compliment
        if ((integer > std::numeric_limits<int32_t>::max()) || (integer < std::numeric_limits<int32_t>::min()))
            throw std::runtime_error("Invalid binary integer literal (out-of-range)");
        num->num = int32_t(integer);
        setSourceInfo(num,context->BIN_INTEGER());
    }
    else
        throw std::runtime_error("unimplemented integer literal:"+context->getText());

    return typeValue(num);

    visitEnd();
}
