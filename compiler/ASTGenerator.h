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

    virtual std::any visitFile_input(RoxalParser::File_inputContext *context);

    virtual std::any visitSingle_input(RoxalParser::Single_inputContext *context);

    virtual std::any visitImport_stmt(RoxalParser::Import_stmtContext *context);

    virtual std::any visitIdentifier_list(RoxalParser::Identifier_listContext *context);

    virtual std::any visitDeclaration(RoxalParser::DeclarationContext *context);

    virtual std::any visitStatement(RoxalParser::StatementContext *context);

    virtual std::any visitExpr_stmt(RoxalParser::Expr_stmtContext *context);

    virtual std::any visitCompound_stmt(RoxalParser::Compound_stmtContext *context);

    virtual std::any visitBlock_stmt(RoxalParser::Block_stmtContext *context);

    virtual std::any visitReturn_stmt(RoxalParser::Return_stmtContext *context);

    virtual std::any visitIf_stmt(RoxalParser::If_stmtContext *context);

    virtual std::any visitWhile_stmt(RoxalParser::While_stmtContext *context);

    virtual std::any visitFor_stmt(RoxalParser::For_stmtContext *context);

    virtual std::any visitVar_decl(RoxalParser::Var_declContext *context);

    virtual std::any visitIdent_opt_type(RoxalParser::Ident_opt_typeContext *context);

    virtual std::any visitFunc_decl(RoxalParser::Func_declContext *context);

    virtual std::any visitFunction(RoxalParser::FunctionContext *context);

    virtual std::any visitFunc_sig(RoxalParser::Func_sigContext *context);

    virtual std::any visitParameters(RoxalParser::ParametersContext *context);

    virtual std::any visitParameter(RoxalParser::ParameterContext *context);

    virtual std::any visitSuite(RoxalParser::SuiteContext *context);

    virtual std::any visitType_decl(RoxalParser::Type_declContext *context);

    virtual std::any visitMethod(RoxalParser::MethodContext *context);

    virtual std::any visitProperty(RoxalParser::PropertyContext *context);

    virtual std::any visitEnum_label(RoxalParser::Enum_labelContext *context);

    virtual std::any visitAnnotation(RoxalParser::AnnotationContext *context);

    virtual std::any visitAnnot_argument(RoxalParser::Annot_argumentContext *context);

    virtual std::any visitLambda_func(RoxalParser::Lambda_funcContext *context);

    virtual std::any visitExpression(RoxalParser::ExpressionContext *context);

    virtual std::any visitAssignment(RoxalParser::AssignmentContext *context);


    virtual std::any visitLogic_or(RoxalParser::Logic_orContext *context);

    virtual std::any visitLogic_and(RoxalParser::Logic_andContext *context);

    virtual std::any visitEquality(RoxalParser::EqualityContext *context);

    virtual std::any visitEqualnotequal(RoxalParser::EqualnotequalContext *context);

    virtual std::any visitComparison(RoxalParser::ComparisonContext *context);

    virtual std::any visitTerm(RoxalParser::TermContext *context);

    virtual std::any visitPlusminus(RoxalParser::PlusminusContext *context);

    virtual std::any visitFactor(RoxalParser::FactorContext *context);

    virtual std::any visitMultdiv(RoxalParser::MultdivContext *context);

    virtual std::any visitUnary(RoxalParser::UnaryContext *context);

    virtual std::any visitCall(RoxalParser::CallContext *context);

    virtual std::any visitArgs_or_index_or_accessor(RoxalParser::Args_or_index_or_accessorContext *context);

    virtual std::any visitRanges(RoxalParser::RangesContext *context);

    virtual std::any visitRange(RoxalParser::RangeContext *context);

    virtual std::any visitOptional_expression(RoxalParser::Optional_expressionContext *context);

    virtual std::any visitArguments(RoxalParser::ArgumentsContext *context);

    virtual std::any visitArgument(RoxalParser::ArgumentContext *context);

    virtual std::any visitPrimary(RoxalParser::PrimaryContext *context);

    virtual std::any visitBuiltin_type(RoxalParser::Builtin_typeContext *context);

    virtual std::any visitList(RoxalParser::ListContext *context);

    virtual std::any visitDict(RoxalParser::DictContext *context);

    virtual std::any visitStr(RoxalParser::StrContext *context);

    virtual std::any visitNum(RoxalParser::NumContext *context);

    virtual std::any visitInteger(RoxalParser::IntegerContext *context);


protected:
    void setSourceInfo(ptr<ast::AST> ast, antlr4::ParserRuleContext* context);
    void setSourceInfo(ptr<ast::AST> ast, antlr4::tree::TerminalNode* terminal);

    antlr4::Token* currentToken;
    ptr<std::string> source;
    std::string sourceName;
};



}
