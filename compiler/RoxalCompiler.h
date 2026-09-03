#pragma once

#include <cassert>
#include <stack>
#include <utility>
#include <unordered_set>
#include <unordered_map>
#include <optional>
#include <filesystem>

#include <core/AST.h>
#include <core/ordered_map.h>

#include "Chunk.h"
#include "GCRoots.h"
#include "Object.h"
#include "TypeDeducer.h"


namespace roxal {

class VM;



class RoxalCompiler : public ast::ASTVisitor
{
public:
    RoxalCompiler();

    // Compile the specified source code and return a Value ObjFunction reference
    Value compile(std::istream& source, const std::string& name,
                  Value existingModule = Value::nilVal(),
                  const std::string& sourceNameOverride = "");

    // Attempt to load/store cached bytecode for a standalone source file (.rox)
    Value loadFileCache(const std::filesystem::path& sourcePath) const;
    void storeFileCache(const std::filesystem::path& sourcePath, const Value& function) const;

    void setOutputBytecodeDisassembly(bool outputBytecodeDisassembly);
    void setModulePaths(const std::vector<std::string>& modulePaths);
    void setReplMode(bool replMode);
    void setCacheReadEnabled(bool enabled);
    // Process-wide debug-metadata default (the CLI's --no-debug-info clears
    // it); every compiler instance snapshots it at construction.  The debug
    // tier is part of the .roc cache header, so a stripped cache can never
    // satisfy a debug-enabled run or vice versa -- mismatches recompile.
    static void setDebugInfoDefault(bool on) { s_debugInfoDefault = on; }
    static bool debugInfoDefault() { return s_debugInfoDefault; }
    void setCacheWriteEnabled(bool enabled);
    void setModuleResolverVM(VM* vm);
    bool replMode() const { return replModeFlag; }

    virtual TraversalOrder traversalOrder() const;

    virtual std::any visit(ptr<ast::File> ast);
    virtual std::any visit(ptr<ast::SingleInput> ast);
    virtual std::any visit(ptr<ast::Annotation> ast);
    virtual std::any visit(ptr<ast::Import> ast);
    virtual std::any visit(ptr<ast::TypeDecl> ast);
    virtual std::any visit(ptr<ast::FuncDecl> ast);
    virtual std::any visit(ptr<ast::VarDecl> ast);
    virtual std::any visit(ptr<ast::PropertyAccessor> ast);
    virtual std::any visit(ptr<ast::Suite> ast);
    virtual std::any visit(ptr<ast::ExpressionStatement> ast);
    virtual std::any visit(ptr<ast::ReturnStatement> ast);
    virtual std::any visit(ptr<ast::BreakStatement> ast);
    virtual std::any visit(ptr<ast::ContinueStatement> ast);
    virtual std::any visit(ptr<ast::JumpStatement> ast);
    virtual std::any visit(ptr<ast::LabelStatement> ast);
    virtual std::any visit(ptr<ast::IfStatement> ast);
    virtual std::any visit(ptr<ast::WhileStatement> ast);
    virtual std::any visit(ptr<ast::ForStatement> ast);
    virtual std::any visit(ptr<ast::WhenStatement> ast);
    virtual std::any visit(ptr<ast::UntilStatement> ast);
    virtual std::any visit(ptr<ast::AdheringIfStatement> ast);
    virtual std::any visit(ptr<ast::TryStatement> ast);
    virtual std::any visit(ptr<ast::MatchStatement> ast);
    virtual std::any visit(ptr<ast::WithStatement> ast);
    virtual std::any visit(ptr<ast::RaiseStatement> ast);
    virtual std::any visit(ptr<ast::AssertStatement> ast);
    void emitComparison(ast::BinaryOp::Op op);
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
    virtual std::any visit(ptr<ast::StrInterp> ast);
    virtual std::any visit(ptr<ast::Type> ast);
    virtual std::any visit(ptr<ast::Num> ast);
    virtual std::any visit(ptr<ast::SuffixedNum> ast);
    virtual std::any visit(ptr<ast::SuffixedStr> ast);
    virtual std::any visit(ptr<ast::List> ast);
    virtual std::any visit(ptr<ast::Vector> ast);
    virtual std::any visit(ptr<ast::Matrix> ast);
    virtual std::any visit(ptr<ast::Dict> ast);

    struct ModuleInfo {
        std::string modulePathRoot; // which module search path root is the module in? (from moduleRootPaths)
        ustring packagePath; // package path of the module
        ustring name;    // name of the module
        bool isPackage;
        std::string filename;       // filename of the module (e.g. with .rox extension)
        bool invalidFolder{false};  // folder existed but didn't contain init.rox
        std::filesystem::path resolvedPath; // canonical path to resolved .rox file
        std::filesystem::path cachePath;    // path to compiled cache (.roc)
        bool cacheValid{false};             // true if cache exists and is newer than source
        bool isProto{false};                // true if import refers to a .proto file
        bool isIdl{false};                  // true if import refers to a .idl file
        bool moduleClash{false};            // a module file and module folder share a name in one directory
        std::string clashFilePath;          // path of the clashing module .rox file
        std::string clashFolderPath;        // path of the clashing module folder

        // FIXME: make members protected, cache hashCode

        int32_t hashCode() const {
            int32_t h = packagePath.hashCode() ^ name.hashCode() ^ (isPackage ? 1 : 0);
            if (isProto) h ^= 0x10000;
            if (isIdl) h ^= 0x20000;
            return h;
        }

        bool operator==(const ModuleInfo& other) const {
            // considered the same module if same package path & name (irrespective of module root)
            return packagePath == other.packagePath &&
                   name == other.name &&
                   isPackage == other.isPackage &&
                   isProto == other.isProto &&
                   isIdl == other.isIdl;
        }
        bool operator<(const ModuleInfo& other) const {
            return hashCode() < other.hashCode();
        }
    };

public:
    using VarTypeSpec = std::variant<type::BuiltinType, ast::TypeName>;

protected:
    bool outputBytecodeDisassembly;
    bool replModeFlag{false};
    std::vector<std::string> modulePaths;
    bool cacheReadEnabled;
    bool cacheWriteEnabled;
    bool currentModuleHasDynamicImport{false};
    // Dynamic (.idl/.proto) imports made by the module being compiled,
    // recorded into the .roc cache so cache loads can re-import them.
    // The annotation names attached to the import statement are carried
    // verbatim (the compiler does not interpret them; the importer backing
    // the import decides what, if anything, they mean).
    struct DynImport {
        std::string path;
        std::vector<std::string> annotations;
    };
    std::vector<DynImport> currentDynamicImports;
    VM* moduleResolverVM;

    // Persistent TypeDeducer for REPL mode to maintain type info across lines
    ptr<TypeDeducer> replTypeDeducer;

    // Literal suffix registry: maps suffix string -> function name
    struct SuffixRegistration {
        ustring suffix;
        ustring functionName;
        ustring moduleName;  // for error messages
    };
    std::unordered_map<ustring, SuffixRegistration> suffixRegistry;
    void registerSuffix(const ustring& suffix, const ustring& funcName,
                        const ustring& moduleName);
    const SuffixRegistration* lookupSuffix(const ustring& suffix) const;

    // suffix codegen, split so an interpolated suffixed string can build its
    // argument between the two halves (see visit(ptr<ast::StrInterp>))
    bool emitSuffixCallee(const ustring& suffix);
    void emitSuffixCall();

    std::map<ModuleInfo,Value> importedModules;  // allowed-raw: rooted by importedModulesRoot

public:
    // Drop this compiler's per-instance imported-modules cache.  Used by the
    // REPL's `reload` command together with VM::clearUserModuleRegistry() so
    // the next `import` statement re-runs the dependency's body. Without this
    // the long-lived REPL RoxalCompiler instance would still short-circuit on
    // its own importedModules entry before the VM-level registry lookup
    // happens. Other compiler state (replTypeDeducer, suffixRegistry) is
    // preserved so previously-declared types in the REPL still type-check.
    void clearImportedModules() { importedModules.clear(); }
protected:

    // given the components of an import, such as "package.subpackage.module", return
    //  information about the module, including the file that should be executed
    ModuleInfo findImport(const std::vector<ustring>& components) const;

    struct Local {
        Local(const ustring& _name, int scopeDepth,
               std::optional<VarTypeSpec> t = std::nullopt, bool _isConst = false,
               bool _isTypeConst = false)
            : name(_name), depth(scopeDepth), isCaptured(false), isConst(_isConst),
              isTypeConst(_isTypeConst), type(t) {}

        ustring name;
        int depth;
        bool isCaptured;
        bool isConst;
        bool isTypeConst;   // var x: const T — type is const, but var is reassignable
        bool isParam { false }; // immutable binding (cannot reassign) but value is not const
        std::optional<VarTypeSpec> type;
        // Debug metadata: chunk offset at which this local became
        // initialized (defineVariable); its live range ends where it is
        // popped (exitLocalScope) or at function end.
        uint32_t debugStartOffset { 0 };
    };

    struct Upvalue {
        Upvalue(uint8_t i, bool islocal, const ustring& n = ustring())
            : index(i), isLocal(islocal), name(n) {}
        uint8_t index;
        bool isLocal;
        ustring name;   // debug metadata
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

        LexicalScope(ScopeType st, const ustring& n) : scopeType(st), name(n) {}
        virtual ~LexicalScope() {}

        ScopeType scopeType;
        ustring name;

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

        // Compiler roots: expose the in-progress GC objects
        // this scope retains to the mark phase (see lexicalScopesRoot).  A
        // scope subclass that grows a Value member MUST extend its override.
        virtual void traceValues(ValueVisitor& visitor) const { (void)visitor; }
    };
    typedef std::vector<ptr<LexicalScope>> LexicalScopes;
    typedef LexicalScopes::iterator Scope;
    LexicalScopes lexicalScopes;

    // Position of a binding in the lexical scope stack.  lexicalScopes is a
    // stack, so a larger scopeIndex is strictly more inner; within one function
    // scope a larger blockDepth is more inner.  Used to decide whether a
    // compile-time const is shadowed by a local / parameter / member declared
    // inside its scope.
    struct LexicalRank {
        int scopeIndex { -1 };   // index into lexicalScopes; -1 = unranked
        int blockDepth { -1 };
        bool ranked() const { return scopeIndex >= 0; }
        bool innerThan(const LexicalRank& o) const {
            if (!ranked())   return false;
            if (!o.ranked()) return true;
            return scopeIndex != o.scopeIndex ? scopeIndex > o.scopeIndex
                                              : blockDepth > o.blockDepth;
        }
    };

    // What a bare identifier resolves to, ignoring compile-time consts.
    // Produced by findBinding() (side-effect free); namedVariable() decides
    // const-vs-candidate by rank and then emits for the chosen kind.
    struct Candidate {
        enum class Kind { None, Local, Upvalue,
                          WithEnumLabel, WithProperty, WithMethod,   // innermost `with` context
                          InFlightType, EnclosingTypeConstMember, ThisMember, ThisUpvalueMember,
                          ModuleVar };
        Kind kind { Kind::None };
        LexicalRank rank;        // unranked for With* / InFlightType / ModuleVar
        Scope scope;             // Local/Upvalue: owning FunctionScope; member kinds: the TypeScope
        int16_t index { -1 };    // Local: slot in funcScope(); Upvalue: slot in `scope`; InFlightType: anchor slot
    };

    // Compiler roots: the in-progress compilation products
    // (per-scope ObjFunctions incl. their chunk constant tables, const
    // bindings, module types, imported modules) are reachable only through
    // this compiler's state -- these member roots keep them visible to the
    // mark phase so a thread can PARK mid-compile (nested module load /
    // import) under conservative marking, instead of the barrier waiting
    // out the whole compile (GCNoParkScope, kept only as the
    // precise-mode fallback -- see compile()).  Declared AFTER their
    // targets: member init order guarantees the targets exist first, and
    // C++ codegen temps in stack locals are covered by the parked-stack
    // scan.
    static void traceLexicalScopes(ValueVisitor& visitor, const LexicalScopes& scopes) {
        for (const auto& scope : scopes)
            if (scope)
                scope->traceValues(visitor);
    }
    TracedRef<LexicalScopes> lexicalScopesRoot { lexicalScopes, &RoxalCompiler::traceLexicalScopes };
    TracedRef<std::map<ModuleInfo,Value>> importedModulesRoot { importedModules };
    void outputScopes();

    void enterModuleScope(const ustring& packageName,
                          const ustring& moduleName,
                          const ustring& sourceName,
                          Value existingModule = Value::nilVal());
    void exitModuleScope();

    void enterTypeScope(const ustring& typeName);
    void exitTypeScope();

    void enterFuncScope(Value moduleType, const ustring& funcName, FunctionType funcType, ptr<type::Type> type);
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
        FunctionScope(const ustring& packageName, const ustring& moduleName,
                      const ustring& sourceName,
                      const ustring& funcName, FunctionType funcType, ptr<type::Type> t)
            : LexicalScope(ScopeType::Func, funcName), scopeDepth(0), functionType(funcType), type(t)
        {
            strict = true;
            function = Value::functionVal(funcName, packageName, moduleName, sourceName);
            ObjFunction* funcObj = asFunction(this->function);
            funcObj->funcType = type; // store type for runtime
            funcObj->strict = strict;
            funcObj->fnType = funcType;
            ustring localName { (funcType==FunctionType::Method || funcType==FunctionType::Initializer) ?
                                        "this" : "" };
            locals.push_back(Local(localName,0));
            constBindings.emplace_back();
            pendingLocals.emplace_back();
        }

        std::vector<Local> locals;
        std::vector<Upvalue> upvalues;
        int scopeDepth;

        struct ConstBinding {
            Value value;   // allowed-raw: traced via FunctionScope::traceValues
            ast::LinePos line;
        };
        std::vector<std::unordered_map<ustring, ConstBinding>> constBindings;

        // Names a live block declares LATER on, scanned when the block is
        // entered (visit(Suite)) and cleared per name as its declaration is
        // reached.  A name is reserved for its whole block, so a use before
        // the declaration is a compile error rather than silently meaning an
        // outer binding.  Index-aligned with scopeDepth, like constBindings.
        std::vector<std::unordered_map<ustring, ast::LinePos>> pendingLocals;

        void traceValues(ValueVisitor& visitor) const override {
            if (function.isObj())
                visitor.visit(function);   // chunk constants trace transitively
            for (const auto& bindings : constBindings)
                for (const auto& entry : bindings)
                    if (entry.second.value.isObj())
                        visitor.visit(entry.second.value);
        }

        Value           function; // allowed-raw: traced via traceValues (ObjFunction)
        FunctionType    functionType;
        ptr<type::Type> type;

        // AST-level return types (preserves user-defined type names that TypeDeducer loses).
        // Used by visit(ReturnStatement) to emit return type conversion.
        std::optional<std::vector<VarTypeSpec>> astReturnTypes;

        std::vector<uint16_t> identConsts;

        // Local-scope function overload tracking. Populated by a pre-pass in
        // visit(Function) over the function body. A name with count > 1 will
        // bind to an OverloadSet in its local slot; visit(FuncDecl) emits
        // DefineLocalOverload accordingly. Each new FuncType is appended to
        // localOverloadCandidates as the FuncDecl is processed; visit(Call)
        // consults this for compile-time resolution.
        std::unordered_map<ustring, int> localFuncDeclCounts;
        std::unordered_map<ustring, int16_t> localOverloadSlots;
        std::unordered_map<ustring,
                           std::vector<ptr<type::Type>>> localOverloadCandidates;

        // ---- 'jump'/'label' bookkeeping (confined to a single function body) ----
        // A defined 'label <name>' marker.
        struct LabelInfo {
            ustring name;
            size_t offset;            // bytecode offset of the label (jump target)
            size_t liveLocalCount;    // locals.size() at the label = slot keep-count for jumps here
            int scopeDepth;
            std::vector<int> blockPath;  // active block ids at the label (ancestry path)
            int guardDepth;           // open try/with/when nesting at the label
        };
        // A 'jump <name>' whose label has not yet been seen (forward reference).
        struct PendingJump {
            ustring name;
            size_t popArgOffset;      // offset of the PopToCount 2-byte arg to patch
            size_t jumpArgOffset;     // offset of the Jump 2-byte arg to patch
            std::vector<int> liveLocalDepths; // depths of live locals at the jump (non-decreasing)
            int scopeDepth;
            std::vector<int> blockPath;
            int guardDepth;
            ast::LinePos line;        // for error reporting
        };
        std::vector<LabelInfo> labels;
        std::vector<PendingJump> pendingJumps;
        int guardDepth { 0 };         // open try/with/when nesting at the current point
        std::vector<int> blockPath;   // active block ids (one per open local scope)
        int nextBlockId { 0 };        // monotonic block-id generator
    };


    ptr<FunctionScope> asFuncScope(Scope s) const { return dynamic_ptr_cast<FunctionScope>(*s); }

    struct TypeScope : public LexicalScope
    {
        TypeScope(const ustring& typeName)
          : LexicalScope(ScopeType::Type, typeName), hasSuperType(false) {}

        ast::TypeName superTypeName;

        bool hasSuperType;
        bool isActor { false };
        // Stack slot of the in-flight type during its own body emission, anchored
        // by an unnamed local. -1 means not set. Set after the type's value is
        // pushed onto the runtime stack (via Dup for nested or GetModuleVar for
        // top-level), and cleared when the body finishes. Used by the
        // typescope-const walker to load the in-flight enclosing type directly,
        // bypassing the parent-attachment chain (which fails at runtime when the
        // enclosing type's NESTED_TYPE attachment hasn't run yet).
        int16_t inFlightStackSlot { -1 };
        // The shared record (core/types.h): also what a module publishes for
        // importers via ObjModuleType::typeMembers.
        using MemberInfo = type::MemberInfo;
        // Insertion order preserved so callers (notably `proc init(*)` and
        // dict(obj)/to_json) can walk properties in declaration order.
        ordered_map<ustring, MemberInfo> propertyNames;

        // Weak handle on the enclosing TypeDecl AST. Used by `proc init(*)`
        // synthesis to walk the type's own properties in declaration order.
        // Weak so the TypeScope can't accidentally extend the AST's
        // lifetime; the AST graph is the source of truth and lives at
        // least as long as the TypeScope by construction. Set right after
        // enterTypeScope() via `typeDecl = ast;` (assignment from ptr).
        weak_ptr<ast::TypeDecl> typeDecl;
    };

    ptr<TypeScope> asTypeScope(Scope s) const { return dynamic_ptr_cast<TypeScope>(*s); }

    // Compile-time member registry of the module currently being compiled
    // (see ModuleScope::typePropertyRegistry).  nullptr when the type is not
    // registered in this module.
    ordered_map<ustring, TypeScope::MemberInfo>* findTypeMembers(const ustring& typeName);
    void registerTypeMembers(const ustring& typeName,
                             const ordered_map<ustring, TypeScope::MemberInfo>& members);
    // Publish this module's registry onto its ObjModuleType so importers can
    // read it (including from a cache load), and adopt an imported module's
    // published entries into this module's registry.  `qualifier` is the name
    // the importing source refers to the module by; `alsoUnqualified` covers
    // `import m.*` / `import m: Name`, where the type is also visible bare.
    // Declaration line of `name` if a live block of the current function
    // declares it later on, else nullptr (see FunctionScope::pendingLocals).
    const ast::LinePos* pendingDeclaration(const ustring& name);
    // Record every name a block's own statements declare, so uses earlier in
    // that block can be rejected.  Shallow: a nested block scans its own.
    void scanBlockDeclarations(const std::vector<std::variant<ptr<ast::Declaration>,
                                                              ptr<ast::Statement>>>& declsOrStmts);
    // Reached the declaration: the name is no longer pending.
    void clearPendingDeclaration(const ustring& name);

    void publishTypeMembersToModule();
    void adoptImportedTypeMembers(const Value& importedModuleType,
                                  const ustring& qualifier,
                                  bool alsoUnqualified);


    struct ModuleScope : public FunctionScope
    {
        ModuleScope(const ustring& packageName_,
                    const ustring& moduleName_,
                    const ustring& sourceName_,
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
              ObjModuleType* existingModule = asModuleType(existing);
              auto snapshot = existingModule->vars.snapshot();
              for (const auto& entry : snapshot) {
                  moduleVarLines[entry.first] = ast::LinePos{};
                  if (existingModule->constVars.find(entry.first.hashCode()) != existingModule->constVars.end())
                      moduleConstLines[entry.first] = ast::LinePos{};
              }
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

        void traceValues(ValueVisitor& visitor) const override {
            FunctionScope::traceValues(visitor);
            if (moduleType.isObj())
                visitor.visit(moduleType);
        }

        ustring packageName;
        ustring moduleName;
        ustring sourceName;
        // map type name -> registered member names (properties and methods);
        // inner map preserves declaration order.  Owned by the module, not the
        // compiler: compiling an import re-enters compile() on this same
        // compiler and pushes its own ModuleScope, so a same-named type in the
        // imported module must not overwrite this module's entry.  MemberInfo
        // holds no Value, so this needs no GC tracing.
        std::unordered_map<ustring,
                           ordered_map<ustring, TypeScope::MemberInfo>> typePropertyRegistry;

        Value moduleType;  // allowed-raw: traced via traceValues (ObjModuleType)
        std::unordered_map<ustring, VarTypeSpec> moduleVarTypes;
        std::unordered_set<ustring> moduleVarTypeConst; // vars declared as var x: const T
        std::unordered_map<ustring, ast::LinePos> moduleVarLines;
        std::unordered_map<ustring, ast::LinePos> moduleConstLines;

        // Module-level function overload tracking. Populated by a pre-pass in
        // visit(File) over the file's top-level FuncDecls. A name with count > 1
        // will bind to an OverloadSet via DefineModuleOverload. Each new FuncType
        // is appended to moduleOverloadCandidates as the FuncDecl is processed;
        // visit(Call) consults this for compile-time resolution.
        std::unordered_map<ustring, int> moduleFuncDeclCounts;
        std::unordered_map<ustring,
                           std::vector<ptr<type::Type>>> moduleOverloadCandidates;
    };

    ptr<ModuleScope> asModuleScope(Scope s) const { return dynamic_ptr_cast<ModuleScope>(*s); }


    //
    // Global modules

    std::vector<std::string> moduleRootPaths {};  // filesystem paths of top-level for package directories & module files

    // stack of current exception variable names for nested try/except blocks
    std::vector<ustring> exceptionVarStack {};

    // Stack of with contexts for name resolution
    struct WithContext {
        ast::WithStatement::ContextKind kind;
        ptr<type::Type> type;
        uint16_t stackSlot; // local variable slot holding the context value
    };
    std::vector<WithContext> withContextStack {};





    ptr<Chunk> currentChunk() {
        #ifdef DEBUG_BUILD
        if (!inFuncScope())
            throw std::runtime_error("currentChunk() - not in func scope");
        #endif
        return asFunction(asFuncScope(funcScope())->function)->chunk;
    }

    // Source attribution follows AST traversal nesting.  Each visitor (and
    // synthetic-emission site associated with a sub-node) pushes a scope;
    // child scopes restore the parent automatically on every exit path.
    std::vector<ptr<ast::AST>> sourceNodeStack;

    class SourceNodeScope {
    public:
        SourceNodeScope(RoxalCompiler& compiler, ptr<ast::AST> node)
            : compiler(compiler), depth(compiler.sourceNodeStack.size())
        {
            compiler.sourceNodeStack.push_back(std::move(node));
        }

        ~SourceNodeScope()
        {
            assert(compiler.sourceNodeStack.size() == depth + 1);
            compiler.sourceNodeStack.resize(depth);
        }

        SourceNodeScope(const SourceNodeScope&) = delete;
        SourceNodeScope& operator=(const SourceNodeScope&) = delete;

    private:
        RoxalCompiler& compiler;
        size_t depth;
    };

    ptr<ast::AST> currentNode() const {
        return sourceNodeStack.empty() ? nullptr : sourceNodeStack.back();
    }

    // True while unwinding a compile error (cleanup of scopes). Suppresses the
    // unresolved-'jump' check so error cleanup doesn't throw a second time.
    bool compileUnwinding { false };

    bool compilingNestedType { false }; // true during nested type compilation — skips module registration

    void error(const std::string& message);

    // Reject annotation arguments the module cache cannot round-trip.
    void checkAnnotationArgs(const std::vector<ptr<ast::Annotation>>& annotations,
                             const ptr<ast::AST>& location);

    // Retain the annotations of a top-level var/const/type declaration on the
    // module type (ObjModuleType::declAnnotations), the way visit(FuncDecl)
    // retains a callable's on its ObjFunction.  No-ops when the list is empty
    // or the declaration is not directly at module scope.
    void recordDeclAnnotations(const ustring& name,
                               const std::vector<ptr<ast::Annotation>>& annotations,
                               const ptr<ast::AST>& location);

    ValueType builtinToValueType(ast::BuiltinType bt);

    void emitTypeName(const ast::TypeName& components); // emit namedVariable + GetProp chain for dotted type names
    void emitConvertToVarType(const VarTypeSpec& t);    // emit ToType/ToTypeSpec (strict per func scope) for one declared type
    void emitDefaultValue(ast::BuiltinType bt); // push the default value for a declared builtin type.
                                                // list/dict emit NewList/NewDict 0 — a constant-table
                                                // default would be ONE shared mutable object across
                                                // every invocation/instance
    void emitReturnTypeConversion(); // convert stack top to the declared return type(s);
                                     // multi-return ('-> [T0,..]') verifies arity and
                                     // converts element-wise, rebuilding the list
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
    // Two-arg variant: first arg uses single/double-byte encoding (auto-promotes),
    // second arg is always emitted as a 2-byte big-endian uint16_t. Used by
    // OpCode::GetOverloadAt and OpCode::GetLocalOverloadAt.
    void emitOpArgsBytesPlusIndex(OpCode op, uint16_t arg, uint16_t index, const std::string& comment = "") {
        debug_assert_msg(!isDoubleByte(op), "emitOpArgsBytesPlusIndex(OpCode, ...) accepts only regular OpCode (automatically promoted to double-byte variant).");
        if (arg <= 255) {
            emitBytes(op, uint8_t(arg), comment);
        } else {
            emitBytes(OpCode(uint8_t(op) | DoubleByteArg), uint8_t(arg >> 8), uint8_t(arg & 0xFF), comment);
        }
        emitByte(uint8_t(index >> 8));
        emitByte(uint8_t(index & 0xFF));
    }
    uint8_t lastByte();

    void emitLoop(Chunk::size_type loopStart, const std::string& comment = "");

    Chunk::size_type emitJump(OpCode instruction, const std::string& comment = "");

    void emitReturn(const std::string& comment = "");
    void emitConstant(const Value& value, const std::string& comment = "");

    void patchJump(Chunk::size_type jumpInstrOffset);

    // Emit a PopToCount opcode with a fixed 2-byte arg. If 'count' is given, writes it;
    // otherwise writes a 0xffff placeholder. Returns the offset of the arg bytes (to patch).
    Chunk::size_type emitPopToCount(int count = -1, const std::string& comment = "");
    // Overwrite a raw big-endian uint16 at the given code offset (for backpatching).
    void patchU16At(Chunk::size_type argOffset, uint16_t value);
    // Resolve any forward 'jump's that targeted this just-defined label.
    void resolveLabel(const FunctionScope::LabelInfo& label);
    // Error on any 'jump' whose label was never defined in this function body.
    void checkUnresolvedJumps();

    uint16_t makeConstant(const Value& value);

    // keep track of which chunk string constants table entires are for identifiers and re-use them
    uint16_t identifierConstant(const ustring& ident);

    void addLocal(const ustring& name, std::optional<VarTypeSpec> type = std::nullopt);
    int16_t resolveLocal(Scope scopeState, const ustring& name);
    int addUpvalue(Scope scopeState, uint8_t index, bool isLocal,
                   const ustring& name);

    // ---- Debug metadata emission ----
    // Statement boundaries, local live ranges and upvalue names accumulate
    // in Chunk::debugInfo during compilation; always on by default
    // (--no-debug-info wiring arrives with the cache-tier work).
    void markStmtStart(const ptr<ast::AST>& node);  // block-level statement boundary
    void recordFunctionEntryStmt();                 // offset 0 of a fresh function chunk
    void recordLocalDebug(const Local& local, uint16_t slot);
    void flushFuncDebugInfo();                      // remaining locals + upvalues at func end
    static ustring varTypeSpecDisplayName(const std::optional<VarTypeSpec>& t);
    inline static bool s_debugInfoDefault { true };
    bool debugInfoEnabled_ { s_debugInfoDefault };
    int16_t resolveUpvalue(Scope scopeState, const ustring& name);
    // ---- forward-declared type linkage ----
    // Top-level type names are forward-referenceable through the placeholders
    // visit(File) pre-walks, but Extend / Implements / EventExtend snapshot the
    // referenced type when they run.  After each top-level type body completes,
    // the linkage of every earlier declaration (top-level or nested) that
    // transitively depends on it is re-emitted; the opcodes are idempotent, so
    // this converges on the state a declaration-order program would have.
    struct TypeLinkNode {
        ptr<ast::TypeDecl> decl;
        ast::TypeName qualifiedName;           // [Name] top-level, [Outer, .., Name] nested
        size_t position;                       // index of the owning top-level decl in declsOrStmts
        std::optional<size_t> enclosing;       // node index of the enclosing type, if nested
        std::optional<size_t> extendsNode;     // edges to declared-here types only
        std::vector<size_t> implementsNodes;
    };
    std::vector<TypeLinkNode> buildTypeLinkGraph(
        const std::vector<std::variant<ptr<ast::Declaration>, ptr<ast::Statement>>>& declsOrStmts);
    void emitForwardTypeRelink(const std::vector<TypeLinkNode>& nodes, size_t completed);
    // Compile-time half of forward declarations: visit(TypeDecl) seeds a
    // child's propertyNames from typePropertyRegistry[super], which is only
    // filled when the super's body compiles.  Pre-register every top-level
    // type's members from the AST (closed over in-file `extends`) before any
    // body compiles, so bare inherited names resolve in a forward child too.
    void preRegisterTypeMembers(
        const std::vector<std::variant<ptr<ast::Declaration>, ptr<ast::Statement>>>& declsOrStmts);

    // Side-effect-free discovery of what `name` resolves to, ignoring consts.
    // Walks the same branches as namedVariable(), in the same order, but does
    // not emit and does not capture upvalues.
    Candidate findBinding(const ustring& name);
    int scopeIndexOf(Scope s) { return int(s - lexicalScopes.begin()); }
    void declareVariable(const ustring& name, std::optional<VarTypeSpec> type = std::nullopt);
    void declareConstant(const ustring& name, const Value& value, std::optional<VarTypeSpec> type = std::nullopt);
    void defineVariable(uint16_t moduleVar = 0, bool isConst = false); // moduleVar unused if defining a local
    bool namedVariable(const ustring& name, bool assign=false, bool asSignal=false);
    void namedModuleVariable(const ustring& name, bool assign=false);
    CallSpec buildCallSpec(const ptr<ast::Call>& ast);
    bool isRemoteActorConstructorCall(const ptr<ast::Expression>& expr) const;
    void emitRemoteActorConstructorCall(const ptr<ast::Call>& callAst, const ptr<ast::Expression>& hostExpr);

    // Per-member view used by `proc init(*)` synthesis. Abstracts over plain
    // data `var` declarations and accessor-equipped declarations so the
    // prologue iterates members in source-declaration order regardless of
    // which AST list they came from.
    struct StarInitMember {
        ustring name;
        std::optional<ast::VarType> declaredType;
        std::optional<ptr<ast::Expression>> initializer;
        // The actual property name written by the SetProp opcode. Equals
        // `name` for plain data props; `_<name>` for accessor-equipped props
        // (writes go directly to the synthetic backing field, bypassing the
        // user setter).
        ustring storageName;
        bool isConst { false };
    };

    // Synthesize parameters + assignment prologue for `proc init(*)`. Called
    // from visit(Function) when the function is a star-init. Declares one
    // local per public property (in source-declaration order — data and
    // accessor-equipped props interleaved as written), populates the
    // surrounding function's FuncType params accordingly, sets up
    // paramDefaultFunc entries for every param (explicit initializer if
    // present, otherwise the type-construction default), and emits
    // `this.<storageName> = <param>` for each.
    void emitStarInitPrologue(const std::vector<StarInitMember>& members);

    std::optional<VarTypeSpec> localVarType(const ustring& name);
    std::optional<VarTypeSpec> moduleVarType(const ustring& name);
    // Result of a compile-time const lookup: a copy of the binding (Value +
    // declaration line — the map in FunctionScope::constBindings stays the
    // owner) plus where in the scope stack it was found.
    struct ConstLookup {
        FunctionScope::ConstBinding binding;
        LexicalRank rank;
    };
    // Raw const lookup: innermost compile-time const binding named `name`, with
    // no regard to whether an inner local / member shadows it.  Most callers
    // want visibleConstBinding() instead; the remaining raw call sites are
    // commented as intentionally raw.
    std::optional<ConstLookup> lookupConstBinding(const ustring& name) const;
    // Shadowing-aware const lookup: the const binding for `name` unless a
    // non-const binding at `shadowRank` is more inner.  The one-argument form
    // runs findBinding() itself; namedVariable() passes the rank of the
    // candidate it has already discovered so discovery runs once.
    std::optional<ConstLookup> visibleConstBinding(const ustring& name, LexicalRank shadowRank) const;
    std::optional<ConstLookup> visibleConstBinding(const ustring& name);
    bool constExistsInCurrentScope(const ustring& name) const;
    bool moduleConstExists(const ustring& name) const;
    Value evaluateConstExpression(ptr<ast::Expression> expr, bool strictContext);
    Value applyConstType(Value value, std::optional<VarTypeSpec> type, bool strictContext);

    // Stack of enclosing loops, used to resolve break/continue jump targets.
    // bodyScopeDepth is the scopeDepth at the loop body's entry — break/continue
    // emit Pop opcodes for any locals declared at greater depth before jumping.
    // For while: continue emits a backward Loop opcode directly to loopStart.
    // For for: continue emits a forward Jump (target is before the increment),
    //   patched by the for-stmt visitor after body emission.
    struct LoopContext {
        int bodyScopeDepth;
        bool isForLoop;
        Chunk::size_type whileLoopStart;  // unused for for-loops
        std::vector<Chunk::size_type> breakOffsets;
        std::vector<Chunk::size_type> continueOffsets;  // for-loops only
    };
    std::vector<LoopContext> loopStack;

    void emitPopsForLoopExit(int targetDepth);

};


std::ostream& operator<<(std::ostream& out, const RoxalCompiler::ModuleInfo& mi);


}
