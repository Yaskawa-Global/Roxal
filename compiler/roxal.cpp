
#include <filesystem>

#include "RoxalIndentationLexer.h"
#include "RoxalCompiler.h"

#include "VM.h"


using namespace antlr4;
using namespace roxal;


static void repl() 
{
    //...
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


        // tmp testing

        // VM vm {};

        // auto chunk { std::make_shared<Chunk>() };

        // //auto constant = chunk.addConstant(1.2);
        // //chunk.write(OpCode::Constant, 123);
        // //chunk.write(constant, 123);
        // chunk->writeConsant(1.2,123);

        // chunk->writeConsant(3.4, 123);

        // chunk->write(OpCode::Add, 123);

        // chunk->writeConsant(5.6, 123);

        // chunk->write(OpCode::Divide, 123);

        // chunk->write(OpCode::Negate, 123);

        // chunk->write(OpCode::Return, 123);
        // chunk->disassemble("test chunk");

        // std::cout << "starting execution" << std::endl;
        // vm.interpret(chunk);

    return 0;
}
 

