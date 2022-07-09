#!/bin/bash

set -x

ANTLR4_ROOT=~/dev/antlr4/runtime/Cpp/run
INC="-I$ANTLR4_ROOT/usr/local/include/antlr4-runtime -Icpp-gen -I.."
LIBDIRS="-L$ANTLR4_ROOT/usr/local/lib/ -Lcpp-gen"
LIBS="-lantlr4-runtime cpp-gen/RoxalParserBase.a -licuio -licui18n -licuuc -licudata -lpthread -lboost_program_options"
#OPTS="-std=c++2a -fPIC -Wno-attributes -Werror=return-type -g -fmax-errors=1"
#OPTS="-std=c++2a -DDEBUG_BUILD -DDEBUG_TRACE_PARSE -DDEBUG_OUTPUT_PARSE_TREE -fPIC -Wno-attributes -Werror=return-type -g -fmax-errors=1"
OPTS="-std=c++2a -DDEBUG_BUILD -fPIC -Wno-attributes -Werror=return-type -g -fmax-errors=1"
#OPTS="-std=c++2a -fPIC -Wno-attributes -Werror=return-type -O4 -fmax-errors=1"
g++ ${OPTS} ${INC} ${LIBDIRS} ../core/*.cpp *.c *.cpp ${LIBS} -o roxal

