#pragma once

#include <stack>
#include <unordered_set>
#include <unordered_map>
#include <optional>
#include <filesystem>

#include <core/AST.h>

#include "Chunk.h"
#include "Object.h"


namespace roxal {



class RoxalCompiler : public ast::ASTVisitor
{
public:
    RoxalCompiler();

    // Compile the specified source code and return a Value ObjFunction reference
    Value compile(std::istream& source, const std::string& name,
                  Value existingModule = Value::nilVal());

    // Attempt to load/store cached bytecode for a standalone source file (.rox)
    Value loadFileCache(const std::filesystem::path& sourcePath) const;
    void storeFileCache(const std::filesystem::path& sourcePath, const Value& function) const;

    void setOutputBytecodeDisassembly(bool outputBytecodeDisassembly);
    void setModulePaths(const std::vector<std::string>& modulePaths);
    void setReplMode(bool replMode);
    bool replMode() const { return replModeFlag; }

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
    virtual std::any visit(ptr<ast::OnStatement> ast);
    virtual std::any visit(ptr<ast::UntilStatement> ast);
    virtual std::any visit(ptr<ast::TryStatement> ast);
    virtual std::any visit(ptr<ast::RaiseStatement> ast);
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
    virtual std::any visit(ptr<ast::Vector> ast);
    virtual std::any visit(ptr<ast::Matrix> ast);
    virtual std::any visit(ptr<ast::Dict> ast);

    struct ModuleInfo {
        std::string modulePathRoot; // which module search path root is the module in? (from moduleRootPaths)
        icu::UnicodeString packagePath; // package path of the module
        icu::UnicodeString name;    // name of the module
        bool isPackage;
        std::string filename;       // filename of the module (e.g. with .rox extension)
        bool invalidFolder{false};  // folder existed but didn't contain init.rox or a single .rox file
        std::filesystem::path resolvedPath; // canonical path to resolved .rox file
        std::filesystem::path cachePath;    // path to compiled cache (.roc)
        bool cacheValid{false};             // true if cache exists and is newer than source

        // FIXME: make members protected, cache hashCode

        int32_t hashCode() const {
            return packagePath.hashCode() ^ name.hashCode() ^ (isPackage ? 1 : 0);
        }

        bool operator==(const ModuleInfo& other) const {
            // considered the same module if same package path & name (irrespective of module root)
            return hashCode() == other.hashCode();
        }
        bool operator<(const ModuleInfo& other) const {
            return hashCode() < other.hashCode();
        }
    };

public:
    using VarTypeSpec = std::variant<type::BuiltinType, icu::UnicodeString>;

protected:
    bool outputBytecodeDisassembly;
    bool replModeFlag{false};
    std::vector<std::string> modulePaths;

    std::map<ModuleInfo,Value> importedModules;

    // given the components of an import, such as "package.subpackage.module", return
    //  information about the module, including the file that should be executed
    ModuleInfo findImport(const std::vector<icu::UnicodeString>& components) const;

    struct Local {
        Local(const icu::UnicodeString& _name, int scopeDepth,
               std::optional<VarTypeSpec> t = std::nullopt)
            : name(_name), depth(scopeDepth), isCaptured(false), type(t) {}

        icu::UnicodeString name;
        int depth;
        bool isCaptured;
        std::optional<VarTypeSpec> type;
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

    void enterModuleScope(const icu::UnicodeString& packageName,
                          const icu::UnicodeString& moduleName,
                          const icu::UnicodeString& sourceName,
                          Value existingModule = Value::nilVal());
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

    Value loadModuleFromCache(const ModuleInfo& module) const;
    void storeModuleCache(const ModuleInfo& module, const Value& function) const;
    void reconcileModuleReferences(const Value& function) const;


    // stack new states when we enter new functions to compile
    struct FunctionScope : public LexicalScope
    {
        FunctionScope(const icu::UnicodeString& packageName, const icu::UnicodeString& moduleName,
                      const icu::UnicodeString& sourceName,
                      const icu::UnicodeString& funcName, FunctionType funcType, ptr<type::Type> t)
            : LexicalScope(ScopeType::Func, funcName), scopeDepth(0), functionType(funcType), type(t)
        {
            strict = true;
            function = Value::functionVal(funcName, packageName, moduleName, sourceName);
            ObjFunction* funcObj = asFunction(this->function);
            funcObj->funcType = type; // store type for runtime
            funcObj->strict = strict;
            funcObj->fnType = funcType;
            UnicodeString localName { (funcType==FunctionType::Method || funcType==FunctionType::Initializer) ?
                                        "this" : "" };
            locals.push_back(Local(localName,0));
        }

        std::vector<Local> locals;
        std::vector<Upvalue> upvalues;
        int scopeDepth;

        Value           function; // ObjFunction
        FunctionType    functionType;
        ptr<type::Type> type;

        std::vector<uint16_t> identConsts;
    };


    ptr<FunctionScope> asFuncScope(Scope s) const { return dynamic_ptr_cast<FunctionScope>(*s); }

    struct TypeScope : public LexicalScope
    {
        TypeScope(const icu::UnicodeString& typeName)
          : LexicalScope(ScopeType::Type, typeName), hasSuperType(false) {}

        icu::UnicodeString superTypeName;

        bool hasSuperType;
        struct MemberInfo {
            ast::Access access { ast::Access::Public };
            icu::UnicodeString owner;
        };
        std::unordered_map<icu::UnicodeString, MemberInfo> propertyNames;
    };

    ptr<TypeScope> asTypeScope(Scope s) const { return dynamic_ptr_cast<TypeScope>(*s); }

    // map type name -> registered member names (properties and methods)
    std::unordered_map<icu::UnicodeString,
                       std::unordered_map<icu::UnicodeString, TypeScope::MemberInfo>> typePropertyRegistry;


    struct ModuleScope : public FunctionScope
    {
        ModuleScope(const icu::UnicodeString& packageName_,
                    const icu::UnicodeString& moduleName_,
                    const icu::UnicodeString& sourceName_,
                    Value existing = Value::nilVal())
            : FunctionScope(packageName_, moduleName_, sourceName_, moduleName_,
                            FunctionType::Module,
                            make_ptr<type::Type>(type::BuiltinType::Func)),
              packageName(packageName_), moduleName(moduleName_), sourceName(sourceName_)
        {
            //this->functionType = FunctionType::Module;
            scopeType = ScopeType::Module;
            type->func = type::Type::FuncType();

            // while modules are lexically static, variables are declared in them at runtime
            // create a new ObjModuleType in which module vars are held
          if (existing.isNonNil()) {
              moduleType = existing;
              auto names = asModuleType(existing)->vars.variableNames();
              for (const auto& n : names)
                  moduleVarLines[n] = ast::LinePos{};
          }
          else {
              moduleType = Value::objVal(newModuleTypeObj(moduleName_));
              ObjModuleType::allModules.push_back(moduleType);
          }

            // since this scope only persists during compilation, store the moduleType
            //  in the function for runtime access
            asFunction(function)->moduleType = moduleType.weakRef();
        }
        virtual ~ModuleScope() {}

        icu::UnicodeString packageName;
        icu::UnicodeString moduleName;
        icu::UnicodeString sourceName;
        Value moduleType;  // ObjModuleType
        std::unordered_map<icu::UnicodeString, VarTypeSpec> moduleVarTypes;
        std::unordered_map<icu::UnicodeString, ast::LinePos> moduleVarLines;
    };

    ptr<ModuleScope> asModuleScope(Scope s) const { return dynamic_ptr_cast<ModuleScope>(*s); }


    //
    // Global modules

    std::vector<std::string> moduleRootPaths {};  // filesystem paths of top-level for package directories & module files

    // stack of current exception variable names for nested try/except blocks
    std::vector<icu::UnicodeString> exceptionVarStack {};





    ptr<Chunk> currentChunk() {
        #ifdef DEBUG_BUILD
        if (!inFuncScope())
            throw std::runtime_error("currentChunk() - not in func scope");
        #endif
        return asFunction(asFuncScope(funcScope())->function)->chunk;
    }

    ptr<ast::AST> currentNode;


    void error(const std::string& message);

    ValueType builtinToValueType(ast::BuiltinType bt);

    void emitByte(uint8_t byte, const std::string& comment = "");
    void emitByte(OpCode op, const std::string& comment = "");
    void emitBytes(uint8_t byte1, uint8_t byte2, const std::string& comment = "");
    void emitBytes(OpCode op, uint8_t byte2, const std::string& comment = "");
    void emitBytes(OpCode op, uint8_t byte2, uint8_t byte3, const std::string& comment = "");
    void emitBytes(OpCode op, uint16_t value, const std::string& comment = "") {
        debug_assert_msg(isDoubleByte(op), "emitBytes(OpCode, uint16_t) only allowed for double-byte arg OpCodes.");
        emitBytes(op, uint8_t(value >> 8), uint8_t(value & 0xFF), comment);
    }
    // if arg <= 255 output op and single byte,
    // if arg >  255 output op and two bytes (most and least significant byte of arg)
    void emitOpArgsBytes(OpCode op, uint16_t arg, const std::string& comment = "") {
        debug_assert_msg(!isDoubleByte(op), "emitOpArgsBytes(OpCode, int16_t) accepts only regular OpCode (automatically promoted to double-byte variant).");
        if (arg <= 255)
            emitBytes(op, uint8_t(arg), comment);
        else
            emitBytes(OpCode(uint8_t(op) | DoubleByteArg), uint8_t(arg >> 8), uint8_t(arg & 0xFF), comment);
    }
    uint8_t lastByte();

    void emitLoop(Chunk::size_type loopStart, const std::string& comment = "");

    Chunk::size_type emitJump(OpCode instruction, const std::string& comment = "");

    void emitReturn(const std::string& comment = "");
    void emitConstant(const Value& value, const std::string& comment = "");

    void patchJump(Chunk::size_type jumpInstrOffset);

    uint16_t makeConstant(const Value& value);

    // keep track of which chunk string constants table entires are for identifiers and re-use them
    uint16_t identifierConstant(const icu::UnicodeString& ident);

    void addLocal(const icu::UnicodeString& name, std::optional<VarTypeSpec> type = std::nullopt);
    int16_t resolveLocal(Scope scopeState, const icu::UnicodeString& name);
    int addUpvalue(Scope scopeState, uint8_t index, bool isLocal);
    int16_t resolveUpvalue(Scope scopeState, const icu::UnicodeString& name);
    void declareVariable(const icu::UnicodeString& name, std::optional<VarTypeSpec> type = std::nullopt);
    void defineVariable(uint16_t moduleVar = 0); // moduleVar unused if defining a local
    bool namedVariable(const icu::UnicodeString& name, bool assign=false);
    void namedModuleVariable(const icu::UnicodeString& name, bool assign=false);

    std::optional<VarTypeSpec> localVarType(const icu::UnicodeString& name);
    std::optional<VarTypeSpec> moduleVarType(const icu::UnicodeString& name);

};


std::ostream& operator<<(std::ostream& out, const RoxalCompiler::ModuleInfo& mi);


}
