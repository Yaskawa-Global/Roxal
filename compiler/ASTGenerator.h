#pragma once

#include <core/common.h>
#include <core/AST.h>
#include "RoxalVisitor.h"

namespace antlr4 {
class Token;
}

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
    virtual std::any visitWhen_stmt(RoxalParser::When_stmtContext *context);
    virtual std::any visitEmit_stmt(RoxalParser::Emit_stmtContext *context);
    virtual std::any visitTry_stmt(RoxalParser::Try_stmtContext *context);
    virtual std::any visitMatch_stmt(RoxalParser::Match_stmtContext *context);
    virtual std::any visitMatch_case(RoxalParser::Match_caseContext *context);
    virtual std::any visitCase_pattern(RoxalParser::Case_patternContext *context);
    virtual std::any visitDefault_case(RoxalParser::Default_caseContext *context);
    virtual std::any visitWith_stmt(RoxalParser::With_stmtContext *context);
    virtual std::any visitRaise_stmt(RoxalParser::Raise_stmtContext *context);
    virtual std::any visitExcept_clause(RoxalParser::Except_clauseContext *context);
    virtual std::any visitFinally_clause(RoxalParser::Finally_clauseContext *context);

    virtual std::any visitUntil_clause(RoxalParser::Until_clauseContext *context);

    virtual std::any visitVar_decl(RoxalParser::Var_declContext *context);

    virtual std::any visitIdent_opt_type(RoxalParser::Ident_opt_typeContext *context);

    virtual std::any visitFunc_decl(RoxalParser::Func_declContext *context);

    virtual std::any visitFunction(RoxalParser::FunctionContext *context);

    virtual std::any visitFunc_sig(RoxalParser::Func_sigContext *context);

    virtual std::any visitOperator_name(RoxalParser::Operator_nameContext *context) override { return {}; }
    virtual std::any visitOperator_symbol(RoxalParser::Operator_symbolContext *context) override { return {}; }
    virtual std::any visitConversion_target(RoxalParser::Conversion_targetContext *context) override { return {}; }

    virtual std::any visitParameters(RoxalParser::ParametersContext *context);

    virtual std::any visitParameter(RoxalParser::ParameterContext *context);

    virtual std::any visitSuite(RoxalParser::SuiteContext *context);

    virtual std::any visitType_decl(RoxalParser::Type_declContext *context);
    virtual std::any visitObject_type_decl(RoxalParser::Object_type_declContext *context);
    virtual std::any visitEnum_type_decl(RoxalParser::Enum_type_declContext *context);
    virtual std::any visitEvent_type_decl(RoxalParser::Event_type_declContext *context);

    virtual std::any visitMethod(RoxalParser::MethodContext *context);

    virtual std::any visitMember_var(RoxalParser::Member_varContext *context);

    virtual std::any visitProperty_getter(RoxalParser::Property_getterContext *context);
    virtual std::any visitProperty_setter(RoxalParser::Property_setterContext *context);

    virtual std::any visitEnum_label(RoxalParser::Enum_labelContext *context);

    virtual std::any visitAnnotation(RoxalParser::AnnotationContext *context);

    virtual std::any visitAnnot_argument(RoxalParser::Annot_argumentContext *context);

    virtual std::any visitLambda_func(RoxalParser::Lambda_funcContext *context);

    virtual std::any visitLambda_proc(RoxalParser::Lambda_procContext *context);

    virtual std::any visitExpression(RoxalParser::ExpressionContext *context);

    virtual std::any visitAssignment(RoxalParser::AssignmentContext *context);


    virtual std::any visitLogic_or(RoxalParser::Logic_orContext *context);

    virtual std::any visitLogic_and(RoxalParser::Logic_andContext *context);

    virtual std::any visitBitwise_or(RoxalParser::Bitwise_orContext *context);
    virtual std::any visitBitwise_xor(RoxalParser::Bitwise_xorContext *context);
    virtual std::any visitBitwise_and(RoxalParser::Bitwise_andContext *context);

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

    virtual std::any visitIdentifier_word(RoxalParser::Identifier_wordContext *context);

    virtual std::any visitPrimary(RoxalParser::PrimaryContext *context);

    virtual std::any visitReturn_type(RoxalParser::Return_typeContext *context);
    virtual std::any visitType_spec(RoxalParser::Type_specContext *context);
    virtual std::any visitBuiltin_type(RoxalParser::Builtin_typeContext *context);
    virtual std::any visitConst_qualifier(RoxalParser::Const_qualifierContext *context);

    virtual std::any visitList(RoxalParser::ListContext *context);

    virtual std::any visitVector(RoxalParser::VectorContext *context);

    virtual std::any visitMatrix(RoxalParser::MatrixContext *context);

    virtual std::any visitRow(RoxalParser::RowContext *context);

    virtual std::any visitSigned_num(RoxalParser::Signed_numContext *context);

    virtual std::any visitDict(RoxalParser::DictContext *context);

    virtual std::any visitStr(RoxalParser::StrContext *context);

    virtual std::any visitNum(RoxalParser::NumContext *context);

    virtual std::any visitInteger(RoxalParser::IntegerContext *context);


protected:
    void setSourceInfo(ptr<ast::AST> ast, antlr4::ParserRuleContext* context);
    void setSourceInfo(ptr<ast::AST> ast, antlr4::tree::TerminalNode* terminal);
    ptr<ast::Expression> parseInterpolationExpression(const std::string& text, antlr4::ParserRuleContext* context);
    icu::UnicodeString normalizeIdentifier(const std::string& text);
    icu::UnicodeString identifierFromTerminal(antlr4::tree::TerminalNode* terminal);
    icu::UnicodeString identifierFromContext(antlr4::ParserRuleContext* context);
    icu::UnicodeString operatorNameFromContext(RoxalParser::Operator_nameContext* context);

    antlr4::Token* currentToken;
    ptr<std::string> source;
    std::string sourceName;

private:
    void reportError(antlr4::Token* token, const std::string& message);
    bool hadError { false };
};



}
