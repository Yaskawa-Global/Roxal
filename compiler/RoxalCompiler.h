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

    virtual antlrcpp::Any visitArguments(RoxalParser::ArgumentsContext *context);

    virtual antlrcpp::Any visitPrimary(RoxalParser::PrimaryContext *context);

    virtual antlrcpp::Any visitBuiltin_type(RoxalParser::Builtin_typeContext *context);

    virtual antlrcpp::Any visitStr(RoxalParser::StrContext *context);

    virtual antlrcpp::Any visitNum(RoxalParser::NumContext *context);

    virtual antlrcpp::Any visitInteger(RoxalParser::IntegerContext *context);
   
protected:

    struct Local {
        Local(const icu::UnicodeString& _name, int scopeDepth)
            : name(_name), depth(scopeDepth) {}
        icu::UnicodeString name;
        int depth;
    };


    // stack new states are we enter new functions to compile
    struct CompileState {
        CompileState(const icu::UnicodeString& funcName, FunctionType funcType) 
            : scopeDepth(0), functionType(funcType), strict(true)
        {
            function = functionVal();
            function->name = funcName;
            locals.push_back(Local(UnicodeString(),0)); // no name for funcs
        }

        std::vector<Local> locals;
        int scopeDepth;

        bool strict; 

        ObjFunction* function;
        FunctionType functionType;

        std::vector<uint16_t> identConsts;
    };

    std::vector<CompileState> states;

    CompileState& state() {
        if (!states.empty())
            return states.back();
        throw std::runtime_error("CompilerState stack underflow");
    }


    ptr<Chunk> currentChunk() const {
            return states.back().function->chunk;
    }

    antlr4::Token* currentToken;


    void beginScope();
    void endScope();

    void error(const std::string& message);

    void emitByte(uint8_t byte);
    void emitByte(OpCode op);
    void emitBytes(uint8_t byte1, uint8_t byte2);
    void emitBytes(OpCode op, uint8_t byte2);

    void emitLoop(Chunk::size_type loopStart);

    Chunk::size_type emitJump(OpCode instruction);

    void emitReturn();
    void emitConstant(const Value& value);

    void patchJump(Chunk::size_type jumpInstrOffset);

    int16_t makeConstant(const Value& value);

    // keep track of which chunk string constants table entires are for identifiers and re-use them
    int16_t identifierConstant(const icu::UnicodeString& ident);

    void addLocal(const icu::UnicodeString& name);
    int16_t resolveLocal(const icu::UnicodeString& name);
    void declareVariable(const icu::UnicodeString& name);
    void defineVariable(uint16_t global);
    bool namedVariable(const icu::UnicodeString& ident, bool assign=false);

};


}
