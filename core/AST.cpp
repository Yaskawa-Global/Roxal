#include <deque>

#include "AST.h"

//using namespace roxal;
using namespace roxal::ast;



void AST::output(std::ostream& os, int indent) const 
{ 
    os << spaces(indent) << "AST" << std::endl; 
}

void AST::outputType(std::ostream& os, int indent) const
{
    if (type.has_value()) {
        if (indent > 0)
            os << spaces(indent+1) << "type: " << type.value()->toString() << std::endl;
        else
            os << " →" << type.value()->toString();
    }
}



std::ostream& roxal::ast::operator<<(std::ostream& os, std::shared_ptr<AST> ast)
{ ast->output(os,0); return os; }

std::ostream& roxal::ast::operator<<(std::ostream& os, const AST& ast)
{ ast.output(os,0); return os; }


void File::accept(ASTVisitor& v) 
{
    if (v.visitFirst())
        v.visit(std::dynamic_pointer_cast<File>(shared_from_this()));

    if (v.visitChildren())
        acceptChildren(v);

    if (v.visitLast())
        v.visit(std::dynamic_pointer_cast<File>(shared_from_this()));
}

void File::acceptChildren(ASTVisitor& v)
{
    for(auto& annot : annotations)
        annot->accept(v);

    for(auto& declOrStmt : declsOrStmts ) {

        if (std::holds_alternative<ptr<Declaration>>(declOrStmt))
            std::get<ptr<Declaration>>(declOrStmt)->accept(v);
        else if (std::holds_alternative<ptr<Statement>>(declOrStmt))
            std::get<ptr<Statement>>(declOrStmt)->accept(v);
        else
            throw std::runtime_error("unimplemented accept() alternative");
    }
}



// #define sourceOut() \
//     os << spaces(indent) << "(" << interval.first.line << ":" << interval.first.pos << ".." << interval.second.line << ":" << interval.second.pos << ")" << std::endl;\
// if (source != nullptr) { \
//     os << "\"" << fullSource << "\"" << std::endl;\
// }\
// else \
//     os << spaces(indent) << "no-source" << std::endl;


void File::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"File" << std::endl;

    for(auto& annot : annotations)
        annot->output(os,indent+1);

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
    if (v.visitFirst())
        v.visit(std::dynamic_pointer_cast<SingleInput>(shared_from_this()));

    if (v.visitChildren())
        acceptChildren(v);

    if (v.visitLast())
        v.visit(std::dynamic_pointer_cast<SingleInput>(shared_from_this()));
}

void SingleInput::acceptChildren(ASTVisitor& v)
{
    stmt->accept(v);
}


void SingleInput::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"SingleInput" << std::endl;
    stmt->output(os,indent+1);
}



bool Annotation::namedArgs() const
{
    for(const auto& arg : args)
        if (!arg.first.isEmpty())
            return true;
    return false;
}


void Annotation::accept(ASTVisitor& v)
{
    if (v.visitFirst())
        v.visit(std::dynamic_pointer_cast<Annotation>(shared_from_this()));

    if (v.visitChildren())
        acceptChildren(v);

    if (v.visitLast())
        v.visit(std::dynamic_pointer_cast<Annotation>(shared_from_this()));
}

void Annotation::acceptChildren(ASTVisitor& v)
{
    for(auto& arg : args)
        arg.second->accept(v);
}



void Annotation::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"@" << toUTF8StdString(name) << std::endl;
    for(auto& arg : args) {
        auto argName { arg.first };
        if (!argName.isEmpty())
            os << spaces(indent+2) << toUTF8StdString(argName) << " =" << std::endl;
        arg.second->output(os,indent+3);
    }
}





void Declaration::accept(ASTVisitor& v) 
{
    // downcast and pass-through (Declaration is abstract base)
    switch (declType) {
        case Type: { std::dynamic_pointer_cast<ast::TypeDecl>(shared_from_this())->accept(v); break; }
        case Func: { std::dynamic_pointer_cast<ast::FuncDecl>(shared_from_this())->accept(v); break; }
        case Var: { std::dynamic_pointer_cast<ast::VarDecl>(shared_from_this())->accept(v); break; }
        default: throw std::runtime_error("unimplemented Declaration::accept() alternative");
    }
}




void Statement::accept(ASTVisitor& v) 
{
    // downcast and pass-through (Statement is abstract base)
    switch (stmtType) {
        case Suite: { std::dynamic_pointer_cast<ast::Suite>(shared_from_this())->accept(v); break; }
        case Expression: { std::dynamic_pointer_cast<ast::Expression>(shared_from_this())->accept(v); break; }
        case Return: { std::dynamic_pointer_cast<ast::Literal>(shared_from_this())->accept(v); break; }
        case If: { std::dynamic_pointer_cast<ast::IfStatement>(shared_from_this())->accept(v); break; }
        case While: { std::dynamic_pointer_cast<ast::WhileStatement>(shared_from_this())->accept(v); break; }
        default: throw std::runtime_error("unimplemented Statement::accept() alternative");
    }
}





void Suite::accept(ASTVisitor& v) 
{
    if (v.visitFirst())
        v.visit(std::dynamic_pointer_cast<Suite>(shared_from_this()));

    if (v.visitChildren())
        acceptChildren(v);

    if (v.visitLast())
        v.visit(std::dynamic_pointer_cast<Suite>(shared_from_this()));
}

void Suite::acceptChildren(ASTVisitor& v)
{
    for(auto& declOrStmt :declsOrStmts ) {

        if (std::holds_alternative<ptr<Declaration>>(declOrStmt))
            std::get<ptr<Declaration>>(declOrStmt)->accept(v);
        else if (std::holds_alternative<ptr<Statement>>(declOrStmt))
            std::get<ptr<Statement>>(declOrStmt)->accept(v);
        else
            throw std::runtime_error("unimplemented accept() alternative");
    }
}



void Suite::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"Suite" << std::endl;
    //sourceOut();

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
    if (v.visitFirst())
        v.visit(std::dynamic_pointer_cast<ExpressionStatement>(shared_from_this())); 

    if (v.visitChildren())
        acceptChildren(v);

    if (v.visitLast())
        v.visit(std::dynamic_pointer_cast<ExpressionStatement>(shared_from_this())); 
}

void ExpressionStatement::acceptChildren(ASTVisitor& v)
{
    expr->accept(v);
}



void ExpressionStatement::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"ExprStmt" << std::endl;
    //sourceOut();
    expr->output(os,indent+1);
}



void ReturnStatement::accept(ASTVisitor& v) 
{ 
    if (v.visitFirst())
        v.visit(std::dynamic_pointer_cast<ReturnStatement>(shared_from_this())); 

    if (v.visitChildren())
        acceptChildren(v);

    if (v.visitLast())
        v.visit(std::dynamic_pointer_cast<ReturnStatement>(shared_from_this())); 
}

void ReturnStatement::acceptChildren(ASTVisitor& v)
{
    if (expr.has_value())
        expr.value()->accept(v);
}


void ReturnStatement::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"Return" << std::endl;
    //sourceOut();
    if (expr.has_value())
        expr.value()->output(os,indent+1);
}




void IfStatement::accept(ASTVisitor& v) 
{
    if (v.visitFirst())
        v.visit(std::dynamic_pointer_cast<IfStatement>(shared_from_this())); 

    if (v.visitChildren())
        acceptChildren(v);

    if (v.visitLast())
        v.visit(std::dynamic_pointer_cast<IfStatement>(shared_from_this())); 
}

void IfStatement::acceptChildren(ASTVisitor& v)
{
    for(auto& condSuite :conditionalSuites ) {
        condSuite.first->accept(v);
        condSuite.second->accept(v);
    }
    if (elseSuite.has_value())
        elseSuite.value()->accept(v);
}



void IfStatement::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"If" << std::endl;
    //sourceOut();
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
    if (v.visitFirst())
        v.visit(std::dynamic_pointer_cast<WhileStatement>(shared_from_this())); 

    if (v.visitChildren())
        acceptChildren(v);

    if (v.visitLast())
        v.visit(std::dynamic_pointer_cast<WhileStatement>(shared_from_this())); 
}

void WhileStatement::acceptChildren(ASTVisitor& v)
{
    condition->accept(v);
    body->accept(v);
}



void WhileStatement::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"While" << std::endl;
    //sourceOut();
    os << spaces(indent)+" cond:" << std::endl;
    condition->output(os,indent+2);
    os << spaces(indent)+" body:" << std::endl;
    body->output(os,indent+2);
}




void VarDecl::accept(ASTVisitor& v) 
{
    if (v.visitFirst())
        v.visit(std::dynamic_pointer_cast<VarDecl>(shared_from_this())); 

    if (v.visitChildren()) 
        acceptChildren(v);

    if (v.visitLast())
        v.visit(std::dynamic_pointer_cast<VarDecl>(shared_from_this())); 
}

void VarDecl::acceptChildren(ASTVisitor& v)
{
    if (initializer.has_value())
        initializer.value()->accept(v);
}


void VarDecl::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"VarDecl " << toUTF8StdString(name) << std::endl;

    for(auto& annot : annotations)
        annot->output(os,indent+1);

    if (initializer.has_value())
        initializer.value()->output(os,indent+1);
}


void FuncDecl::accept(ASTVisitor& v)
{
    if (v.visitFirst())
        v.visit(std::dynamic_pointer_cast<FuncDecl>(shared_from_this())); 

    if (v.visitChildren())
        acceptChildren(v);

    if (v.visitLast())
        v.visit(std::dynamic_pointer_cast<FuncDecl>(shared_from_this())); 
}

void FuncDecl::acceptChildren(ASTVisitor& v)
{
    func->accept(v);
}


void FuncDecl::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"FuncDecl" << std::endl;
    outputType(os,indent);
    for(auto& annot : annotations)
        annot->output(os,indent+1);
    func->output(os,indent+1);
}



void Function::accept(ASTVisitor& v)
{
    if (v.visitFirst())
        v.visit(std::dynamic_pointer_cast<Function>(shared_from_this())); 

    if (v.visitChildren())
        acceptChildren(v);

    if (v.visitLast())
        v.visit(std::dynamic_pointer_cast<Function>(shared_from_this())); 
}

void Function::acceptChildren(ASTVisitor& v)
{
    for(auto& param : params)
        param->accept(v);
    body->accept(v);
}


void Function::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"Function" << (isProc? " (proc)" : "") << " " << toUTF8StdString(name);
    if (returnType.has_value()) {
        os << " → ";
        if (std::holds_alternative<BuiltinType>(returnType.value()))
            os << to_string(std::get<BuiltinType>(returnType.value()));
        else if (std::holds_alternative<icu::UnicodeString>(returnType.value()))
            os << toUTF8StdString(std::get<icu::UnicodeString>(returnType.value()));
    }
    os << std::endl;
    for(auto& annot : annotations)
        annot->output(os,indent+1);
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
    if (v.visitFirst())
        v.visit(std::dynamic_pointer_cast<Parameter>(shared_from_this())); 

    if (v.visitChildren())
        acceptChildren(v);

    if (v.visitLast())
        v.visit(std::dynamic_pointer_cast<Parameter>(shared_from_this())); 
}

void Parameter::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"Parameter " << toUTF8StdString(name);
    //sourceOut();
    if (type.has_value()) {
        os << " : ";
        if (std::holds_alternative<BuiltinType>(type.value()))
            os << to_string(std::get<BuiltinType>(type.value()));
        else if (std::holds_alternative<icu::UnicodeString>(type.value()))
            os << toUTF8StdString(std::get<icu::UnicodeString>(type.value()));
    }
    os << std::endl;
    if (defaultValue.has_value()) {
        os << spaces(indent) << " = " << std::endl;
        defaultValue.value()->output(os,indent+2);
    }
    for(auto& annot : annotations)
        annot->output(os,indent+1);
}


void Parameter::acceptChildren(ASTVisitor& v)
{
    if (defaultValue.has_value())
        defaultValue.value()->accept(v);
}



void TypeDecl::accept(ASTVisitor& v)
{
    if (v.visitFirst())
        v.visit(std::dynamic_pointer_cast<TypeDecl>(shared_from_this())); 

    if (v.visitChildren())
        acceptChildren(v);

    if (v.visitLast())
        v.visit(std::dynamic_pointer_cast<TypeDecl>(shared_from_this())); 
}

void TypeDecl::acceptChildren(ASTVisitor& v)
{
    for(auto& method : methods)
        method->accept(v);
}



void TypeDecl::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"TypeDecl " 
       << (kind==Object ? "object" : (kind==Actor?"actor":"?")) << " " << toUTF8StdString(name)
       << (extends.has_value() ? " "+toUTF8StdString(extends.value()) :"")
       << std::endl;
    if (!implements.empty()) {
        os << spaces(indent)
           << " implements " << toUTF8StdString(implements.at(0));
        for(int i=1; i<implements.size();i++)
            os << ", " << toUTF8StdString(implements.at(i));
    }
    for(auto& annot : annotations)
        annot->output(os, indent+1);
    for(auto& method : methods)
        method->output(os, indent+1);
}


void Expression::accept(ASTVisitor& v)
{
    // downcast and pass-through (Expression is abstract base)
    switch (exprType) {
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
: Expression(ExprType::BinaryOp), op(_op) 
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
        case FollowedBy: return "\u21A6";
        default: return "?";
    }
}


void BinaryOp::accept(ASTVisitor& v)
{
    if (v.visitFirst())
        v.visit(std::dynamic_pointer_cast<BinaryOp>(shared_from_this()));

    if (v.visitChildren())
        acceptChildren(v);

    if (v.visitLast())
        v.visit(std::dynamic_pointer_cast<BinaryOp>(shared_from_this()));
}

void BinaryOp::acceptChildren(ASTVisitor& v)
{
    lhs->accept(v);
    rhs->accept(v);
}


void BinaryOp::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"BinaryOp " << opString() << " ";
    outputType(os,-1);
    os << std::endl;
    //sourceOut();
    lhs->output(os,indent+1);
    rhs->output(os,indent+1);
}




UnaryOp::UnaryOp(Op _op) 
: Expression(ExprType::UnaryOp), op(_op) 
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
    if (v.visitFirst())
        v.visit(std::dynamic_pointer_cast<UnaryOp>(shared_from_this()));

    if (v.visitChildren())
        acceptChildren(v);

    if (v.visitLast())
        v.visit(std::dynamic_pointer_cast<UnaryOp>(shared_from_this()));
}

void UnaryOp::acceptChildren(ASTVisitor& v)
{
    arg->accept(v);
}


void UnaryOp::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"UnaryOp " << opString() 
       << ((op==Accessor) ? (member.has_value() ? toUTF8StdString(member.value()) : "?") : "")
       << " ";
    outputType(os,-1);
    os << std::endl;
    //sourceOut();
    arg->output(os,indent+1);
}



void Assignment::accept(ASTVisitor& v)
{
    if (v.visitFirst())
        v.visit(std::dynamic_pointer_cast<Assignment>(shared_from_this()));

    if (v.visitChildren())
        acceptChildren(v);

    if (v.visitLast())
        v.visit(std::dynamic_pointer_cast<Assignment>(shared_from_this()));
}

void Assignment::acceptChildren(ASTVisitor& v)
{
    lhs->accept(v);
    rhs->accept(v);
}


void Assignment::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"Assignment" << std::endl;
    //sourceOut();
    lhs->output(os,indent+1);
    rhs->output(os,indent+1);
}


void Variable::accept(ASTVisitor& v)
{
    if (v.visitFirst() || v.visitLast())
        v.visit(std::dynamic_pointer_cast<Variable>(shared_from_this()));
}

void Variable::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"Variable " << toUTF8StdString(name) << std::endl;
    //sourceOut();
}




bool Call::namedArgs() const
{
    for(const auto& arg : args)
        if (!arg.first.isEmpty())
            return true;
    return false;
}


void Call::accept(ASTVisitor& v)
{
    if (v.visitFirst())
        v.visit(std::dynamic_pointer_cast<Call>(shared_from_this()));

    if (v.visitChildren())
        acceptChildren(v);

    if (v.visitLast())
        v.visit(std::dynamic_pointer_cast<Call>(shared_from_this()));
}

void Call::acceptChildren(ASTVisitor& v)
{
    callable->accept(v);
    for(auto& arg : args)
        arg.second->accept(v);
}


void Call::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"Call" << std::endl;
    //sourceOut();
    callable->output(os,indent+1);
    os << spaces(indent)+" args:" << std::endl;
    for(auto& arg : args) {
        auto argName { arg.first };
        if (!argName.isEmpty())
            os << spaces(indent+2) << toUTF8StdString(argName) << " =" << std::endl;
        arg.second->output(os,indent+2);
    }
}



void Index::accept(ASTVisitor& v)
{
    if (v.visitFirst())
        v.visit(std::dynamic_pointer_cast<Index>(shared_from_this()));

    if (v.visitChildren())
        acceptChildren(v);

    if (v.visitLast())
        v.visit(std::dynamic_pointer_cast<Index>(shared_from_this()));
}

void Index::acceptChildren(ASTVisitor& v)
{
    indexable->accept(v);
    for(auto& arg : args)
        arg->accept(v);
}


void Index::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"[]" << std::endl;
    indexable->output(os,indent+1);
    os << spaces(indent)+" indices:" << std::endl;
    for(auto& arg : args)
        arg->output(os,indent+2);
}





void Literal::accept(ASTVisitor& v)
{
    switch (literalType) {
        case Nil: { v.visit(std::dynamic_pointer_cast<Literal>(shared_from_this())); break; }
        case Bool: { std::dynamic_pointer_cast<ast::Bool>(shared_from_this())->accept(v); break; }
        case Num: { std::dynamic_pointer_cast<ast::Num>(shared_from_this())->accept(v); break; }
        case Str: { std::dynamic_pointer_cast<ast::Str>(shared_from_this())->accept(v); break; }
        default: throw std::runtime_error("unimplemented Literal::accept() alternative");
    } 
}

void Literal::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"Literal " << (literalType==Nil ? std::string("nil") : std::to_string(int(literalType))) << std::endl;
    //sourceOut();
}


void Bool::accept(ASTVisitor& v)
{
    if (v.visitFirst() || v.visitLast())
        v.visit(std::dynamic_pointer_cast<Bool>(shared_from_this()));
}

void Bool::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"Bool " << (value ? "true":"false") << std::endl;
    //sourceOut();
}



void Num::accept(ASTVisitor& v)
{
    if (v.visitFirst() || v.visitLast())
        v.visit(std::dynamic_pointer_cast<Num>(shared_from_this()));
}

void Num::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"Num ";
    if (std::holds_alternative<int32_t>(num)) 
        os << std::to_string(std::get<int32_t>(num)) << ":int";
    else if (std::holds_alternative<double>(num)) 
        os << std::to_string(std::get<double>(num)) << ":real";
    else
        os << "?";
    os << std::endl;
    //sourceOut();
}



void Str::accept(ASTVisitor& v)
{
    if (v.visitFirst() || v.visitLast())
        v.visit(std::dynamic_pointer_cast<Str>(shared_from_this()));
}

void Str::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"Str \"" << toUTF8StdString(str) << "\"" << std::endl;
}


void Type::accept(ASTVisitor& v)
{
    if (v.visitFirst() || v.visitLast())
        v.visit(std::dynamic_pointer_cast<Type>(shared_from_this()));
}

void Type::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"Type " << to_string(t) << std::endl;
}



void List::accept(ASTVisitor& v)
{
    if (v.visitFirst())
        v.visit(std::dynamic_pointer_cast<List>(shared_from_this()));

    if (v.visitChildren())
        acceptChildren(v);

    if (v.visitLast())
        v.visit(std::dynamic_pointer_cast<List>(shared_from_this()));
}

void List::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"List" << std::endl;
    for(auto& element : elements)
        element->output(os,indent+2);
}

void List::acceptChildren(ASTVisitor& v)
{
    for(auto& element : elements)
        element->accept(v);
}




void Dict::accept(ASTVisitor& v)
{
    if (v.visitFirst())
        v.visit(std::dynamic_pointer_cast<Dict>(shared_from_this()));

    if (v.visitChildren())
        acceptChildren(v);

    if (v.visitLast())
        v.visit(std::dynamic_pointer_cast<Dict>(shared_from_this()));
}

void Dict::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"Dict" << std::endl;
    for(auto& entry : entries) {
        os << spaces(indent+1) << "key:" << std::endl;
        entry.first->output(os,indent+2);
        os << spaces(indent+1) << "value:" << std::endl;
        entry.second->output(os,indent+2);
    }
}

void Dict::acceptChildren(ASTVisitor& v)
{
    for(auto& entry : entries) {
        entry.first->accept(v);
        entry.second->accept(v);
    }
}


