#pragma once

#include <stack>
#include <unordered_set>
#include <unordered_map>

#include <core/AST.h>

#include "Chunk.h"
#include "Object.h"


namespace roxal {



class RoxalCompiler : public ast::ASTVisitor
{
public:
    RoxalCompiler();

    ObjFunction* compile(std::istream& source, const std::string& name);

    void setOutputBytecodeDisassembly(bool outputBytecodeDisassembly);
    void setModulePaths(const std::vector<std::string>& modulePaths);

    virtual TraversalOrder traversalOrder() const;

    virtual std::any visit(ptr<ast::File> ast);
    virtual std::any visit(ptr<ast::SingleInput> ast);
    virtual std::any visit(ptr<ast::Annotation> ast);
    virtual std::any visit(ptr<ast::Import> ast);
    virtual std::any visit(ptr<ast::TypeDecl> ast);
    virtual std::any visit(ptr<ast::FuncDecl> ast);
    virtual std::any visit(ptr<ast::VarDecl> ast);
    virtual std::any visit(ptr<ast::Suite> ast);
    virtual std::any visit(ptr<ast::ExpressionStatement> ast);
    virtual std::any visit(ptr<ast::ReturnStatement> ast);
    virtual std::any visit(ptr<ast::IfStatement> ast);
    virtual std::any visit(ptr<ast::WhileStatement> ast);
    virtual std::any visit(ptr<ast::ForStatement> ast);
    virtual std::any visit(ptr<ast::Function> ast);
    virtual std::any visit(ptr<ast::Parameter> ast);
    virtual std::any visit(ptr<ast::Assignment> ast);
    virtual std::any visit(ptr<ast::BinaryOp> ast);
    virtual std::any visit(ptr<ast::UnaryOp> ast);
    virtual std::any visit(ptr<ast::Variable> ast);
    virtual std::any visit(ptr<ast::Call> ast);
    virtual std::any visit(ptr<ast::Range> ast);
    virtual std::any visit(ptr<ast::Index> ast);
    virtual std::any visit(ptr<ast::LambdaFunc> ast);
    virtual std::any visit(ptr<ast::Literal> ast);
    virtual std::any visit(ptr<ast::Bool> ast);
    virtual std::any visit(ptr<ast::Str> ast);
    virtual std::any visit(ptr<ast::Type> ast);
    virtual std::any visit(ptr<ast::Num> ast);
    virtual std::any visit(ptr<ast::List> ast);
    virtual std::any visit(ptr<ast::Dict> ast);

    struct ModuleInfo {
        std::string modulePathRoot; // which module search path root is the module in? (from moduleRootPaths)
        std::string packagePath;    // package path of the module
        std::string name;           // name of the module
        bool isPackage;
        std::string filename;       // filename of the module (e.g. with .rox extension)

        // FIXME: make members protected, cache hashCode

        int32_t hashCode() const {
            // TODO: consider making the members UnicodeStrings
            icu::UnicodeString packagePathU { toUnicodeString(packagePath) };
            icu::UnicodeString nameU { toUnicodeString(name) };
            return packagePathU.hashCode() ^ nameU.hashCode() ^ (isPackage ? 1 : 0);
        }

        bool operator==(const ModuleInfo& other) const {
            // considered the same module if same package path & name (irrespective of module root)
            return hashCode() == other.hashCode();
        }
        bool operator<(const ModuleInfo& other) const {
            return hashCode() < other.hashCode();
        }
    };

protected:
    bool outputBytecodeDisassembly;
    std::vector<std::string> modulePaths;

    std::map<ModuleInfo,Value> importedModules;

    // given the components of an import, such as "package.subpackage.module", return
    //  the module path root that contains it, the relative path to the package and the module filename
    ModuleInfo findImport(const std::vector<icu::UnicodeString>& components) const;

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



    // stack new scope when entering new lexical level (global, module, type, func/method, scope:, for etc)
    struct LexicalScope {
        enum class ScopeType {
            Global,
            Module,
            Type,
            Func,
            Scope // scope: for .. : etc.
        };

        LexicalScope(ScopeType st, const icu::UnicodeString& n) : scopeType(st), name(n) {}
        virtual ~LexicalScope() {}

        ScopeType scopeType;
        icu::UnicodeString name;

        bool strict;

        bool isGlobal() const { return scopeType==ScopeType::Global; }
        bool isModule() const { return scopeType==ScopeType::Module; }
        bool isFunc() const { return scopeType==ScopeType::Func; }
        bool isFuncOrModule() const { return scopeType==ScopeType::Func || scopeType==ScopeType::Module; }

        std::string typeString() const {
            if (scopeType == ScopeType::Global) return "Global";
            if (scopeType == ScopeType::Module) return "Module";
            if (scopeType == ScopeType::Type) return "Type";
            if (scopeType == ScopeType::Func) return "Func";
            if (scopeType == ScopeType::Scope) return "Scope";
            return "?";
        }
    };
    typedef std::vector<ptr<LexicalScope>> LexicalScopes;
    typedef LexicalScopes::iterator Scope;
    LexicalScopes lexicalScopes;
    void outputScopes();

    void enterModuleScope(const icu::UnicodeString& packageName, const icu::UnicodeString& moduleName);
    void exitModuleScope();

    void enterTypeScope(const icu::UnicodeString& typeName);
    void exitTypeScope();

    void enterFuncScope(Value moduleType, const icu::UnicodeString& funcName, FunctionType funcType, ptr<type::Type> type);
    void exitFuncScope();

    void enterLocalScope();
    void exitLocalScope();


    int scopeDepth() const;
    Scope scope();
    bool hasEnclosingScope(Scope s);
    Scope enclosingScope(Scope s);

    bool inFuncScope();
    bool inFuncScope(Scope s);
    Scope funcScope();
    bool hasEnclosingFuncScope(Scope s);
    Scope enclosingFuncScope(Scope s);

    bool inTypeScope();
    Scope typeScope();
    Scope enclosingTypeScope(Scope s);

    bool inModuleScope();
    Scope moduleScope();
    Scope enclosingModuleScope(Scope s);


    // stack new states when we enter new functions to compile
    struct FunctionScope : public LexicalScope
    {
        FunctionScope(const icu::UnicodeString& packageName, const icu::UnicodeString& moduleName,
                      const icu::UnicodeString& funcName, FunctionType funcType, ptr<type::Type> t)
            : LexicalScope(ScopeType::Func, funcName), scopeDepth(0), functionType(funcType), type(t)
        {
            strict = true;
            function = functionVal(packageName, moduleName);
            function->name = funcName;
            function->funcType = type; // store type for runtime
            UnicodeString localName { (funcType==FunctionType::Method || funcType==FunctionType::Initializer) ?
                                        "this" : "" };
            locals.push_back(Local(localName,0));
        }

        std::vector<Local> locals;
        std::vector<Upvalue> upvalues;
        int scopeDepth;

        ObjFunction*    function;
        FunctionType    functionType;
        ptr<type::Type> type;

        std::vector<uint16_t> identConsts;
    };


    ptr<FunctionScope> asFuncScope(Scope s) const { return std::dynamic_pointer_cast<FunctionScope>(*s); }

    struct TypeScope : public LexicalScope
    {
        TypeScope(const icu::UnicodeString& typeName)
          : LexicalScope(ScopeType::Type, typeName), hasSuperType(false) {}

        bool hasSuperType;
        std::unordered_set<std::string> propertyNames;
    };

    ptr<TypeScope> asTypeScope(Scope s) const { return std::dynamic_pointer_cast<TypeScope>(*s); }

    // map type name -> registered member names (properties and methods)
    std::unordered_map<std::string, std::unordered_set<std::string>> typePropertyRegistry;


    struct ModuleScope : public FunctionScope
    {
        ModuleScope(const icu::UnicodeString& packageName_, const icu::UnicodeString& moduleName_)
            : FunctionScope(packageName_, moduleName_, moduleName_, FunctionType::Module, std::make_shared<type::Type>(type::BuiltinType::Func)),
              packageName(packageName_), moduleName(moduleName_)
        {
            //this->functionType = FunctionType::Module;
            scopeType = ScopeType::Module;
            type->func = type::Type::FuncType();

            // while modules are lexically static, variables are declared in them at runtime
            // create a new ObjModuleType in which module vars are held
            moduleType = Value(moduleTypeVal(moduleName_));

            // since this scope only persists during compilation, store the moduleType
            //  in the function for runtime access
            function->moduleType = moduleType;
        }
        virtual ~ModuleScope() {}

        icu::UnicodeString packageName;
        icu::UnicodeString moduleName;
        Value moduleType;  // ObjModuleType
    };

    ptr<ModuleScope> asModuleScope(Scope s) const { return std::dynamic_pointer_cast<ModuleScope>(*s); }


    //
    // Global modules

    std::vector<std::string> moduleRootPaths {};  // filesystem paths of top-level for package directories & module files





    ptr<Chunk> currentChunk() {
        #ifdef DEBUG_BUILD
        if (!inFuncScope())
            throw std::runtime_error("currentChunk() - not in func scope");
        #endif
        return asFuncScope(funcScope())->function->chunk;
    }

    ptr<ast::AST> currentNode;


    void error(const std::string& message);

    ValueType builtinToValueType(ast::BuiltinType bt);

    void emitByte(uint8_t byte, const std::string& comment = "");
    void emitByte(OpCode op, const std::string& comment = "");
    void emitBytes(uint8_t byte1, uint8_t byte2, const std::string& comment = "");
    void emitBytes(OpCode op, uint8_t byte2, const std::string& comment = "");
    void emitBytes(OpCode op, uint8_t byte2, uint8_t byte3, const std::string& comment = "");
    uint8_t lastByte();

    void emitLoop(Chunk::size_type loopStart, const std::string& comment = "");

    Chunk::size_type emitJump(OpCode instruction, const std::string& comment = "");

    void emitReturn(const std::string& comment = "");
    void emitConstant(const Value& value, const std::string& comment = "");

    void patchJump(Chunk::size_type jumpInstrOffset);

    int16_t makeConstant(const Value& value);

    // keep track of which chunk string constants table entires are for identifiers and re-use them
    int16_t identifierConstant(const icu::UnicodeString& ident);

    void addLocal(const icu::UnicodeString& name);
    int16_t resolveLocal(Scope scopeState, const icu::UnicodeString& name);
    int addUpvalue(Scope scopeState, uint8_t index, bool isLocal);
    int16_t resolveUpvalue(Scope scopeState, const icu::UnicodeString& name);
    void declareVariable(const icu::UnicodeString& name);
    void defineVariable(uint16_t moduleVar = 0); // moduleVar unused if defining a local
    bool namedVariable(const icu::UnicodeString& name, bool assign=false);
    void namedModuleVariable(const icu::UnicodeString& name, bool assign=false);

};


std::ostream& operator<<(std::ostream& out, const RoxalCompiler::ModuleInfo& mi);


}
