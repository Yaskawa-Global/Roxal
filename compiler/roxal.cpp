
#include <algorithm>
#include <filesystem>
#include <boost/program_options.hpp>
#include <cstdint>
#include <cstdlib>
#include <csignal>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include <core/AST.h>
#include "RoxalIndentationLexer.h"
#include "ASTGenerator.h"
#include "TypeDeducer.h"
#include "ASTGraphviz.h"
#include "RoxalCompiler.h"

#include "VM.h"
#include "Error.h"
#include "SimpleMarkSweepGC.h"


extern "C" {
    #include "linenoise.h"
}


using namespace antlr4;
using namespace roxal;
namespace po = boost::program_options;

namespace {

using OpcodeFrequencyMap = std::map<OpCode, std::uint64_t>;

struct OpcodeFrequencyResult {
    OpcodeFrequencyMap frequencies;
    std::vector<std::filesystem::path> files;
};

static const char* opcodeName(OpCode op)
{
    switch (op) {
        case OpCode::Nop:               return "Nop";
        case OpCode::Constant:          return "Constant";
        case OpCode::ConstNil:          return "ConstNil";
        case OpCode::ConstTrue:         return "ConstTrue";
        case OpCode::ConstFalse:        return "ConstFalse";
        case OpCode::ConstInt0:         return "ConstInt0";
        case OpCode::ConstInt1:         return "ConstInt1";
        case OpCode::Equal:             return "Equal";
        case OpCode::Is:                return "Is";
        case OpCode::Greater:           return "Greater";
        case OpCode::Less:              return "Less";
        case OpCode::Add:               return "Add";
        case OpCode::Subtract:          return "Subtract";
        case OpCode::Multiply:          return "Multiply";
        case OpCode::Divide:            return "Divide";
        case OpCode::Modulo:            return "Modulo";
        case OpCode::Negate:            return "Negate";
        case OpCode::And:               return "And";
        case OpCode::Or:                return "Or";
        case OpCode::BitAnd:            return "BitAnd";
        case OpCode::BitOr:             return "BitOr";
        case OpCode::BitXor:            return "BitXor";
        case OpCode::BitNot:            return "BitNot";
        case OpCode::Pop:               return "Pop";
        case OpCode::PopN:              return "PopN";
        case OpCode::Dup:               return "Dup";
        case OpCode::DupBelow:          return "DupBelow";
        case OpCode::Swap:              return "Swap";
        case OpCode::JumpIfFalse:       return "JumpIfFalse";
        case OpCode::JumpIfTrue:        return "JumpIfTrue";
        case OpCode::Jump:              return "Jump";
        case OpCode::Loop:              return "Loop";
        case OpCode::Call:              return "Call";
        case OpCode::Index:             return "Index";
        case OpCode::SetIndex:          return "SetIndex";
        case OpCode::Invoke:            return "Invoke";
        case OpCode::Closure:           return "Closure";
        case OpCode::CloseUpvalue:      return "CloseUpvalue";
        case OpCode::Return:            return "Return";
        case OpCode::ReturnStore:       return "ReturnStore";
        case OpCode::ObjectType:        return "ObjectType";
        case OpCode::ActorType:         return "ActorType";
        case OpCode::InterfaceType:     return "InterfaceType";
        case OpCode::EnumerationType:   return "EnumerationType";
        case OpCode::Property:          return "Property";
        case OpCode::Method:            return "Method";
        case OpCode::EnumLabel:         return "EnumLabel";
        case OpCode::Extend:            return "Extend";
        case OpCode::DefineModuleVar:   return "DefineModuleVar";
        case OpCode::GetModuleVar:      return "GetModuleVar";
        case OpCode::SetModuleVar:      return "SetModuleVar";
        case OpCode::SetNewModuleVar:   return "SetNewModuleVar";
        case OpCode::ImportModuleVars:  return "ImportModuleVars";
        case OpCode::GetUpvalue:        return "GetUpvalue";
        case OpCode::SetUpvalue:        return "SetUpvalue";
        case OpCode::GetLocal:          return "GetLocal";
        case OpCode::SetLocal:          return "SetLocal";
        case OpCode::SetProp:           return "SetProp";
        case OpCode::GetProp:           return "GetProp";
        case OpCode::SetPropCheck:      return "SetPropCheck";
        case OpCode::GetPropCheck:      return "GetPropCheck";
        case OpCode::GetSuper:          return "GetSuper";
        case OpCode::NewRange:          return "NewRange";
        case OpCode::NewList:           return "NewList";
        case OpCode::NewDict:           return "NewDict";
        case OpCode::NewVector:         return "NewVector";
        case OpCode::NewMatrix:         return "NewMatrix";
        case OpCode::IfDictToKeys:      return "IfDictToKeys";
        case OpCode::IfDictToItems:     return "IfDictToItems";
        case OpCode::ToType:            return "ToType";
        case OpCode::ToTypeStrict:      return "ToTypeStrict";
        case OpCode::ToTypeSpec:        return "ToTypeSpec";
        case OpCode::ToTypeSpecStrict:  return "ToTypeSpecStrict";
        case OpCode::EventOn:           return "EventOn";
        case OpCode::EventOff:          return "EventOff";
        case OpCode::SetupExcept:       return "SetupExcept";
        case OpCode::EndExcept:         return "EndExcept";
        case OpCode::Throw:             return "Throw";
        case OpCode::CopyInto:          return "CopyInto";
        default:                        return "Unknown";
    }
}

static void appendUniquePath(std::vector<std::string>& target, const std::filesystem::path& candidate)
{
    if (candidate.empty())
        return;
    const std::string asString = candidate.string();
    if (asString.empty())
        return;
    if (std::find(target.begin(), target.end(), asString) == target.end())
        target.push_back(asString);
}

static std::optional<std::filesystem::path> tryResolveExisting(const std::filesystem::path& candidate)
{
    std::ifstream stream(candidate);
    if (!stream.is_open())
        return std::nullopt;
    try {
        return std::filesystem::canonical(candidate);
    } catch (...) {
        return std::filesystem::absolute(candidate);
    }
}

static std::filesystem::path resolveScriptPath(const std::string& requestedPath,
                                               const std::vector<std::string>& modulePaths)
{
    std::filesystem::path directPath(requestedPath);
    if (auto resolved = tryResolveExisting(directPath))
        return *resolved;

    for (const auto& base : modulePaths) {
        std::filesystem::path candidate = std::filesystem::path(base) / directPath;
        if (auto resolved = tryResolveExisting(candidate))
            return *resolved;
    }

    throw std::runtime_error("file not found: " + requestedPath);
}

static Value compileForFrequency(const std::filesystem::path& filePath,
                                 const std::vector<std::string>& modulePaths,
                                 VM::CacheMode cacheMode)
{
    std::ifstream source(filePath);
    if (!source.is_open())
        throw std::runtime_error("file not found: " + filePath.string());

    std::filesystem::path absolutePath = std::filesystem::absolute(filePath);
    std::filesystem::path parentPath = absolutePath.parent_path();
    std::filesystem::path relativePath;
    try {
        relativePath = std::filesystem::relative(parentPath, std::filesystem::current_path());
    } catch (...) {
        relativePath.clear();
    }

    std::vector<std::string> compilerModulePaths;
    appendUniquePath(compilerModulePaths, relativePath);
    appendUniquePath(compilerModulePaths, parentPath);
    for (const auto& base : modulePaths)
        appendUniquePath(compilerModulePaths, std::filesystem::path(base));

    RoxalCompiler compiler {};
    compiler.setOutputBytecodeDisassembly(false);
    compiler.setModulePaths(compilerModulePaths);
    compiler.setCacheReadEnabled(cacheMode == VM::CacheMode::Normal);
    compiler.setCacheWriteEnabled(cacheMode != VM::CacheMode::NoCache);

    std::filesystem::path cacheSourcePath;
    try {
        cacheSourcePath = std::filesystem::canonical(absolutePath);
    } catch (...) {
        cacheSourcePath.clear();
    }

    Value function { Value::nilVal() };
    bool loadedFromCache = false;
    if (!cacheSourcePath.empty() && cacheMode == VM::CacheMode::Normal) {
        Value cached = compiler.loadFileCache(cacheSourcePath);
        if (cached.isNonNil()) {
            function = cached;
            loadedFromCache = true;
        }
    }

    if (!loadedFromCache) {
        function = compiler.compile(source, filePath.string());
        if (function.isNil())
            throw std::runtime_error("failed to compile " + filePath.string());
        if (!cacheSourcePath.empty() && cacheMode != VM::CacheMode::NoCache)
            compiler.storeFileCache(cacheSourcePath, function);
    }

    return function;
}

static void countChunkOpcodes(const Chunk& chunk, OpcodeFrequencyMap& frequencies)
{
    const auto& code = chunk.code;
    auto ip = code.begin();
    auto end = code.end();

    while (ip != end) {
        bool doubleByteArg = false;
        uint8_t instructionByte = *ip++;
        OpCode instruction;
        if ((instructionByte & DoubleByteArg) == 0)
            instruction = OpCode(instructionByte);
        else {
            instruction = OpCode(instructionByte & ~DoubleByteArg);
            doubleByteArg = true;
        }

        frequencies[instruction]++;

        auto readByte = [&]() -> uint8_t {
            if (ip == end)
                throw std::runtime_error("Unexpected end of bytecode while decoding instruction arguments.");
            return *ip++;
        };

        auto readShort = [&]() -> uint16_t {
            uint16_t hi = readByte();
            uint16_t lo = readByte();
            return static_cast<uint16_t>((hi << 8) | lo);
        };

        auto skipArg = [&](bool expectsDouble) {
            if (expectsDouble)
                (void)readShort();
            else
                (void)readByte();
        };

        switch (instruction) {
            case OpCode::Constant:
            case OpCode::DefineModuleVar:
            case OpCode::GetModuleVar:
            case OpCode::SetModuleVar:
            case OpCode::SetNewModuleVar:
            case OpCode::ObjectType:
            case OpCode::ActorType:
            case OpCode::InterfaceType:
            case OpCode::EnumerationType:
            case OpCode::Property:
            case OpCode::Method:
            case OpCode::EnumLabel:
            case OpCode::SetProp:
            case OpCode::GetProp:
            case OpCode::SetPropCheck:
            case OpCode::GetPropCheck:
            case OpCode::GetSuper:
                skipArg(doubleByteArg);
                break;

            case OpCode::Call: {
                uint8_t spec = readByte();
                if ((spec & 0x80) != 0) {
                    uint8_t argCount = spec & 0x7f;
                    for (uint8_t i = 0; i < argCount; ++i) {
                        uint8_t marker = readByte();
                        if (marker != 0)
                            (void)readByte();
                    }
                }
                break;
            }

            case OpCode::Closure: {
                uint16_t constantIndex = doubleByteArg ? readShort() : readByte();
                if (constantIndex >= chunk.constants.size())
                    throw std::runtime_error("Closure instruction referenced out-of-range constant index");
                const Value& fnValue = chunk.constants.at(constantIndex);
                if (!isFunction(fnValue))
                    throw std::runtime_error("Closure instruction constant was not a function");
                ObjFunction* nested = asFunction(fnValue);
                for (int i = 0; i < nested->upvalueCount; ++i) {
                    (void)readByte();
                    (void)readByte();
                }
                break;
            }

            case OpCode::JumpIfFalse:
            case OpCode::JumpIfTrue:
            case OpCode::Jump:
            case OpCode::Loop:
            case OpCode::SetupExcept:
                (void)readShort();
                break;

            case OpCode::Invoke: {
                skipArg(doubleByteArg);
                uint8_t spec = readByte();
                if ((spec & 0x80) != 0) {
                    uint8_t argCount = spec & 0x7f;
                    for (uint8_t i = 0; i < argCount; ++i) {
                        uint8_t marker = readByte();
                        if (marker != 0)
                            (void)readByte();
                    }
                }
                break;
            }

            case OpCode::PopN:
            case OpCode::Index:
            case OpCode::SetIndex:
            case OpCode::NewRange:
            case OpCode::NewList:
            case OpCode::NewDict:
            case OpCode::NewVector:
            case OpCode::NewMatrix:
            case OpCode::ToType:
            case OpCode::ToTypeStrict:
                (void)readByte();
                break;

            case OpCode::GetLocal:
            case OpCode::SetLocal:
            case OpCode::GetUpvalue:
            case OpCode::SetUpvalue:
                skipArg(doubleByteArg);
                break;

            default:
                break;
        }
    }
}

static void accumulateOpcodesFromFunction(const Value& value,
                                          OpcodeFrequencyMap& frequencies,
                                          std::unordered_set<const ObjFunction*>& visited)
{
    if (!isFunction(value))
        return;

    ObjFunction* function = asFunction(value);
    if (!visited.insert(function).second)
        return;
    if (!function->chunk)
        return;

    countChunkOpcodes(*function->chunk, frequencies);

    for (const Value& constant : function->chunk->constants) {
        if (isFunction(constant))
            accumulateOpcodesFromFunction(constant, frequencies, visited);
    }
}

static OpcodeFrequencyResult collectOpcodeFrequencies(const std::vector<std::string>& scriptPaths,
                                                      const std::vector<std::string>& modulePaths,
                                                      VM::CacheMode cacheMode)
{
    OpcodeFrequencyResult result;
    std::unordered_set<const ObjFunction*> visited;

    for (const auto& script : scriptPaths) {
        std::filesystem::path resolved = resolveScriptPath(script, modulePaths);
        result.files.push_back(resolved);
        Value function = compileForFrequency(resolved, modulePaths, cacheMode);
        accumulateOpcodesFromFunction(function, result.frequencies, visited);
    }

    return result;
}

} // namespace

static void sigint_handler(int)
{
    try {
        VM::instance().dumpStackTraces();
    } catch (...) {
    }
    std::signal(SIGINT, SIG_DFL);
    std::raise(SIGINT);
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

static int repl()
{
    linenoiseHistorySetMaxLen(1000);

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

    po::options_description desc("Allowed options");
    desc.add_options()
        ("help,h", "options help")
        ("input-file,f", po::value< std::vector<std::string> >(), "input .rox file to execute (run)")
        ("module-paths,p", po::value< std::vector<std::string> >(), "module search paths")
        ("opcode-freq", po::value<std::vector<std::string>>()->multitoken(),
         "analyze opcode frequencies for specified .rox files")
        ("execute,e", po::value<std::string>(), "execute code supplied as a string")
        ("nocache", "disable reading and writing .moc cache files")
        ("recompile", "ignore existing .moc cache files but write new ones")
        ("dis", "output dissasembly of VM bytecodes during compilation")
        ("ast", "parse only and output text Abstract Syntax Tree (AST)")
        ("astgraph", po::value< std::vector<std::string> >(), "parse only and output GraphViz dot file")
        ("gc-threshold", po::value<long long>(), gcOptionHelp.c_str())
    ;

    po::positional_options_description pos;
    pos.add("input-file", -1);

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

    const bool disableCache = vmap.count("nocache") > 0;
    const bool forceRecompile = (!disableCache) && vmap.count("recompile") > 0;
    VM::CacheMode cacheMode = VM::CacheMode::Normal;
    if (disableCache)
        cacheMode = VM::CacheMode::NoCache;
    else if (forceRecompile)
        cacheMode = VM::CacheMode::Recompile;


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

    if (vmap.count("opcode-freq")) {
        auto scripts = vmap["opcode-freq"].as<std::vector<std::string>>();
        if (scripts.empty()) {
            std::cerr << "Error: --opcode-freq requires at least one file" << std::endl;
            return 1;
        }

        try {
            OpcodeFrequencyResult result = collectOpcodeFrequencies(scripts, modulePaths, cacheMode);

            if (!result.files.empty()) {
                std::cout << "Analyzed " << result.files.size() << " file(s):" << std::endl;
                for (const auto& path : result.files)
                    std::cout << "  " << path.string() << std::endl;
            } else {
                std::cout << "No files were analyzed." << std::endl;
            }

            const auto& frequencies = result.frequencies;
            if (frequencies.empty()) {
                std::cout << std::endl << "No opcodes were emitted." << std::endl;
            } else {
                std::vector<std::pair<OpCode, std::uint64_t>> entries(frequencies.begin(), frequencies.end());
                std::sort(entries.begin(), entries.end(), [](const auto& lhs, const auto& rhs) {
                    if (lhs.second != rhs.second)
                        return lhs.second > rhs.second;
                    return lhs.first < rhs.first;
                });

                std::uint64_t total = 0;
                std::size_t nameWidth = std::string("OpCode").size();
                std::size_t idWidth = std::string("ID").size();
                std::size_t countWidth = std::string("Count").size();
                std::size_t percentWidth = std::string("Percent").size();

                for (const auto& entry : entries) {
                    nameWidth = std::max(nameWidth, std::string(opcodeName(entry.first)).size());
                    idWidth = std::max(idWidth, std::to_string(static_cast<int>(entry.first)).size());
                    countWidth = std::max(countWidth, std::to_string(entry.second).size());
                    total += entry.second;
                }

                std::vector<std::string> percentStrings;
                percentStrings.reserve(entries.size());
                for (const auto& entry : entries) {
                    double percentValue = total > 0
                        ? (100.0 * static_cast<double>(entry.second) / static_cast<double>(total))
                        : 0.0;
                    std::ostringstream percentStream;
                    percentStream << std::fixed << std::setprecision(2) << percentValue << '%';
                    percentStrings.push_back(percentStream.str());
                    percentWidth = std::max(percentWidth, percentStrings.back().size());
                }

                std::cout << std::endl
                          << "Opcode frequencies (" << total << " total instructions):" << std::endl;

                std::ostringstream header;
                header << "  " << std::left << std::setw(static_cast<int>(nameWidth)) << "OpCode";
                header << "  " << std::right << std::setw(static_cast<int>(idWidth)) << "ID";
                header << "  " << std::right << std::setw(static_cast<int>(countWidth)) << "Count";
                header << "  " << std::right << std::setw(static_cast<int>(percentWidth)) << "Percent";
                std::cout << header.str() << std::endl;

                std::ostringstream separator;
                separator << "  " << std::string(nameWidth, '-');
                separator << "  " << std::string(idWidth, '-');
                separator << "  " << std::string(countWidth, '-');
                separator << "  " << std::string(percentWidth, '-');
                std::cout << separator.str() << std::endl;

                for (std::size_t i = 0; i < entries.size(); ++i) {
                    const auto& entry = entries[i];
                    const std::string& percentStr = percentStrings[i];
                    std::ostringstream line;
                    line << "  " << std::left << std::setw(static_cast<int>(nameWidth))
                         << opcodeName(entry.first);
                    line << "  " << std::right << std::setw(static_cast<int>(idWidth))
                         << static_cast<int>(entry.first);
                    line << "  " << std::right << std::setw(static_cast<int>(countWidth))
                         << entry.second;
                    line << "  " << std::right << std::setw(static_cast<int>(percentWidth))
                         << percentStr;
                    std::cout << line.str() << std::endl;
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << std::endl;
            return 1;
        }

        return 0;
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
