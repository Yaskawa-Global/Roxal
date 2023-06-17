#pragma once

#include <stack>

#include <core/AST.h>

#include "Chunk.h"
#include "Object.h"


namespace roxal {


class RoxalCompiler : public ast::ASTVisitor
{
public:
    RoxalCompiler();

    ObjFunction* compile(std::istream& source, const std::string& name);

    void setOutputBytecodeDissasembly(bool outputBytecodeDissasembly);

    virtual TraversalOrder traversalOrder() const;

    virtual std::any visit(ptr<ast::File> ast);
    virtual std::any visit(ptr<ast::SingleInput> ast);
    virtual std::any visit(ptr<ast::Annotation> ast);
    virtual std::any visit(ptr<ast::TypeDecl> ast);
    virtual std::any visit(ptr<ast::FuncDecl> ast);
    virtual std::any visit(ptr<ast::VarDecl> ast);
    virtual std::any visit(ptr<ast::Suite> ast);
    virtual std::any visit(ptr<ast::ExpressionStatement> ast);
    virtual std::any visit(ptr<ast::ReturnStatement> ast);
    virtual std::any visit(ptr<ast::IfStatement> ast);
    virtual std::any visit(ptr<ast::WhileStatement> ast);
    virtual std::any visit(ptr<ast::Function> ast);
    virtual std::any visit(ptr<ast::Parameter> ast);
    virtual std::any visit(ptr<ast::Assignment> ast);
    virtual std::any visit(ptr<ast::BinaryOp> ast);
    virtual std::any visit(ptr<ast::UnaryOp> ast);
    virtual std::any visit(ptr<ast::Variable> ast);
    virtual std::any visit(ptr<ast::Call> ast);
    virtual std::any visit(ptr<ast::Range> ast);
    virtual std::any visit(ptr<ast::Index> ast);
    virtual std::any visit(ptr<ast::Literal> ast);
    virtual std::any visit(ptr<ast::Bool> ast);
    virtual std::any visit(ptr<ast::Str> ast);
    virtual std::any visit(ptr<ast::Type> ast);
    virtual std::any visit(ptr<ast::Num> ast);
    virtual std::any visit(ptr<ast::List> ast);
    virtual std::any visit(ptr<ast::Dict> ast);
    
   
protected:
    bool outputBytecodeDissasembly;

    struct Local {
        Local(const icu::UnicodeString& _name, int scopeDepth)
            : name(_name), depth(scopeDepth), isCaptured(false) {}
        icu::UnicodeString name;
        int depth;
        bool isCaptured;
    };

    struct Upvalue {
        Upvalue(uint8_t i, bool islocal) 
            : index(i), isLocal(islocal) {}
        uint8_t index;
        bool isLocal;
    };


    // stack new states when we enter new functions to compile
    struct FunctionScope {
        FunctionScope(const icu::UnicodeString& funcName, FunctionType funcType, ptr<type::Type> type) 
            : scopeDepth(0), strict(true), functionType(funcType), type(type)
        {
            function = functionVal();
            function->name = funcName;
            function->funcType = type; // store type for runtime
            UnicodeString localName { (funcType==FunctionType::Method || funcType==FunctionType::Initializer) ? 
                                        "this" : "" };
            locals.push_back(Local(localName,0)); 
        }

        std::vector<Local> locals;
        std::vector<Upvalue> upvalues;
        int scopeDepth;

        bool strict; 

        ObjFunction*    function;
        FunctionType    functionType;
        ptr<type::Type> type;

        std::vector<uint16_t> identConsts;
    };

    typedef std::vector<FunctionScope> FunctionScopes;
    FunctionScopes funcScopes;

    auto funcScope() {
        if (!funcScopes.empty())
            return funcScopes.end()-1;
        throw std::runtime_error("FunctionScope stack underflow");
    }

    auto enclosingFuncScope(FunctionScopes::iterator s) {
        if (s != funcScopes.begin())
            return --s;
        throw std::runtime_error("FunctionScope stack underflow");
    }


    struct TypeScope {
        TypeScope(const icu::UnicodeString& typeName)
          : name(typeName), hasSuperType(false) {}

        icu::UnicodeString name;
        bool hasSuperType;
    };

    typedef std::vector<TypeScope> TypeScopes;
    TypeScopes typeScopes;

    auto typeScope() {
        if (!typeScopes.empty())
            return typeScopes.end()-1;
        throw std::runtime_error("TypeScope stack underflow");
    }

    auto enclosingTypeScope(TypeScopes::iterator s) {
        if (s != typeScopes.begin())
            return --s;
        throw std::runtime_error("TypeScope stack underflow");
    }

    void beginTypeScope(const icu::UnicodeString& typeName) {
        typeScopes.push_back(TypeScope(typeName));
    }
    void endTypeScope() {
        if (!typeScopes.empty()) {
            typeScopes.pop_back();
            return;
        }
        throw std::runtime_error("TypeScope stack underflow");
    }




    ptr<Chunk> currentChunk() const {
        #ifdef DEBUG_BUILD
        if (funcScopes.empty())
            throw std::runtime_error("currentChunk() - funcScopes is empty");
        #endif
        return funcScopes.back().function->chunk;
    }

    ptr<ast::AST> currentNode;


    void beginScope();
    void endScope();

    void error(const std::string& message);

    ValueType builtinToValueType(ast::BuiltinType bt);

    void emitByte(uint8_t byte, const std::string& comment = "");
    void emitByte(OpCode op, const std::string& comment = "");
    void emitBytes(uint8_t byte1, uint8_t byte2, const std::string& comment = "");
    void emitBytes(OpCode op, uint8_t byte2, const std::string& comment = "");
    void emitBytes(OpCode op, uint8_t byte2, uint8_t byte3, const std::string& comment = "");

    void emitLoop(Chunk::size_type loopStart, const std::string& comment = "");

    Chunk::size_type emitJump(OpCode instruction, const std::string& comment = "");

    void emitReturn(const std::string& comment = "");
    void emitConstant(const Value& value, const std::string& comment = "");

    void patchJump(Chunk::size_type jumpInstrOffset);

    int16_t makeConstant(const Value& value);

    // keep track of which chunk string constants table entires are for identifiers and re-use them
    int16_t identifierConstant(const icu::UnicodeString& ident);

    void addLocal(const icu::UnicodeString& name);
    int16_t resolveLocal(FunctionScopes::iterator scopeState, const icu::UnicodeString& name);
    int addUpvalue(FunctionScopes::iterator scopeState, uint8_t index, bool isLocal);
    int16_t resolveUpvalue(FunctionScopes::iterator scopeState, const icu::UnicodeString& name);
    void declareVariable(const icu::UnicodeString& name);
    void defineVariable(uint16_t global);
    bool namedVariable(const icu::UnicodeString& ident, bool assign=false);

};


}
