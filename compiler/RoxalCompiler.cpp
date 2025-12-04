#include <filesystem>
#include <system_error>
#include <boost/algorithm/string/replace.hpp>

#include <core/common.h>

#include <cstdint>
#include <fstream>
#include <functional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Object.h"

#include "ASTGenerator.h"
#include "TypeDeducer.h"
#include "VM.h"
#include "Error.h"

#include "RoxalCompiler.h"

using namespace roxal;
using namespace roxal::ast;
using ast::Access;

namespace {

constexpr char ModuleCacheMagic[4] = {'R', 'O', 'X', 'C'};
constexpr std::uint32_t ModuleCacheVersion = 16;

std::filesystem::path moduleCachePathFor(const std::filesystem::path& sourcePath) {
    if (sourcePath.empty())
        return {};

    std::filesystem::path directory = sourcePath.parent_path();
    std::string stem = sourcePath.stem().string();
    if (stem.empty())
        stem = sourcePath.filename().string();

    std::string cacheFilename = "." + stem + ".roc";
    return directory / cacheFilename;
}

// Compose a dotted module name from the package path and the leaf module.
icu::UnicodeString makeFullModuleName(const icu::UnicodeString& packagePath,
                                      const icu::UnicodeString& moduleName) {
    icu::UnicodeString full;
    if (!packagePath.isEmpty()) {
        full = packagePath;
        for (int32_t i = 0; i < full.length(); ++i) {
            if (full.charAt(i) == '/')
                full.setCharAt(i, '.');
        }
    }
    if (!moduleName.isEmpty()) {
        if (!full.isEmpty())
            full += ".";
        full += moduleName;
    }
    return full.isEmpty() ? moduleName : full;
}

} // namespace





// is ptr<P> p down-castable to ptr<C> where C is a subclass of P (or the same class)?
template<typename P, typename C>
bool isa(ptr<P> p) {
    if (p==nullptr) return false;
    return dynamic_ptr_cast<C>(p)!=nullptr;
}

template<typename C>
bool isa(ptr<AST> p) {
    if (p==nullptr) return false;
    return dynamic_ptr_cast<C>(p)!=nullptr;
}

// down-cast ptr<P> p to ptr<C> where C is a subclass of P (or the same class)
template<typename P, typename C>
ptr<C> as(ptr<P> p) {
    if (!isa<P,C>(p))
        throw std::runtime_error("Can't cast ptr<"+demangle(typeid(*p).name())+"> to ptr<"+demangle(typeid(C).name())+">");
    return dynamic_ptr_cast<C>(p);
}

template<typename C>
ptr<C> as(ptr<AST> p) {
    if (!isa<AST,C>(p))
        throw std::runtime_error("Can't cast ptr<"+demangle(typeid(*p).name())+"> to ptr<"+demangle(typeid(C).name())+">");
    return dynamic_ptr_cast<C>(p);
}


RoxalCompiler::RoxalCompiler()
    : outputBytecodeDisassembly(false)
    , cacheReadEnabled(true)
    , cacheWriteEnabled(true)
    , moduleResolverVM(nullptr)
{}



Value RoxalCompiler::compile(std::istream& source, const std::string& name,
                             Value existingModule)
{
    Value function { Value::nilVal() };

    ptr<ast::AST> ast {};
    try {
        ASTGenerator astGenerator {};
        ast = astGenerator.ast(source, name);
    } catch (std::exception& e) {
        compileError(e.what());
        clearCompileContext();
        return function;
    }

    if (!isa<File>(ast))
        throw std::runtime_error("ASTGenerator root node is not a File");

    try {
        // In REPL mode, use a persistent TypeDeducer to maintain type info across lines
        if (replModeFlag) {
            if (!replTypeDeducer) {
                replTypeDeducer = make_ptr<TypeDeducer>();
                replTypeDeducer->setReplMode(true);
            }
            replTypeDeducer->visit(as<File>(ast));
        } else {
            TypeDeducer typeDeducer {};
            typeDeducer.visit(as<File>(ast));
        }
    } catch (std::exception& e) {
        compileError(e.what());
        clearCompileContext();
        return function;
    }


    #if defined(DEBUG_OUTPUT_PARSE_TREE)
    std::cout << "== parse tree ==" << std::endl << ast << std::endl;
    #endif


    if (ast != nullptr) {

        std::filesystem::path p{name};
        std::string moduleName = p.stem().filename().string();
        enterModuleScope("", toUnicodeString(moduleName), toUnicodeString(name), existingModule);

        auto module { asModuleScope(moduleScope()) };

        bool strictContext = false;
        if (auto file = dynamic_ptr_cast<ast::File>(ast)) {
            for (const auto& annot : file->annotations) {
                if (annot->name == "strict")
                    strictContext = true;
                else if (annot->name == "nonstrict")
                    strictContext = false;
            }
        }

        module->strict = strictContext;
        asFunction(module->function)->strict = strictContext;

        try {
            auto file = as<File>(ast);

            file->accept(*this);

            function = module->function;

            debug_assert_msg(!function.isNil() && isFunction(function),"Value holds function");

            if (outputBytecodeDisassembly)
                asFunction(module->function)->chunk->disassemble(asFunction(module->function)->name);

            //std::cout << "value:" << value->repr() << std::endl;
        } catch (std::logic_error& e) {
            compileError(e.what());

            while (!lexicalScopes.empty() && (*scope())->isFunc() && !(*scope())->isModule())
                exitFuncScope();

            while (inTypeScope())
                exitTypeScope();

            exitModuleScope();

            clearCompileContext();

            return Value::nilVal();
        } catch (std::exception& e) {
            compileError(e.what());

            while (!lexicalScopes.empty() && (*scope())->isFunc() && !(*scope())->isModule())
                exitFuncScope();

            while (inTypeScope())
                exitTypeScope();

            exitModuleScope();

            clearCompileContext();

            throw e;
        }

        exitModuleScope();

        clearCompileContext();

        //std::cout << "\n" << interpreter.stackAsString(false) << std::endl;
    }

    return function;
}

Value RoxalCompiler::loadFileCache(const std::filesystem::path& sourcePath) const
{
    if (!cacheReadEnabled)
        return Value::nilVal();

    if (sourcePath.empty())
        return Value::nilVal();

    try {
        std::filesystem::path resolved = std::filesystem::absolute(sourcePath);
        if (!std::filesystem::exists(resolved))
            return Value::nilVal();

        resolved = std::filesystem::canonical(resolved);
        if (resolved.extension() != ".rox")
            return Value::nilVal();

        std::filesystem::path cachePath = moduleCachePathFor(resolved);
        if (cachePath.empty())
            return Value::nilVal();
        if (!std::filesystem::exists(cachePath))
            return Value::nilVal();

        auto sourceTime = std::filesystem::last_write_time(resolved);
        auto cacheTime = std::filesystem::last_write_time(cachePath);
        if (cacheTime < sourceTime)
            return Value::nilVal();

        ModuleInfo module{};
        module.cachePath = cachePath;
        return loadModuleFromCache(module);
    } catch (...) {
        return Value::nilVal();
    }
}

void RoxalCompiler::storeFileCache(const std::filesystem::path& sourcePath, const Value& function) const
{
    if (!cacheWriteEnabled || function.isNil() || !isFunction(function))
        return;

    try {
        std::filesystem::path resolved = std::filesystem::absolute(sourcePath);
        if (!std::filesystem::exists(resolved))
            return;

        resolved = std::filesystem::canonical(resolved);
        if (resolved.extension() != ".rox")
            return;

        ModuleInfo module{};
        module.cachePath = moduleCachePathFor(resolved);
        if (module.cachePath.empty())
            return;
        storeModuleCache(module, function);
    } catch (...) {
        // ignore cache write failures
    }
}

void RoxalCompiler::reconcileModuleReferences(const Value& function) const
{
    if (function.isNil() || !isFunction(function))
        return;

    VM* resolverVM = moduleResolverVM;
    if (resolverVM == nullptr)
        resolverVM = &VM::instance();

    // Helpers --------------------------------------------------------------

    auto mergeModuleTypes = [](ObjModuleType* target, ObjModuleType* source) {
        if (target == nullptr || source == nullptr || target == source)
            return;

        if (!source->fullName.isEmpty())
            target->fullName = source->fullName;
        if (!source->sourcePath.isEmpty())
            target->sourcePath = source->sourcePath;

        target->vars.clear();
        for (const auto& entry : source->vars.snapshot())
            target->vars.store(entry, true);

        target->constVars = source->constVars;

        target->clearModuleAliases();
        for (const auto& alias : source->moduleAliasSnapshot())
            target->registerModuleAlias(alias.first, alias.second);

        target->cstructArch = source->cstructArch;
        target->propertyCTypes = source->propertyCTypes;
    };

    auto toKey = [](const icu::UnicodeString& value) {
        std::string result;
        value.toUTF8String(result);
        return result;
    };

    auto moduleQualifiedName = [&](ObjModuleType* module) {
        if (module->fullName.isEmpty())
            return module->name;
        return module->fullName;
    };

    auto canonicalizeModuleValue = [&](const Value& moduleValue) -> Value {
        Value strong = moduleValue.strongRef();
        if (!isModuleType(strong))
            return strong;

        ObjModuleType* module = asModuleType(strong);
        icu::UnicodeString qualified = moduleQualifiedName(module);
        Value builtin = resolverVM->getBuiltinModuleType(qualified);
        if (builtin.isNil())
            builtin = resolverVM->getBuiltinModuleType(module->name);
        if (builtin.isNonNil()) {
            mergeModuleTypes(asModuleType(builtin), module);
            return builtin.strongRef();
        }

        return strong;
    };

    // Walk every function owned by the entry chunk and collect the module
    // types they reference (both directly and via nested functions).  At the
    // same time, remember any alias information recorded on the module so we
    // can restore the import table after we rebuild the canonical module
    // hierarchy below.
    std::unordered_set<ObjFunction*> visited;
    std::vector<ObjFunction*> stack;
    using AliasList = std::vector<std::pair<icu::UnicodeString, icu::UnicodeString>>;
    std::unordered_map<ObjModuleType*, AliasList> moduleImports;
    std::unordered_map<std::string, Value> canonicalModules;

    auto enqueueFunction = [&](const Value& fnValue) {
        if (!isFunction(fnValue))
            return;

        ObjFunction* candidate = asFunction(fnValue);
        if (visited.insert(candidate).second)
            stack.push_back(candidate);
    };

    enqueueFunction(function);

    while (!stack.empty()) {
        ObjFunction* fn = stack.back();
        stack.pop_back();

        if (!isModuleType(fn->moduleType) || fn->chunk == nullptr)
            continue;

        Value fnModuleValue = canonicalizeModuleValue(fn->moduleType);
        fn->moduleType = fnModuleValue.weakRef();
        ObjModuleType* moduleType = asModuleType(fnModuleValue);

        std::unordered_set<int32_t> importHashes;
        AliasList imports;

        auto aliasSnapshot = moduleType->moduleAliasSnapshot();
        for (const auto& alias : aliasSnapshot) {
            if (importHashes.insert(alias.first.hashCode()).second)
                imports.emplace_back(alias.first, alias.second);
        }

        if (imports.empty()) {
            // Fall back to the variable table when the module did not record
            // explicit alias metadata (this covers older cache files or
            // modules that populated the table manually).
            for (const auto& entry : moduleType->vars.snapshot()) {
                const icu::UnicodeString& name = entry.first;
                if (importHashes.insert(name.hashCode()).second)
                    imports.emplace_back(name, icu::UnicodeString());
            }
        }

        for (auto& constant : fn->chunk->constants) {
            if (isFunction(constant)) {
                enqueueFunction(constant);
                Value moduleTypeValue = asFunction(constant)->moduleType;
                if (isModuleType(moduleTypeValue)) {
                    Value moduleValue = canonicalizeModuleValue(moduleTypeValue);
                    asFunction(constant)->moduleType = moduleValue.weakRef();
                    if (isModuleType(moduleValue)) {
                        ObjModuleType* imported = asModuleType(moduleValue);
                        canonicalModules[toKey(moduleQualifiedName(imported))] = moduleValue.strongRef();
                    }
                }
            } else if (isModuleType(constant)) {
                Value moduleValue = canonicalizeModuleValue(constant);
                constant = moduleValue;
                if (isModuleType(moduleValue)) {
                    ObjModuleType* imported = asModuleType(moduleValue);
                    canonicalModules[toKey(moduleQualifiedName(imported))] = moduleValue.strongRef();
                }
            }
        }

        moduleImports[moduleType] = std::move(imports);
    }

    std::unordered_map<std::string, Value> ensuredModules;

    std::function<Value(const icu::UnicodeString&)> ensureModuleHierarchy =
        [&](const icu::UnicodeString& fullName) -> Value {
            if (fullName.isEmpty())
                return Value::nilVal();

            std::string key = toKey(fullName);
            auto ensuredIt = ensuredModules.find(key);
            if (ensuredIt != ensuredModules.end())
                return ensuredIt->second.strongRef();

            Value moduleValue { Value::nilVal() };
            auto canonicalIt = canonicalModules.find(key);
            if (canonicalIt != canonicalModules.end()) {
                moduleValue = canonicalIt->second.strongRef();
            } else {
                // Lazily create placeholder modules for missing entries so we
                // can rebuild a consistent hierarchy (e.g. when a cached
                // module references a package parent that was not serialized
                // in the cache file).
                int32_t dotIndex = fullName.lastIndexOf('.');
                icu::UnicodeString localName = dotIndex >= 0 ? fullName.tempSubString(dotIndex + 1)
                                                             : fullName;
                moduleValue = Value::moduleTypeVal(localName);
                ObjModuleType* created = asModuleType(moduleValue);
                created->fullName = fullName;
                ObjModuleType::allModules.push_back(moduleValue);
            }

            ObjModuleType* moduleType = asModuleType(moduleValue);
            if (moduleType->fullName.isEmpty())
                moduleType->fullName = fullName;

            ensuredModules.emplace(key, moduleValue.strongRef());
            canonicalModules[key] = moduleValue.strongRef();

            int32_t dotIndex = fullName.lastIndexOf('.');
            if (dotIndex >= 0) {
                icu::UnicodeString parentFullName = fullName.tempSubString(0, dotIndex);
                Value parentValue = ensureModuleHierarchy(parentFullName);
                if (parentValue.isNonNil()) {
                    icu::UnicodeString alias = fullName.tempSubString(dotIndex + 1);
                    ObjModuleType* parentModule = asModuleType(parentValue);
                    // Recreate the parent->child relationship so lookups on
                    // the parent module continue to work as they did during
                    // the original compile.
                    parentModule->vars.store(alias, moduleValue, true);
                    parentModule->registerModuleAlias(alias, fullName);
                }
            }

            return moduleValue.strongRef();
        };

    for (const auto& canonicalEntry : canonicalModules)
        ensureModuleHierarchy(icu::UnicodeString::fromUTF8(canonicalEntry.first));

    for (const auto& entry : moduleImports) {
        ObjModuleType* moduleType = entry.first;

        std::unordered_map<int32_t, icu::UnicodeString> previousAliases;
        for (const auto& alias : moduleType->moduleAliasSnapshot())
            previousAliases.emplace(alias.first.hashCode(), alias.second);

        moduleType->vars.clear();
        moduleType->clearModuleAliases();

        for (const auto& alias : entry.second) {
            const icu::UnicodeString& aliasName = alias.first;
            icu::UnicodeString aliasFullName = alias.second;
            if (aliasFullName.isEmpty()) {
                auto fallback = previousAliases.find(aliasName.hashCode());
                if (fallback != previousAliases.end())
                    aliasFullName = fallback->second;
            }
            if (aliasFullName.isEmpty())
                aliasFullName = aliasName;

            Value moduleValue = ensureModuleHierarchy(aliasFullName);
            if (moduleValue.isNonNil()) {
                // Re-populate the module with the canonical module reference
                // and re-register the alias so subsequent cache loads know
                // where the import originated.
                moduleType->vars.store(aliasName, moduleValue, true);
                moduleType->registerModuleAlias(aliasName, aliasFullName);
            }
        }
    }
}


void RoxalCompiler::setOutputBytecodeDisassembly(bool outputBytecodeDisassembly)
{
    this->outputBytecodeDisassembly = outputBytecodeDisassembly;
}

void RoxalCompiler::setModulePaths(const std::vector<std::string>& modulePaths)
{
    this->modulePaths = modulePaths;
}

void RoxalCompiler::setReplMode(bool replMode)
{
    this->replModeFlag = replMode;
}

void RoxalCompiler::setCacheReadEnabled(bool enabled)
{
    cacheReadEnabled = enabled;
}

void RoxalCompiler::setCacheWriteEnabled(bool enabled)
{
    cacheWriteEnabled = enabled;
}

void RoxalCompiler::setModuleResolverVM(VM* vm)
{
    moduleResolverVM = vm;
}


ASTVisitor::TraversalOrder RoxalCompiler::traversalOrder() const
{
    // we don't want AST implemented pre- or post-order tree traversal.
    //  instead, we'll dictate the traversal order by explicitly calling visit() on children
    return TraversalOrder::VisitorDetermined;
}






std::any RoxalCompiler::visit(ptr<ast::File> ast)
{
    currentNode = ast;
    Anys results {};
    ast->acceptChildren(*this, results);
    emitReturn();
    return {};
}


std::any RoxalCompiler::visit(ptr<ast::SingleInput> ast)
{
    currentNode = ast;
    Anys results {};
    ast->acceptChildren(*this, results);
    return results;
}


std::any RoxalCompiler::visit(ptr<ast::Annotation> ast)
{
    currentNode = ast;
    // currently, we don't generate any code for annotations
    //ast->acceptChildren(*this);
    return {};
}


std::any RoxalCompiler::visit(ptr<ast::Import> ast)
{

    currentNode = ast;

    // search the module paths (as package component roots)
    //  for the specified module
    ModuleInfo module = findImport(ast->packages);
    bool builtinModule = false;

    // Check if this is a builtin module (even if a file also exists)
    if (ast->packages.size() == 1) {
        icu::UnicodeString modName { ast->packages[0] };
        if (VM::instance().getBuiltinModuleType(modName).isNonNil()) {
            module.name = modName;
            builtinModule = true;
        }
    }

    if (!builtinModule && module.name.isEmpty()) {
        if (module.invalidFolder) {
            error("import '"+toUTF8StdString(join(ast->packages,"."))+"' not found: folder lacks init.rox or a single .rox file");
        } else {
            error("import '"+toUTF8StdString(join(ast->packages,"."))+"' not found.");
        }
        return {};
    }

    std::string absoluteModuleFilePath;
    icu::UnicodeString moduleFullName = makeFullModuleName(module.packagePath, module.name);
    if (!builtinModule) {
        if (!module.resolvedPath.empty()) {
            absoluteModuleFilePath = module.resolvedPath.string();
        } else {
            absoluteModuleFilePath = std::filesystem::canonical(std::filesystem::absolute(
                module.modulePathRoot + "/" + toUTF8StdString(module.packagePath) + '/' + module.filename));
        }

        // extra check the module file exists
        if (absoluteModuleFilePath.empty() || !std::filesystem::exists(std::filesystem::path(absoluteModuleFilePath))) {
            error("import file '"+toUTF8StdString(module.packagePath) + '/' + module.filename+"' not found.");
            return {};
        }
    }

    // has this module already been imported?
    auto importedEntry = importedModules.find(module);
    bool imported = importedEntry != importedModules.end();

    //std::cout << module << std::endl;

    Value importedModuleType {};

    if (!imported) {  // import it
        if (builtinModule) {
            ptr<BuiltinModule> importedModule = VM::instance().getBuiltinModule(module.name);
            if (!importedModule)
                throw std::runtime_error("builtin module '" + toUTF8StdString(module.name) + "' not registered");
            importedModuleType = importedModule->moduleType();
            importedModuleType = importedModuleType.strongRef();
            importedModules[module] = importedModuleType;

            if (isModuleType(importedModuleType)) {
                bool hasConstant = false;
                for (const auto& constant : currentChunk()->constants) {
                    if (constant.is(importedModuleType, true)) {
                        hasConstant = true;
                        break;
                    }
                }
                if (!hasConstant)
                    makeConstant(importedModuleType);
                ObjModuleType* builtinType = asModuleType(importedModuleType);
                if (builtinType->fullName.isEmpty())
                    builtinType->fullName = moduleFullName;

                // Check if the builtin module has an _init() function and call it if so
                // This allows builtin modules to perform native initialization when imported
                auto initFnOpt = builtinType->vars.load(toUnicodeString("_init"));
                if (initFnOpt.has_value() && isClosure(initFnOpt.value())) {
                    // Emit code to call module._init()
                    // The value is already a closure, so just load it as a constant and call it
                    emitConstant(*initFnOpt, "_init closure");

                    CallSpec callSpec {};
                    callSpec.allPositional = true;
                    callSpec.argCount = 0;
                    auto bytes = callSpec.toBytes();
                    assert(bytes.size()==1);
                    emitBytes(OpCode::Call, bytes[0]);

                    // Pop the return value (we don't need it)
                    emitByte(OpCode::Pop);
                }
            }
        } else if (module.isProto) {
            try {
#ifdef ROXAL_ENABLE_GRPC
                importedModuleType = VM::instance().importProtoModule(absoluteModuleFilePath);
                importedModules[module] = importedModuleType;
#else
                throw std::runtime_error("proto import requires ROXAL_ENABLE_GRPC");
#endif
            } catch (std::exception& e) {
                error(e.what());
                return {};
            }
        } else {
            // compile or load it, emit code to execute it
            Value function { Value::nilVal() }; // ObjFunction
            bool prevRepl = replModeFlag;
            bool loadedFromCache = false;

            try {
                if (module.cacheValid)
                    function = loadModuleFromCache(module);

                if (function.isNonNil())
                    loadedFromCache = true;

                if (!loadedFromCache) {
                    std::ifstream sourcestream(absoluteModuleFilePath);
                    if (!sourcestream.is_open())
                        throw std::runtime_error("unable to open module source: " + absoluteModuleFilePath);

                    replModeFlag = false; // don't auto-print expressions when compiling imported module
                    function = compile(sourcestream,
                                       !absoluteModuleFilePath.empty() ?
                                              absoluteModuleFilePath
                                            : toUTF8StdString(module.name)
                                      );
                    storeModuleCache(module, function);
                }

                replModeFlag = prevRepl;

                importedModuleType = asFunction(function)->moduleType;
                if (isModuleType(importedModuleType)) {
                    ObjModuleType* imported = asModuleType(importedModuleType);
                    imported->fullName = moduleFullName;
                }

                // emit code to place module's main chunk on stack as closure
                assert(asFunction(function)->upvalueCount == 0);
                {
                    uint16_t constIdx = makeConstant(function);
                    emitOpArgsBytes(OpCode::Closure, constIdx);
                }

                // call it to have it executed (which will result in module vars being declared)
                CallSpec callSpec {};
                callSpec.allPositional = true;
                callSpec.argCount = 0;
                auto bytes = callSpec.toBytes();
                assert(bytes.size()==1);
                emitBytes(OpCode::Call, bytes[0]);

                importedModules[module] = importedModuleType;

            } catch (std::exception& e) {
                replModeFlag = prevRepl;
                error(e.what());
                return {};
            }
        }
    } else { // already previously imported
        importedModuleType = importedEntry->second;
    }

    // create or retrieve package modules and build module hierarchy
    const auto& importingModuleType = asFunction(asFuncScope(funcScope())->function)->moduleType;
    auto& importingModuleVars = asModuleType(importingModuleType)->vars;

    std::vector<icu::UnicodeString> importComponents;
    if (module.isProto) {
        // split packagePath on '/'
        std::string pkg = toUTF8StdString(module.packagePath);
        std::stringstream ss(pkg);
        std::string item;
        while (std::getline(ss, item, '/')) {
            if (!item.empty())
                importComponents.push_back(toUnicodeString(item));
        }
        importComponents.push_back(module.name);
    } else {
        importComponents = ast->packages;
    }

    Value parentModuleVal { Value::nilVal() };
    icu::UnicodeString packagePath;
    for(size_t i=0; i+1 < importComponents.size(); ++i) {
        icu::UnicodeString pkgName { importComponents[i] };
        ModuleInfo pkgInfo;
        pkgInfo.modulePathRoot = module.modulePathRoot;
        pkgInfo.packagePath = packagePath;
        pkgInfo.name = pkgName;
        pkgInfo.isPackage = true;

        Value pkgModuleVal {};
        auto pkgEntry = importedModules.find(pkgInfo);
        if (pkgEntry == importedModules.end()) {
            pkgModuleVal = Value::moduleTypeVal(pkgName);
            ObjModuleType::allModules.push_back(pkgModuleVal);
            importedModules[pkgInfo] = pkgModuleVal;
        } else {
            pkgModuleVal = pkgEntry->second;
        }

        ObjModuleType* pkgModule = asModuleType(pkgModuleVal);
        icu::UnicodeString pkgFullName = makeFullModuleName(pkgInfo.packagePath, pkgName);
        pkgModule->fullName = pkgFullName;

        if (parentModuleVal.isObj()) {
            ObjModuleType* parentModule = asModuleType(parentModuleVal);
            parentModule->vars.store(pkgName, pkgModuleVal);
            parentModule->registerModuleAlias(pkgName, pkgFullName);
        } else {
            importingModuleVars.store(pkgName, pkgModuleVal);
            ObjModuleType* importingModule = asModuleType(importingModuleType);
            importingModule->registerModuleAlias(pkgName, pkgFullName);
        }

        parentModuleVal = pkgModuleVal;
        if (!packagePath.isEmpty())
            packagePath += "/";
        packagePath += pkgName;
    }

    if (parentModuleVal.isObj()) {
        ObjModuleType* parentModule = asModuleType(parentModuleVal);
        parentModule->vars.store(module.name, importedModuleType);
        parentModule->registerModuleAlias(module.name, moduleFullName);
    }

    // For non-nested imports expose the module directly in the importing module
    if (importComponents.size() <= 1) {
        icu::UnicodeString moduleName { module.name };
        importingModuleVars.store(moduleName, importedModuleType);
        ObjModuleType* importingModule = asModuleType(importingModuleType);
        importingModule->registerModuleAlias(moduleName, moduleFullName);
    }


    // if any (or all) symbols are explicitly imported into the importing module scope,
    //  create vars for those too
    if (!ast->symbols.empty()) {

        // convert AST symbols to a List of Values
        std::vector<Value> symbolsList {};
        for(const auto& symbol : ast->symbols)
            symbolsList.push_back(Value::stringVal(symbol));

        Value symbolsListVal { Value::listVal() };
        asList(symbolsListVal)->elts = symbolsList;

        // Opcode::ImportModuleVars expects a list (of symbols) and the source module & target module
        emitConstant(symbolsListVal, "import vars "+toUTF8StdString(join(ast->symbols)));

        emitConstant(importedModuleType, "imported module type "+toUTF8StdString(module.name));
        emitConstant(importingModuleType, "importing module type "+toUTF8StdString(asModuleType(importingModuleType)->name));

        emitByte(OpCode::ImportModuleVars);
    }


    return {};
}



std::any RoxalCompiler::visit(ptr<ast::TypeDecl> ast)
{
    currentNode = ast;

    if (ast->kind == TypeDecl::Event) {
        enterTypeScope(ast->name);

        if (!ast->methods.empty())
            error("Events cannot declare methods");
        if (!ast->implements.empty())
            error("Events cannot implement interfaces");

        if (ast->extends.has_value()) {
            auto superName = ast->extends.value();
            auto it = typePropertyRegistry.find(superName);
            if (it != typePropertyRegistry.end())
                asTypeScope(typeScope())->propertyNames.insert(it->second.begin(), it->second.end());
        }

        uint16_t typeNameConstant = identifierConstant(ast->name);
        declareVariable(ast->name);
        emitOpArgsBytes(OpCode::EventType, typeNameConstant);
        defineVariable(typeNameConstant);

        auto moduleScopePtr = asModuleScope(moduleScope());
        ObjModuleType* moduleTypeObj = asModuleType(moduleScopePtr->moduleType);
        moduleScopePtr->moduleConstLines[ast->name] = currentNode->interval.first;
        moduleTypeObj->constVars.insert(ast->name.hashCode());

        if (ast->extends.has_value()) {
            asTypeScope(typeScope())->hasSuperType = true;
            asTypeScope(typeScope())->superTypeName = ast->extends.value();

            namedVariable(ast->extends.value(), false);
            enterLocalScope();
            addLocal("super");
            defineVariable(0);
            namedVariable(ast->name, false);
            emitByte(OpCode::EventExtend);
        }

        namedVariable(ast->name, false);

        for (const auto& prop : ast->properties) {
            if (prop->access == Access::Private)
                error("Event payload member '" + toUTF8StdString(prop->name) + "' cannot be private");

            auto propName { prop->name };
            uint16_t propNameConstant = identifierConstant(propName);
        asTypeScope(typeScope())->propertyNames[propName] = {prop->access, ast->name, prop->isConst};

            if (prop->varType.has_value()) {
                auto varType { prop->varType.value() };
                if (std::holds_alternative<BuiltinType>(varType)) {
                    auto builtinType { std::get<BuiltinType>(varType) };
                    // Events cannot have signal members
                    if (builtinType == BuiltinType::Signal)
                        error("Event payload member '" + toUTF8StdString(propName) + "' cannot be typed as signal");
                    Value typeValue { Value::typeSpecVal(builtinToValueType(builtinType)) };
                    emitConstant(typeValue, "event payload " + toUTF8StdString(propName) + " type");
                } else {
                    namedVariable(std::get<icu::UnicodeString>(varType), false);
                }
            } else {
                emitByte(OpCode::ConstNil, "event payload " + toUTF8StdString(propName) + " (no type)");
            }

            if (prop->initializer.has_value()) {
                prop->initializer.value()->accept(*this);
            } else {
                bool declaredBuiltinType = prop->varType.has_value() && std::holds_alternative<BuiltinType>(prop->varType.value());
                if (declaredBuiltinType) {
                    auto bt = std::get<BuiltinType>(prop->varType.value());
                    if (bt == BuiltinType::Signal)
                        error("Can't default-construct signal");
                    emitConstant(defaultValue(builtinToValueType(bt)));
                } else {
                    emitByte(OpCode::ConstNil);
                }
            }

            emitOpArgsBytes(OpCode::EventPayload, propNameConstant, "event payload " + toUTF8StdString(propName));
        }

        emitByte(OpCode::Pop, "event type");

        if (asTypeScope(typeScope())->hasSuperType)
            exitLocalScope();

        typePropertyRegistry[ast->name] = asTypeScope(typeScope())->propertyNames;

        exitTypeScope();
        return {};
    }

    bool isActor = ast->kind==TypeDecl::Actor;
    bool isInterface = ast->kind==TypeDecl::Interface;
    bool isEnumeration = ast->kind==TypeDecl::Enumeration;

    // check for @cstruct annotation
    for(const auto& annot : ast->annotations) {
        if (annot->name == "cstruct") {
            int arch = hostArch;
            for(const auto& arg : annot->args) {
                if (toUTF8StdString(arg.first) == "arch") {
                    if (auto n = dynamic_ptr_cast<ast::Num>(arg.second)) {
                        arch = std::get<int32_t>(n->num);
                    }
                }
            }
            ObjModuleType* mod = asModuleType(asModuleScope(moduleScope())->moduleType);
            mod->cstructArch[ast->name.hashCode()] = arch;
        }
    }

    enterTypeScope(ast->name);
    asTypeScope(typeScope())->isActor = isActor;

    // inherit property registry from super type if available
    if (ast->extends.has_value()) {
        auto superName = ast->extends.value();
        auto it = typePropertyRegistry.find(superName);
        if (it != typePropertyRegistry.end())
            asTypeScope(typeScope())->propertyNames.insert(it->second.begin(), it->second.end());
    }

    uint16_t typeNameConstant = identifierConstant(ast->name);
    declareVariable(ast->name);

    if (ast->implements.size()>2)
        throw std::runtime_error("Multiple implements types unimplemented.");

    if (isInterface && (ast->implements.size() > 0))
        throw std::runtime_error("Interfaces can't implement (only extend)");

    // Write type opcode with automatic single/double-byte argument handling
    if (isActor) emitOpArgsBytes(OpCode::ActorType, typeNameConstant);
    else if (isInterface) emitOpArgsBytes(OpCode::InterfaceType, typeNameConstant);
    else if (isEnumeration) emitOpArgsBytes(OpCode::EnumerationType, typeNameConstant);
    else emitOpArgsBytes(OpCode::ObjectType, typeNameConstant);
    defineVariable(typeNameConstant);

    auto moduleScopePtr = asModuleScope(moduleScope());
    ObjModuleType* moduleTypeObj = asModuleType(moduleScopePtr->moduleType);
    moduleScopePtr->moduleConstLines[ast->name] = currentNode->interval.first;
    moduleTypeObj->constVars.insert(ast->name.hashCode());


    // handle extension (inheritance)
    if (ast->extends.has_value() && !isEnumeration) {
        asTypeScope(typeScope())->hasSuperType = true;
        asTypeScope(typeScope())->superTypeName = ast->extends.value();

        auto superTypeName = ast->extends.value();

        // can't inherit yourself
        if (superTypeName == ast->name)
            error("Type object, actor or interface '"+toUTF8StdString(ast->name)+"' can't extend itself.");

        namedVariable(superTypeName, /*assign=*/false); // parent (super)

        enterLocalScope();
        addLocal("super");
        defineVariable(0);

        namedVariable(ast->name, /*assign=*/false); // child (sub)
        emitByte(OpCode::Extend);
    }


    namedVariable(ast->name, false); // make type accessible on the stack

    for(size_t i=0; i<ast->properties.size(); i++) {

        if (isInterface) {
            error("Interfaces cannot declare properties");
            break;
        }

        ptr<VarDecl> prop { ast->properties.at(i) };

        if (isActor && prop->access != Access::Private) {
            error("Actors cannot declare shared properties (use private)");
            break;
        }

        // emit code to push type & initial value (if any) on stack, then OpCode::Property

        auto propName { prop->name };
        uint16_t propNameConstant = identifierConstant(propName);

        // record property name for implicit access within methods
        asTypeScope(typeScope())->propertyNames[propName] = {prop->access, ast->name, prop->isConst};

        // store @ctype annotation
        for(const auto& a : prop->annotations) {
            if (a->name == "ctype") {
                for(const auto& arg : a->args) {
                    if (toUTF8StdString(arg.first) == "ctype") {
                        if (auto s = dynamic_ptr_cast<ast::Str>(arg.second)) {
                            ObjModuleType* mod = asModuleType(asModuleScope(moduleScope())->moduleType);
                            mod->propertyCTypes[ast->name.hashCode()][propName.hashCode()] = s->str;
                        }
                    }
                }
            }
        }

        // type
        if (prop->varType.has_value()) {
            auto varType { prop->varType.value() };

            if (std::holds_alternative<BuiltinType>(varType)) {
                auto builtinType { std::get<BuiltinType>(varType) };
                Value typeValue { Value::typeSpecVal(builtinToValueType(builtinType)) };

                emitConstant(typeValue, "prop "+toUTF8StdString(propName)+" type");
            }
            else { // assume string names module scope (local?) type var
                // will emit GetLocal or GetModuleVar (or GetUpValue)
                namedVariable( std::get<icu::UnicodeString>(varType), false );
            }

        }
        else {
            emitByte(OpCode::ConstNil, "prop "+toUTF8StdString(propName)+" (no type)"); // nil value will be interpreted as no type (or any type)
        }

        // initial value
        if (prop->initializer.has_value()) {
            prop->initializer.value()->accept(*this);
        }
        else { // no initializer
            bool declaredBuiltinType = prop->varType.has_value() && std::holds_alternative<BuiltinType>(prop->varType.value());
            if (declaredBuiltinType) {
                auto bt = std::get<BuiltinType>(prop->varType.value());
                if (bt == BuiltinType::Signal)
                    error("Can't default-construct signal");
                emitConstant(defaultValue(builtinToValueType(bt)));
            }
            else
                emitByte(OpCode::ConstNil);
        }

        emitByte(prop->access == Access::Private ? OpCode::ConstTrue : OpCode::ConstFalse);
        emitByte(prop->isConst ? OpCode::ConstTrue : OpCode::ConstFalse);

        emitOpArgsBytes(OpCode::Property, propNameConstant, "property "+toUTF8StdString(propName));

    } // properties


    for(size_t i=0; i<ast->methods.size(); i++) {

        auto func { ast->methods.at(i) };

        assert(func->name.has_value()); // methods must have names
        auto methodName { func->name.value() };
        asTypeScope(typeScope())->propertyNames[methodName] = {func->access, ast->name, /*isConst=*/false};
        uint16_t methodNameConstant = identifierConstant(methodName);

        func->accept(*this);

        emitOpArgsBytes(OpCode::Method, methodNameConstant, "method "+toUTF8StdString(methodName));
    }


    if (isEnumeration) {

        for(size_t i=0; i<ast->enumLabels.size(); i++) {

            const auto& enumLabel { ast->enumLabels.at(i) };

            // TODO: TypeDeducer currenly adds values to enum labels if missing
            //  (but maybe this will be moved to another pass or to here)
            assert(enumLabel.second != nullptr);

            auto labelName { enumLabel.first };
            uint16_t propNameConstant = identifierConstant(labelName);

            assert(enumLabel.second->type.has_value());
            auto valType { enumLabel.second->type.value() };

            ptr<ast::Literal> literalExpr { dynamic_ptr_cast<ast::Literal>(enumLabel.second) };
            assert(literalExpr != nullptr); // currently expected to be a literal
            Value value {};
            if (literalExpr->literalType == ast::Literal::LiteralType::Num) {
                ptr<ast::Num> numExpr { dynamic_ptr_cast<ast::Num>(literalExpr) };
                if (valType->builtin == BuiltinType::Byte)
                    value = Value::byteVal(std::get<int>(numExpr->num));
                else if (valType->builtin == BuiltinType::Int)
                    value = Value::intVal(std::get<int>(numExpr->num));
                else
                    error("Unsupported literal type for enum label.");
            }
            else
                error("Unsupported literal type for enum label.");

            emitConstant(value);

            emitOpArgsBytes(OpCode::EnumLabel, propNameConstant, "enum value "+toUTF8StdString(labelName));
        }
    }


    emitByte(OpCode::Pop, "type name");

    if (asTypeScope(typeScope())->hasSuperType)
        exitLocalScope();

    // record collected property names for this type for use by derived types
    typePropertyRegistry[ast->name] = asTypeScope(typeScope())->propertyNames;

    exitTypeScope();

    return {};
}

std::any RoxalCompiler::visit(ptr<ast::FuncDecl> ast)
{
    currentNode = ast;

    auto func {as<Function>(ast->func) };
    assert(func->name.has_value()); // func declarations must have names
    auto name { func->name.value() };

    declareVariable(name);

    uint16_t var { 0 };
    if (asFuncScope(funcScope())->scopeDepth == 0) // module variable
        var = identifierConstant(name); // create constant table entry for name

    if (asFuncScope(funcScope())->scopeDepth > 0) {
        // mark initialized
        asFuncScope(funcScope())->locals.back().depth = asFuncScope(funcScope())->scopeDepth;
    }

    Anys results {};
    ast->acceptChildren(*this, results);

    // unwrap ObjFunction* returned by visit(ptr<Function>)
    auto function = std::any_cast<ObjFunction*>(std::any_cast<Anys>(results.at(0)).at(0));

    // attached the FuncDecl annotations (which appear right before the func declaration)
    //  to the function object to make them available at runtime
    function->annotations = ast->annotations;
    function->annotations.insert(function->annotations.end(), ast->func->annotations.begin(), ast->func->annotations.end());
    for(const auto& annot : function->annotations) {
        if (annot->name == "doc") {
            std::string d;
            for(size_t i=0;i<annot->args.size();++i) {
                auto expr = annot->args[i].second;
                if (auto s = dynamic_ptr_cast<ast::Str>(expr)) {
                    if (!d.empty()) d += "\n";
                    std::string t; s->str.toUTF8String(t);
                    d += t;
                }
            }
            function->doc = toUnicodeString(d);
        }
    }

    defineVariable(var);

    return {};
}


std::any RoxalCompiler::visit(ptr<ast::VarDecl> ast)
{
    currentNode = ast;

    std::optional<VarTypeSpec> declType{};
    if (ast->varType.has_value()) {
        if (std::holds_alternative<BuiltinType>(ast->varType.value()))
            declType = std::get<BuiltinType>(ast->varType.value());
        else
            declType = std::get<icu::UnicodeString>(ast->varType.value());
    }

    if (ast->isConst) {
        if (!ast->initializer.has_value())
            error("Const declarations require an initializer.");

        bool strictContext = asFuncScope(funcScope())->strict;
        Value constValue = evaluateConstExpression(ast->initializer.value(), strictContext);
        constValue = applyConstType(constValue, declType, strictContext);
        if (constValue.isObj())
            error("Const declarations are currently limited to builtin value types.");

        declareConstant(ast->name, constValue, declType);

        if (asFuncScope(funcScope())->scopeDepth == 0) {
            uint16_t var = identifierConstant(ast->name);
            emitConstant(constValue, toUTF8StdString(ast->name));
            defineVariable(var, true);
        }

        return {};
    }

    declareVariable(ast->name, declType);
    uint16_t var { 0 };
    if (asFuncScope(funcScope())->scopeDepth == 0) { // global variable
        var = identifierConstant(ast->name); // create constant table entry for name
        if (declType.has_value())
            asModuleScope(moduleScope())->moduleVarTypes[ast->name] = declType.value();
    }

    if (ast->initializer.has_value()) {
        ast->initializer.value()->accept(*this);
        if (declType.has_value()) {
            if (std::holds_alternative<BuiltinType>(*declType))
                emitBytes(asFuncScope(funcScope())->strict ? OpCode::ToTypeStrict : OpCode::ToType,
                          uint8_t(builtinToValueType(std::get<BuiltinType>(*declType))));
            else {
                namedVariable(std::get<icu::UnicodeString>(*declType), false);
                emitByte(asFuncScope(funcScope())->strict ? OpCode::ToTypeSpecStrict : OpCode::ToTypeSpec);
            }
        }
    } else {
        if (declType.has_value()) {
            if (std::holds_alternative<BuiltinType>(*declType)) {
                auto bt = std::get<BuiltinType>(*declType);
                if (bt == BuiltinType::Signal)
                    error("Can't default-construct signal");
                emitConstant(defaultValue(builtinToValueType(bt)));
            }
            else
                emitByte(OpCode::ConstNil);
        } else
            emitByte(OpCode::ConstNil);
    }

    defineVariable(var);

    return {};
}


std::any RoxalCompiler::visit(ptr<ast::Suite> ast)
{
    currentNode = ast;
    Anys results {};

    enterLocalScope();
    ast->acceptChildren(*this, results);
    exitLocalScope();
    return results;
}


std::any RoxalCompiler::visit(ptr<ast::ExpressionStatement> ast)
{
    currentNode = ast;
    ast::Anys results {};
    ast->acceptChildren(*this, results);

    // In REPL mode at module scope with no nested local scope, automatically
    // print the value of expression statements
    if (replModeFlag && inModuleScope() && asFuncScope(funcScope())->scopeDepth == 0) {
        // stack currently: <expr_value>
        namedModuleVariable(toUnicodeString("print")); // push print function
        emitByte(OpCode::Swap);                       // [print_fn, value]
        CallSpec cs{1};
        cs.allPositional = true;
        auto bytes = cs.toBytes();
        if (bytes.size()==1)
            emitBytes(OpCode::Call, bytes[0]);
        else {
            emitByte(OpCode::Call);
            for(auto b : bytes) emitByte(b);
        }
        emitByte(OpCode::Pop); // discard print return
    } else {
        // expressions leave their value on the stack, but statements don't
        // have a value, so discard it
        emitByte(OpCode::Pop, "expr_stmt value");
    }
    return results;
}


std::any RoxalCompiler::visit(ptr<ast::ReturnStatement> ast)
{
    currentNode = ast;
    ast::Anys results {};

    ast->acceptChildren(*this, results);

    if (ast->expr.has_value()) {

        if (asFuncScope(funcScope())->functionType == FunctionType::Initializer)
            error("A value cannot be returned from an 'init' method.");
        if (asFuncScope(funcScope())->type->func.has_value() && asFuncScope(funcScope())->type->func.value().isProc)
            error("A value cannot be returned from a proc method.");

        emitByte(OpCode::Return);
    }
    else
        emitReturn();

    return results;
}


std::any RoxalCompiler::visit(ptr<ast::IfStatement> ast)
{
    currentNode = ast;

    // (first) if condition
    ast->conditionalSuites.at(0).first->accept(*this);

    auto jumpOverIf = emitJump(OpCode::JumpIfFalse);
    emitByte(OpCode::Pop, "if cond");

    enterLocalScope();
    ast->conditionalSuites.at(0).second->accept(*this);
    exitLocalScope();

    auto jumpOverElse = emitJump(OpCode::Jump);

    patchJump(jumpOverIf);

    if (ast->conditionalSuites.size()>1) {
        throw std::runtime_error("elseif unimplemented");
        // for(int i=1; i<context->expression().size();i++) {
        //     visitExpression(context->expression().at(i));
        //     visitSuite(context->suite().at(i));
        // }
    }

    emitByte(OpCode::Pop, "if cond");
    if (ast->elseSuite.has_value()) {
        enterLocalScope();
        ast->elseSuite.value()->accept(*this);
        exitLocalScope();
    }

    patchJump(jumpOverElse);

    return {};
}


std::any RoxalCompiler::visit(ptr<ast::WhileStatement> ast)
{
    currentNode = ast;

    auto loopStart = currentChunk()->code.size();

    // while condition
    ast->condition->accept(*this);

    auto jumpToExit = emitJump(OpCode::JumpIfFalse);
    emitByte(OpCode::Pop, "while cond");

    ast->body->accept(*this);

    emitLoop(loopStart);

    patchJump(jumpToExit);
    emitByte(OpCode::Pop, "while cond");

    return {};
}


std::any RoxalCompiler::visit(ptr<ast::ForStatement> ast)
{
    currentNode = ast;

    #ifdef DEBUG_BUILD
    emitByte(OpCode::Nop, "for scope");
    #endif
    enterLocalScope();

    // declare locals for the iterable, its length, and the loop index
    icu::UnicodeString iterableName = "__iterable__";
    icu::UnicodeString lenName = "__len__";
    icu::UnicodeString iname = "__index__";

    declareVariable(iterableName);
    emitByte(OpCode::ConstNil);
    defineVariable();

    declareVariable(lenName);
    emitByte(OpCode::ConstNil);
    defineVariable();

    declareVariable(iname);
    emitByte(OpCode::ConstInt0);
    defineVariable();

    // declare local vars for each for target
    std::vector<icu::UnicodeString> targetVarNames {};
    std::vector<std::optional<VarTypeSpec>> targetVarTypes {};

    uint8_t numTargets = ast->targetList.size();
    if (numTargets > 128) {
        error("Too many target variables in for statement.");
        return {};
    }
    for(auto i = 0; i < numTargets; i++) {
        assert(isa<VarDecl>(ast->targetList.at(i)));
        auto vdecl = as<VarDecl>(ast->targetList.at(i));
        auto name = vdecl->name;
        std::optional<VarTypeSpec> vtype{};
        if (vdecl->varType.has_value()) {
            if (std::holds_alternative<BuiltinType>(vdecl->varType.value()))
                vtype = std::get<BuiltinType>(vdecl->varType.value());
            else
                vtype = std::get<icu::UnicodeString>(vdecl->varType.value());
        }
        targetVarNames.push_back(name);
        targetVarTypes.push_back(vtype);
        declareVariable(name, vtype);
        if (vtype.has_value() && std::holds_alternative<BuiltinType>(*vtype)) {
            auto bt = std::get<BuiltinType>(*vtype);
            if (bt == BuiltinType::Signal)
                error("Can't default-construct signal");
            emitConstant(defaultValue(builtinToValueType(bt)));
        } else
            emitByte(OpCode::ConstNil);
        defineVariable();
    }



    // evaluate the iterable
    ast->iterable->accept(*this);

    // special case for iterating over dicts:
    //  if single target, convert to list of keys
    //  otherwise, convert to list of key-value pairs (list of two elements)
    if (numTargets == 1)
        emitByte(OpCode::IfDictToKeys);
    else if (numTargets >= 2)
        emitByte(OpCode::IfDictToItems);

    // store the iterable in a synthetic local
    namedVariable(iterableName, /*assign=*/true);
    emitByte(OpCode::Pop, "__iterable__ value");

    // compute the length of the iterable

    // first find built-in global "len" function
    namedModuleVariable("len");

    // push the iterable as argument for len
    namedVariable(iterableName);

    // call it
    CallSpec lenCallSpec { 1 };
    auto lenCallSpecBytes = lenCallSpec.toBytes();
    assert(lenCallSpecBytes.size() == 1);
    emitBytes(OpCode::Call, lenCallSpecBytes[0]);

    // store len(iterable) in a synthetic local
    namedVariable(lenName, /*assign=*/true);
    emitByte(OpCode::Pop, "__len__ value");

    // check if len(iterable) == nil (e.g. for range, implies the range isn't definite)
    namedVariable(lenName);
    emitByte(OpCode::ConstNil);
    emitByte(OpCode::Equal);
    auto jumpToAbort = emitJump(OpCode::JumpIfTrue);
    emitByte(OpCode::Pop, "abort cond");

    auto loopStart = currentChunk()->code.size();
    #ifdef DEBUG_BUILD
    emitByte(OpCode::Nop, "for loop body");
    #endif

    // check condition iname < len(iterable)
    namedVariable(iname);
    namedVariable(lenName);
    emitByte(OpCode::Less);
    auto jumpToExit = emitJump(OpCode::JumpIfFalse);
    emitByte(OpCode::Pop, "exit cond");


    // index the iterable via the loop index
    namedVariable(iterableName);
    namedVariable(iname);
    emitBytes(OpCode::Index, uint8_t(1)); // single index/arg indexing

    // if there is a single target, just assign the target the result of indexing the iterable (stack top)
    bool strict = asFuncScope(funcScope())->strict;
    if (numTargets == 1) {
        if (targetVarTypes.at(0).has_value()) {
            auto tv = targetVarTypes.at(0).value();
            if (std::holds_alternative<BuiltinType>(tv))
                emitBytes(strict ? OpCode::ToTypeStrict : OpCode::ToType,
                          uint8_t(builtinToValueType(std::get<BuiltinType>(tv))));
            else {
                namedVariable(std::get<icu::UnicodeString>(tv), false);
                emitByte(strict ? OpCode::ToTypeSpecStrict : OpCode::ToTypeSpec);
            }
        }
        namedVariable(targetVarNames.at(0),/*assign=*/true);
        emitByte(OpCode::Pop, "index result"); // discard index
    }
    else {
        // otherwise, index into the index result for the number of targets and assign each target
        for(auto i = 0; i < numTargets; i++) {
            emitByte(OpCode::Dup); // dup index result
            if (i==0)
                emitByte(OpCode::ConstInt0);
            else if (i==1)
                emitByte(OpCode::ConstInt1);
            else
                emitConstant(Value::intVal(i));
            emitBytes(OpCode::Index, uint8_t(1));

            // assign it to target
            if (targetVarTypes.at(i).has_value()) {
                auto tv = targetVarTypes.at(i).value();
                if (std::holds_alternative<BuiltinType>(tv))
                    emitBytes(strict ? OpCode::ToTypeStrict : OpCode::ToType,
                              uint8_t(builtinToValueType(std::get<BuiltinType>(tv))));
                else {
                    namedVariable(std::get<icu::UnicodeString>(tv), false);
                    emitByte(strict ? OpCode::ToTypeSpecStrict : OpCode::ToTypeSpec);
                }
            }
            namedVariable(targetVarNames.at(i),/*assign=*/true);

            emitByte(OpCode::Pop, "subindex result"); // discard index
        }
        emitByte(OpCode::Pop, "index result"); // discard index
    }

    // generate code for the body
    ast->body->accept(*this);

    // increment the loop index
    //  TODO: add Inc opcode (or IncLocal?)
    namedVariable(iname);
    emitByte(OpCode::ConstInt1);
    emitByte(OpCode::Add);
    namedVariable(iname, /*assign=*/true);
    emitByte(OpCode::Pop);

    emitLoop(loopStart);

    patchJump(jumpToExit);
    patchJump(jumpToAbort);
    emitByte(OpCode::Pop, "exit/abort cond");

    exitLocalScope();

    return {};
}

std::any RoxalCompiler::visit(ptr<ast::WhenStatement> ast)
{
    currentNode = ast;

    bool emittedTrigger = false;
    if (ast->requiresSignalChange) {
        if (auto variable = dynamic_ptr_cast<ast::Variable>(ast->trigger)) {
            currentNode = variable;
            emittedTrigger = namedVariable(variable->name, /*assign=*/false, /*asSignal=*/true);
            currentNode = ast;
        }
    }

    if (!emittedTrigger)
        ast->trigger->accept(*this);

    if (ast->matchesBecomes && ast->becomes.has_value())
        ast->becomes.value()->accept(*this);

    // compile handler body as closure proc
    ptr<type::Type> funcType = make_ptr<type::Type>(BuiltinType::Func);
    funcType->func = type::Type::FuncType();
    funcType->func->isProc = true;

    auto enclosingModuleScope { asModuleScope(moduleScope()) };
    icu::UnicodeString funcName = icu::UnicodeString::fromUTF8("__when_" + std::to_string(ast->interval.first.line) + "_" + std::to_string(ast->interval.first.pos));

    enterFuncScope(enclosingModuleScope->moduleType, funcName, FunctionType::Function, funcType);
    enterLocalScope();
    int handlerArity = ast->binding.has_value() ? 1 : 0;
    asFunction(asFuncScope(funcScope())->function)->arity = handlerArity;
    if (handlerArity == 1) {
        auto bindingName = ast->binding.value();
        declareVariable(bindingName);
        defineVariable(identifierConstant(bindingName));
    }
    ast->body->accept(*this);
    emitReturn();

    auto fs = asFuncScope(funcScope());
    Value function { fs->function };
    ObjFunction* functionObj = asFunction(function);
    exitFuncScope();

    uint16_t constIdx = makeConstant(function);
    emitOpArgsBytes(OpCode::Closure, constIdx);

    for (int i = 0; i < functionObj->upvalueCount; i++) {
        emitByte(fs->upvalues[i].isLocal ? 1 : 0);
        emitByte(fs->upvalues[i].index);
    }

    uint8_t whenMode = ast->matchesBecomes ? 3 : (ast->requiresSignalChange ? 1 : 2);
    emitOpArgsBytes(OpCode::EventOn, whenMode);

    return {};
}

std::any RoxalCompiler::visit(ptr<ast::UntilStatement> ast)
{
    currentNode = ast;

    // until <eventExpr>: <stmt>
    //   is compiled as:
    //   declare temp local for event expression
    //   eventExpr -> local
    //   event.on(local, __conditional_interrupt)
    //   try:
    //       <stmt>
    //   except e:
    //       event.off(local, __conditional_interrupt)
    //       if not isinstance(e, ConditionalInterrupt):
    //           raise
    //   event.off(local, __conditional_interrupt)

    enterLocalScope();

    // store condition expression (event) in temporary local
    icu::UnicodeString tmpName = "__until_event";
    declareVariable(tmpName);
    ast->condition->accept(*this);          // [event]
    defineVariable();                       // local = event

    // subscribe conditional interrupt handler
    namedVariable(tmpName, false);          // [event]
    namedVariable(toUnicodeString("__conditional_interrupt"), false); // [event, closure]
    emitOpArgsBytes(OpCode::EventOn, 0);

    // setup try/except
    auto handlerJump = emitJump(OpCode::SetupExcept);

    // body
    enterLocalScope();
    ast->stmt->accept(*this);
    exitLocalScope();

    emitByte(OpCode::EndExcept);

    // remove handler on normal path
    namedVariable(tmpName, false);
    namedVariable(toUnicodeString("__conditional_interrupt"), false);
    emitByte(OpCode::EventOff);

    auto jumpOverHandlers = emitJump(OpCode::Jump);

    // exception handler
    patchJump(handlerJump);

    // remove handler on exceptional path
    namedVariable(tmpName, false);
    namedVariable(toUnicodeString("__conditional_interrupt"), false);
    emitByte(OpCode::EventOff);

    emitByte(OpCode::Dup); // exception
    namedVariable(toUnicodeString("ConditionalInterrupt"), false);
    emitByte(OpCode::Is);
    auto jumpNext = emitJump(OpCode::JumpIfFalse);
    emitByte(OpCode::Pop, "is result");
    emitByte(OpCode::Pop, "exception"); // ignore exception
    auto jumpEnd = emitJump(OpCode::Jump);

    patchJump(jumpNext);
    emitByte(OpCode::Throw); // rethrow if not ConditionalInterrupt

    patchJump(jumpEnd);
    patchJump(jumpOverHandlers);

    exitLocalScope();

    return {};
}

std::any RoxalCompiler::visit(ptr<ast::TryStatement> ast)
{
    currentNode = ast;
    // emit handler setup and compile body
    auto handlerJump = emitJump(OpCode::SetupExcept);

    enterLocalScope();
    ast->body->accept(*this);
    exitLocalScope();

    emitByte(OpCode::EndExcept);

    if (ast->finallySuite.has_value())
        ast->finallySuite.value()->accept(*this);

    auto jumpOverHandlers = emitJump(OpCode::Jump);

    // patch handler start
    patchJump(handlerJump);

    std::vector<Chunk::size_type> jumpsToEnd;

    for (size_t i = 0; i < ast->exceptClauses.size(); ++i) {
        const auto& ec = ast->exceptClauses[i];

        Chunk::size_type jumpNext = 0;
        if (ec.type.has_value()) {
            emitByte(OpCode::Dup); // exception
            ec.type.value()->accept(*this);
            emitByte(OpCode::Is);
            jumpNext = emitJump(OpCode::JumpIfFalse);
            emitByte(OpCode::Pop, "is result");
        }

        enterLocalScope();
        icu::UnicodeString excVar = ec.name.value_or(toUnicodeString("$exception"));
        declareVariable(excVar);
        defineVariable(0);
        exceptionVarStack.push_back(excVar);
        ec.body->accept(*this);
        exceptionVarStack.pop_back();
        exitLocalScope();

        if (!ec.name.has_value())
            emitByte(OpCode::Pop, "exception");

        if (ast->finallySuite.has_value())
            ast->finallySuite.value()->accept(*this);

        jumpsToEnd.push_back(emitJump(OpCode::Jump));

        if (ec.type.has_value())
            patchJump(jumpNext);
    }

    if (ast->finallySuite.has_value())
        ast->finallySuite.value()->accept(*this);

    emitByte(OpCode::Throw); // rethrow if not handled

    for (auto j : jumpsToEnd)
        patchJump(j);

    patchJump(jumpOverHandlers);

    return {};
}

std::any RoxalCompiler::visit(ptr<ast::RaiseStatement> ast)
{
    currentNode = ast;
    if (ast->exception.has_value()) {
        ast->exception.value()->accept(*this);
    } else {
        if (exceptionVarStack.empty())
            error("Bare raise outside of except clause");
        else
            namedVariable(exceptionVarStack.back(), false);
    }
    emitByte(OpCode::Throw);
    return {};
}


std::any RoxalCompiler::visit(ptr<ast::Function> ast)
{
    currentNode = ast;

    bool isProc = ast->isProc;
    bool isMethod = inTypeScope() // methods can't be outside type decl
                     && (asFuncScope(funcScope())->functionType!=FunctionType::Method) // or directly inside another method
                     && (asFuncScope(funcScope())->functionType!=FunctionType::Initializer);
    // std::cout << " visit <Function> " << toUTF8StdString(ast->name)
    //           << " current funcScope:" << toUTF8StdString(asFuncScope(funcScope())->function->name)
    //            << "[type:" << toString(asFuncScope(funcScope())->functionType) << "]"
    //           << " isMethod?" << isMethod << std::endl;
    bool isInitializer = isMethod && (ast->name == "init");

    if (isInitializer && !isProc)
        error("object or actor type 'init' method must be a proc.");

    FunctionType ftype = isMethod ?
                              (isInitializer ? FunctionType::Initializer : FunctionType::Method)
                            : FunctionType::Function;

    assert(ast->type.has_value());

    auto enclosingModuleScope { asModuleScope(moduleScope()) };

    icu::UnicodeString funcName;
    if (ast->name.has_value())
        funcName = ast->name.value();
    else { // lambda func? create unique name using module name and source line position
        funcName = icu::UnicodeString::fromUTF8("__func_" + toUTF8StdString(enclosingModuleScope->moduleName)
                    +"_"+std::to_string(ast->interval.first.line)
                    +"_"+std::to_string(ast->interval.first.pos));
    }

    enterFuncScope(enclosingModuleScope->moduleType, funcName, ftype, ast->type.value());

    bool strictContext = true;
    for (const auto& annot : ast->annotations) {
        if (annot->name == "strict")
            strictContext = true;
        else if (annot->name == "nonstrict")
            strictContext = false;
    }

    ptr<FunctionScope> funcScopePtr { asFuncScope(this->funcScope()) };
    funcScopePtr->strict = strictContext;
    ObjFunction* funcObj = asFunction(funcScopePtr->function);
    funcObj->strict = strictContext;
    funcObj->access = ast->access;

    #ifdef DEBUG_BUILD
    emitByte(OpCode::Nop, "func "+toUTF8StdString(funcName));
    #endif
    enterLocalScope();

    asFunction(asFuncScope(funcScope())->function)->arity = ast->params.size();
    if (asFunction(asFuncScope(funcScope())->function)->arity > 255)
        error("Maximum of function or procedure 255 parameters exceeded.");

    Anys results {};
    ast->acceptChildren(*this, results);

    // if the body is an expression (e.g. lambda func), leaves the result on the stack, so return it
    if (std::holds_alternative<ptr<Expression>>(ast->body))
        emitByte(OpCode::Return);

    //exitLocalScope();

    if (lastByte() != uint8_t(OpCode::Return)) // if the code didn't conclude with a return, add one
        emitReturn();

    if (outputBytecodeDisassembly)
        asFunction(asFuncScope(funcScope())->function)->chunk->disassemble(asFunction(asFuncScope(funcScope())->function)->name);

    ObjFunction* function = asFunction(asFuncScope(funcScope())->function);
    function->annotations = ast->annotations;
    for (const auto& annot : function->annotations) {
        if (annot->name == "doc") {
            std::string d;
            for (const auto& arg : annot->args) {
                auto expr = arg.second;
                if (auto s = dynamic_ptr_cast<ast::Str>(expr)) {
                    if (!d.empty())
                        d += "\n";
                    std::string t;
                    s->str.toUTF8String(t);
                    d += t;
                }
            }
            function->doc = toUnicodeString(d);
        }
    }

    auto functionScope { *asFuncScope(funcScope()) };

    exitFuncScope(); // back to surrounding scope

    // std::cout << "Closure " << toUTF8StdString(function->name) << ": #" << function->upvalueCount << std::endl;
    // std::cout << "   #" << functionState.upvalues.size() << std::endl;

    uint16_t constIdx = makeConstant(Value::objRef(function));
    emitOpArgsBytes(OpCode::Closure, constIdx);

    for (int i = 0; i < function->upvalueCount; i++) {
        #ifdef DEBUG_BUILD
        if (i >= functionScope.upvalues.size())
            throw std::runtime_error("invalid upvalue index");
        #endif
        //std::cout << "    - " << int(functionState.upvalues[i].index) << " " << std::string(functionState.upvalues[i].isLocal ?"local":"nonlocal") << std::endl;
        emitByte(functionScope.upvalues[i].isLocal ? 1 : 0);
        emitByte(functionScope.upvalues[i].index);
    }

    return function; // used by caller visit(ptr<FuncDecl>)
}


std::any RoxalCompiler::visit(ptr<ast::Parameter> ast)
{
    currentNode = ast;

    // TODO: handle optional type

    declareVariable(ast->name);
    uint16_t var = identifierConstant(ast->name); // create constant table entry for name

    defineVariable(var);

    // output code for evaluating default value (if any)
    if (ast->defaultValue.has_value()) {

        // treat like another func decl
        ptr<type::Type> defFuncType = make_ptr<type::Type>(BuiltinType::Func);
        defFuncType->func = type::Type::FuncType();
        // TODO: specify return type? (necessary?)

        auto enclosingModuleScope { asModuleScope(moduleScope()) };

        enterFuncScope(enclosingModuleScope->moduleType, ast->name, FunctionType::Function, defFuncType);

        #ifdef DEBUG_BUILD
        emitByte(OpCode::Nop, "param_def "+toUTF8StdString(ast->name));
        #endif
        enterLocalScope();

        asFunction(asFuncScope(funcScope())->function)->arity = 0;

        Anys results;
        ast->acceptChildren(*this, results);

        exitLocalScope();

        // since this closure was called directly by being queued by OpCode::Call
        //  rather than through byte code pushing the callable/closure, we
        //  need get the return value copied into the placeholder arg slots in the parent frame
        //  rather than leaving it on the stack
        emitByte(OpCode::ReturnStore);

        ptr<FunctionScope> funcScopePtr { asFuncScope(funcScope()) };
        Value function = funcScopePtr->function;
        if (outputBytecodeDisassembly) {
            asFunction(function)->chunk->disassemble(asFunction(function)->name);
        }

        exitFuncScope(); // back to surrounding scope

        // store the func that evaluates the default param value in the function
        //  for which it is a param
        auto surroundingFunction = asFuncScope(funcScope())->function;
        asFunction(surroundingFunction)->paramDefaultFunc[ast->name.hashCode()] = function;
    }
    return {};
}


std::any RoxalCompiler::visit(ptr<ast::Assignment> ast)
{
    currentNode = ast;

    if (ast->op == ast::Assignment::CopyInto) {
        if (isa<Variable>(ast->lhs)) {
            auto name { as<Variable>(ast->lhs)->name };
            namedVariable(name, false); // push current value

            ast->rhs->accept(*this);

            auto vtype = localVarType(name);
            if (!vtype.has_value())
                vtype = moduleVarType(name);
            if (vtype.has_value()) {
                if (std::holds_alternative<BuiltinType>(*vtype))
                    emitBytes(asFuncScope(funcScope())->strict ? OpCode::ToTypeStrict : OpCode::ToType,
                              uint8_t(builtinToValueType(std::get<BuiltinType>(*vtype))));
                else {
                    namedVariable(std::get<icu::UnicodeString>(*vtype), false);
                    emitByte(asFuncScope(funcScope())->strict ? OpCode::ToTypeSpecStrict : OpCode::ToTypeSpec);
                }
            }

            emitByte(OpCode::CopyInto);
            namedVariable(name, /*assign=*/true); // store result back
        }
        else if (isa<UnaryOp>(ast->lhs) && as<UnaryOp>(ast->lhs)->op==UnaryOp::Accessor) {
            auto accessor = as<UnaryOp>(ast->lhs);
            accessor->arg->accept(*this);

            if (!accessor->member.has_value())
                throw std::runtime_error("accessor unary operator expects member name");
            uint16_t propName = identifierConstant(accessor->member.value());

            OpCode getOp = OpCode::GetPropCheck;
            OpCode setOp = OpCode::SetPropCheck;
            if (isa<Variable>(accessor->arg) && as<Variable>(accessor->arg)->name == "this" && inTypeScope()) {
                auto typeScopePtr = asTypeScope(typeScope());
                auto itMem = typeScopePtr->propertyNames.find(accessor->member.value());
                if (itMem != typeScopePtr->propertyNames.end()) {
                    const auto& info = itMem->second;
                    if (info.access == Access::Private && info.owner != typeScopePtr->name)
                        error("Cannot access private member '"+toUTF8StdString(accessor->member.value())+"'");
                    if (info.isConst)
                        error("Cannot assign to constant '"+toUTF8StdString(accessor->member.value())+"'");
                    getOp = OpCode::GetProp;
                    setOp = OpCode::SetProp;
                }
            }

            emitByte(OpCode::Dup);             // keep instance for SetProp
            emitOpArgsBytes(getOp, propName);  // push current property value

            ast->rhs->accept(*this);

            emitByte(OpCode::CopyInto);        // mutate property value
            emitOpArgsBytes(setOp, propName);  // store back
        }
        else if (isa<Index>(ast->lhs)) {
            auto index { as<Index>(ast->lhs) };

            // obtain current element
            index->indexable->accept(*this);
            for(auto& arg : index->args)
                arg->accept(*this);
            debug_assert_msg(index->args.size() <= 255, "Indexing with more than 255 arguments is not supported");
            emitBytes(OpCode::Index, uint8_t(index->args.size()));

            ast->rhs->accept(*this);
            emitByte(OpCode::CopyInto);          // mutate element

            // set element back
            index->indexable->accept(*this);
            for(auto& arg : index->args)
                arg->accept(*this);
            debug_assert_msg(index->args.size() <= 255, "Indexing with more than 255 arguments is not supported");
            emitBytes(OpCode::SetIndex, uint8_t(index->args.size()));
        }
        else {
            throw std::runtime_error("LHS of copy into must be a variable, property accessor or indexing");
        }
        return {};
    }

    if (isa<Variable>(ast->lhs)) {

        ast->rhs->accept(*this);

        auto name { as<Variable>(ast->lhs)->name };

        auto vtype = localVarType(name);
        if (!vtype.has_value())
            vtype = moduleVarType(name);
        if (vtype.has_value()) {
            if (std::holds_alternative<BuiltinType>(*vtype))
                emitBytes(asFuncScope(funcScope())->strict ? OpCode::ToTypeStrict : OpCode::ToType,
                          uint8_t(builtinToValueType(std::get<BuiltinType>(*vtype))));
            else {
                namedVariable(std::get<icu::UnicodeString>(*vtype), false);
                emitByte(asFuncScope(funcScope())->strict ? OpCode::ToTypeSpecStrict : OpCode::ToTypeSpec);
            }
        }

        namedVariable(name, /*assign=*/true);
    }
    else if (isa<UnaryOp>(ast->lhs) && as<UnaryOp>(ast->lhs)->op==UnaryOp::Accessor) {
        auto accessor = as<UnaryOp>(ast->lhs);
        // visit the lhs of the accessor operator to generate code to evaluate it
        //  (so we don't evaluate the access, since we want to set the member, not get it)
        accessor->arg->accept(*this);

        if (!accessor->member.has_value())
            throw std::runtime_error("accessor unary operator expects member name");
        uint16_t propName = identifierConstant(accessor->member.value());

        OpCode op = OpCode::SetPropCheck;
        if (isa<Variable>(accessor->arg) && as<Variable>(accessor->arg)->name == "this" && inTypeScope()) {
            auto typeScopePtr = asTypeScope(typeScope());
            auto itMem = typeScopePtr->propertyNames.find(accessor->member.value());
            if (itMem != typeScopePtr->propertyNames.end()) {
                const auto& info = itMem->second;
                if (info.access == Access::Private && info.owner != typeScopePtr->name)
                    error("Cannot access private member '"+toUTF8StdString(accessor->member.value())+"'");
                if (info.isConst)
                    error("Cannot assign to constant '"+toUTF8StdString(accessor->member.value())+"'");
                op = OpCode::SetProp;
            }
        }

        ast->rhs->accept(*this);

        emitOpArgsBytes(op, propName);
    }
    else if (isa<Index>(ast->lhs)) {

        // evaluate rhs
        ast->rhs->accept(*this);

        auto index { as<Index>(ast->lhs) };

        // value being indexed
        index->indexable->accept(*this);

        // index args
        for(auto& arg : index->args)
            arg->accept(*this);

        debug_assert_msg(index->args.size() <= 255, "Indexing with more than 255 arguments is not supported");
        emitBytes(OpCode::SetIndex, uint8_t(index->args.size()));
    }
    else if (isa<List>(ast->lhs)) {
        // binding assignment - assign east LHS element of list seperately from indexed element of RHS

        // evaluate rhs (expected to leave list on stack)
        //  TOOD: consider also supporting dict lhs so return components can be named(?)
        ast->rhs->accept(*this);

        auto lhsList = as<List>(ast->lhs);
        auto lhsSize = lhsList->elements.size();
        for(auto li=0; li<lhsSize; li++) {
            auto lhsElt = lhsList->elements.at(li);

            // first index the RHS list to get the RHS element to assign
            emitByte(OpCode::Dup); // duplicate the RHS (as Index will pop it)
            emitConstant(Value::intVal(li));
            emitBytes(OpCode::Index, uint8_t(1));

            if (isa<Variable>(lhsElt)) {
                auto varname { as<Variable>(lhsElt)->name };

                auto vtype = localVarType(varname);
                if (!vtype.has_value())
                    vtype = moduleVarType(varname);
                if (vtype.has_value()) {
                    if (std::holds_alternative<BuiltinType>(*vtype))
                        emitBytes(asFuncScope(funcScope())->strict ? OpCode::ToTypeStrict : OpCode::ToType,
                                  uint8_t(builtinToValueType(std::get<BuiltinType>(*vtype))));
                    else {
                        namedVariable(std::get<icu::UnicodeString>(*vtype), false);
                        emitByte(asFuncScope(funcScope())->strict ? OpCode::ToTypeSpecStrict : OpCode::ToTypeSpec);
                    }
                }
                namedVariable(varname, /*assign=*/true);

            }
            else if (isa<UnaryOp>(lhsElt) && as<UnaryOp>(lhsElt)->op==UnaryOp::Accessor) {
                auto accessor = as<UnaryOp>(lhsElt);

                if (isa<Variable>(accessor->arg) && as<Variable>(accessor->arg)->name == "this" && inTypeScope() && accessor->member.has_value()) {
                    auto typeScopePtr = asTypeScope(typeScope());
                    auto itMem = typeScopePtr->propertyNames.find(accessor->member.value());
                if (itMem != typeScopePtr->propertyNames.end() && itMem->second.isConst)
                    error("Cannot assign to constant '"+toUTF8StdString(accessor->member.value())+"'");
                }

                accessor->arg->accept(*this);

                if (!accessor->member.has_value())
                    throw std::runtime_error("accessor unary operator expects member name");
                uint16_t propName = identifierConstant(accessor->member.value());

                emitByte(OpCode::Swap);

                emitOpArgsBytes(OpCode::SetProp, propName);
            }
            else if (isa<Index>(lhsElt)) {

                auto index { as<Index>(lhsElt) };

                // value being indexed
                index->indexable->accept(*this);

                // index args
                for(auto& arg : index->args)
                    arg->accept(*this);

                debug_assert_msg(index->args.size() <= 255, "Indexing with more than 255 arguments is not supported");
                emitBytes(OpCode::SetIndex, uint8_t(index->args.size()));
            }
            else
                throw std::runtime_error("Elements of LHS list of binding assignment must be variables, property accessors or indexing");

            emitByte(OpCode::Pop,"RHS #"+std::to_string(li)); // discard RHS element, leaving RHS list on top
        }
    }
    else
        throw std::runtime_error("LHS of assignment must be a variable, property accessor or indexing");
    return {};
}


std::any RoxalCompiler::visit(ptr<ast::BinaryOp> ast)
{
    currentNode = ast;

    // Logical And and Or operators have short-circuit semantics, so may not need to evaluate all
    //  children, so handle them differently
    if (ast->op == BinaryOp::Or) {
        ast->lhs->accept(*this);

        Chunk::size_type jumpToEnd = emitJump(OpCode::JumpIfTrue);
        emitByte(OpCode::Pop);

        ast->rhs->accept(*this);

        patchJump(jumpToEnd);
    }
    else if (ast->op == BinaryOp::And) {
        ast->lhs->accept(*this);
        Chunk::size_type jumpToEnd = emitJump(OpCode::JumpIfFalse);
        emitByte(OpCode::Pop);

        ast->rhs->accept(*this);

        patchJump(jumpToEnd);
    }
    else {
        Anys results;
        ast->acceptChildren(*this, results);

        switch (ast->op) {
            case BinaryOp::Add: emitByte(OpCode::Add); break;
            case BinaryOp::Subtract: emitByte(OpCode::Subtract); break;
            case BinaryOp::Multiply: emitByte(OpCode::Multiply); break;
            case BinaryOp::Divide: emitByte(OpCode::Divide); break;
            case BinaryOp::Equal: emitByte(OpCode::Equal); break;
            case BinaryOp::NotEqual: emitByte(OpCode::Equal); emitByte(OpCode::Negate); break;
            case BinaryOp::Is: emitByte(OpCode::Is); break;
            case BinaryOp::Modulo: emitByte(OpCode::Modulo); break;
            case BinaryOp::BitAnd: emitByte(OpCode::BitAnd); break;
            case BinaryOp::BitOr: emitByte(OpCode::BitOr); break;
            case BinaryOp::BitXor: emitByte(OpCode::BitXor); break;
            case BinaryOp::LessThan: emitByte(OpCode::Less); break;
            case BinaryOp::GreaterThan: emitByte(OpCode::Greater); break;
            case BinaryOp::LessOrEqual: emitByte(OpCode::Greater); emitByte(OpCode::Negate); break;
            case BinaryOp::GreaterOrEqual: emitByte(OpCode::Less); emitByte(OpCode::Negate); break;
            default:
                throw std::runtime_error("unimplemented binary opertor:"+ast->opString());
        }
    }
    return {};
}


std::any RoxalCompiler::visit(ptr<ast::UnaryOp> ast)
{
    currentNode = ast;
    Anys results {};

    // special case for super.<member>
    if ((ast->op == UnaryOp::Accessor) && isa<Variable>(ast->arg)
        && as<Variable>(ast->arg)->name == "super") {

        if (!inTypeScope())
            error("Can't use 'super' outside of a type object or actor declaration.");

        if (!asTypeScope(typeScope())->hasSuperType)
            error("Can't use 'super' in a type object or actor that doesn't extend another type");

        if (!ast->member.has_value())
            throw std::runtime_error("super. accessor requires member name");

        // check access of member in super type
        auto superName = asTypeScope(typeScope())->superTypeName;
        auto itType = typePropertyRegistry.find(superName);
        if (itType != typePropertyRegistry.end()) {
            auto itMem = itType->second.find(ast->member.value());
            if (itMem != itType->second.end() && itMem->second.access == Access::Private)
                error("Cannot access private member '"+toUTF8StdString(ast->member.value())+"' of super type");
        }

        int16_t identConstant = identifierConstant(ast->member.value());
        if (identConstant > 255)
            error("Too many constants in scope");

        namedVariable("this", false);
        namedVariable("super", false);
        emitOpArgsBytes(OpCode::GetSuper, identConstant);
        return {};
    }

    ast->acceptChildren(*this, results);

    switch (ast->op) {
        case UnaryOp::Negate: emitByte(OpCode::Negate); break;
        case UnaryOp::Not: emitByte(OpCode::Negate); break;
        case UnaryOp::BitNot: emitByte(OpCode::BitNot); break;
        case UnaryOp::Accessor: {
            if (!ast->member.has_value())
                throw std::runtime_error("Accessor . requires member name");

            uint16_t identConstant = identifierConstant(ast->member.value());
            OpCode op = OpCode::GetPropCheck;
            if (isa<Variable>(ast->arg) && as<Variable>(ast->arg)->name == "this" && inTypeScope()) {
                auto itType = typePropertyRegistry.find(asTypeScope(typeScope())->name);
                if (itType != typePropertyRegistry.end()) {
                    auto itMem = itType->second.find(ast->member.value());
                    if (itMem != itType->second.end()) {
                        const auto& info = itMem->second;
                        if (info.access == Access::Private && info.owner != asTypeScope(typeScope())->name)
                            error("Cannot access private member '"+toUTF8StdString(ast->member.value())+"'");
                        op = OpCode::GetProp;
                    }
                }
            }
            emitOpArgsBytes(op, identConstant);
        } break;
        default:
            throw std::runtime_error("unimplemented unary operator:"+ast->opString());
    }
    return {};
}


std::any RoxalCompiler::visit(ptr<ast::Variable> ast)
{
    currentNode = ast;
    namedVariable(ast->name);
    return {};
}


std::any RoxalCompiler::visit(ptr<ast::Call> ast)
{
    currentNode = ast;
    Anys results {};

    ast->acceptChildren(*this, results);

    if (auto accessor = dynamic_ptr_cast<ast::UnaryOp>(ast->callable)) {
        if (accessor->op == ast::UnaryOp::Accessor && accessor->member.has_value() &&
            accessor->member.value() == toUnicodeString("emit")) {
            auto originalCall = dynamic_ptr_cast<ast::Call>(accessor->arg);
            if (originalCall != nullptr && originalCall->callable->type.has_value()) {
                auto calleeType = originalCall->callable->type.value();
                if (calleeType->builtin == type::BuiltinType::Event)
                    error("Event instances are not callable; call the event type to create one.");
            }
        }
    }

    // Restore current node to the call expression so the CALL opcode
    // emitted below uses the location of the call rather than that of
    // the final argument.
    currentNode = ast;

    auto argCount = ast->args.size();
    if (argCount > 127)
        error("Number of call parameters is limited to 127");

    //
    // create call param spec
    CallSpec callSpec {};

    if (!ast->namedArgs()) {
        // only positional arguments
        callSpec.allPositional = true;
        callSpec.argCount = ast->args.size();
    }
    else {
        #ifdef DEBUG_BUILD
        // keep track of hashes to check for collisions
        std::map<UnicodeString,uint16_t> hashes {};
        #endif
        // mix of positional & named args
        callSpec.allPositional = false;
        callSpec.argCount = ast->args.size();
        for(const auto& arg : ast->args) {
            CallSpec::ArgSpec aspec {};
            if (arg.first.isEmpty())
                aspec.positional = true;
            else {
                aspec.positional = false;
                // 15bits of param name string hash
                aspec.paramNameHash =0x8000 | (arg.first.hashCode() & 0x7fff);
                #ifdef DEBUG_BUILD
                hashes[arg.first] = aspec.paramNameHash;
                #endif
            }
            callSpec.args.push_back(aspec);
        }
        #ifdef DEBUG_BUILD
        // if any hash collisions occurs, the size of the set of hashes
        //  won't match the arg count
        std::set<uint16_t> hashSet;
        for(auto const& hash: hashes)
            hashSet.insert(hash.second);
        if (hashSet.size() != hashes.size())
            throw std::runtime_error("Hash collision occured between two argument names");
        #endif
    }

    auto bytes = callSpec.toBytes();
    if (bytes.size()==1)
        emitBytes(OpCode::Call, bytes[0]);
    else {
        emitByte(OpCode::Call);
        for(auto i=0; i<bytes.size();i++)
            emitByte(bytes[i]);
    }
    return {};
}


std::any RoxalCompiler::visit(ptr<ast::Range> ast)
{
    currentNode = ast;
    Anys results {};

    // always push 3 values, nil for implicit

    // special case for range n:n - don't visit same
    //  expression twice and we just leave single expr on stack
    if ((ast->start != nullptr) && (ast->start == ast->stop)) {
        results.push_back( ast->start->accept(*this) );
    }
    else {
        if (ast->start != nullptr)
            results.push_back( ast->start->accept(*this) );
        else
            emitByte(OpCode::ConstNil);

        if (ast->stop != nullptr)
            results.push_back( ast->stop->accept(*this) );
        else
            emitByte(OpCode::ConstNil);

        if (ast->step != nullptr)
            results.push_back( ast->step->accept(*this) );
        else
            emitByte(OpCode::ConstNil);

        emitBytes(OpCode::NewRange, uint8_t(ast->closed ? 1 : 0));
    }

    return results;
}


std::any RoxalCompiler::visit(ptr<ast::Index> ast)
{
    currentNode = ast;
    Anys results {};
    ast->acceptChildren(*this, results);

    auto argCount = ast->args.size();
    if (argCount > 255)
        error("Number of indices is limited to 255");
    emitBytes(OpCode::Index, uint8_t(argCount));
    return {};
}


std::any RoxalCompiler::visit(ptr<ast::LambdaFunc> ast)
{
    currentNode = ast;

    auto func {as<Function>(ast->func) };

    Anys results {};
    ast->acceptChildren(*this, results);

    // unwrap ObjFunction* returned by visit(ptr<Function>)
    auto function = std::any_cast<ObjFunction*>(std::any_cast<Anys>(results.at(0)).at(0));

    return {};
}


std::any RoxalCompiler::visit(ptr<ast::Literal> ast)
{
    currentNode = ast;
    // non-Nil typed literals handled by specialized visit methods
    if (ast->literalType==Literal::Nil)
        emitByte(OpCode::ConstNil);
    else
        throw std::runtime_error("Literal type unhandled");
    return {};
}


std::any RoxalCompiler::visit(ptr<ast::Bool> ast)
{
    currentNode = ast;
    emitByte( ast->value ? OpCode::ConstTrue : OpCode::ConstFalse );
    return {};
}


std::any RoxalCompiler::visit(ptr<ast::Str> ast)
{
    currentNode = ast;

    // new ObjString or existing one if exists in strings intern map
    emitConstant(Value::stringVal(ast->str));
    return {};
}


std::any RoxalCompiler::visit(ptr<ast::Type> ast)
{
    currentNode = ast;
    ValueType type { builtinToValueType(ast->t) };

    emitConstant(Value::typeVal(type));
    return {};
}


std::any RoxalCompiler::visit(ptr<ast::Num> ast)
{
    currentNode = ast;

    if (std::holds_alternative<double>(ast->num)) {
        emitConstant(Value::realVal(std::get<double>(ast->num)));
    }
    else if (std::holds_alternative<int32_t>(ast->num)) {
        emitConstant(Value::intVal(std::get<int32_t>(ast->num)));
    }
    else if (std::holds_alternative<int64_t>(ast->num)) {
        emitConstant(Value::intVal(std::get<int64_t>(ast->num)));
    }
    else
        throw std::runtime_error("unhandled Num type");
    return {};
}


std::any RoxalCompiler::visit(ptr<ast::List> ast)
{
    currentNode = ast;
    Anys results {};

    // generate code to eval each elements and leave on stack
    ast->acceptChildren(*this, results);

    if (ast->elements.size() > 255)
        error("Number of literal list elements is limited to 255");

    emitBytes(OpCode::NewList, uint8_t(ast->elements.size()));
    return {};
}


std::any RoxalCompiler::visit(ptr<ast::Vector> ast)
{
    currentNode = ast;

    // generate code to eval each element and leave on stack
    for(auto& elt : ast->elements)
        elt->accept(*this);

    if (ast->elements.size() > 255)
        error("Number of literal vector elements is limited to 255");

    emitBytes(OpCode::NewVector, uint8_t(ast->elements.size()));
    return {};
}


std::any RoxalCompiler::visit(ptr<ast::Matrix> ast)
{
    currentNode = ast;

    // generate code for each row vector
    for(auto& row : ast->rows)
        row->accept(*this);

    if (ast->rows.size() > 255)
        error("Number of literal matrix rows is limited to 255");

    emitBytes(OpCode::NewMatrix, uint8_t(ast->rows.size()));
    return {};
}


std::any RoxalCompiler::visit(ptr<ast::Dict> ast)
{
    currentNode = ast;
    Anys results {};

    // generate code to eval each key & value and leave on stack
    ast->acceptChildren(*this, results);

    if (ast->entries.size() > 255)
        error("Number of literal dict entries is limited to 255");

    // arg is entry count, so 2x as many stack values (key & value for each entry)
    emitBytes(OpCode::NewDict, uint8_t(ast->entries.size()));
    return {};
}



RoxalCompiler::ModuleInfo RoxalCompiler::findImport(const std::vector<icu::UnicodeString>& components) const
{
    bool endsWithProtoExt = components.size() >= 2 && (components.back() == toUnicodeString("proto"));

    // search the module paths (as package component roots)
    //  for the specified module
    std::vector<std::filesystem::path> candidatePaths; // paths that match the prefix, thus far
    for (const auto& modulePath : modulePaths) {
        try {
            candidatePaths.push_back(std::filesystem::canonical(std::filesystem::absolute(modulePath)));
        } catch (...) {
            // ignore invalid paths
        }
    }

    //std::cout << "initial candidates:";
    //for (const auto& path : candidatePaths)
    //    std::cout << path << std::endl;

    size_t importComponentIndex = 0;
    // for each component of the import
    while (importComponentIndex < components.size()) {
        bool isLastComponent = (importComponentIndex == components.size()-1);
        bool isFinalProtoComponent = endsWithProtoExt && (importComponentIndex == components.size()-2);

        // filter for the paths from the candidates thus far that match upto the current component
        std::vector<std::filesystem::path> newCandidatePaths {};
        for (const auto& modulePath : candidatePaths) {
            try {
                if (!std::filesystem::is_directory(modulePath)) {
                    if (isLastComponent)
                        newCandidatePaths.push_back(modulePath);
                    continue;
                }
                std::filesystem::path protoCandidate;
                bool hasProtoCandidate = false;
                bool matchedFile = false;
                // list of folders and files in modulePath
                for (const auto& entry : std::filesystem::directory_iterator(modulePath)) {
                    auto entryName = toUnicodeString(entry.path().filename().string());
                    if (entry.is_directory()) {
                        if (entryName == components.at(importComponentIndex))
                            newCandidatePaths.push_back(entry.path());
                    } else {
                        bool matchRox = isLastComponent && (entryName == components.at(importComponentIndex)+".rox");
                        bool matchProto = false;
                        if (isFinalProtoComponent && components.size() >= 2) {
                            // match <basename>.proto where basename is penultimate component
                            matchProto = (entryName == components.at(importComponentIndex)+".proto");
                        } else if (isLastComponent && !endsWithProtoExt) {
                            matchProto = (entryName == components.at(importComponentIndex)+".proto");
                        }
                        if (matchRox) {
                            newCandidatePaths.push_back(entry.path());
                            matchedFile = true;
                            break; // prefer .rox if present
                        }
                        if (matchProto) {
                            protoCandidate = entry.path();
                            hasProtoCandidate = true;
                        }
                    }
                }
                if (!matchedFile && hasProtoCandidate)
                    newCandidatePaths.push_back(protoCandidate);
            } catch (...) {
                // ignore invalid paths
            }
        }
        candidatePaths = newCandidatePaths;
        importComponentIndex++;
    }


    // debug - output candidate paths
    //std::cout << "final candidates:";
    //for (const auto& path : candidatePaths)
    //    std::cout << path << std::endl;

    if (candidatePaths.empty()) // not found
        return {};


    // found
    auto path { candidatePaths.at(0) }; // take first (if multiple)
    ModuleInfo module {};
    module.isPackage = std::filesystem::is_directory(path);
    module.isProto = (!module.isPackage && path.extension() == ".proto");
    module.name = toUnicodeString(path.stem().string());

    module.filename = path.filename().string();
    if (module.isPackage) {
        std::filesystem::path initPath = path / "init.rox";
        if (std::filesystem::exists(initPath)) {
            path = initPath;
            module.filename += "/init.rox";
        } else {
            std::vector<std::filesystem::path> roxFiles;
            for (const auto& entry : std::filesystem::directory_iterator(path)) {
                if (!entry.is_directory() && entry.path().extension() == ".rox")
                    roxFiles.push_back(entry.path());
            }
            if (roxFiles.size() == 1) {
                path = roxFiles[0];
                module.filename += "/" + roxFiles[0].filename().string();
            } else {
                module.invalidFolder = true;
                module.name = icu::UnicodeString();
                return module;
            }
        }
    }

    // join components to build packagePath (exclude file component)
    icu::UnicodeString pkgPath;
    size_t limit = components.size();
    if (endsWithProtoExt && limit >= 2)
        limit -= 2; // drop basename and 'proto'
    else if (limit > 0)
        limit -= 1; // drop module name
    for (size_t i=0; i < limit; ++i) {
        if (i>0) pkgPath += "/";
        pkgPath += components[i];
    }
    module.packagePath = pkgPath;

    // find the module path root that, combined with the package path and filename,
    //  resolves to the located module file or directory
    for (auto& modulePath : modulePaths) {
        try {
            auto absModulePath = std::filesystem::canonical(std::filesystem::absolute(modulePath));
            auto composed = absModulePath / toUTF8StdString(module.packagePath) / module.filename;
            if (std::filesystem::canonical(composed) == std::filesystem::canonical(path)) {
                module.modulePathRoot = modulePath;
                break;
            }
        } catch (...) { }
    }

    try {
        module.resolvedPath = std::filesystem::canonical(path);
        module.cachePath = moduleCachePathFor(module.resolvedPath);
        if (module.isProto) {
            module.cacheValid = false;
            module.cachePath.clear();
        }

        if (cacheReadEnabled && !module.cachePath.empty() && std::filesystem::exists(module.cachePath)) {
            auto sourceTime = std::filesystem::last_write_time(module.resolvedPath);
            auto cacheTime = std::filesystem::last_write_time(module.cachePath);
            if (cacheTime >= sourceTime)
                module.cacheValid = true;
        }
    } catch (...) {
        module.cacheValid = false;
        module.cachePath.clear();
    }

    return module;
}

Value RoxalCompiler::loadModuleFromCache(const ModuleInfo& module) const
{
    if (module.isProto)
        return Value::nilVal();

    if (!cacheReadEnabled || module.cachePath.empty())
        return Value::nilVal();

    try {
        std::ifstream cacheStream(module.cachePath, std::ios::binary);
        if (!cacheStream.is_open())
            return Value::nilVal();

        char magic[4];
        cacheStream.read(magic, sizeof(magic));
        if (!cacheStream || magic[0] != ModuleCacheMagic[0] ||
            magic[1] != ModuleCacheMagic[1] ||
            magic[2] != ModuleCacheMagic[2] ||
            magic[3] != ModuleCacheMagic[3])
            return Value::nilVal();

        std::uint32_t version = 0;
        cacheStream.read(reinterpret_cast<char*>(&version), sizeof(version));
        if (!cacheStream || version != ModuleCacheVersion)
            return Value::nilVal();

        auto ctx = ptr<SerializationContext>::from_raw(new SerializationContext());
        Value value = readValue(cacheStream, ctx);
        if (!isFunction(value))
            return Value::nilVal();
        reconcileModuleReferences(value);
        return value;
    } catch (...) {
        return Value::nilVal();
    }
}

void RoxalCompiler::storeModuleCache(const ModuleInfo& module, const Value& function) const
{
    if (!cacheWriteEnabled || module.cachePath.empty() || function.isNil() || !isFunction(function))
        return;

    try {
        std::ofstream cacheStream(module.cachePath, std::ios::binary | std::ios::trunc);
        if (!cacheStream.is_open())
            return;

        cacheStream.write(ModuleCacheMagic, sizeof(ModuleCacheMagic));
        cacheStream.write(reinterpret_cast<const char*>(&ModuleCacheVersion), sizeof(ModuleCacheVersion));

        auto ctx = ptr<SerializationContext>::from_raw(new SerializationContext());
        writeValue(cacheStream, function, ctx);
    } catch (...) {
        // ignore cache write failures
    }
}





void RoxalCompiler::outputScopes()
{
    std::cout << "Scopes: ";
    for(auto s = lexicalScopes.cbegin(); s != lexicalScopes.cend(); ++s) {
        std::cout << toUTF8StdString((*s)->name) << "[" << (*s)->typeString() << "]";
        if (s+1 != lexicalScopes.cend())
            std::cout << " : ";
    }
    std::cout << std::endl;
}



void RoxalCompiler::enterModuleScope(const icu::UnicodeString& packageName,
                                    const icu::UnicodeString& moduleName,
                                    const icu::UnicodeString& sourceName,
                                    Value existingModule)
{
    ptr<ModuleScope> moduleScope { make_ptr<ModuleScope>(packageName, moduleName,
                                                         sourceName,
                                                         existingModule) };

    if (moduleScope->moduleType.isObj()) {
        ObjModuleType* moduleType = asModuleType(moduleScope->moduleType);
        std::string sourceUtf8 = toUTF8StdString(sourceName);
        bool assigned = false;
        if (!sourceUtf8.empty()) {
            std::error_code ec;
            std::filesystem::path candidate = std::filesystem::absolute(sourceUtf8, ec);
            if (!ec) {
                std::filesystem::path normalized = candidate.lexically_normal();
                moduleType->sourcePath = toUnicodeString(normalized.string());
                assigned = true;
            }
        }
        if (!assigned)
            moduleType->sourcePath = sourceName;
    }

    lexicalScopes.push_back(moduleScope);
    #ifdef DEBUG_TRACE_SCOPES
    std::cout << "enterModuleScope(" << toUTF8StdString(moduleName) << ")" << std::endl;
    outputScopes();
    #endif
}

void RoxalCompiler::exitModuleScope()
{
    #ifdef DEBUG_BUILD
    if (lexicalScopes.empty())
        throw std::runtime_error("exitModuleScope() stack underflow");
    if ((*scope())->scopeType != LexicalScope::ScopeType::Module)
        throw std::runtime_error("exitModuleScope() - not in module scope");
    #endif
    #ifdef DEBUG_TRACE_SCOPES
    auto module { asModuleScope(moduleScope() };
    std::cout << "exitModuleScope("
              << (!module->packageName.isEmpty() ? toUTF8StdString(module->packageName)+"." : "")
              << toUTF8StdString(module->moduleName) << ")" << std::endl;
    #endif

    // store the ObjModuleType for this module in its associated objFunction, so the VM
    //  can access it at runtime to declare and access module variables
    auto modScope { asModuleScope(scope()) };
    #ifdef DEBUG_BUILD
    assert(!modScope->moduleType.isNil() && modScope->moduleType.isObj());
    assert(!modScope->function.isNil());
    #endif
    asFunction(modScope->function)->moduleType = modScope->moduleType.weakRef();

    lexicalScopes.pop_back();

    #ifdef DEBUG_TRACE_SCOPES
    outputScopes();
    #endif
}


void RoxalCompiler::enterTypeScope(const icu::UnicodeString& typeName)
{
    lexicalScopes.push_back(make_ptr<TypeScope>(typeName));

    #ifdef DEBUG_TRACE_SCOPES
    std::cout << "enterTypeScope(" << toUTF8StdString(typeName) << ")" << std::endl;
    outputScopes();
    #endif
}

void RoxalCompiler::exitTypeScope()
{
    #ifdef DEBUG_BUILD
    if (lexicalScopes.empty())
        throw std::runtime_error("exitTypeScope() stack underflow");
    if ((*scope())->scopeType != LexicalScope::ScopeType::Type)
        throw std::runtime_error("exitTypeScope() - not in type scope");
    #endif
    #ifdef DEBUG_TRACE_SCOPES
    std::cout << "exitTypeScope(" << toUTF8StdString(asTypeScope(typeScope())->name) << ")" << std::endl;
    #endif

    lexicalScopes.pop_back();

    #ifdef DEBUG_TRACE_SCOPES
    outputScopes();
    #endif
}


void RoxalCompiler::enterFuncScope(Value moduleType, const icu::UnicodeString& funcName, FunctionType funcType, ptr<type::Type> type)
{
    // function scopes only valid in a module
    auto modScope { asModuleScope(moduleScope()) };

    ptr<FunctionScope> funcScope {make_ptr<FunctionScope>(modScope->packageName,
                                                          modScope->moduleName,
                                                          modScope->sourceName,
                                                          funcName,funcType,type)};

    asFunction(funcScope->function)->moduleType = moduleType.weakRef();

    lexicalScopes.push_back(funcScope);

    #ifdef DEBUG_TRACE_SCOPES
    std::cout << "enterFuncScope(" << toUTF8StdString(funcName) << ",funcType=" << toString(funcType) << ")" << std::endl;
    outputScopes();
    #endif
}

void RoxalCompiler::exitFuncScope()
{
    #ifdef DEBUG_BUILD
    if (lexicalScopes.empty())
        throw std::runtime_error("exitFuncScope() stack underflow");
    if ((*scope())->scopeType != LexicalScope::ScopeType::Func)
        throw std::runtime_error("exitFuncScope() - not in func scope");
    #endif
    #ifdef DEBUG_TRACE_SCOPES
    std::cout << "exitFuncScope(" << toUTF8StdString(asFuncScope(funcScope())->function->name) << ")" << std::endl;
    #endif

    lexicalScopes.pop_back();

    #ifdef DEBUG_TRACE_SCOPES
    outputScopes();
    #endif
}


void RoxalCompiler::enterLocalScope()
{
    asFuncScope(funcScope())->scopeDepth++;
    asFuncScope(funcScope())->constBindings.emplace_back();
    #ifdef DEBUG_TRACE_SCOPES
    std::cout << "enterLocalScope() depth:" << asFuncScope(funcScope())->scopeDepth << std::endl;
    outputScopes();
    #endif
}

void RoxalCompiler::exitLocalScope()
{
    #ifdef DEBUG_BUILD
    if (lexicalScopes.empty())
        throw std::runtime_error("exitLocalScope() stack underflow");
    if (!inFuncScope())
        throw std::runtime_error("exitLocalScope() - not in func scope");
    if (asFuncScope(funcScope())->scopeDepth == 0)
        throw std::runtime_error("exitLocalScope() depth underflow");
    #endif
    asFuncScope(funcScope())->scopeDepth--;

    auto& locals { asFuncScope(funcScope())->locals };

    while (!locals.empty()
           && locals.back().depth > asFuncScope(funcScope())->scopeDepth) {

        std::string popComment { "local "+toUTF8StdString(locals.back().name)+" depth:"+std::to_string(locals.back().depth) };

        if (locals.back().isCaptured)
            emitByte(OpCode::CloseUpvalue, popComment);
        else
            emitByte(OpCode::Pop, popComment);

        locals.pop_back();
    }
    auto& constBindings = asFuncScope(funcScope())->constBindings;
    if (!constBindings.empty())
        constBindings.pop_back();

    #ifdef DEBUG_TRACE_SCOPES
    std::cout << "exitLexicalScope()" << std::endl;
    outputScopes();
    #endif
}


int RoxalCompiler::scopeDepth() const
{
    return int(lexicalScopes.size());
}

RoxalCompiler::Scope RoxalCompiler::scope()
{
    #ifdef DEBUG_BUILD
    if (lexicalScopes.empty())
        throw std::runtime_error("scope() stack underflow");
    #endif
    return lexicalScopes.end()-1;
}

bool RoxalCompiler::hasEnclosingScope(Scope s)
{
    return (s != lexicalScopes.begin());
}

RoxalCompiler::Scope RoxalCompiler::enclosingScope(RoxalCompiler::Scope s)
{
    #ifdef DEBUG_BUILD
    if (s == lexicalScopes.begin())
        throw std::runtime_error("enclosingScope() stack underflow");
    #endif

    return --s;
}


bool RoxalCompiler::inFuncScope()
{
    for(auto i = lexicalScopes.rbegin(); i != lexicalScopes.rend(); ++i)
        if ((*i)->isFuncOrModule())
            return true;
    return false;
}

bool RoxalCompiler::inFuncScope(Scope s)
{
    // is this a func scope, or is enclosed by one?
    return (*s)->isFuncOrModule() || hasEnclosingFuncScope(s);
}


RoxalCompiler::Scope RoxalCompiler::funcScope()
{
    // find top-most func scope
    auto s = scope();
    while (!(*s)->isFuncOrModule())
        s = enclosingScope(s);
    return s;
}

bool RoxalCompiler::hasEnclosingFuncScope(Scope s)
{
    auto es = s;
    while (hasEnclosingScope(es)) {
        es = enclosingScope(es);
        if ((*es)->isFuncOrModule())
            return true;
    }

    return false;
}


RoxalCompiler::Scope RoxalCompiler::enclosingFuncScope(Scope s)
{
    auto es = enclosingScope(s);
    while (!(*es)->isFuncOrModule())
        es = enclosingScope(es);
    return es;
}

bool RoxalCompiler::inTypeScope()
{
    for(auto i = lexicalScopes.rbegin(); i != lexicalScopes.rend(); ++i)
        if ((*i)->scopeType == LexicalScope::ScopeType::Type)
            return true;
    return false;
}

RoxalCompiler::Scope RoxalCompiler::typeScope()
{
    auto s = scope();
    while ((*s)->scopeType != LexicalScope::ScopeType::Type)
        s = enclosingScope(s);
    return s;
}

RoxalCompiler::Scope RoxalCompiler::enclosingTypeScope(Scope s)
{
    auto es = enclosingScope(s);
    while ((*es)->scopeType != LexicalScope::ScopeType::Type)
        es = enclosingScope(es);
    return es;
}


bool RoxalCompiler::inModuleScope()
{
    for(auto i = lexicalScopes.rbegin(); i != lexicalScopes.rend(); ++i)
        if ((*i)->scopeType == LexicalScope::ScopeType::Module)
            return true;
    return false;
}

RoxalCompiler::Scope RoxalCompiler::moduleScope()
{
    auto s = scope();
    while ((*s)->scopeType != LexicalScope::ScopeType::Module)
        s = enclosingScope(s);
    return s;
}

RoxalCompiler::Scope RoxalCompiler::enclosingModuleScope(Scope s)
{
    auto es = enclosingScope(s);
    while ((*es)->scopeType != LexicalScope::ScopeType::Module)
        es = enclosingScope(es);
    return es;
}



static std::string linePos(ptr<AST> node)
{
    return std::to_string(node->interval.first.line)+":"+std::to_string(node->interval.first.pos);
}


void RoxalCompiler::error(const std::string& message)
{
    if (!currentNode)
        throw std::logic_error(message);
    throw std::logic_error(linePos(currentNode) + " - " + message);
}


ValueType RoxalCompiler::builtinToValueType(ast::BuiltinType bt)
{
    ValueType type {};
    switch(bt) {
        case ast::BuiltinType::Nil: type = ValueType::Nil; break;
        case ast::BuiltinType::Bool: type = ValueType::Bool; break;
        case ast::BuiltinType::Byte: type = ValueType::Byte; break;
        //case ast::BuiltinType::Number:  // not concrete
        case ast::BuiltinType::Int: type = ValueType::Int; break;
        case ast::BuiltinType::Real: type = ValueType::Real; break;
        case ast::BuiltinType::Decimal: type = ValueType::Decimal; break;
        case ast::BuiltinType::String: type = ValueType::String; break;
        case ast::BuiltinType::Range: type = ValueType::Range; break;
        case ast::BuiltinType::List: type = ValueType::List; break;
        case ast::BuiltinType::Dict: type = ValueType::Dict; break;
        case ast::BuiltinType::Vector: type = ValueType::Vector; break;
        case ast::BuiltinType::Matrix: type = ValueType::Matrix; break;
        case ast::BuiltinType::Tensor: type = ValueType::Tensor; break;
        case ast::BuiltinType::Signal: type = ValueType::Signal; break;
        case ast::BuiltinType::Orient: type = ValueType::Orient; break;
        case ast::BuiltinType::Event: type = ValueType::Event; break;
        default:
            throw std::runtime_error("unhandled builtin type "+ast::to_string(bt));
    }
    return type;
}


void RoxalCompiler::emitByte(uint8_t byte, const std::string& comment)
{
    currentChunk()->write(byte, currentNode->interval.first.line,
                          currentNode->interval.first.pos, comment);
}


void RoxalCompiler::emitByte(OpCode op, const std::string& comment)
{
    currentChunk()->write(asByte(op), currentNode->interval.first.line,
                          currentNode->interval.first.pos, comment);
}


void RoxalCompiler::emitBytes(uint8_t byte1, uint8_t byte2, const std::string& comment)
{
    currentChunk()->write(byte1, currentNode->interval.first.line,
                          currentNode->interval.first.pos, comment);
    currentChunk()->write(byte2, currentNode->interval.first.line,
                          currentNode->interval.first.pos);
}

void RoxalCompiler::emitBytes(OpCode op, uint8_t byte2, const std::string& comment)
{
    currentChunk()->write(op, currentNode->interval.first.line,
                          currentNode->interval.first.pos, comment);
    currentChunk()->write(byte2, currentNode->interval.first.line,
                          currentNode->interval.first.pos);
}

void RoxalCompiler::emitBytes(OpCode op, uint8_t byte2, uint8_t byte3, const std::string& comment)
{
    #ifdef DEBUG_BUILD
    if (!isDoubleByte(op))
        std::cerr << "Warning: Emitting single-byte opcode " << int(op) << " with double-byte argument." << std::endl;
    #endif
    currentChunk()->write(op, currentNode->interval.first.line,
                          currentNode->interval.first.pos, comment);
    currentChunk()->write(byte2, currentNode->interval.first.line,
                          currentNode->interval.first.pos);
    currentChunk()->write(byte3, currentNode->interval.first.line,
                          currentNode->interval.first.pos);
}

uint8_t RoxalCompiler::lastByte()
{
    return currentChunk()->lastByte();
}




void RoxalCompiler::emitLoop(Chunk::size_type loopStart, const std::string& comment)
{
    emitByte(OpCode::Loop, comment);

    auto offset = currentChunk()->code.size() - loopStart + 2;
    if (offset > std::numeric_limits<uint16_t>::max())
        error("Loop body contains too many statements.");

    emitByte((uint16_t(offset) >> 8) & 0xff);
    emitByte(uint8_t(uint16_t(offset) & 0xff));
}


Chunk::size_type RoxalCompiler::emitJump(OpCode instruction, const std::string& comment)
{
    emitByte(instruction, comment);
    emitByte(0xff);
    emitByte(0xff);
    return currentChunk()->code.size() - 2;
}



void RoxalCompiler::emitReturn(const std::string& comment)
{
    if (asFuncScope(funcScope())->functionType == FunctionType::Initializer)
        emitBytes(OpCode::GetLocal, uint8_t(0));
    else
        emitByte(OpCode::ConstNil, comment);

    emitByte(OpCode::Return);
}


void RoxalCompiler::emitConstant(const Value& value, const std::string& comment)
{
    uint16_t constant = makeConstant(value);
    emitOpArgsBytes(OpCode::Constant, constant, comment);
}


void RoxalCompiler::patchJump(Chunk::size_type jumpInstrOffset)
{
    int32_t jumpDist = (currentChunk()->code.size() - jumpInstrOffset) - 2;

    if (jumpDist > std::numeric_limits<uint16_t>::max()) {
        error("Too must code in conditional block");
    }

    currentChunk()->code[jumpInstrOffset] = (uint16_t(jumpDist) >> 8) & 0xff;
    currentChunk()->code[jumpInstrOffset+1] = uint8_t(uint16_t(jumpDist) & 0xff);
}


uint16_t RoxalCompiler::makeConstant(const Value& value)
{
    size_t constant = currentChunk()->addConstant(value);
    if (constant >= std::numeric_limits<uint16_t>::max()) {
        error("Too many constants in one chunk.");
        return 0;
    }
    return uint16_t(constant);
}


uint16_t RoxalCompiler::identifierConstant(const icu::UnicodeString& ident)
{
    // search for existing identifier string constant to re-use first
    bool found { false };
    uint16_t constant {};
    for(auto identConst : asFuncScope(funcScope())->identConsts) {
        if (asStringObj(currentChunk()->constants.at(identConst))->s == ident) {
            constant = identConst;
            found = true;
            break;
        }
    }

    if (!found) {
        // not found, create new string constant
        //  (globals are late bound, so it may only be declared afterward)
        constant = makeConstant(Value::stringVal(ident));
        asFuncScope(funcScope())->identConsts.push_back(constant);
    }
    return constant;
}


void RoxalCompiler::addLocal(const icu::UnicodeString& name, std::optional<VarTypeSpec> type)
{
    //std::cout << (&(*state()) - &(*states.begin())) << " addLocal(" << toUTF8StdString(name) << ")" << std::endl;
    if (asFuncScope(funcScope())->locals.size() == 255) {
        error("Maximum of 255 local variables per function exceeded.");
        return;
    }
    #ifdef DEBUG_TRACE_NAME_RESOLUTION
    std::cout << "addLocal(" << toUTF8StdString(name) << ")" << std::endl;
    #endif

    asFuncScope(funcScope())->locals.push_back(Local(name, -1, type)); // scopeDepth=-1 --> uninitialized

    #ifdef DEBUG_BUILD
    auto index { asFuncScope(funcScope())->locals.size()-1 };
    emitByte(OpCode::Nop, "local "+toUTF8StdString(name)+ "("+std::to_string(index)+") depth:"+std::to_string(asFuncScope(funcScope())->scopeDepth));
    #endif

}


int16_t RoxalCompiler::resolveLocal(Scope scopeState, const icu::UnicodeString& name)
{
    #ifdef DEBUG_BUILD
    if (!(*scopeState)->isFuncOrModule())
        throw std::runtime_error("resolveLocal() scopeState is not a func/module scope");
    #endif
    #ifdef DEBUG_TRACE_NAME_RESOLUTION
    std::cout << "resolveLocal(scope=" << toUTF8StdString((*scopeState)->name) << ", " << toUTF8StdString(name) << ")";
    #endif
    //std::cout << (&(*scopeState) - &(*states.begin()))<< " resolveLocal(" << toUTF8StdString(name) << ")" << std::endl;
    auto locals { asFuncScope(scopeState)->locals };
    if (!locals.empty())
        for(int32_t i=locals.size()-1; i>=0; i--) {
            #ifdef DEBUG_BUILD
                if (locals.at(i).name == name) {
            #else
                if (locals[i].name == name) {
            #endif
                    if (locals[i].depth == -1)
                        error("Reference to local variable in initializer not allowed.");
                    #ifdef DEBUG_TRACE_NAME_RESOLUTION
                    std::cout << " - found " << i << std::endl;
                    #endif
                    return i;
                }
        }

    #ifdef DEBUG_TRACE_NAME_RESOLUTION
    std::cout << " - not found" << std::endl;
    #endif
    return -1;
}


int RoxalCompiler::addUpvalue(Scope scopeState, uint8_t index, bool isLocal)
{
    //std::cout << (&(*scopeState) - &(*states.begin())) << " addUpvalue(" << index << " " << (isLocal ? "local" : "notlocal") << ")" << std::endl;
    int upvalueCount = asFunction(asFuncScope(scopeState)->function)->upvalueCount;
    auto& upvalues { asFuncScope(scopeState)->upvalues };

    for (int i=0; i<upvalueCount; i++) {
        const Upvalue& upvalue = upvalues[i];
        if (upvalue.index == index && upvalue.isLocal == isLocal)
            return i;
    }

    if (upvalueCount == std::numeric_limits<uint8_t>::max()) {
        error("Maximum closure variables exceeded in function.");
        return 0;
    }

    upvalues.push_back(Upvalue(index, isLocal));

    // std::cout << "Upvalues: ";
    // for(int i=0; i<upvalues.size();i++) {
    //     std:: cout << int(upvalues[i].index) << (upvalues[i].isLocal?"L":"n") << "  ";
    // }
    // std::cout << std::endl;
    // std::cout << "  function.upvalueCount="+std::to_string(upvalueCount) << std::endl;
    return asFunction(asFuncScope(scopeState)->function)->upvalueCount++;
}


int16_t RoxalCompiler::resolveUpvalue(Scope scopeState, const icu::UnicodeString& name)
{
    //std::cout << (&(*scopeState) - &(*states.begin())) << " resolveUpvalue(" << toUTF8StdString(name) << ")" << std::endl;
    //std::string sname { toUTF8StdString(name) };
    #ifdef DEBUG_TRACE_NAME_RESOLUTION
    std::cout << "resolveUpvalue(scope=" << toUTF8StdString((*scopeState)->name) << ", " << toUTF8StdString(name) << ")";
    #endif

    if (!hasEnclosingFuncScope(scopeState)) { // no enclosing func scope
        #ifdef DEBUG_TRACE_NAME_RESOLUTION
        std::cout << " - not found" << std::endl;
            #ifdef DEBUG_TRACE_SCOPES
            outputScopes();
            #endif
        #endif
        return -1;
    }

    int local = resolveLocal(enclosingFuncScope(scopeState), name);
    if (local != -1) {
        #ifdef DEBUG_BUILD
        asFuncScope(enclosingFuncScope(scopeState))->locals.at(local).isCaptured = true;
        #else
        asFuncScope(enclosingFuncScope(scopeState))->locals[local].isCaptured = true;
        #endif
        return addUpvalue(scopeState, uint8_t(local), true);
    }

    int upvalue = resolveUpvalue(enclosingFuncScope(scopeState), name);
    if (upvalue != -1) {
        #ifdef DEBUG_TRACE_NAME_RESOLUTION
        std::cout << " - found " << upvalue << std::endl;
        #endif
        return addUpvalue(scopeState, uint8_t(upvalue), false);
    }

    #ifdef DEBUG_TRACE_NAME_RESOLUTION
    std::cout << " - not found" << std::endl;
    #endif
    return -1;
}



bool RoxalCompiler::constExistsInCurrentScope(const icu::UnicodeString& name) const
{
    for (auto it = lexicalScopes.crbegin(); it != lexicalScopes.crend(); ++it) {
        if (!(*it)->isFuncOrModule())
            continue;
        auto func = dynamic_ptr_cast<FunctionScope>(*it);
        if (!func)
            continue;
        if (func->constBindings.empty())
            return false;
        const auto& current = func->constBindings.back();
        return current.find(name) != current.end();
    }
    return false;
}

bool RoxalCompiler::moduleConstExists(const icu::UnicodeString& name) const
{
    for (auto it = lexicalScopes.crbegin(); it != lexicalScopes.crend(); ++it) {
        if ((*it)->scopeType != LexicalScope::ScopeType::Module)
            continue;
        auto module = dynamic_ptr_cast<ModuleScope>(*it);
        if (!module)
            continue;
        return module->moduleConstLines.find(name) != module->moduleConstLines.end();
    }
    return false;
}

const RoxalCompiler::FunctionScope::ConstBinding* RoxalCompiler::lookupConstBinding(const icu::UnicodeString& name) const
{
    for (auto it = lexicalScopes.crbegin(); it != lexicalScopes.crend(); ++it) {
        if (!(*it)->isFuncOrModule())
            continue;
        auto func = dynamic_ptr_cast<FunctionScope>(*it);
        if (!func)
            continue;
        for (auto mapIt = func->constBindings.rbegin(); mapIt != func->constBindings.rend(); ++mapIt) {
            auto found = mapIt->find(name);
            if (found != mapIt->end())
                return &found->second;
        }
    }
    return nullptr;
}


void RoxalCompiler::declareVariable(const icu::UnicodeString& name, std::optional<VarTypeSpec> type)
{
    if (asFuncScope(funcScope())->scopeDepth == 0) {
        auto module = asModuleScope(moduleScope());
        auto varIt = module->moduleVarLines.find(name);
        if (varIt != module->moduleVarLines.end()) {
            error("A variable with this name already exists in this scope (previously declared at line " + std::to_string(varIt->second.line) + ").");
        }
        auto constIt = module->moduleConstLines.find(name);
        if (constIt != module->moduleConstLines.end()) {
            error("A const with this name already exists in this scope (previously declared at line " + std::to_string(constIt->second.line) + ").");
        }
        module->moduleVarLines[name] = currentNode->interval.first;
        if (type.has_value())
            module->moduleVarTypes[name] = type.value();
        return;
    }

    if (constExistsInCurrentScope(name)) {
        error("A const with this name already exists in this scope.");
    }

    // check there is no variable with the same name in this scope (an error)
    for(auto li = asFuncScope(funcScope())->locals.rbegin(); li != asFuncScope(funcScope())->locals.rend(); ++li) {
        if ((li->depth != -1) && (li->depth < asFuncScope(funcScope())->scopeDepth))
            break;

        if (li->name == name) {
            error("A variable with this name already exists in this scope.");
        }
    }

    addLocal(name, type);
}

std::optional<RoxalCompiler::VarTypeSpec> RoxalCompiler::localVarType(const icu::UnicodeString& name)
{
    auto& locals { asFuncScope(funcScope())->locals };
    if (!locals.empty()) {
        for(int i = locals.size()-1; i>=0; i--) {
            if (locals[i].name == name) {
                if (locals[i].type.has_value())
                    return locals[i].type.value();
                break;
            }
        }
    }
    return {};
}

void RoxalCompiler::declareConstant(const icu::UnicodeString& name, const Value& value, std::optional<VarTypeSpec> type)
{
    auto func = asFuncScope(funcScope());
    if (func->scopeDepth == 0) {
        auto module = asModuleScope(moduleScope());

        auto varIt = module->moduleVarLines.find(name);
        if (varIt != module->moduleVarLines.end()) {
            error("A variable with this name already exists in this scope (previously declared at line " + std::to_string(varIt->second.line) + ").");
        }
        auto constIt = module->moduleConstLines.find(name);
        if (constIt != module->moduleConstLines.end()) {
            error("A const with this name already exists in this scope (previously declared at line " + std::to_string(constIt->second.line) + ").");
        }

        ObjModuleType* moduleTypeObj = asModuleType(module->moduleType);
        moduleTypeObj->constVars.insert(name.hashCode());

        module->moduleConstLines[name] = currentNode->interval.first;
        module->moduleVarLines[name] = currentNode->interval.first;
        if (type.has_value())
            module->moduleVarTypes[name] = type.value();
    }
    else {
        if (constExistsInCurrentScope(name))
            error("A const with this name already exists in this scope.");

        auto& locals = func->locals;
        for (auto li = locals.rbegin(); li != locals.rend(); ++li) {
            if ((li->depth != -1) && (li->depth < func->scopeDepth))
                break;
            if (li->name == name)
                error("A variable with this name already exists in this scope.");
        }
    }

    auto& constMap = func->constBindings.back();
    auto [it, inserted] = constMap.emplace(name, FunctionScope::ConstBinding{value, currentNode->interval.first});
    if (!inserted)
        error("A const with this name already exists in this scope.");
}

Value RoxalCompiler::applyConstType(Value value, std::optional<VarTypeSpec> type, bool strictContext)
{
    if (!type.has_value())
        return value;

    if (std::holds_alternative<type::BuiltinType>(*type)) {
        auto builtin = std::get<type::BuiltinType>(*type);
        ValueType vt = builtinToValueType(builtin);
        if (vt == ValueType::String || vt == ValueType::Range || vt == ValueType::List ||
            vt == ValueType::Dict || vt == ValueType::Vector || vt == ValueType::Matrix ||
            vt == ValueType::Signal || vt == ValueType::Tensor || vt == ValueType::Orient || vt == ValueType::Event) {
            error("Const declarations are currently limited to builtin value types.");
        }
        try {
            return toType(vt, value, strictContext);
        } catch (const std::exception& e) {
            error(std::string("Unable to convert const initializer to declared type: ") + e.what());
        }
    }

    error("Const declarations currently support only builtin types.");
    return value; // unreachable, keeps compiler happy
}

Value RoxalCompiler::evaluateConstExpression(ptr<ast::Expression> expr, bool strictContext)
{
    if (!expr)
        error("Const declarations require an initializer.");

    if (isa<ast::Literal>(expr)) {
        auto literal = as<ast::Literal>(expr);
        switch (literal->literalType) {
            case ast::Literal::Nil:
                return Value::nilVal();
            case ast::Literal::Bool: {
                auto bl = as<ast::Bool>(expr);
                return Value::boolVal(bl->value);
            }
            case ast::Literal::Num: {
                auto num = as<ast::Num>(expr);
                if (std::holds_alternative<double>(num->num))
                    return Value::realVal(std::get<double>(num->num));
                else
                    return Value::intVal(std::get<int32_t>(num->num));
            }
            default:
                error("Const initializers currently support only nil, bool, and numeric literals.");
        }
    }

    if (auto unary = dynamic_ptr_cast<ast::UnaryOp>(expr)) {
        auto operand = evaluateConstExpression(unary->arg, strictContext);
        switch (unary->op) {
            case ast::UnaryOp::Negate:
                if (operand.isReal())
                    return Value::realVal(-operand.asReal());
                if (operand.isInt())
                    return Value::intVal(-operand.asInt());
                error("Unary '-' constant expressions require numeric operands.");
                break;
            case ast::UnaryOp::Not:
                if (operand.isBool())
                    return Value::boolVal(!operand.asBool());
                error("Unary 'not' constant expressions require boolean operands.");
                break;
            case ast::UnaryOp::BitNot:
                if (operand.isInt())
                    return Value::intVal(~operand.asInt());
                error("Unary '~' constant expressions require integer operands.");
                break;
            default:
                break;
        }
        error("Unsupported unary operator in const initializer.");
    }

    if (auto variable = dynamic_ptr_cast<ast::Variable>(expr)) {
        auto binding = lookupConstBinding(variable->name);
        if (!binding)
            error("Const initializer references an identifier that is not a const.");
        return binding->value;
    }

    error("Const initializer must be a compile-time constant expression.");
    return Value::nilVal(); // unreachable, suppress compiler warning
}

std::optional<RoxalCompiler::VarTypeSpec> RoxalCompiler::moduleVarType(const icu::UnicodeString& name)
{
    auto module = asModuleScope(moduleScope());
    auto it = module->moduleVarTypes.find(name);
    if (it != module->moduleVarTypes.end())
        return it->second;
    return {};
}


void RoxalCompiler::defineVariable(uint16_t moduleVar, bool isConst)
{
    // local variables are already on the stack
    if (asFuncScope(funcScope())->scopeDepth > 0) {
        // mark initialized
        asFuncScope(funcScope())->locals.back().depth = asFuncScope(funcScope())->scopeDepth;
        return;
    }

    // emit code to define named module scope variable at runtime
    emitOpArgsBytes(isConst ? OpCode::DefineModuleConst : OpCode::DefineModuleVar, moduleVar);
}


bool RoxalCompiler::namedVariable(const icu::UnicodeString& name, bool assign, bool asSignal)
{
    //std::cout << (&(*state()) - &(*states.begin())) << " namedVariable(" << toUTF8StdString(name) << ")" << std::endl;
    //std::cout << toUTF8StdString(funcScope()->function->name) << " namedVariable(" << toUTF8StdString(name) << ")" << std::endl;

    if (auto binding = lookupConstBinding(name)) {
        if (asSignal)
            error("'changes' requires a module variable binding; use a signal expression instead");
        if (assign) {
            std::string message = "Cannot assign to constant '" + toUTF8StdString(name) + "'";
            if (binding->line.line > 0)
                message += " (declared at line " + std::to_string(binding->line.line) + ")";
            error(message);
        }
        emitConstant(binding->value, toUTF8StdString(name));
        return true;
    }

    OpCode getOp, setOp;
    bool found = false;

    uint16_t arg;
    int16_t localArg = resolveLocal(funcScope(),name);
    if (localArg != -1) { // found
        if (asSignal)
            error("'changes' requires a module variable binding; use a signal expression instead");
        found = true;
        arg = localArg;
        getOp = OpCode::GetLocal;
        setOp = OpCode::SetLocal;
    }
    // else if ((funcScope()->functionType == FunctionType::Method ) && ((arg = resolveLocal(funcScope(),"this") != -1))) {
    //     // if we have a property name, allow access without 'this.' prefix
    //     if (!typeScopes.empty()) {
    //         // FIXME: statically check the property exists.. (otherwise we're blocking all outer scope local access..)
    //         //std::cout << funcScope() << std::endl;
    //         arg = identifierConstant(name);
    //         namedVariable("this", false);
    //         //emitBytes(OpCode::GetProp, uint8_t(identConstant));
    //         getOp = OpCode::GetProp;
    //         setOp = OpCode::SetProp;
    //         found = true;
    //     }
    // }

    int16_t upValueArg;
    if (!found && ((upValueArg = resolveUpvalue(funcScope(),name)) != -1)) {
        if (asSignal)
            error("'changes' requires a module variable binding; use a signal expression instead");
        found = true;
        arg = upValueArg;
        getOp = OpCode::GetUpvalue;
        setOp = OpCode::SetUpvalue;
    }

    if (!found) { // local or upvalue not found
        // try module scope first
        arg = identifierConstant(name);
        getOp = OpCode::GetModuleVar;
        auto module = asModuleScope(moduleScope());
        auto moduleVarIt = module->moduleVarLines.find(name);
        bool exists = moduleVarIt != module->moduleVarLines.end();
        bool isModuleConst = module->moduleConstLines.find(name) != module->moduleConstLines.end();

        bool inActorMethod = inTypeScope() && asTypeScope(typeScope())->isActor &&
                             (asFuncScope(funcScope())->functionType == FunctionType::Method ||
                              asFuncScope(funcScope())->functionType == FunctionType::Initializer);
        if (inActorMethod && exists && !isModuleConst) {
            error("Actor methods cannot access module variable '"+toUTF8StdString(name)+"'; use a module constant instead.");
        }
        if (asFuncScope(funcScope())->functionType != FunctionType::Module || exists)
            setOp = OpCode::SetModuleVar;
        else
            setOp = OpCode::SetNewModuleVar;
        if (assign && asFuncScope(funcScope())->functionType == FunctionType::Module && !exists &&
            !asFuncScope(funcScope())->strict)
            module->moduleVarLines[name] = currentNode->interval.first;

        // if module variable isn't found at runtime, the VM will raise an error.
        // to allow implicit property access, check for 'this' method context as fallback
        if (asFuncScope(funcScope())->functionType == FunctionType::Method ||
            asFuncScope(funcScope())->functionType == FunctionType::Initializer) {
            int16_t thisLocal = resolveLocal(funcScope(), UnicodeString("this"));
            auto itMem = asTypeScope(typeScope())->propertyNames.find(name);
            if (thisLocal != -1 && itMem != asTypeScope(typeScope())->propertyNames.end()) {
                const auto& info = itMem->second;
                if (info.access == Access::Private && info.owner != asTypeScope(typeScope())->name)
                    error("Cannot access private member '"+toUTF8StdString(name)+"'");
                if (assign && info.isConst)
                    error("Cannot assign to const property '"+toUTF8StdString(name)+"'");
                // treat as property access
                if (!assign) {
                    namedVariable(UnicodeString("this"), false);
                    if (asSignal)
                        emitOpArgsBytes(OpCode::GetPropSignal, arg, toUTF8StdString(name));
                    else
                        emitOpArgsBytes(OpCode::GetProp, arg);
                } else {
                    namedVariable(UnicodeString("this"), false);
                    emitByte(OpCode::Swap);
                    emitOpArgsBytes(OpCode::SetProp, arg);
                }
                return true;
            }
        }
    }

    if (!assign) {
        if (asSignal) {
            if (getOp != OpCode::GetModuleVar)
                error("'changes' requires a module variable binding; use a signal expression instead");
            emitOpArgsBytes(OpCode::GetModuleVarSignal, arg, toUTF8StdString(name));
        } else {
            emitOpArgsBytes(getOp, arg, toUTF8StdString(name));
        }
    }
    else
        emitOpArgsBytes(setOp, arg, toUTF8StdString(name));

    return true;
}


void RoxalCompiler::namedModuleVariable(const icu::UnicodeString& name, bool assign)
{
    OpCode getOp, setOp;

    uint16_t arg = identifierConstant(name);
    getOp = OpCode::GetModuleVar;
    //  allow assigning without previously declaring, except within functions
    if (asFuncScope(funcScope())->functionType != FunctionType::Module)
        setOp = OpCode::SetModuleVar;
    else
        setOp = OpCode::SetNewModuleVar;

    if (!assign) {
        emitOpArgsBytes(getOp, arg, toUTF8StdString(name));
    }
    else {
        emitOpArgsBytes(setOp, arg, toUTF8StdString(name));
    }
}


std::ostream& roxal::operator<<(std::ostream& out, const RoxalCompiler::ModuleInfo& mi) {
    out << "ModuleInfo {"
        << "modulePathRoot: " << mi.modulePathRoot << ", "
        << "packagePath: " << toUTF8StdString(mi.packagePath) << ", "
        << "name: " << toUTF8StdString(mi.name) << ", "
        << "isPackage: " << (mi.isPackage ? "true" : "false") << ", "
        << "filename: " << mi.filename << ", "
        << "invalidFolder: " << (mi.invalidFolder ? "true" : "false")
        << "}";
    return out;
}
