#pragma once

#include <variant>
#include <optional>
#include <map>
#include <any>

#include <core/common.h>
#include <core/types.h>


namespace roxal::ast {

using roxal::type::BuiltinType;
using roxal::type::to_string;



class File;
class SingleInput;
class Annotation;
class Declaration;
class Statement;
class TypeDecl;
class FuncDecl;
class VarDecl;
class Expression;
class Suite;
class ExpressionStatement;
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
class Index;
class Literal;
class Bool;
class Str;
class Type;
class Num;
class List;
class Dict;


class ASTVisitor 
{
public:
    enum class TraversalOrder {
        Preorder,   // AST will visit node, then children
        Postorder,  // AST will visit chidren, then node
        // AST will visit node, but not children
        //  - concrete visitor visit() method should explicity call
        //    accept on children to control visit order
        VisitorDetermined 
    };

    virtual TraversalOrder traversalOrder() const 
      { return TraversalOrder::Postorder; }

    virtual std::any visit(ptr<File> ast) = 0;
    virtual std::any visit(ptr<SingleInput> ast) = 0;
    virtual std::any visit(ptr<Annotation> ast) = 0;
    virtual std::any visit(ptr<TypeDecl> ast) = 0;
    virtual std::any visit(ptr<FuncDecl> ast) = 0;
    virtual std::any visit(ptr<VarDecl> ast) = 0;
    virtual std::any visit(ptr<Suite> ast) = 0;
    virtual std::any visit(ptr<ExpressionStatement> ast) = 0;
    virtual std::any visit(ptr<ReturnStatement> ast) = 0;
    virtual std::any visit(ptr<IfStatement> ast) = 0;
    virtual std::any visit(ptr<WhileStatement> ast) = 0;
    virtual std::any visit(ptr<Function> ast) = 0;
    virtual std::any visit(ptr<Parameter> ast) = 0;
    virtual std::any visit(ptr<Assignment> ast) = 0;
    virtual std::any visit(ptr<BinaryOp> ast) = 0;
    virtual std::any visit(ptr<UnaryOp> ast) = 0;
    virtual std::any visit(ptr<Variable> ast) = 0;
    virtual std::any visit(ptr<Call> ast) = 0;
    virtual std::any visit(ptr<Index> ast) = 0;
    virtual std::any visit(ptr<Literal> ast) = 0;
    virtual std::any visit(ptr<Bool> ast) = 0;
    virtual std::any visit(ptr<Str> ast) = 0;
    virtual std::any visit(ptr<Type> ast) = 0;
    virtual std::any visit(ptr<Num> ast) = 0;
    virtual std::any visit(ptr<List> ast) = 0;
    virtual std::any visit(ptr<Dict> ast) = 0;


    inline bool visitFirst() const {
        auto order { traversalOrder() };
        return (order==TraversalOrder::VisitorDetermined || order==TraversalOrder::Preorder);
    }
    inline bool visitLast() const {
        return traversalOrder()==TraversalOrder::Postorder;
    }
    inline bool visitChildren() const {
        return traversalOrder()!=TraversalOrder::VisitorDetermined;
    }


};


struct LinePos { 
    LinePos() : line(0), pos(0) {}
    LinePos(size_t l, size_t p) : line(l), pos(p) {}
    size_t line; size_t pos; 
};


typedef std::pair<icu::UnicodeString, ptr<Expression>> ArgNameExpr;
typedef std::vector<std::any> Anys;



struct AST : public std::enable_shared_from_this<AST>
{
    AST() {}
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

    virtual std::any accept(ASTVisitor& v) { return {}; }
    virtual void output(std::ostream& os, int indent) const;

    std::optional<ptr<type::Type>> type; // type information, if known
    void outputType(std::ostream& os, int indent) const;

    // annotations (select AST node types)
    std::vector<ptr<Annotation>> annotations;


    // user-defined attributes
    //  (e.g. allow client code to annotate the AST)
    std::map<std::string, std::any> attrs;
    bool existsAttr(const std::string& attr) const { return attrs.find(attr)!=attrs.cend(); }
};


// output tree to stream using indentation to show depth
std::ostream& operator<<(std::ostream& os, std::shared_ptr<AST> ast);
std::ostream& operator<<(std::ostream& os, const AST& ast);


struct File : public AST {
    std::vector<std::variant<ptr<Declaration>, ptr<Statement>>> declsOrStmts;

    virtual std::any accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;

    // convenience for visitors when using VisitorDetermined traversal
    void acceptChildren(ASTVisitor& v, Anys& results);
};


struct SingleInput : public AST {
    ptr<Statement> stmt;

    virtual std::any accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;

    void acceptChildren(ASTVisitor& v, Anys& results);
};


struct Annotation : public AST {

    icu::UnicodeString name;

    std::vector<ArgNameExpr> args;

    bool namedArgs() const; // any args named?

    virtual std::any accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;

    void acceptChildren(ASTVisitor& v, Anys& results);
};


struct Declaration : public AST {
    enum DeclType {
        Type,
        Func,
        Var
    };

    Declaration(DeclType dt) : declType(dt) {}

    DeclType declType;

    virtual std::any accept(ASTVisitor& v);
};


struct Statement : public AST {
    enum StmtType {
        Suite,
        Expression,
        Return,
        If,
        While
    };

    Statement(StmtType st) : stmtType(st) {}

    StmtType stmtType;

    virtual std::any accept(ASTVisitor& v);
};


struct Suite : public Statement {
    Suite() : Statement(StmtType::Suite) {}

    std::vector<std::variant<ptr<Declaration>, ptr<Statement>>> declsOrStmts;

    virtual std::any accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;

    void acceptChildren(ASTVisitor& v, Anys& results);
};


struct ExpressionStatement : public Statement {
    ExpressionStatement() : Statement(StmtType::Expression) {}

    ptr<ast::Expression> expr;

    virtual std::any accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;

    void acceptChildren(ASTVisitor& v, Anys& results);
};


struct ReturnStatement : public Statement {
    ReturnStatement() : Statement(StmtType::Return) {}

    std::optional<ptr<ast::Expression>> expr;

    virtual std::any accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;

    void acceptChildren(ASTVisitor& v, Anys& results);
};



struct IfStatement : public Statement {
    IfStatement() : Statement(StmtType::If) {}

    std::vector<std::pair<ptr<ast::Expression>, ptr<ast::Suite>>> conditionalSuites;
    std::optional<ptr<ast::Suite>> elseSuite;

    virtual std::any accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;

    void acceptChildren(ASTVisitor& v, Anys& results);
};


struct WhileStatement : public Statement {
    WhileStatement() : Statement(StmtType::While) {}

    ptr<ast::Expression> condition;
    ptr<ast::Suite> body;

    virtual std::any accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;

    void acceptChildren(ASTVisitor& v, Anys& results);
};


struct VarDecl : public Declaration {
    VarDecl() : Declaration(DeclType::Var) {}

    icu::UnicodeString name;
    std::optional<ptr<Expression>> initializer;
    std::optional<std::variant<BuiltinType,icu::UnicodeString>> varType;

    virtual std::any accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;

    void acceptChildren(ASTVisitor& v, Anys& results);
};


struct FuncDecl : public Declaration {
    FuncDecl() : Declaration(DeclType::Func) {}

    ptr<Function> func;

    virtual std::any accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;

    void acceptChildren(ASTVisitor& v, Anys& results);
};


struct Function : public AST {
    bool isProc;
    icu::UnicodeString name;
    std::vector<ptr<Parameter>> params;
    std::optional<std::variant<BuiltinType,icu::UnicodeString>> returnType;
    ptr<Suite> body;

    virtual std::any accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;

    void acceptChildren(ASTVisitor& v, Anys& results);
};


struct Parameter : public AST {
    icu::UnicodeString name;
    std::optional<std::variant<BuiltinType,icu::UnicodeString>> type;
    std::optional<ptr<Expression>> defaultValue;

    virtual std::any accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;

    void acceptChildren(ASTVisitor& v, Anys& results);
};


struct TypeDecl : public Declaration {
    TypeDecl() : Declaration(DeclType::Type) {}

    enum Kind { Object, Actor };
    Kind kind;

    icu::UnicodeString name;
    std::optional<icu::UnicodeString> extends;
    std::vector<icu::UnicodeString> implements;

    std::vector<ptr<Function>> methods;

    // pre-declared properties (same syntax as variable declarations)
    std::vector<ptr<VarDecl>> properties;
 
    virtual std::any accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;

    void acceptChildren(ASTVisitor& v, Anys& results);
};


struct Expression : public AST {
    enum ExprType {
        Assignment,
        BinaryOp,
        UnaryOp,
        Literal,
        Variable,
        Call,
        Index
    };
    Expression(ExprType et) : exprType(et) {}

    ExprType exprType;

    virtual std::any accept(ASTVisitor& v); // needed?
};


struct BinaryOp : public Expression {    
    enum Op {
        None,
        Add, Subtract, Multiply, Divide, Modulo,
        And, Or, 
        Equal, NotEqual,
        LessThan, GreaterThan, LessOrEqual, GreaterOrEqual,
        FollowedBy
    };

    BinaryOp() : Expression(ExprType::BinaryOp), op(None) {}
    BinaryOp(Op _op);

    Op op;
    std::string opString() const;

    ptr<Expression> lhs;
    ptr<Expression> rhs;

    virtual std::any accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;

    void acceptChildren(ASTVisitor& v, Anys& results);
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

    virtual std::any accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;

    void acceptChildren(ASTVisitor& v, Anys& results);
};


struct Assignment : public Expression {
    Assignment() : Expression(ExprType::Assignment) {}

    ptr<Expression> lhs;
    ptr<Expression> rhs;

    virtual std::any accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;

    void acceptChildren(ASTVisitor& v, Anys& results);
};


struct Variable : public Expression {
    Variable() : Expression(ExprType::Variable) {}
    Variable(const icu::UnicodeString& s) : Expression(ExprType::Variable), name(s) {}

    icu::UnicodeString name;

    virtual std::any accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;
};


struct Call : public Expression {
    Call() : Expression(ExprType::Call) {}

    ptr<Expression> callable;

    std::vector<ArgNameExpr> args;

    bool namedArgs() const; // any args named?

    virtual std::any accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;

    void acceptChildren(ASTVisitor& v, Anys& results);
};



struct Index : public Expression {
    Index() : Expression(ExprType::Index) {}

    ptr<Expression> indexable;
    std::vector<ptr<Expression>> args;

    virtual std::any accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;

    void acceptChildren(ASTVisitor& v, Anys& results);
};


struct Literal : public Expression {
    enum LiteralType {
        Nil,
        Bool,
        Num,
        Str,
        Type,
        List,
        Dict
    };
    Literal() : Expression(ExprType::Literal), literalType(Nil) {}

    LiteralType literalType;

    virtual std::any accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;
};


struct Bool : public Literal {
    Bool() { literalType = LiteralType::Bool; }
    Bool(bool b) : value(b) { literalType = LiteralType::Bool; }
    bool value;

    virtual std::any accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;
};

struct Num : public Literal {
    Num() { literalType = LiteralType::Num; }

    std::variant<int32_t,double> num;

    virtual std::any accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;
};

struct Str : public Literal {
    Str() { literalType = LiteralType::Str; }

    icu::UnicodeString str;

    virtual std::any accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;
};

struct Type : public Literal {
    Type() { literalType = LiteralType::Type; }

    BuiltinType t;

    virtual std::any accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;
};

struct List : public Literal {
    List() { literalType = LiteralType::List; }

    std::vector<ptr<Expression>> elements;

    virtual std::any accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;

    void acceptChildren(ASTVisitor& v, Anys& results);
};

struct Dict : public Literal {
    Dict() { literalType = LiteralType::Dict; }

    // key -> value pairs
    std::vector<std::pair<ptr<Expression>,ptr<Expression>>> entries;

    virtual std::any accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;

    void acceptChildren(ASTVisitor& v, Anys& results);
};


}