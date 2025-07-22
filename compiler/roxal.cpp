
#include <filesystem>
#include <boost/program_options.hpp>
#include <stdexcept>
#include <sstream>
#include <cstdlib>

#include <core/AST.h>
#include "RoxalIndentationLexer.h"
#include "ASTGenerator.h"
#include "TypeDeducer.h"
#include "ASTGraphviz.h"
#include "RoxalCompiler.h"

#include "VM.h"
#include "Error.h"


extern "C" {
    #include "linenoise.h"
}


using namespace antlr4;
using namespace roxal;
namespace po = boost::program_options;


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

    VM& vm { VM::instance() };
    vm.setDisassemblyOutput(outputBytecodeDisassembly);

    // Add the folder containing the script to the search paths
    vm.appendModulePaths({relativePath.string()});
    vm.appendModulePaths(modulePaths);
    return vm.interpret(sourcestream, filePath.string());
}

static InterpretResult runString(const std::string& source,
                                 const std::vector<std::string>& modulePaths,
                                 bool outputBytecodeDisassembly=false)
{
    std::istringstream sourcestream(source);
    VM& vm { VM::instance() };
    vm.setDisassemblyOutput(outputBytecodeDisassembly);
    vm.appendModulePaths(modulePaths);
    return vm.interpret(sourcestream, "cli");
}


static void generateAST(const std::string& inputPath, bool graph, const std::string& outputPath="")
{
    std::ifstream sourcestream(inputPath); // assumed UTF-8

    std::filesystem::path fileAndPath(inputPath);
    std::string name { fileAndPath.stem().filename().string() };

    ptr<ast::AST> ast {};
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
        typeDeducer.visit(std::dynamic_pointer_cast<ast::File>(ast));
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
    po::options_description desc("Allowed options");
    desc.add_options()
        ("help,h", "options help")
        ("input-file,f", po::value< std::vector<std::string> >(), "input .rox file to execute (run)")
        ("module-paths,p", po::value< std::vector<std::string> >(), "module search paths")
        ("execute,e", po::value<std::string>(), "execute code supplied as a string")
        ("dis", "output dissasembly of VM bytecodes during compilation")
        ("ast", "parse only and output text Abstract Syntax Tree (AST)")
        ("astgraph", po::value< std::vector<std::string> >(), "parse only and output GraphViz dot file")
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


    if (vmap.count("execute")) {
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
        VM::instance().appendModulePaths(modulePaths);
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
