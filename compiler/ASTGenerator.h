#pragma once

#include <core/common.h>
#include <core/AST.h>
#include "RoxalVisitor.h"


namespace roxal {

//
// Take Roxal source code and produce an Abstract Sytnax Tree (AST)
//  (uses ANTLR to create a parse tree and visit it to produce the AST)
class ASTGenerator : public RoxalVisitor
{
public:
    ASTGenerator() {}

    ptr<ast::AST> ast(std::istream& source, const std::string& name);

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
    void setSourceInfo(ptr<ast::AST> ast, antlr4::ParserRuleContext* context);

    antlr4::Token* currentToken;
    ptr<std::string> source;
    std::string sourceName;
};



}