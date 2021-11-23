#pragma once

#include <stack>

#include "antlr4-runtime.h"

#include "Chunk.h"
#include "Object.h"

#include "RoxalVisitor.h"

namespace roxal {


class RoxalCompiler : public RoxalVisitor
{
public:
    RoxalCompiler() {}

    ObjFunction* compile(std::istream& source, const std::string& name);


    virtual antlrcpp::Any visitFile_input(RoxalParser::File_inputContext *context);

    virtual antlrcpp::Any visitSingle_input(RoxalParser::Single_inputContext *context);

    virtual antlrcpp::Any visitDeclaration(RoxalParser::DeclarationContext *context);

    virtual antlrcpp::Any visitStatement(RoxalParser::StatementContext *context);

    virtual antlrcpp::Any visitExpr_stmt(RoxalParser::Expr_stmtContext *context);

    virtual antlrcpp::Any visitExpression(RoxalParser::ExpressionContext *context);

    virtual antlrcpp::Any visitCompound_stmt(RoxalParser::Compound_stmtContext *context);

    virtual antlrcpp::Any visitBlock_stmt(RoxalParser::Block_stmtContext *context);

    virtual antlrcpp::Any visitPrint_stmt(RoxalParser::Print_stmtContext *context);

    virtual antlrcpp::Any visitReturn_stmt(RoxalParser::Return_stmtContext *context);

    virtual antlrcpp::Any visitIf_stmt(RoxalParser::If_stmtContext *context);

    virtual antlrcpp::Any visitWhile_stmt(RoxalParser::While_stmtContext *context);

    virtual antlrcpp::Any visitVar_decl(RoxalParser::Var_declContext *context);

    virtual antlrcpp::Any visitFunc_decl(RoxalParser::Func_declContext *context);

    virtual antlrcpp::Any visitFunction(RoxalParser::FunctionContext *context);

    virtual antlrcpp::Any visitParameters(RoxalParser::ParametersContext *context);

    virtual antlrcpp::Any visitParameter(RoxalParser::ParameterContext *context);

    virtual antlrcpp::Any visitSuite(RoxalParser::SuiteContext *context);

    virtual antlrcpp::Any visitType_decl(RoxalParser::Type_declContext *context);

    virtual antlrcpp::Any visitAssignment(RoxalParser::AssignmentContext *context);

    virtual antlrcpp::Any visitLogic_or(RoxalParser::Logic_orContext *context);

    virtual antlrcpp::Any visitLogic_and(RoxalParser::Logic_andContext *context);

    virtual antlrcpp::Any visitEquality(RoxalParser::EqualityContext *context);

    virtual antlrcpp::Any visitEqualnotequal(RoxalParser::EqualnotequalContext *context);

    virtual antlrcpp::Any visitComparison(RoxalParser::ComparisonContext *context);

    virtual antlrcpp::Any visitTerm(RoxalParser::TermContext *context);

    virtual antlrcpp::Any visitFactor(RoxalParser::FactorContext *context);

    virtual antlrcpp::Any visitMultdiv(RoxalParser::MultdivContext *context);

    virtual antlrcpp::Any visitUnary(RoxalParser::UnaryContext *context);

    virtual antlrcpp::Any visitCall(RoxalParser::CallContext *context);

    virtual antlrcpp::Any visitArgs_or_accessor(RoxalParser::Args_or_accessorContext *context);

    virtual antlrcpp::Any visitArguments(RoxalParser::ArgumentsContext *context);

    virtual antlrcpp::Any visitPrimary(RoxalParser::PrimaryContext *context);

    virtual antlrcpp::Any visitBuiltin_type(RoxalParser::Builtin_typeContext *context);

    virtual antlrcpp::Any visitStr(RoxalParser::StrContext *context);

    virtual antlrcpp::Any visitNum(RoxalParser::NumContext *context);

    virtual antlrcpp::Any visitInteger(RoxalParser::IntegerContext *context);
   
protected:

    struct Local {
        Local(const icu::UnicodeString& _name, int scopeDepth)
            : name(_name), depth(scopeDepth), isCaptured(false) {}
        icu::UnicodeString name;
        int depth;
        bool isCaptured;
    };

    struct Upvalue {
        Upvalue(uint8_t i, bool islocal) 
            : index(i), isLocal(islocal) {}
        uint8_t index;
        bool isLocal;
    };


    // stack new states are we enter new functions to compile
    struct FunctionScope {
        FunctionScope(const icu::UnicodeString& funcName, FunctionType funcType) 
            : scopeDepth(0), functionType(funcType), strict(true)
        {
            function = functionVal();
            function->name = funcName;
            UnicodeString localName { funcType==FunctionType::Method ? "this" : "" };
            locals.push_back(Local(localName,0)); 
        }

        std::vector<Local> locals;
        std::vector<Upvalue> upvalues;
        int scopeDepth;

        bool strict; 

        ObjFunction* function;
        FunctionType functionType;

        std::vector<uint16_t> identConsts;
    };

    typedef std::vector<FunctionScope> FunctionScopes;
    FunctionScopes funcScopes;

    auto funcScope() {
        if (!funcScopes.empty())
            return funcScopes.end()-1;
        throw std::runtime_error("FunctionScope stack underflow");
    }

    auto enclosingFuncScope(FunctionScopes::iterator s) {
        if (s != funcScopes.begin())
            return --s;
        throw std::runtime_error("FunctionScope stack underflow");
    }


    struct TypeScope {

    };

    typedef std::vector<TypeScope> TypeScopes;
    TypeScopes typeScopes;

    auto typeScope() {
        if (!typeScopes.empty())
            return typeScopes.end()-1;
        throw std::runtime_error("TypeScope stack underflow");
    }

    auto enclosingTypeScope(TypeScopes::iterator s) {
        if (s != typeScopes.begin())
            return --s;
        throw std::runtime_error("TypeScope stack underflow");
    }




    ptr<Chunk> currentChunk() const {
            return funcScopes.back().function->chunk;
    }

    antlr4::Token* currentToken;


    void beginScope();
    void endScope();

    void error(const std::string& message);

    void emitByte(uint8_t byte, const std::string& comment = "");
    void emitByte(OpCode op, const std::string& comment = "");
    void emitBytes(uint8_t byte1, uint8_t byte2, const std::string& comment = "");
    void emitBytes(OpCode op, uint8_t byte2, const std::string& comment = "");

    void emitLoop(Chunk::size_type loopStart, const std::string& comment = "");

    Chunk::size_type emitJump(OpCode instruction, const std::string& comment = "");

    void emitReturn(const std::string& comment = "");
    void emitConstant(const Value& value, const std::string& comment = "");

    void patchJump(Chunk::size_type jumpInstrOffset);

    int16_t makeConstant(const Value& value);

    // keep track of which chunk string constants table entires are for identifiers and re-use them
    int16_t identifierConstant(const icu::UnicodeString& ident);

    void addLocal(const icu::UnicodeString& name);
    int16_t resolveLocal(FunctionScopes::iterator scopeState, const icu::UnicodeString& name);
    int addUpvalue(FunctionScopes::iterator scopeState, uint8_t index, bool isLocal);
    int16_t resolveUpvalue(FunctionScopes::iterator scopeState, const icu::UnicodeString& name);
    void declareVariable(const icu::UnicodeString& name);
    void defineVariable(uint16_t global);
    bool namedVariable(const icu::UnicodeString& ident, bool assign=false);

};


}
