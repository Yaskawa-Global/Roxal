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

enum class Access { Public, Private };



class File;
class SingleInput;
class Annotation;
class Import;
class Declaration;
class Statement;
class TypeDecl;
class FuncDecl;
class VarDecl;
class PropertyAccessor;
class Expression;
class Suite;
class ExpressionStatement;
class ReturnStatement;
class IfStatement;
class WhileStatement;
class ForStatement;
class WhenStatement;
class UntilStatement;
class TryStatement;
class MatchStatement;
class WithStatement;
class RaiseStatement;
class Function;
class Parameter;
class Assignment;
class BinaryOp;
class UnaryOp;
class Variable;
class Call;
class Range;
class Index;
class LambdaFunc;
class Literal;
class Bool;
class Str;
class Type;
class Num;
class SuffixedNum;
class SuffixedStr;
class List;
class Vector;
class Matrix;
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
    virtual std::any visit(ptr<Import> ast) = 0;
    virtual std::any visit(ptr<TypeDecl> ast) = 0;
    virtual std::any visit(ptr<FuncDecl> ast) = 0;
    virtual std::any visit(ptr<VarDecl> ast) = 0;
    virtual std::any visit(ptr<PropertyAccessor> ast) = 0;
    virtual std::any visit(ptr<Suite> ast) = 0;
    virtual std::any visit(ptr<ExpressionStatement> ast) = 0;
    virtual std::any visit(ptr<ReturnStatement> ast) = 0;
    virtual std::any visit(ptr<IfStatement> ast) = 0;
    virtual std::any visit(ptr<WhileStatement> ast) = 0;
    virtual std::any visit(ptr<ForStatement> ast) = 0;
    virtual std::any visit(ptr<WhenStatement> ast) = 0;
    virtual std::any visit(ptr<UntilStatement> ast) = 0;
    virtual std::any visit(ptr<TryStatement> ast) = 0;
    virtual std::any visit(ptr<MatchStatement> ast) = 0;
    virtual std::any visit(ptr<WithStatement> ast) = 0;
    virtual std::any visit(ptr<RaiseStatement> ast) = 0;
    virtual std::any visit(ptr<Function> ast) = 0;
    virtual std::any visit(ptr<Parameter> ast) = 0;
    virtual std::any visit(ptr<Assignment> ast) = 0;
    virtual std::any visit(ptr<BinaryOp> ast) = 0;
    virtual std::any visit(ptr<UnaryOp> ast) = 0;
    virtual std::any visit(ptr<Variable> ast) = 0;
    virtual std::any visit(ptr<Call> ast) = 0;
    virtual std::any visit(ptr<Range> ast) = 0;
    virtual std::any visit(ptr<Index> ast) = 0;
    virtual std::any visit(ptr<LambdaFunc> ast) = 0;
    virtual std::any visit(ptr<Literal> ast) = 0;
    virtual std::any visit(ptr<Bool> ast) = 0;
    virtual std::any visit(ptr<Str> ast) = 0;
    virtual std::any visit(ptr<Type> ast) = 0;
    virtual std::any visit(ptr<Num> ast) = 0;
    virtual std::any visit(ptr<SuffixedNum> ast) = 0;
    virtual std::any visit(ptr<SuffixedStr> ast) = 0;
    virtual std::any visit(ptr<List> ast) = 0;
    virtual std::any visit(ptr<Vector> ast) = 0;
    virtual std::any visit(ptr<Matrix> ast) = 0;
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
// start,stop,step,is-stop-inclusive
typedef std::tuple<ptr<Expression>,ptr<Expression>,ptr<Expression>,bool> RangeExpr;
typedef std::vector<std::any> Anys;



struct AST
  : public enable_ptr_from_this<AST>
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
std::ostream& operator<<(std::ostream& os, ptr<AST> ast);
std::ostream& operator<<(std::ostream& os, const AST& ast);


struct File : public AST {

    std::vector<ptr<Import>> imports;

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


struct Import : public AST {

    std::vector<icu::UnicodeString> packages; // [[package.]package.]module
    std::vector<icu::UnicodeString> symbols;  // empty, or ["*"] or list

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
        While,
        For,
        When,
        Until,
        Try,
        Raise,
        Match,
        With
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
    std::optional<ptr<ast::Expression>> atHost; // optional: host expression from 'at <expr>'

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


struct ForStatement : public Statement {
    ForStatement() : Statement(StmtType::For) {}

    std::vector<ptr<VarDecl>> targetList; // like var decl with name & optional type, but no initializer
    ptr<ast::Expression> iterable;
    ptr<ast::Suite> body;

    virtual std::any accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;

    void acceptChildren(ASTVisitor& v, Anys& results);
};


struct WhenStatement : public Statement {
    WhenStatement() : Statement(StmtType::When) {}

    ptr<ast::Expression> trigger;
    std::optional<icu::UnicodeString> binding;
    std::optional<ptr<ast::Expression>> becomes;
    std::optional<ptr<ast::Expression>> targetFilter;  // RHS of: where <binding>.target == <expr>
    bool matchesBecomes { false };
    bool requiresSignalChange { false };
    ptr<ast::Suite> body;

    virtual std::any accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;

    void acceptChildren(ASTVisitor& v, Anys& results);
};


struct UntilStatement : public Statement {
    UntilStatement() : Statement(StmtType::Until) {}

    ptr<ast::Statement> stmt;
    ptr<ast::Expression> condition;

    virtual std::any accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;

    void acceptChildren(ASTVisitor& v, Anys& results);
};


struct RaiseStatement : public Statement {
    RaiseStatement() : Statement(StmtType::Raise) {}

    std::optional<ptr<ast::Expression>> exception;

    virtual std::any accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;

    void acceptChildren(ASTVisitor& v, Anys& results);
};


struct TryStatement : public Statement {
    TryStatement() : Statement(StmtType::Try) {}

    ptr<ast::Suite> body;
    struct ExceptClause {
        std::optional<ptr<ast::Expression>> type;
        std::optional<icu::UnicodeString> name;
        ptr<ast::Suite> body;
    };
    std::vector<ExceptClause> exceptClauses;
    std::optional<ptr<ast::Suite>> finallySuite;

    virtual std::any accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;

    void acceptChildren(ASTVisitor& v, Anys& results);
};


struct MatchStatement : public Statement {
    MatchStatement() : Statement(StmtType::Match) {}

    // The expression being matched on
    ptr<ast::Expression> matchExpr;

    // Each case: (list of patterns, suite)
    // Multiple patterns in one case act as OR
    std::vector<std::pair<std::vector<ptr<ast::Expression>>, ptr<ast::Suite>>> cases;

    // Optional default case
    std::optional<ptr<ast::Suite>> defaultCase;

    // Metadata for optimization (set during type deduction)
    bool isEnumMatch = false;      // True if matching on enum
    bool isIntegralMatch = false;  // True if matching on int/byte
    bool hasRangeCase = false;     // True if any case uses range
    std::optional<uint16_t> enumTypeId;  // If enum, which type

    virtual std::any accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;

    void acceptChildren(ASTVisitor& v, Anys& results);
};

struct WithStatement : public Statement {
    WithStatement() : Statement(StmtType::With) {}

    // The expression being used as context (enum type, object instance, etc.)
    ptr<ast::Expression> contextExpr;

    // Body of the with block
    ptr<ast::Suite> body;

    // Metadata filled in by TypeDeducer
    enum ContextKind {
        Unknown,
        EnumType,      // with EnumTypeName:
        ObjectType,    // with objectInstance: (where type is known)
        ActorType      // with actorInstance: (where type is known)
    };

    ContextKind contextKind = Unknown;

    // The type information (filled by TypeDeducer)
    // For enums: type->builtin == Enum, type->enumer has the EnumType
    // For objects/actors: type->builtin == Object/Actor, type->obj has the ObjectType
    std::optional<ptr<roxal::type::Type>> contextType;

    virtual std::any accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;

    void acceptChildren(ASTVisitor& v, Anys& results);
};


struct VarDecl : public Declaration {
    VarDecl() : Declaration(DeclType::Var) {}

    icu::UnicodeString name;
    std::optional<ptr<Expression>> initializer;
    std::optional<std::variant<BuiltinType,icu::UnicodeString>> varType;
    Access access { Access::Public };
    bool isConst { false };        // declaration is 'const' (cannot reassign)
    bool isTypeConst { false };    // type is qualified with 'const' (e.g. var x: const T)
    bool isTypeMutable { false };  // type is qualified with 'mutable' (e.g. const x: mutable T)
    std::optional<ptr<Expression>> atHost; // optional: host expression from 'at <expr>'

    virtual std::any accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;

    void acceptChildren(ASTVisitor& v, Anys& results);
};


struct PropertyAccessor : public AST {
    icu::UnicodeString name;
    std::variant<BuiltinType, icu::UnicodeString> propType;
    std::optional<ptr<Expression>> initializer;
    Access access { Access::Public };
    bool isConst { false }; // true if declared with const instead of var

    // At least one must be present (validated during semantic analysis)
    // For one-liner: variant holds ptr<Statement> (compound_stmt or expr_stmt)
    // For block: variant holds ptr<Suite>
    std::optional<std::variant<ptr<Suite>, ptr<Statement>>> getter;
    std::optional<std::variant<ptr<Suite>, ptr<Statement>>> setter;

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
    std::optional<icu::UnicodeString> name; // none if lambda func
    std::vector<ptr<Parameter>> params;
    std::optional<std::vector<std::variant<BuiltinType,icu::UnicodeString>>> returnTypes;
    std::vector<bool> returnTypeConst; // parallel to returnTypes: true if 'const' qualifier
    std::variant<ptr<Suite>, ptr<Expression>, std::monostate> body; // no body if abstract
    Access access { Access::Public };

    virtual std::any accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;

    void acceptChildren(ASTVisitor& v, Anys& results);
};


struct Parameter : public AST {
    icu::UnicodeString name;
    std::optional<std::variant<BuiltinType,icu::UnicodeString>> type;
    std::optional<ptr<Expression>> defaultValue;
    bool variadic = false;  // true if ...name syntax (collects remaining positional args)
    bool isConst = false;   // true if parameter type is qualified with 'const'
    bool isMutable = false; // true if parameter type is qualified with 'mutable'

    virtual std::any accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;

    void acceptChildren(ASTVisitor& v, Anys& results);
};


struct TypeDecl : public Declaration {
    TypeDecl() : Declaration(DeclType::Type) {}

    enum Kind { Object, Actor, Interface, Enumeration, Event };
    Kind kind;

    icu::UnicodeString name;
    std::optional<icu::UnicodeString> extends;
    std::vector<icu::UnicodeString> implements;

    std::vector<ptr<Function>> methods;

    // pre-declared properties (same syntax as variable declarations)
    std::vector<ptr<VarDecl>> properties;

    // Property accessors (getters/setters)
    std::vector<ptr<PropertyAccessor>> propertyAccessors;

    // only for enumerations
    std::vector<std::pair<icu::UnicodeString, ptr<Expression>>> enumLabels;

    // nested type declarations within object/actor types
    std::vector<ptr<TypeDecl>> nestedTypes;

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
        Range,
        Index,
        LambdaFunc
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
        BitAnd, BitOr, BitXor,
        Equal, NotEqual, Is, In, NotIn,
        LessThan, GreaterThan, LessOrEqual, GreaterOrEqual
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
        BitNot,
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
    enum Op {
        Assign,      // "="
        CopyInto   // "<-"
    };

    Assignment() : Expression(ExprType::Assignment), op(Assign) {}

    Op op;
    ptr<Expression> lhs;
    ptr<Expression> rhs;
    std::optional<ptr<Expression>> atHost; // optional: host expression from 'at <expr>'

    std::string opString() const;

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


struct Range : public Expression {
    Range() : Expression(ExprType::Range),
              start(nullptr), stop(nullptr), step(nullptr)
     {}

    // a range includes
    //  an optional start
    //  an optional stop
    //  an optional step
    //  flag indicating if the stop is inclusive
    // if closed==false, behaves like Python slice or Ruby ... (half-open interval)
    // if closed==true, behaves like Ruby [a..b] (closed interval)

    // if start is omitted, implies implicit start (e.g. first element)
    // if stop  is omitted, implies implicit stop (e.g. last element)
    // if step  is omitted, implies 1
    // if closed==true, stop is inclusive (closed interval), otherwise exclusive (half-open interval)

    ptr<Expression> start; // may be null
    ptr<Expression> stop;  // may be null
    ptr<Expression> step;  // may be null

    bool closed;

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


struct LambdaFunc : public Expression {
    LambdaFunc() : Expression(ExprType::LambdaFunc) {}

    ptr<Function> func;

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
        Dict,
        Vector,
        Matrix,
        SuffixedNum,
        SuffixedStr
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

    std::variant<int32_t,int64_t,double> num;

    virtual std::any accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;
};

struct Str : public Literal {
    Str() { literalType = LiteralType::Str; }

    icu::UnicodeString str;

    virtual std::any accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;
};


struct SuffixedNum : public Literal {
    SuffixedNum() { literalType = LiteralType::SuffixedNum; }

    std::variant<int32_t,int64_t,double> num;
    icu::UnicodeString suffix;  // raw suffix string: "m", "m/s", "kg·m/s²", etc.

    virtual std::any accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;
};

struct SuffixedStr : public Literal {
    SuffixedStr() { literalType = LiteralType::SuffixedStr; }

    icu::UnicodeString str;
    icu::UnicodeString suffix;

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

struct Vector : public Literal {
    Vector() { literalType = LiteralType::Vector; }

    std::vector<ptr<ast::Expression>> elements;

    virtual std::any accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;

    void acceptChildren(ASTVisitor& v, Anys& results);
};

struct Matrix : public Literal {
    Matrix() { literalType = LiteralType::Matrix; }

    std::vector<ptr<ast::Vector>> rows;

    virtual std::any accept(ASTVisitor& v);
    virtual void output(std::ostream& os, int indent) const;

    void acceptChildren(ASTVisitor& v, Anys& results);
};


}
