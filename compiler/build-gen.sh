#!/bin/bash
ANTLR4_ROOT=~/dev/antlr4/runtime/Cpp/run
cd  cpp-gen
g++ -c -std=c++17 -DDEBUG_BUILD -fPIC -fmax-errors=1 -I$ANTLR4_ROOT/usr/local/include/antlr4-runtime -L$ANTLR4_ROOT/usr/local/lib/ *.cpp -lantlr4-runtime 
ar rvs RoxalParserBase.a *.o
