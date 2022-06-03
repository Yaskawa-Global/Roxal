
#include <filesystem>
#include <boost/program_options.hpp>

#include <core/AST.h>
#include "RoxalIndentationLexer.h"
#include "ASTGenerator.h"
#include "ASTGraphviz.h"
#include "RoxalCompiler.h"

#include "VM.h"


extern "C" {
    #include "linenoise.h"
}


using namespace antlr4;
using namespace roxal;
namespace po = boost::program_options;


static void repl() 
{
    linenoiseHistorySetMaxLen(1000);

    std::stringstream stream;

    VM vm { stream };

    char* cline;
    std::string line;
    //int indent = 0;
    bool quit=false;
    while(!quit && ((cline = linenoise("rox> ")) != nullptr)) {

        linenoiseHistoryAdd(cline);

        line = std::string(cline);
        if ((line=="end")||(line=="END"))
            quit=true;
        else if (!line.empty()) {

            try {
                stream << line << std::endl << std::flush;

                vm.interpretLine();

                //std::cout << value->repr() << std::endl;
            } catch (std::exception& e) {
                std::cout << std::string("error: ") << e.what() << std::endl;
            }
            
        }
        if (cline != NULL) {
            linenoiseFree(cline);
        }            

    }

}


static void runFile(const std::string& path) 
{

    std::ifstream sourcestream(path); // assumed UTF-8

    std::filesystem::path fileAndPath(path);
    std::string name { fileAndPath.stem().filename().string() };

    VM vm {};
    vm.interpret(sourcestream, name);
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
        std::cout << "Exception in parsing - " << std::string(e.what()) << std::endl; 
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
}


int main(int argc, const char* argv[]) 
{
    po::options_description desc("Allowed options");
    desc.add_options()
        ("help,h", "options help")
        ("input-file,f", po::value< std::vector<std::string> >(), "input .rox file to execute (run)")
        ("ast", "parse only and output text Abstract Syntax Tree (AST)")
        ("astgraph", po::value< std::vector<std::string> >(), "parse only and output GraphViz dot file")
    ;

    po::positional_options_description pos;
    pos.add("input-file", -1);

    po::variables_map vm;
    po::store(po::command_line_parser(argc, argv).options(desc).positional(pos).run(), vm);
    po::notify(vm);    

    if (vm.count("help")) {
        std::cout << desc << std::endl;
        return 1;
    }

    if (vm.count("input-file")==0) {
        repl();
    }
    else if (vm.count("input-file")) {

        try {
            auto filename = vm["input-file"].as<std::vector<std::string>>().at(0);

            if (vm.count("ast"))
                generateAST(filename, false);
            else if (vm.count("astgraph"))
                generateAST(filename, true, vm["astgraph"].as<std::vector<std::string>>().at(0));
            else 
                runFile(filename);
        } catch (std::exception& e) {
            std::cerr << "Runtime error: " << e.what() << std::endl;
        }

    }
    else {
        std::cout << desc << std::endl;
        return 1;
    }

    return 0;
}
 

