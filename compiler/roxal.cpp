
#include <filesystem>

#include "RoxalIndentationLexer.h"
#include "RoxalCompiler.h"

#include "VM.h"


extern "C" {
    #include "linenoise.h"
}


using namespace antlr4;
using namespace roxal;


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


int main(int argc, const char* argv[]) {
    
    if (argc == 1) {
        repl();
    }
    else if (argc == 2) {

        runFile(argv[1]);

    }
    else {
        std::cerr << "Usage: roxal [<jobfile.rox>]" << std::endl;
        return -1;
    }

    return 0;
}
 

