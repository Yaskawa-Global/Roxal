#include <deque>

#include "AST.h"

//using namespace roxal;
using namespace roxal::ast;



std::string roxal::ast::to_string(BuiltinType t)
{
    switch (t) {
        case BuiltinType::Nil: return "nil";
        case BuiltinType::Bool : return "bool";
        case BuiltinType::Byte : return "byte";
        case BuiltinType::Number : return "number";
        case BuiltinType::Int : return "int";
        case BuiltinType::Real : return "real";
        case BuiltinType::Decimal : return "decimal";
        case BuiltinType::Char : return "char";
        case BuiltinType::String : return "string";
        case BuiltinType::List : return "list";
        case BuiltinType::Dict : return "dict";
        case BuiltinType::Vector : return "vector";
        case BuiltinType::Matrix : return "matrix";
        case BuiltinType::Tensor : return "tensor";
        case BuiltinType::Orient : return "stream";
        case BuiltinType::Stream : return "orient";
        default: throw std::runtime_error("to_string(BuiltinType) unhandled alternative");
    }
}


void AST::output(std::ostream& os, int indent) const 
{ 
    os << spaces(indent) << "AST" << std::endl; 
}

std::ostream& roxal::ast::operator<<(std::ostream& os, std::shared_ptr<AST> ast)
{ ast->output(os,0); return os; }

std::ostream& roxal::ast::operator<<(std::ostream& os, const AST& ast)
{ ast.output(os,0); return os; }




void File::accept(ASTVisitor& v) 
{
    for(auto& declOrStmt : declsOrStmts ) {

        if (std::holds_alternative<ptr<Declaration>>(declOrStmt))
            std::get<ptr<Declaration>>(declOrStmt)->accept(v);
        else if (std::holds_alternative<ptr<Statement>>(declOrStmt))
            std::get<ptr<Statement>>(declOrStmt)->accept(v);
        else
            throw std::runtime_error("unimplemented accept() alternative");
    }
    v.visit(std::dynamic_pointer_cast<File>(shared_from_this()));
}


void File::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"File" << std::endl;
    for(auto& declOrStmt : declsOrStmts ) 
        if (std::holds_alternative<ptr<Declaration>>(declOrStmt))
            std::get<ptr<Declaration>>(declOrStmt)->output(os,indent+1);
        else if (std::holds_alternative<ptr<Statement>>(declOrStmt))
            std::get<ptr<Statement>>(declOrStmt)->output(os,indent+1);
        else
            throw std::runtime_error("unimplemented output() alternative");
}


void SingleInput::accept(ASTVisitor& v) 
{ 
    stmt->accept(v);
    v.visit(std::dynamic_pointer_cast<SingleInput>(shared_from_this()));
}

void SingleInput::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"SingleInput" << std::endl;
    stmt->output(os,indent+1);
}



void Declaration::accept(ASTVisitor& v) 
{
    // downcast and pass-through (Declaration is abstract base)
    switch (type) {
        case Type: { std::dynamic_pointer_cast<ast::TypeDecl>(shared_from_this())->accept(v); break; }
        case Func: { std::dynamic_pointer_cast<ast::FuncDecl>(shared_from_this())->accept(v); break; }
        case Var: { std::dynamic_pointer_cast<ast::VarDecl>(shared_from_this())->accept(v); break; }
        default: throw std::runtime_error("unimplemented Declaration::accept() alternative");
    }
}




void Statement::accept(ASTVisitor& v) 
{
    // downcast and pass-through (Statement is abstract base)
    switch (type) {
        case Suite: { std::dynamic_pointer_cast<ast::Suite>(shared_from_this())->accept(v); break; }
        case Expression: { std::dynamic_pointer_cast<ast::Expression>(shared_from_this())->accept(v); break; }
        case Print: { std::dynamic_pointer_cast<ast::PrintStatement>(shared_from_this())->accept(v); break; }
        case Return: { std::dynamic_pointer_cast<ast::Literal>(shared_from_this())->accept(v); break; }
        case If: { std::dynamic_pointer_cast<ast::IfStatement>(shared_from_this())->accept(v); break; }
        case While: { std::dynamic_pointer_cast<ast::WhileStatement>(shared_from_this())->accept(v); break; }
        default: throw std::runtime_error("unimplemented Statement::accept() alternative");
    }
}





void Suite::accept(ASTVisitor& v) 
{
    for(auto& declOrStmt :declsOrStmts ) {

        if (std::holds_alternative<ptr<Declaration>>(declOrStmt))
            std::get<ptr<Declaration>>(declOrStmt)->accept(v);
        else if (std::holds_alternative<ptr<Statement>>(declOrStmt))
            std::get<ptr<Statement>>(declOrStmt)->accept(v);
        else
            throw std::runtime_error("unimplemented accept() alternative");
    }
    v.visit(std::dynamic_pointer_cast<Suite>(shared_from_this()));
}


void Suite::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"Suite" << std::endl;

    for(auto& declOrStmt :declsOrStmts ) {

        if (std::holds_alternative<ptr<Declaration>>(declOrStmt))
            std::get<ptr<Declaration>>(declOrStmt)->output(os,indent+1);
        else if (std::holds_alternative<ptr<Statement>>(declOrStmt))
            std::get<ptr<Statement>>(declOrStmt)->output(os,indent+1);
        else
            throw std::runtime_error("unimplemented output() alternative");
    }
}



void ExpressionStatement::accept(ASTVisitor& v)
{
    expr->accept(v);
    v.visit(std::dynamic_pointer_cast<ExpressionStatement>(shared_from_this())); 
}


void ExpressionStatement::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"ExprStmt" << std::endl;
    expr->output(os,indent+1);
}



void PrintStatement::accept(ASTVisitor& v) 
{ 
    expr->accept(v);
    v.visit(std::dynamic_pointer_cast<PrintStatement>(shared_from_this())); 
}


void PrintStatement::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"Print" << std::endl;
    expr->output(os,indent+1);
}


void ReturnStatement::accept(ASTVisitor& v) 
{ 
    if (expr.has_value())
        expr.value()->accept(v);
    v.visit(std::dynamic_pointer_cast<ReturnStatement>(shared_from_this())); 
}

void ReturnStatement::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"Return" << std::endl;
    if (expr.has_value())
        expr.value()->output(os,indent+1);
}




void IfStatement::accept(ASTVisitor& v) 
{
    for(auto& condSuite :conditionalSuites ) {
        condSuite.first->accept(v);
        condSuite.second->accept(v);
    }
    if (elseSuite.has_value())
        elseSuite.value()->accept(v);
    v.visit(std::dynamic_pointer_cast<IfStatement>(shared_from_this())); 
}


void IfStatement::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"If" << std::endl;
    for(auto& condSuite :conditionalSuites ) {
        os << spaces(indent+1) << "if cond:" << std::endl;
        condSuite.first->output(os,indent+2);
        os << spaces(indent+1) << "body:" << std::endl;
        condSuite.second->output(os,indent+2);
    }
    if (elseSuite.has_value()) {
        os << spaces(indent+1) << "else:" << std::endl;
        elseSuite.value()->output(os,indent+2);
    }
}



void WhileStatement::accept(ASTVisitor& v)
{
    condition->accept(v);
    body->accept(v);
    v.visit(std::dynamic_pointer_cast<WhileStatement>(shared_from_this())); 
}


void WhileStatement::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"While" << std::endl;
    os << spaces(indent)+" cond:" << std::endl;
    condition->output(os,indent+2);
    os << spaces(indent)+" body:" << std::endl;
    body->output(os,indent+2);
}




void VarDecl::accept(ASTVisitor& v) 
{
    if (initializer.has_value())
        initializer.value()->accept(v);
    v.visit(std::dynamic_pointer_cast<VarDecl>(shared_from_this())); 
}


void VarDecl::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"VarDecl " << toUTF8StdString(name) << std::endl;
    if (initializer.has_value())
        initializer.value()->output(os,indent+1);
}


void FuncDecl::accept(ASTVisitor& v)
{
    func->accept(v);
    v.visit(std::dynamic_pointer_cast<FuncDecl>(shared_from_this())); 
}

void FuncDecl::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"FuncDecl" << std::endl;
    func->output(os,indent+1);
}



void Function::accept(ASTVisitor& v)
{
    for(auto& param : params)
        param->accept(v);
    body->accept(v);
    v.visit(std::dynamic_pointer_cast<Function>(shared_from_this())); 
}

void Function::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"Function " << (isProc? "(proc)" : "") << std::endl;
    if (params.size()>0) {
        os << spaces(indent)+" params:" << std::endl;
        for(auto& param : params)
            param->output(os,indent+2);
    }
    os << spaces(indent)+" body:" << std::endl;
    body->output(os,indent+2);
}



void Parameter::accept(ASTVisitor& v)
{
    v.visit(std::dynamic_pointer_cast<Parameter>(shared_from_this())); 
}

void Parameter::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"Parameter " << toUTF8StdString(name);
    if (type.has_value()) {
        os << " : ";
        if (std::holds_alternative<BuiltinType>(type.value()))
            os << int(std::get<BuiltinType>(type.value()));
        else if (std::holds_alternative<icu::UnicodeString>(type.value()))
            os << toUTF8StdString(std::get<icu::UnicodeString>(type.value()));
    }
    os << std::endl;
}



void TypeDecl::accept(ASTVisitor& v)
{
    for(auto& method : methods)
        method->accept(v);
    v.visit(std::dynamic_pointer_cast<TypeDecl>(shared_from_this())); 
}


void TypeDecl::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"TypeDecl (unimplemented)" << std::endl;
}


void Expression::accept(ASTVisitor& v)
{
    // downcast and pass-through (Expression is abstract base)
    switch (type) {
        case Assignment: { std::dynamic_pointer_cast<ast::Assignment>(shared_from_this())->accept(v); break; }
        case BinaryOp: { std::dynamic_pointer_cast<ast::BinaryOp>(shared_from_this())->accept(v); break; }
        case UnaryOp: { std::dynamic_pointer_cast<ast::UnaryOp>(shared_from_this())->accept(v); break; }
        case Literal: { std::dynamic_pointer_cast<ast::Literal>(shared_from_this())->accept(v); break; }
        case Variable: { std::dynamic_pointer_cast<ast::Variable>(shared_from_this())->accept(v); break; }
        case Call: { std::dynamic_pointer_cast<ast::Call>(shared_from_this())->accept(v); break; }
        default: throw std::runtime_error("unimplemented Expression::accept() alternative");
    }
}


BinaryOp::BinaryOp(Op _op) 
: op(_op) 
{
    if (op == None)
         throw std::runtime_error("Invalid null/none operator");
}


std::string BinaryOp::opString() const
{
    switch (op) {
        case None: return "";
        case Add: return "+";
        case Subtract: return "-";
        case Multiply: return "\u00D7";
        case Divide: return "/";
        case Modulo: return "%";
        case And: return "and";
        case Or: return "or";
        case Equal: return "\u225F";
        case NotEqual: return "\u2260";
        case LessThan: return "<";
        case GreaterThan: return ">";
        case LessOrEqual: return "\u2A7D";
        case GreaterOrEqual: return "\u2A7E";
        default: return "";
    }
}


void BinaryOp::accept(ASTVisitor& v)
{
    lhs->accept(v);
    rhs->accept(v);
    v.visit(std::dynamic_pointer_cast<BinaryOp>(shared_from_this()));
}

void BinaryOp::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"BinaryOp " << opString() << std::endl;
    lhs->output(os,indent+1);
    rhs->output(os,indent+1);
}




UnaryOp::UnaryOp(Op _op) 
: op(_op) 
{
    if (op == None)
         throw std::runtime_error("Invalid null/none operator");
}

std::string UnaryOp::opString() const
{
    switch (op) {
        case None: return "";
        case Negate: return "-";
        case Not: return "not";
        case Accessor: return ".";
        default: return "";
    }
}



void UnaryOp::accept(ASTVisitor& v)
{
    arg->accept(v);
    v.visit(std::dynamic_pointer_cast<UnaryOp>(shared_from_this()));
}

void UnaryOp::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"UnaryOp " << opString() << std::endl;
    arg->output(os,indent+1);
}



void Assignment::accept(ASTVisitor& v)
{
    lhs->accept(v);
    rhs->accept(v);
    v.visit(std::dynamic_pointer_cast<Assignment>(shared_from_this()));
}

void Assignment::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"Assignment" << std::endl;
    lhs->output(os,indent+1);
    rhs->output(os,indent+1);
}


void Variable::accept(ASTVisitor& v)
{
    v.visit(std::dynamic_pointer_cast<Variable>(shared_from_this()));
}

void Variable::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"Variable " << toUTF8StdString(name) << std::endl;
}




void Call::accept(ASTVisitor& v)
{
    callable->accept(v);
    for(auto& arg : args)
        arg->accept(v);
    v.visit(std::dynamic_pointer_cast<Call>(shared_from_this()));
}

void Call::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"Call" << std::endl;
    callable->output(os,indent+1);
    os << spaces(indent)+" args:" << std::endl;
    for(auto& arg : args)
        arg->output(os,indent+2);
}




void Literal::accept(ASTVisitor& v)
{
    switch (type) {
        case Nil: { v.visit(std::dynamic_pointer_cast<Literal>(shared_from_this())); break; }
        case Bool: { std::dynamic_pointer_cast<ast::Bool>(shared_from_this())->accept(v); break; }
        case Num: { std::dynamic_pointer_cast<ast::Num>(shared_from_this())->accept(v); break; }
        case Str: { std::dynamic_pointer_cast<ast::Str>(shared_from_this())->accept(v); break; }
        default: throw std::runtime_error("unimplemented Literal::accept() alternative");
    } 
}

void Literal::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"Literal " << (type==Nil ? std::string("nil") : std::to_string(int(type))) << std::endl;    
}


void Bool::accept(ASTVisitor& v)
{
    v.visit(std::dynamic_pointer_cast<Bool>(shared_from_this()));
}

void Bool::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"Bool " << (value ? "true":"false") << std::endl;
}



void Num::accept(ASTVisitor& v)
{
    v.visit(std::dynamic_pointer_cast<Num>(shared_from_this()));
}

void Num::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"Num ";
    if (std::holds_alternative<int32_t>(num)) 
        os << "int:" << std::to_string(std::get<int32_t>(num));
    else if (std::holds_alternative<double>(num)) 
        os << "real:" << std::to_string(std::get<double>(num));
    else
        os << "?";
    os << std::endl;
}



void Str::accept(ASTVisitor& v)
{
    v.visit(std::dynamic_pointer_cast<Str>(shared_from_this()));
}

void Str::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"Str \"" << toUTF8StdString(str) << "\"" << std::endl;
}
