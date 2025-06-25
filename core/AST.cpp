#include <deque>
#include <assert.h>

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
            os << " → " << type.value()->toString();
    }
}



std::ostream& roxal::ast::operator<<(std::ostream& os, std::shared_ptr<AST> ast)
{ ast->output(os,0); return os; }

std::ostream& roxal::ast::operator<<(std::ostream& os, const AST& ast)
{ ast.output(os,0); return os; }


std::any File::accept(ASTVisitor& v)
{
    std::vector<std::any> results {};
    if (v.visitFirst())
        results.push_back( v.visit(std::dynamic_pointer_cast<File>(shared_from_this())) );

    if (v.visitChildren())
        acceptChildren(v, results);

    if (v.visitLast())
        results.push_back( v.visit(std::dynamic_pointer_cast<File>(shared_from_this())) );

    return results;
}

void File::acceptChildren(ASTVisitor& v, Anys& results)
{
    for(auto& annot : annotations)
        results.push_back( annot->accept(v) );

    for(auto& import : imports)
        results.push_back( import->accept(v) );


    for(auto& declOrStmt : declsOrStmts ) {

        if (std::holds_alternative<ptr<Declaration>>(declOrStmt))
            results.push_back( std::get<ptr<Declaration>>(declOrStmt)->accept(v) );
        else if (std::holds_alternative<ptr<Statement>>(declOrStmt))
            results.push_back( std::get<ptr<Statement>>(declOrStmt)->accept(v) );
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

    for(auto& import : imports)
        import->output(os,indent+1);

    for(auto& declOrStmt : declsOrStmts )
        if (std::holds_alternative<ptr<Declaration>>(declOrStmt))
            std::get<ptr<Declaration>>(declOrStmt)->output(os,indent+1);
        else if (std::holds_alternative<ptr<Statement>>(declOrStmt))
            std::get<ptr<Statement>>(declOrStmt)->output(os,indent+1);
        else
            throw std::runtime_error("unimplemented output() alternative");
}


std::any SingleInput::accept(ASTVisitor& v)
{
    Anys results {};

    if (v.visitFirst())
        results.push_back( v.visit(std::dynamic_pointer_cast<SingleInput>(shared_from_this())) );

    if (v.visitChildren())
        acceptChildren(v, results);

    if (v.visitLast())
        results.push_back( v.visit(std::dynamic_pointer_cast<SingleInput>(shared_from_this())) );

    return results;
}

void SingleInput::acceptChildren(ASTVisitor& v, Anys& results)
{
    results.push_back(stmt->accept(v));
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


std::any Annotation::accept(ASTVisitor& v)
{
    Anys results {};

    if (v.visitFirst())
        results.push_back( v.visit(std::dynamic_pointer_cast<Annotation>(shared_from_this())) );

    if (v.visitChildren())
        acceptChildren(v, results);

    if (v.visitLast())
        results.push_back( v.visit(std::dynamic_pointer_cast<Annotation>(shared_from_this())) );

    return results;
}

void Annotation::acceptChildren(ASTVisitor& v, Anys& results)
{
    for(auto& arg : args)
        results.push_back( arg.second->accept(v) );
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



std::any Import::accept(ASTVisitor& v)
{
    Anys results {};

    if (v.visitFirst())
        results.push_back( v.visit(std::dynamic_pointer_cast<Import>(shared_from_this())) );

    if (v.visitChildren())
        acceptChildren(v, results);

    if (v.visitLast())
        results.push_back( v.visit(std::dynamic_pointer_cast<Import>(shared_from_this())) );

    return results;
}

void Import::output(std::ostream& os, int indent) const
{
    if (packages.size() > 0) {
        os << spaces(indent)+"import ";
        for(auto i=0; i < packages.size(); i++) {
            os << toUTF8StdString(packages[i]);
            if (i != packages.size()-1)
                os << ".";
        }
        if (symbols.size() > 0) {
            os << ".";
            if (symbols.at(0) != "*") {
                os << "[";
                for(auto i=0; i < symbols.size(); i++) {
                    os << toUTF8StdString(symbols[i]);
                    if (i != symbols.size()-1)
                        os << ",";
                }
                os << "]";
            }
            else
                os << "*";
        }
        os << std::endl;
    }
}

void Import::acceptChildren(ASTVisitor& v, Anys& results)
{
}




std::any Declaration::accept(ASTVisitor& v)
{
    // downcast and pass-through (Declaration is abstract base)
    switch (declType) {
        case Type: { return std::dynamic_pointer_cast<ast::TypeDecl>(shared_from_this())->accept(v); break; }
        case Func: { return std::dynamic_pointer_cast<ast::FuncDecl>(shared_from_this())->accept(v); break; }
        case Var: { return std::dynamic_pointer_cast<ast::VarDecl>(shared_from_this())->accept(v); break; }
        default: throw std::runtime_error("unimplemented Declaration::accept() alternative");
    }
    return {};
}




std::any Statement::accept(ASTVisitor& v)
{
    // downcast and pass-through (Statement is abstract base)
    switch (stmtType) {
        case Suite: { return std::dynamic_pointer_cast<ast::Suite>(shared_from_this())->accept(v); break; }
        case Expression: { return std::dynamic_pointer_cast<ast::Expression>(shared_from_this())->accept(v); break; }
        case Return: { return std::dynamic_pointer_cast<ast::Literal>(shared_from_this())->accept(v); break; }
        case If: { return std::dynamic_pointer_cast<ast::IfStatement>(shared_from_this())->accept(v); break; }
        case While: { return std::dynamic_pointer_cast<ast::WhileStatement>(shared_from_this())->accept(v); break; }
        default: throw std::runtime_error("unimplemented Statement::accept() alternative");
    }
    return {};
}





std::any Suite::accept(ASTVisitor& v)
{
    Anys results {};

    if (v.visitFirst())
        results.push_back( v.visit(std::dynamic_pointer_cast<Suite>(shared_from_this())) );

    if (v.visitChildren())
        acceptChildren(v, results);

    if (v.visitLast())
        results.push_back( v.visit(std::dynamic_pointer_cast<Suite>(shared_from_this())) );

    return results;
}

void Suite::acceptChildren(ASTVisitor& v, Anys& results)
{
    for(auto& declOrStmt :declsOrStmts ) {

        if (std::holds_alternative<ptr<Declaration>>(declOrStmt))
            results.push_back( std::get<ptr<Declaration>>(declOrStmt)->accept(v) );
        else if (std::holds_alternative<ptr<Statement>>(declOrStmt))
            results.push_back( std::get<ptr<Statement>>(declOrStmt)->accept(v) );
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



std::any ExpressionStatement::accept(ASTVisitor& v)
{
    Anys results {};

    if (v.visitFirst())
        results.push_back( v.visit(std::dynamic_pointer_cast<ExpressionStatement>(shared_from_this())) );

    if (v.visitChildren())
        acceptChildren(v, results);

    if (v.visitLast())
        results.push_back( v.visit(std::dynamic_pointer_cast<ExpressionStatement>(shared_from_this())) );

    return results;
}

void ExpressionStatement::acceptChildren(ASTVisitor& v, Anys& results)
{
    results.push_back( expr->accept(v) );
}



void ExpressionStatement::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"ExprStmt" << std::endl;
    //sourceOut();
    expr->output(os,indent+1);
}



std::any ReturnStatement::accept(ASTVisitor& v)
{
    Anys results {};

    if (v.visitFirst())
        results.push_back( v.visit(std::dynamic_pointer_cast<ReturnStatement>(shared_from_this())) );

    if (v.visitChildren())
        acceptChildren(v, results);

    if (v.visitLast())
        results.push_back( v.visit(std::dynamic_pointer_cast<ReturnStatement>(shared_from_this())) );

    return results;
}

void ReturnStatement::acceptChildren(ASTVisitor& v, Anys& results)
{
    if (expr.has_value())
        results.push_back( expr.value()->accept(v) );
}


void ReturnStatement::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"Return" << std::endl;
    //sourceOut();
    if (expr.has_value())
        expr.value()->output(os,indent+1);
}




std::any IfStatement::accept(ASTVisitor& v)
{
    Anys results {};

    if (v.visitFirst())
        results.push_back( v.visit(std::dynamic_pointer_cast<IfStatement>(shared_from_this())) );

    if (v.visitChildren())
        acceptChildren(v, results);

    if (v.visitLast())
        results.push_back( v.visit(std::dynamic_pointer_cast<IfStatement>(shared_from_this())) );

    return results;
}

void IfStatement::acceptChildren(ASTVisitor& v, Anys& results)
{
    for(auto& condSuite :conditionalSuites ) {
        results.push_back( condSuite.first->accept(v) );
        results.push_back( condSuite.second->accept(v) );
    }
    if (elseSuite.has_value())
        results.push_back( elseSuite.value()->accept(v) );
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



std::any WhileStatement::accept(ASTVisitor& v)
{
    Anys results {};

    if (v.visitFirst())
        results.push_back( v.visit(std::dynamic_pointer_cast<WhileStatement>(shared_from_this())) );

    if (v.visitChildren())
        acceptChildren(v, results);

    if (v.visitLast())
        results.push_back( v.visit(std::dynamic_pointer_cast<WhileStatement>(shared_from_this())) );

    return results;
}

void WhileStatement::acceptChildren(ASTVisitor& v, Anys& results)
{
    results.push_back( condition->accept(v) );
    results.push_back( body->accept(v) );
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



std::any ForStatement::accept(ASTVisitor& v)
{
    Anys results {};

    if (v.visitFirst())
        results.push_back( v.visit(std::dynamic_pointer_cast<ForStatement>(shared_from_this())) );

    if (v.visitChildren())
        acceptChildren(v, results);

    if (v.visitLast())
        results.push_back( v.visit(std::dynamic_pointer_cast<ForStatement>(shared_from_this())) );

    return results;
}

void ForStatement::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"for" << std::endl;
    for(const auto& target : targetList)
        target->output(os,indent+1);
    os << spaces(indent)+" in:" << std::endl;
    iterable->output(os,indent+2);
    os << spaces(indent)+" body:" << std::endl;
    body->output(os,indent+2);
}

void ForStatement::acceptChildren(ASTVisitor& v, Anys& results)
{
    for(auto& target : targetList )
        results.push_back( target->accept(v) );

    results.push_back( iterable->accept(v) );
    results.push_back( body->accept(v) );

}

std::any OnStatement::accept(ASTVisitor& v)
{
    Anys results {};

    if (v.visitFirst())
        results.push_back( v.visit(std::dynamic_pointer_cast<OnStatement>(shared_from_this())) );

    if (v.visitChildren())
        acceptChildren(v, results);

    if (v.visitLast())
        results.push_back( v.visit(std::dynamic_pointer_cast<OnStatement>(shared_from_this())) );

    return results;
}

void OnStatement::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"On" << std::endl;
    os << spaces(indent+1) << "event:" << toUTF8StdString(event) << std::endl;
    os << spaces(indent+1) << "body:" << std::endl;
    body->output(os,indent+2);
}

void OnStatement::acceptChildren(ASTVisitor& v, Anys& results)
{
    results.push_back( body->accept(v) );
}





std::any VarDecl::accept(ASTVisitor& v)
{
    Anys results {};

    if (v.visitFirst())
        results.push_back( v.visit(std::dynamic_pointer_cast<VarDecl>(shared_from_this())) );

    if (v.visitChildren())
        acceptChildren(v, results);

    if (v.visitLast())
        results.push_back( v.visit(std::dynamic_pointer_cast<VarDecl>(shared_from_this())) );

    return results;
}

void VarDecl::acceptChildren(ASTVisitor& v, Anys& results)
{
    if (initializer.has_value())
        results.push_back( initializer.value()->accept(v) );
}


void VarDecl::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"VarDecl " << (access==Access::Private?"private ":"") << toUTF8StdString(name);
    if (varType.has_value()) {
        if (std::holds_alternative<icu::UnicodeString>(varType.value()))
            os << " :" << toUTF8StdString(std::get<icu::UnicodeString>(varType.value()));
        else if (std::holds_alternative<BuiltinType>(varType.value()))
            os << " :" << to_string(std::get<BuiltinType>(varType.value()));
    }
    os << std::endl;

    for(auto& annot : annotations)
        annot->output(os,indent+1);

    if (initializer.has_value())
        initializer.value()->output(os,indent+1);
}


std::any FuncDecl::accept(ASTVisitor& v)
{
    Anys results {};

    if (v.visitFirst())
        results.push_back( v.visit(std::dynamic_pointer_cast<FuncDecl>(shared_from_this())) );

    if (v.visitChildren())
        acceptChildren(v, results);

    if (v.visitLast())
        results.push_back( v.visit(std::dynamic_pointer_cast<FuncDecl>(shared_from_this())) );

    return {};
}

void FuncDecl::acceptChildren(ASTVisitor& v, Anys& results)
{
    results.push_back( func->accept(v) );
}


void FuncDecl::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"FuncDecl" << std::endl;
    outputType(os,indent);
    for(auto& annot : annotations)
        annot->output(os,indent+1);
    func->output(os,indent+1);
}



std::any Function::accept(ASTVisitor& v)
{
    Anys results {};

    if (v.visitFirst())
        results.push_back( v.visit(std::dynamic_pointer_cast<Function>(shared_from_this())) );

    if (v.visitChildren())
        acceptChildren(v, results);

    if (v.visitLast())
        results.push_back( v.visit(std::dynamic_pointer_cast<Function>(shared_from_this())) );

    return results;
}

void Function::acceptChildren(ASTVisitor& v, Anys& results)
{
    for(auto& param : params)
        results.push_back( param->accept(v) );
    if (std::holds_alternative<ptr<Suite>>(body)) { // abstract & interface methods have no function body
        auto suite = std::get<ptr<Suite>>(body);
        assert(suite != nullptr);
        results.push_back( suite->accept(v) );
    }
    else if (std::holds_alternative<ptr<Expression>>(body)) {
        auto expr = std::get<ptr<Expression>>(body);
        assert(expr != nullptr);
        results.push_back( expr->accept(v) );
    }
    else if (!std::holds_alternative<std::monostate>(body))
        throw std::runtime_error("Function body must be a suite or an expression (or nothing if abstract)");
}


void Function::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"Function" << (isProc? " (proc)" : "")
       << (access==Access::Private?" private":"")
       << " " << (name.has_value() ? toUTF8StdString(name.value()) : "");
    if (returnTypes.has_value()) {
        os << " → ";
        auto& types = returnTypes.value();
        if (types.size() == 1) {
            if (std::holds_alternative<BuiltinType>(types[0]))
                os << to_string(std::get<BuiltinType>(types[0]));
            else if (std::holds_alternative<icu::UnicodeString>(types[0]))
                os << toUTF8StdString(std::get<icu::UnicodeString>(types[0]));
        } else {
            os << "[";
            for (size_t i = 0; i < types.size(); i++) {
                if (i > 0) os << ", ";
                if (std::holds_alternative<BuiltinType>(types[i]))
                    os << to_string(std::get<BuiltinType>(types[i]));
                else if (std::holds_alternative<icu::UnicodeString>(types[i]))
                    os << toUTF8StdString(std::get<icu::UnicodeString>(types[i]));
            }
            os << "]";
        }
    }
    os << std::endl;
    for(auto& annot : annotations)
        annot->output(os,indent+1);
    if (params.size()>0) {
        os << spaces(indent)+" params:" << std::endl;
        for(auto& param : params)
            param->output(os,indent+2);
    }
    if (std::holds_alternative<ptr<Suite>>(body)) {
        auto suite = std::get<ptr<Suite>>(body);
        assert(suite != nullptr);
        os << spaces(indent)+" body:" << std::endl;
        suite->output(os,indent+2);
    }
    else if (std::holds_alternative<ptr<Expression>>(body)) {
        auto expr = std::get<ptr<Expression>>(body);
        assert(expr != nullptr);
        os << spaces(indent)+" body:" << std::endl;
        expr->output(os,indent+2);
    }
}



std::any Parameter::accept(ASTVisitor& v)
{
    Anys results {};

    if (v.visitFirst())
        results.push_back( v.visit(std::dynamic_pointer_cast<Parameter>(shared_from_this())) );

    if (v.visitChildren())
        acceptChildren(v, results);

    if (v.visitLast())
        results.push_back( v.visit(std::dynamic_pointer_cast<Parameter>(shared_from_this())) );

    return results;
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


void Parameter::acceptChildren(ASTVisitor& v, Anys& results)
{
    if (defaultValue.has_value())
        results.push_back( defaultValue.value()->accept(v) );
}



std::any TypeDecl::accept(ASTVisitor& v)
{
    Anys results {};

    if (v.visitFirst())
        results.push_back( v.visit(std::dynamic_pointer_cast<TypeDecl>(shared_from_this())) );

    if (v.visitChildren())
        acceptChildren(v, results);

    if (v.visitLast())
        results.push_back( v.visit(std::dynamic_pointer_cast<TypeDecl>(shared_from_this())) );

    return results;
}

void TypeDecl::acceptChildren(ASTVisitor& v, Anys& results)
{
    for(auto& property : properties)
        results.push_back( property->accept(v) );

    for(auto& method : methods)
        results.push_back( method->accept(v) );

    for(auto& enumLabel : enumLabels)
        if (enumLabel.second != nullptr)
            results.push_back( enumLabel.second->accept(v) );
}



void TypeDecl::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"TypeDecl "
       << (kind==Object ? "object" : (kind==Actor?"actor": (kind == Interface?"interface":(kind == Enumeration?"enum":"?")))) << " " << toUTF8StdString(name)
       << (extends.has_value() ? " "+toUTF8StdString(extends.value()) :"")
       << std::endl;
    if (!implements.empty()) {
        os << spaces(indent)
           << " implements " << toUTF8StdString(implements.at(0));
        for(int i=1; i<implements.size();i++)
            os << ", " << toUTF8StdString(implements.at(i));
        os << std::endl;
    }
    for(auto& annot : annotations)
        annot->output(os, indent+1);
    for(auto& property : properties)
        property->output(os, indent+1);
    for(auto& method : methods)
        method->output(os, indent+1);
    for(auto& enumLabels : enumLabels) {
        os << spaces(indent+1) << toUTF8StdString(enumLabels.first);
        if (enumLabels.second != nullptr) {
            os << std::endl;
            enumLabels.second->output(os, indent+2);
        }
        else
            os << std::endl;
    }
}


std::any Expression::accept(ASTVisitor& v)
{
    // downcast and pass-through (Expression is abstract base)
    switch (exprType) {
        case Assignment: { return std::dynamic_pointer_cast<ast::Assignment>(shared_from_this())->accept(v); break; }
        case BinaryOp: { return std::dynamic_pointer_cast<ast::BinaryOp>(shared_from_this())->accept(v); break; }
        case UnaryOp: { return std::dynamic_pointer_cast<ast::UnaryOp>(shared_from_this())->accept(v); break; }
        case Literal: { return std::dynamic_pointer_cast<ast::Literal>(shared_from_this())->accept(v); break; }
        case Variable: { return std::dynamic_pointer_cast<ast::Variable>(shared_from_this())->accept(v); break; }
        case Call: { return std::dynamic_pointer_cast<ast::Call>(shared_from_this())->accept(v); break; }
        case Range: { return std::dynamic_pointer_cast<ast::Range>(shared_from_this())->accept(v); break; }
        case Index: { return std::dynamic_pointer_cast<ast::Index>(shared_from_this())->accept(v); break; }
        default: throw std::runtime_error("unimplemented Expression::accept() alternative");
    }
    return {};
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
        default: return "?";
    }
}


std::any BinaryOp::accept(ASTVisitor& v)
{
    Anys results {};

    if (v.visitFirst())
        results.push_back( v.visit(std::dynamic_pointer_cast<BinaryOp>(shared_from_this())) );

    if (v.visitChildren())
        acceptChildren(v, results);

    if (v.visitLast())
        results.push_back( v.visit(std::dynamic_pointer_cast<BinaryOp>(shared_from_this())) );

    return results;
}

void BinaryOp::acceptChildren(ASTVisitor& v, Anys& results)
{
    results.push_back( lhs->accept(v) );
    results.push_back( rhs->accept(v) );
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



std::any UnaryOp::accept(ASTVisitor& v)
{
    Anys results {};

    if (v.visitFirst())
        results.push_back( v.visit(std::dynamic_pointer_cast<UnaryOp>(shared_from_this())) );

    if (v.visitChildren())
        acceptChildren(v, results);

    if (v.visitLast())
        results.push_back( v.visit(std::dynamic_pointer_cast<UnaryOp>(shared_from_this())) );

    return results;
}

void UnaryOp::acceptChildren(ASTVisitor& v, Anys& results)
{
    results.push_back( arg->accept(v) );
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



std::any Assignment::accept(ASTVisitor& v)
{
    Anys results {};

    if (v.visitFirst())
        results.push_back( v.visit(std::dynamic_pointer_cast<Assignment>(shared_from_this())) );

    if (v.visitChildren())
        acceptChildren(v, results);

    if (v.visitLast())
        results.push_back( v.visit(std::dynamic_pointer_cast<Assignment>(shared_from_this())) );

    return results;
}

void Assignment::acceptChildren(ASTVisitor& v, Anys& results)
{
    results.push_back( lhs->accept(v) );
    results.push_back( rhs->accept(v) );
}


void Assignment::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"Assignment" << std::endl;
    //sourceOut();
    lhs->output(os,indent+1);
    rhs->output(os,indent+1);
}


std::any Variable::accept(ASTVisitor& v)
{
    if (v.visitFirst() || v.visitLast())
        return v.visit(std::dynamic_pointer_cast<Variable>(shared_from_this()));
    return {};
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


std::any Call::accept(ASTVisitor& v)
{
    Anys results {};

    if (v.visitFirst())
        results.push_back( v.visit(std::dynamic_pointer_cast<Call>(shared_from_this())) );

    if (v.visitChildren())
        acceptChildren(v, results);

    if (v.visitLast())
        results.push_back( v.visit(std::dynamic_pointer_cast<Call>(shared_from_this())) );

    return results;
}

void Call::acceptChildren(ASTVisitor& v, Anys& results)
{
    results.push_back( callable->accept(v) );
    for(auto& arg : args)
        results.push_back( arg.second->accept(v) );
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



std::any Range::accept(ASTVisitor& v)
{
    Anys results {};

    if (v.visitFirst())
        results.push_back( v.visit(std::dynamic_pointer_cast<Range>(shared_from_this())) );

    if (v.visitChildren())
        acceptChildren(v, results);

    if (v.visitLast())
        results.push_back( v.visit(std::dynamic_pointer_cast<Range>(shared_from_this())) );

    return results;
}

void Range::acceptChildren(ASTVisitor& v, Anys& results)
{
    // start
    if (start != nullptr)
        results.push_back( start->accept(v) );
    else
        results.push_back( {} );

    // stop
    if (stop != nullptr)
        results.push_back( stop->accept(v) );
    else
        results.push_back( {} );

    // step
    if (step != nullptr)
        results.push_back( step->accept(v) );
    else
        results.push_back( {} );
}


void Range::output(std::ostream& os, int indent) const
{
    bool hasStep = (step != nullptr);
    if (closed) {
        if (hasStep)
            os << spaces(indent+2) << " range start..stop by step" << std::endl;
        else
            os << spaces(indent+2) << " range start..stop" << std::endl;
    }
    else {
        if (hasStep)
            os << spaces(indent+2) << " range start..<end by step" << std::endl;
        else
            os << spaces(indent+2) << " range start..<end" << std::endl;
    }
    if (start != nullptr)
        start->output(os,indent+4);
    else
        os << spaces(indent+4) << "-" << std::endl;
    if (stop != nullptr)
        stop->output(os,indent+4);
    else
        os << spaces(indent+4) << "-" << std::endl;
    if (!closed) {
        if (step != nullptr)
            step->output(os,indent+4);
        else
            os << spaces(indent+4) << "-" << std::endl;
    }
}




std::any Index::accept(ASTVisitor& v)
{
    Anys results {};

    if (v.visitFirst())
        results.push_back( v.visit(std::dynamic_pointer_cast<Index>(shared_from_this())) );

    if (v.visitChildren())
        acceptChildren(v, results);

    if (v.visitLast())
        results.push_back( v.visit(std::dynamic_pointer_cast<Index>(shared_from_this())) );

    return results;
}

void Index::acceptChildren(ASTVisitor& v, Anys& results)
{
    results.push_back( indexable->accept(v) );
    for(auto& arg : args)
        results.push_back( arg->accept(v) );
}


void Index::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"[]" << std::endl;
    indexable->output(os,indent+1);
    os << spaces(indent)+" indices:" << std::endl;
    for(auto& arg : args)
        arg->output(os,indent+2);
}



std::any LambdaFunc::accept(ASTVisitor& v)
{
    Anys results {};

    if (v.visitFirst())
        results.push_back( v.visit(std::dynamic_pointer_cast<LambdaFunc>(shared_from_this())) );

    if (v.visitChildren())
        acceptChildren(v, results);

    if (v.visitLast())
        results.push_back( v.visit(std::dynamic_pointer_cast<LambdaFunc>(shared_from_this())) );

    return results;
}

void LambdaFunc::output(std::ostream& os, int indent) const
{
    func->output(os,indent+1);
}

void LambdaFunc::acceptChildren(ASTVisitor& v, Anys& results)
{
    results.push_back( func->accept(v) );
}





std::any Literal::accept(ASTVisitor& v)
{
    switch (literalType) {
        case Nil: { return v.visit(std::dynamic_pointer_cast<Literal>(shared_from_this())); break; }
        case Bool: { return std::dynamic_pointer_cast<ast::Bool>(shared_from_this())->accept(v); break; }
        case Num: { return std::dynamic_pointer_cast<ast::Num>(shared_from_this())->accept(v); break; }
        case Str: { return std::dynamic_pointer_cast<ast::Str>(shared_from_this())->accept(v); break; }
        default: throw std::runtime_error("unimplemented Literal::accept() alternative");
    }
    return {};
}

void Literal::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"Literal " << (literalType==Nil ? std::string("nil") : std::to_string(int(literalType))) << std::endl;
    //sourceOut();
}


std::any Bool::accept(ASTVisitor& v)
{
    if (v.visitFirst() || v.visitLast())
        return v.visit(std::dynamic_pointer_cast<Bool>(shared_from_this()));
    return {};
}

void Bool::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"Bool " << (value ? "true":"false") << std::endl;
    //sourceOut();
}



std::any Num::accept(ASTVisitor& v)
{
    if (v.visitFirst() || v.visitLast())
        return v.visit(std::dynamic_pointer_cast<Num>(shared_from_this()));
    return {};
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



std::any Str::accept(ASTVisitor& v)
{
    if (v.visitFirst() || v.visitLast())
        return v.visit(std::dynamic_pointer_cast<Str>(shared_from_this()));
    return {};
}

void Str::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"Str \"" << toUTF8StdString(str) << "\"" << std::endl;
}


std::any Type::accept(ASTVisitor& v)
{
    if (v.visitFirst() || v.visitLast())
        return v.visit(std::dynamic_pointer_cast<Type>(shared_from_this()));
    return {};
}

void Type::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"Type " << to_string(t) << std::endl;
}



std::any List::accept(ASTVisitor& v)
{
    Anys results {};

    if (v.visitFirst())
        results.push_back( v.visit(std::dynamic_pointer_cast<List>(shared_from_this())) );

    if (v.visitChildren())
        acceptChildren(v, results);

    if (v.visitLast())
        results.push_back( v.visit(std::dynamic_pointer_cast<List>(shared_from_this())) );

    return {};
}

void List::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"List" << std::endl;
    for(auto& element : elements)
        element->output(os,indent+2);
}

void List::acceptChildren(ASTVisitor& v, Anys& results)
{
    for(auto& element : elements)
        results.push_back( element->accept(v) );
}




std::any Dict::accept(ASTVisitor& v)
{
    Anys results {};

    if (v.visitFirst())
        results.push_back( v.visit(std::dynamic_pointer_cast<Dict>(shared_from_this())) );

    if (v.visitChildren())
        acceptChildren(v, results);

    if (v.visitLast())
        results.push_back( v.visit(std::dynamic_pointer_cast<Dict>(shared_from_this())) );

    return results;
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

void Dict::acceptChildren(ASTVisitor& v, Anys& results)
{
    for(auto& entry : entries) {
        results.push_back( entry.first->accept(v) );
        results.push_back( entry.second->accept(v) );
    }
}


std::any Vector::accept(ASTVisitor& v)
{
    Anys results {};

    if (v.visitFirst())
        results.push_back( v.visit(std::dynamic_pointer_cast<Vector>(shared_from_this())) );

    if (v.visitChildren())
        acceptChildren(v, results);

    if (v.visitLast())
        results.push_back( v.visit(std::dynamic_pointer_cast<Vector>(shared_from_this())) );

    return {};
}

void Vector::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"Vector" << std::endl;
    for(auto& element : elements)
        element->output(os,indent+2);
}

void Vector::acceptChildren(ASTVisitor& v, Anys& results)
{
    for(auto& element : elements)
        results.push_back( element->accept(v) );
}


std::any Matrix::accept(ASTVisitor& v)
{
    Anys results {};

    if (v.visitFirst())
        results.push_back( v.visit(std::dynamic_pointer_cast<Matrix>(shared_from_this())) );

    if (v.visitChildren())
        acceptChildren(v, results);

    if (v.visitLast())
        results.push_back( v.visit(std::dynamic_pointer_cast<Matrix>(shared_from_this())) );

    return {};
}

void Matrix::output(std::ostream& os, int indent) const
{
    os << spaces(indent)+"Matrix" << std::endl;
    for(auto& row : rows)
        row->output(os,indent+2);
}

void Matrix::acceptChildren(ASTVisitor& v, Anys& results)
{
    for(auto& row : rows)
        results.push_back( row->accept(v) );
}
