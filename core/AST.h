#pragma once

#include <variant>
#include <optional>
#include <any>

#include <core/common.h>

namespace roxal::ast {


enum class BuiltinType {
    Nil, 
    Bool, Byte, Number, Int, Real, Decimal, 
    Char, String, 
    List, Dict, 
    Vector, Matrix, Tensor, 
    Orient, Stream 
};

std::string to_string(BuiltinType t);


class File;
class SingleInput;
class Declaration;
class Statement;
class TypeDecl;
class FuncDecl;
class VarDecl;
class Expression;
class Suite;
class ExpressionStatement;
class PrintStatement;
class ReturnStatement;
class IfStatement;
class WhileStatement;
class Function;
class Parameter;
class Assignment;
class BinaryOp;
class UnaryOp;
class Variable;
class Call;
class Literal;
class Bool;
class Str;
class Num;



class ASTVisitor 
{
public:
    virtual void visit(ptr<File> ast) = 0;
    virtual void visit(ptr<SingleInput> ast) = 0;
    virtual void visit(ptr<TypeDecl> ast) = 0;
    virtual void visit(ptr<FuncDecl> ast) = 0;
    virtual void visit(ptr<VarDecl> ast) = 0;
    virtual void visit(ptr<Suite> ast) = 0;
    virtual void visit(ptr<ExpressionStatement> ast) = 0;
    virtual void visit(ptr<PrintStatement> ast) = 0;
    virtual void visit(ptr<ReturnStatement> ast) = 0;
    virtual void visit(ptr<IfStatement> ast) = 0;
    virtual void visit(ptr<WhileStatement> ast) = 0;
    virtual void visit(ptr<Function> ast) = 0;
    virtual void visit(ptr<Parameter> ast) = 0;
    virtual void visit(ptr<Assignment> ast) = 0;
    virtual void visit(ptr<BinaryOp> ast) = 0;
    virtual void visit(ptr<UnaryOp> ast) = 0;
    virtual void visit(ptr<Variable> ast) = 0;
    virtual void visit(ptr<Call> ast) = 0;
    virtual void visit(ptr<Literal> ast) = 0;
    virtual void visit(ptr<Bool> ast) = 0;
    virtual void visit(ptr<Str> ast) = 0;
    virtual void visit(ptr<Num> ast) = 0;
};


struct LinePos { 
    LinePos() : line(0), pos(0) {}
    LinePos(size_t l, size_t p) : line(l), pos(p) {}
    size_t line; size_t pos; 
};


struct AST : public std::enable_shared_from_this<AST>
{
    AST() {}
    //AST(const AST&) = delete;
    virtual ~AST() {}

    AST& operator=(const AST&) = delete;

    // text from source corresponding to this AST subtree
    std::string sourceText() const {
        #if DEBUG_BUILD
        return fullSource;
        #else
        return stringInterval(*source,interval.first.line,interval.first.pos,interval.second.line,interval.second.pos);
        #endif
    }


    // source for this translation unit
    ptr<std::string> source;
    // interval in source string corresponding to this subtree
    //  (lines start from 1, position within line starts from 0)
    std::pair<LinePos,LinePos> interval;

    #ifdef DEBUG_BUILD
    // source for this node and all below
    std::string fullSource;
    #endif

    virtual void accept(ASTVisitor& v) {}
    virtual void output(std::ostream& os, int indent) const;
};


// output tree to stream using indentation to show depth
std::ostream& operator<<(std::ostream& os, std::shared_ptr<AST> ast);
std::ostream& operator<<(std::ostream& os, const AST& ast);


struct File : public AST {
    std::vector<std::variant<ptr<Declaration>, ptr<Statement>>> declsOrStmts;

    virtual void accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;
};


struct SingleInput : public AST {
    ptr<Statement> stmt;

    virtual void accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;
};


struct Declaration : public AST {
    enum DeclType {
        Type,
        Func,
        Var
    };
    DeclType type;

    virtual void accept(ASTVisitor& v);
};


struct Statement : public AST {
    enum StmtType {
        Suite,
        Expression,
        Print,
        Return,
        If,
        While
    };
    StmtType type;

    virtual void accept(ASTVisitor& v);
};


struct Suite : public Statement {
    std::vector<std::variant<ptr<Declaration>, ptr<Statement>>> declsOrStmts;

    virtual void accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;
};


struct ExpressionStatement : public Statement {
    ptr<ast::Expression> expr;

    virtual void accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;
};

struct PrintStatement : public Statement {
    ptr<ast::Expression> expr;

    virtual void accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;
};

struct ReturnStatement : public Statement {
    std::optional<ptr<ast::Expression>> expr;

    virtual void accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;
};



struct IfStatement : public Statement {
    std::vector<std::pair<ptr<ast::Expression>, ptr<ast::Suite>>> conditionalSuites;
    std::optional<ptr<ast::Suite>> elseSuite;

    virtual void accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;
};


struct WhileStatement : public Statement {
    ptr<ast::Expression> condition;
    ptr<ast::Suite> body;

    virtual void accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;
};


struct VarDecl : public Declaration {
    icu::UnicodeString name;
    std::optional<ptr<Expression>> initializer;

    virtual void accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;
};


struct FuncDecl : public Declaration {
    ptr<Function> func;

    virtual void accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;
};


struct Function : public AST {
    bool isProc;
    icu::UnicodeString name;
    std::vector<ptr<Parameter>> params;
    ptr<Suite> body;

    virtual void accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;
};


struct Parameter : public AST {
    icu::UnicodeString name;
    std::optional<std::variant<BuiltinType,icu::UnicodeString>> type;

    virtual void accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;
};


struct TypeDecl : public Declaration {
    enum Kind { Object, Actor };
    Kind kind;

    icu::UnicodeString name;
    std::optional<icu::UnicodeString> extends;
    std::vector<icu::UnicodeString> implements;
    std::vector<ptr<Function>> methods;

    virtual void accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;
};


struct Expression : public AST {
    enum ExprType {
        Assignment,
        BinaryOp,
        UnaryOp,
        Literal,
        Variable,
        Call
    };
    ExprType type;

    virtual void accept(ASTVisitor& v); // needed?
};


struct BinaryOp : public Expression {    
    enum Op {
        None,
        Add, Subtract, Multiply, Divide, Modulo,
        And, Or, 
        Equal, NotEqual,
        LessThan, GreaterThan, LessOrEqual, GreaterOrEqual
    };

    BinaryOp() : op(None) {}
    BinaryOp(Op _op);

    Op op;
    std::string opString() const;

    ptr<Expression> lhs;
    ptr<Expression> rhs;

    virtual void accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;
};


struct UnaryOp : public Expression {
    enum Op {
        None,
        Negate, Not, 
        Accessor
    };

    UnaryOp(Op _op);

    Op op;
    std::string opString() const;

    std::optional<icu::UnicodeString> member;
    ptr<Expression> arg;

    virtual void accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;
};


struct Assignment : public Expression {
    ptr<Expression> lhs;
    ptr<Expression> rhs;

    virtual void accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;
};


struct Variable : public Expression {
    Variable() {}
    Variable(const icu::UnicodeString& s) : name(s) {}

    icu::UnicodeString name;

    virtual void accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;
};


struct Call : public Expression {
    ptr<Expression> callable;
    std::vector<ptr<Expression>> args;

    virtual void accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;
};


struct Literal : public Expression {
    Literal() : type(Nil) {}
    enum LiteralType {
        Nil,
        Bool,
        Num,
        Str
    };
    LiteralType type;

    virtual void accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;
};


struct Bool : public Literal {
    Bool() { type = LiteralType::Bool; }
    Bool(bool b) : value(b) { type = LiteralType::Bool; }
    bool value;

    virtual void accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;
};

struct Num : public Literal {
    std::variant<int32_t,double> num;

    virtual void accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;
};

struct Str : public Literal {
    icu::UnicodeString str;

    virtual void accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;
};





}