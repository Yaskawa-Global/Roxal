
#include <filesystem>
#include <boost/program_options.hpp>
#include <stdexcept>
#include <sstream>
#include <cstdlib>
#include <csignal>
#include <fstream>
#include <limits>
#include <cstdint>
#include <string>
#include <memory>
#include <cstddef>
#include <iomanip>
#include <optional>
#include <algorithm>
#include <iostream>
#include <vector>
#include <unordered_set>
#include <cctype>
#include <iterator>

#include <core/AST.h>
#include "core/common.h"
#include "RoxalIndentationLexer.h"
#include "ASTGenerator.h"
#include "TypeDeducer.h"
#include "ASTGraphviz.h"
#include "RoxalCompiler.h"

#include "VM.h"
#include "Error.h"
#include "SimpleMarkSweepGC.h"
#include "Object.h"
#include "Introspection.h"


extern "C" {
    #include "linenoise.h"
}


using namespace antlr4;
using namespace roxal;
namespace po = boost::program_options;

static void sigint_handler(int)
{
    try {
        VM::instance().dumpStackTraces();
    } catch (...) {
    }
    std::signal(SIGINT, SIG_DFL);
    std::raise(SIGINT);
}


enum class CompletionKind {
    None,
    ModuleSymbol,
    MemberSymbol
};

struct CompletionContext {
    CompletionKind kind { CompletionKind::None };
    std::string owner;
    std::string prefix;
};

static bool isIdentifierStart(char ch)
{
    return std::isalpha(static_cast<unsigned char>(ch)) || ch == '_';
}

static bool isIdentifierPart(char ch)
{
    return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
}

static bool isIdentifier(const std::string& candidate)
{
    if (candidate.empty() || !isIdentifierStart(candidate.front()))
        return false;
    return std::all_of(std::next(candidate.begin()), candidate.end(), isIdentifierPart);
}

static CompletionContext analyzeCompletionContext(const std::string& buffer)
{
    CompletionContext context {};

    // Work only with the content after the last newline; the REPL feeds one line at a time.
    const size_t lineStart = buffer.find_last_of('\n');
    const size_t sliceStart = lineStart == std::string::npos ? 0 : lineStart + 1;
    std::string line = buffer.substr(sliceStart);

    // Ignore trailing whitespace so "foo<space><tab>" still sees the last token.
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
        line.pop_back();

    if (line.empty()) {
        context.kind = CompletionKind::ModuleSymbol;
        return context;
    }

    auto isTokenChar = [](char ch) {
        return isIdentifierPart(ch) || ch == '.';
    };

    size_t tokenStart = line.size();
    while (tokenStart > 0 && isTokenChar(line[tokenStart - 1]))
        --tokenStart;

    const std::string token = line.substr(tokenStart);
    if (token.empty()) {
        context.kind = CompletionKind::ModuleSymbol;
        return context;
    }

    const size_t dot = token.rfind('.');
    if (dot != std::string::npos) {
        const std::string owner = token.substr(0, dot);
        const std::string memberPrefix = token.substr(dot + 1);
        if (isIdentifier(owner)) {
            context.kind = CompletionKind::MemberSymbol;
            context.owner = owner;
            context.prefix = memberPrefix;
        }
        return context;
    }

    if (isIdentifier(token)) {
        context.kind = CompletionKind::ModuleSymbol;
        context.prefix = token;
    }
    return context;
}

static void addCompletions(linenoiseCompletions* lc,
                           const std::vector<std::string>& names,
                           const std::string& prefix)
{
    std::unordered_set<std::string> seen {};
    for (const auto& name : names) {
        if (!prefix.empty() && name.rfind(prefix, 0) != 0)
            continue;
        if (!seen.insert(name).second)
            continue;
        linenoiseAddCompletion(lc, name.c_str());
    }
}

static std::vector<std::string> moduleSymbolNames(ObjModuleType* module)
{
    std::vector<std::string> names;
    if (!module)
        return names;

    const auto vars = module->vars.snapshot();
    names.reserve(vars.size());
    for (const auto& entry : vars) {
        if (!entry.first.isEmpty())
            names.push_back(toUTF8StdString(entry.first));
    }

    // Include builtin globals so common helpers remain discoverable.
    const auto globals = module->vars.snapshotGlobals();
    for (const auto& entry : globals) {
        if (!entry.first.isEmpty())
            names.push_back(toUTF8StdString(entry.first));
    }

    return names;
}

static std::optional<Value> lookupModuleSymbol(ObjModuleType* module, const std::string& name)
{
    if (!module)
        return std::nullopt;

    const auto vars = module->vars.snapshot();
    for (const auto& entry : vars) {
        if (toUTF8StdString(entry.first) == name)
            return entry.second;
    }

    const auto globals = module->vars.snapshotGlobals();
    for (const auto& entry : globals) {
        if (toUTF8StdString(entry.first) == name)
            return entry.second;
    }

    return std::nullopt;
}

static ObjObjectType* resolveObjectType(const Value& value)
{
    if (isObjectInstance(value))
        return asObjectType(asObjectInstance(value)->instanceType);
    if (isActorInstance(value))
        return asObjectType(asActorInstance(value)->instanceType);
    if (isObjectType(value))
        return asObjectType(value);
    return nullptr;
}

static std::vector<std::string> memberNamesForValue(const Value& value)
{
    std::vector<std::string> names;
    ObjObjectType* type = resolveObjectType(value);
    if (!type)
        return names;

    const auto properties = collectPropertyEntries(type);
    names.reserve(properties.size());
    for (const auto& property : properties)
        names.push_back(property.name);

    const auto methods = collectMethodEntries(type);
    for (const auto& method : methods)
        names.push_back(method.name);

    return names;
}

static void replCompletionCallback(const char* buffer, linenoiseCompletions* lc)
{
    if (!buffer || !lc)
        return;

    try {
        const CompletionContext context = analyzeCompletionContext(buffer);
        if (context.kind == CompletionKind::None)
            return;

        ObjModuleType* module = VM::instance().replModuleType();
        if (!module)
            return;

        if (context.kind == CompletionKind::ModuleSymbol) {
            addCompletions(lc, moduleSymbolNames(module), context.prefix);
            return;
        }

        if (context.kind == CompletionKind::MemberSymbol) {
            const auto symbol = lookupModuleSymbol(module, context.owner);
            if (!symbol.has_value())
                return;
            addCompletions(lc, memberNamesForValue(symbol.value()), context.prefix);
        }
    } catch (...) {
        // Suppress completion exceptions to avoid interrupting the REPL.
    }
}


static int indentationLength(const std::string& line)
{
    int count = 0;
    for(char ch : line) {
        if (ch == ' ')
            count++;
        else if (ch == '\t')
            count += 8 - (count % 8);
        else
            break;
    }
    return count;
}

static std::string rstrip(const std::string& s)
{
    size_t end = s.find_last_not_of(" \t");
    if (end == std::string::npos)
        return "";
    return s.substr(0,end+1);
}

static std::string lstrip(const std::string& s)
{
    size_t start = s.find_first_not_of(" \t");
    if (start == std::string::npos)
        return "";
    return s.substr(start);
}

static std::string trimws(const std::string& s)
{
    return rstrip(lstrip(s));
}

static std::string buildDate()
{
    // __DATE__ format: "Mmm dd yyyy"
    const char* raw = __DATE__;
    std::string monthStr(raw, 3);
    std::string dayStr(raw + 4, 2);
    std::string yearStr(raw + 7, 4);

    static const char* months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                   "Jul","Aug","Sep","Oct","Nov","Dec"};
    int month = 1;
    for (int i = 0; i < 12; ++i) {
        if (monthStr == months[i]) {
            month = i + 1;
            break;
        }
    }
    int day = std::stoi(trimws(dayStr));

    std::ostringstream oss;
    oss << yearStr << "-" << std::setfill('0') << std::setw(2) << month
        << "-" << std::setw(2) << day;
    return oss.str();
}

static std::optional<std::filesystem::path> replHistoryPath()
{
    // Resolve a per-user history file; absence of a home directory disables persistence silently.
    const char* home = std::getenv("HOME");
    const char* userProfile = std::getenv("USERPROFILE");
    const char* homeDrive = std::getenv("HOMEDRIVE");
    const char* homePath = std::getenv("HOMEPATH");

    std::filesystem::path base;
    if (home) {
        base = home;
    } else if (userProfile) {
        base = userProfile;
    } else if (homeDrive && homePath) {
        base = std::filesystem::path(homeDrive) / homePath;
    } else {
        return std::nullopt;
    }

    return base / ".roxal_history";
}

static int repl()
{
    linenoiseHistorySetMaxLen(1000);

    // Register a lightweight completion hook to surface module and member symbols.
    linenoiseSetCompletionCallback(replCompletionCallback);

    const auto historyPath = replHistoryPath();
    if (historyPath) {
        linenoiseHistoryLoad(historyPath->string().c_str());
    }

    std::stringstream stream;
    VM& vm { VM::instance() };

    std::string buffer;
    std::vector<int> indents {0};
    bool waitingIndent = false;
    bool quit = false;

    int exitCode = 0;
    while(!quit) {
        const char* prompt = (waitingIndent || indents.size()>1) ? "...> " : "rox> ";
        char* cline = linenoise(prompt);
        if (!cline)
            break;

        linenoiseHistoryAdd(cline);
        if (historyPath) {
            linenoiseHistorySave(historyPath->string().c_str());
        }
        std::string line(cline);
        linenoiseFree(cline);

        if (line=="quit") {
            quit = true;
            break;
        } else if (line.rfind("run ", 0) == 0) {
            std::string path = trimws(line.substr(4));
            if (path.empty()) {
                std::cerr << "Error: no file specified" << std::endl;
            } else {
                std::ifstream script(path);
                if (!script.is_open()) {
                    std::cerr << "Error: file not found: " << path << std::endl;
                } else {
                    std::filesystem::path filePath = std::filesystem::absolute(path);
                    std::filesystem::path parentPath = filePath.parent_path();
                    std::filesystem::path currentPath = std::filesystem::current_path();
                    std::filesystem::path relativePath = std::filesystem::relative(parentPath, currentPath);
                    vm.appendModulePaths({relativePath.string()});
                    std::stringstream scriptStream;
                    scriptStream << script.rdbuf();
                    try {
                        vm.interpretLine(scriptStream, false);
                    } catch (std::exception& e) {
                        std::cerr << "Error: " << e.what() << std::endl;
                    }
                }
            }
            buffer.clear();
            indents.assign(1,0);
            waitingIndent = false;
            if (vm.isExitRequested()) {
                exitCode = vm.exitCode();
                break;
            }
            continue;
        }

        int indent = indentationLength(line);
        while(indent < indents.back())
            indents.pop_back();
        if (waitingIndent && indent > indents.back()) {
            indents.push_back(indent);
            waitingIndent = false;
        } else if (!waitingIndent && indent > indents.back()) {
            indents.push_back(indent);
        }

        buffer += line + "\n";

        std::string trimmed = rstrip(line);
        bool endsWithColon = !trimmed.empty() && trimmed.back() == ':';
        if (endsWithColon)
            waitingIndent = true;

        bool complete = !waitingIndent && indents.size()==1 && ( !trimmed.empty() || line.empty() );

        if (complete) {
            try {
                stream.str("");
                stream.clear();
                stream << buffer << std::flush;
                vm.interpretLine(stream);
            } catch (std::exception& e) {
                std::cerr << "Error: " << e.what() << std::endl;
            }
            buffer.clear();
            indents.assign(1,0);
        }

        if (vm.isExitRequested()) {
            exitCode = vm.exitCode();
            break;
        }
    }

    return exitCode;
}


static InterpretResult runFile(const std::string& path,
                               const std::vector<std::string>& modulePaths,
                               bool outputBytecodeDisassembly=false)
{

    std::filesystem::path filePath(path);
    std::ifstream sourcestream(filePath); // assumed UTF-8
    if (!sourcestream.is_open()) {
        for(const auto& modPath : modulePaths) {
            std::filesystem::path candidate = std::filesystem::path(modPath) / filePath;
            sourcestream.open(candidate);
            if (sourcestream.is_open()) {
                filePath = candidate;
                break;
            }
        }
    }

    if (!sourcestream.is_open())
        throw std::runtime_error("file not found: " + path);

    std::filesystem::path fileAndPath(filePath);
    std::string name { fileAndPath.stem().filename().string() };

    // construct a relative directory path containing the file, from the current working directory
    std::filesystem::path absolutePath = std::filesystem::absolute(filePath);
    std::filesystem::path currentPath = std::filesystem::current_path();
    std::filesystem::path parentPath = absolutePath.parent_path();
    std::filesystem::path relativePath = std::filesystem::relative(parentPath, currentPath);

    VM* vm { nullptr };
    try {
        vm = &VM::instance();
    } catch (std::exception& e) {
        throw std::runtime_error("Failed to initialize VM: " + std::string(e.what()));
    }

    std::signal(SIGINT, sigint_handler);

    try {
        vm->setDisassemblyOutput(outputBytecodeDisassembly);
        // Add the folder containing the script to the search paths
        vm->appendModulePaths({relativePath.string()});
        vm->appendModulePaths(modulePaths);
        return vm->interpret(sourcestream, filePath.string());
    } catch (std::exception& e) {
        throw std::runtime_error("Error interpreting file '" + filePath.string() + "': " + e.what());
    }
}

static InterpretResult runString(const std::string& source,
                                 const std::vector<std::string>& modulePaths,
                                 bool outputBytecodeDisassembly=false)
{
    std::istringstream sourcestream(source);
    VM& vm { VM::instance() };
    std::signal(SIGINT, sigint_handler);
    vm.setDisassemblyOutput(outputBytecodeDisassembly);
    vm.appendModulePaths(modulePaths);
    return vm.interpret(sourcestream, "cli");
}


static void generateAST(const std::string& inputPath, bool graph, const std::string& outputPath="")
{
    std::ifstream sourcestream(inputPath); // assumed UTF-8

    std::filesystem::path fileAndPath(inputPath);
    std::string name { fileAndPath.stem().filename().string() };

    roxal::ptr<ast::AST> ast {};
    try {
        ASTGenerator astGenerator {};
        ast = astGenerator.ast(sourcestream, name);
    } catch (std::exception& e) {
        compileError(e.what());
        clearCompileContext();
        return;
    }
    if (ast == nullptr) // must have been a parse or AST gen error (already reported?)
        return;

    try {
        TypeDeducer typeDeducer {};
        typeDeducer.visit(roxal::dynamic_ptr_cast<ast::File>(ast));
    } catch (std::exception& e) {
        compileError(e.what());
        clearCompileContext();
        return;
    }


    if (!graph) {
        std::cout << ast << std::endl;
    }
    else { // graph
        ASTGraphviz graphvizVisitor;
        auto graph = graphvizVisitor.generateGraphText(ast);
        std::ofstream out(outputPath);
        out << graph;
        out.close();
    }

    clearCompileContext();
}


int main(int argc, const char* argv[])
{
    const std::uint64_t defaultGcThresholdKb =
        SimpleMarkSweepGC::kDefaultAutoTriggerThreshold / 1024ull;
    const std::string gcOptionHelp =
        "set GC auto-trigger threshold in kilobytes (0 disables automatic collections, default " +
        std::to_string(defaultGcThresholdKb) + " KB)";
    const std::string stackOptionHelp =
        "set value stack capacity per thread (default " + std::to_string(VM::DefaultMaxStack) + ")";
    const std::string frameOptionHelp =
        "set maximum call frame depth per thread (default " + std::to_string(VM::DefaultMaxCallFrames) + ")";

    po::options_description desc("Allowed options");
    desc.add_options()
        ("help,h", "options help")
        ("version,v", "print version information")
        ("input-file,f", po::value< std::vector<std::string> >(), "input .rox file to execute (run)")
        ("module-paths,p", po::value< std::vector<std::string> >(), "module search paths")
        ("execute,e", po::value<std::string>(), "execute code supplied as a string")
        ("nocache", "disable reading and writing .roc cache files")
        ("recompile", "ignore existing .roc cache files but write new ones")
        ("dis", "output dissasembly of VM bytecodes during compilation")
        ("ast", "parse only and output text Abstract Syntax Tree (AST)")
        ("astgraph", po::value< std::vector<std::string> >(), "parse only and output GraphViz dot file")
        ("gc-threshold", po::value<long long>(), gcOptionHelp.c_str())
        ("nogc", "disable garbage collection")
        ("stack-size", po::value<size_t>()->default_value(VM::DefaultMaxStack), stackOptionHelp.c_str())
        ("max-call-frames", po::value<size_t>()->default_value(VM::DefaultMaxCallFrames), frameOptionHelp.c_str())
        #ifdef DEBUG_BUILD
        ("opcode-prof", "collect opcode execution frequencies in opcode_profile.json")
        #endif
    ;

    po::positional_options_description pos;
    pos.add("input-file", -1);

    #ifdef DEBUG_BUILD
    struct OpcodeProfileFlushGuard {
        ~OpcodeProfileFlushGuard() {
            VM::instance().writeOpcodeProfile();
        }
    };

    std::unique_ptr<OpcodeProfileFlushGuard> opcodeProfileFlushGuard;
    #endif

    po::variables_map vmap;
    try {
        po::store(po::command_line_parser(argc, argv).options(desc).positional(pos).run(), vmap);
        po::notify(vmap);
    } catch(const po::error& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        std::cerr << desc << std::endl;
        return 1;
    }


    if (vmap.count("help")) {
        std::cout << desc << std::endl;
        return 1;
    }

    if (vmap.count("version")) {
        std::cout << VM::versionString() << " " << buildDate();
        std::vector<std::string> features;
        #ifdef ROXAL_ENABLE_FILEIO
            features.push_back("fileio");
        #endif
        #ifdef ROXAL_ENABLE_GRPC
            features.push_back("grpc");
        #endif
        if (!features.empty()) {
            std::cout << " [";
            for (size_t i = 0; i < features.size(); ++i) {
                if (i > 0) std::cout << ",";
                std::cout << features[i];
            }
            std::cout << "]";
        }
        std::cout << std::endl;
        return 0;
    }

    const size_t stackSizeLimit = vmap["stack-size"].as<size_t>();
    if (stackSizeLimit == 0) {
        std::cerr << "Error: --stack-size must be greater than zero" << std::endl;
        return 1;
    }

    const size_t callFrameLimit = vmap["max-call-frames"].as<size_t>();
    if (callFrameLimit == 0) {
        std::cerr << "Error: --max-call-frames must be greater than zero" << std::endl;
        return 1;
    }

    VM::configureStackLimits(stackSizeLimit, callFrameLimit);

    std::vector<std::string> modulePaths;

    // Allow module search paths via the ROXALPATH environment variable
    const char* envPath = std::getenv("ROXALPATH");
    if (envPath) {
        std::string paths(envPath);
        char sep = ':';
#ifdef _WIN32
        sep = ';';
#endif
        std::stringstream ss(paths);
        std::string item;
        while (std::getline(ss, item, sep)) {
            if (!item.empty())
                modulePaths.push_back(item);
        }
    }

    if (vmap.count("module-paths") > 0) {
        auto cliPaths = vmap["module-paths"].as<std::vector<std::string>>();
        modulePaths.insert(modulePaths.end(), cliPaths.begin(), cliPaths.end());
    }

    for (const auto& defaultPath : VM::defaultModuleSearchPaths()) {
        if (std::find(modulePaths.begin(), modulePaths.end(), defaultPath) == modulePaths.end())
            modulePaths.push_back(defaultPath);
    }

    VM::configureModulePaths(modulePaths);

    const bool disableCache = vmap.count("nocache") > 0;
    const bool forceRecompile = (!disableCache) && vmap.count("recompile") > 0;
    VM::CacheMode cacheMode = VM::CacheMode::Normal;
    if (disableCache)
        cacheMode = VM::CacheMode::NoCache;
    else if (forceRecompile)
        cacheMode = VM::CacheMode::Recompile;

    VM::configureCacheMode(cacheMode);

    #ifdef DEBUG_BUILD
    if (vmap.count("opcode-prof")) {
        VM::instance().enableOpcodeProfiling();
        opcodeProfileFlushGuard = std::make_unique<OpcodeProfileFlushGuard>();
    }
    #endif


    if (vmap.count("nogc")) {
        SimpleMarkSweepGC::instance().setEnabled(false);
    }

    if (vmap.count("gc-threshold")) {
        long long thresholdKb = vmap["gc-threshold"].as<long long>();
        if (thresholdKb < 0) {
            std::cerr << "Error: --gc-threshold must be non-negative" << std::endl;
            return 1;
        }

        const std::uint64_t maxThresholdKb = std::numeric_limits<std::uint64_t>::max() / 1024ull;
        const std::uint64_t thresholdKbUnsigned = static_cast<std::uint64_t>(thresholdKb);
        if (thresholdKbUnsigned > maxThresholdKb) {
            std::cerr << "Error: --gc-threshold value is too large" << std::endl;
            return 1;
        }

        SimpleMarkSweepGC::instance().setAutoTriggerThreshold(thresholdKbUnsigned * 1024ull);
    }

    if (vmap.count("execute")) {
        VM::instance().setCacheMode(cacheMode);
        bool outputBytecodeDisassembly = (vmap.count("dis") > 0);
        InterpretResult res =
            runString(vmap["execute"].as<std::string>(), modulePaths,
                      outputBytecodeDisassembly);
        if (VM::instance().isExitRequested())
            return VM::instance().exitCode();
        if (res != InterpretResult::OK)
            return 1;
    }
    else if (vmap.count("input-file") == 0) {
        VM& vm = VM::instance();
        vm.setCacheMode(cacheMode);
        vm.appendModulePaths(modulePaths);
        int rc = repl();
        if (VM::instance().isExitRequested())
            return rc;
    }
    else if (vmap.count("input-file")) {

        try {
            auto filename = vmap["input-file"].as<std::vector<std::string>>().at(0);

            if (vmap.count("ast"))
                generateAST(filename, false);
            else if (vmap.count("astgraph"))
                generateAST(filename, true, vmap["astgraph"].as<std::vector<std::string>>().at(0));
            else {
                bool outputBytecodeDisassembly = (vmap.count("dis") > 0);
                VM::instance().setCacheMode(cacheMode);
                InterpretResult res =
                    runFile(filename, modulePaths, outputBytecodeDisassembly);
                if (VM::instance().isExitRequested())
                    return VM::instance().exitCode();
                if (res != InterpretResult::OK)
                    return 1;
            }
        } catch (std::exception& e) {
            std::cerr << "Runtime error: " << e.what() << std::endl;
        }

    }
    else {
        std::cout << desc << std::endl;
        return 1;
    }

    if (VM::instance().isExitRequested())
        return VM::instance().exitCode();
    return 0;
}
