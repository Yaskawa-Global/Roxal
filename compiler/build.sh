#!/bin/bash

set -x

ANTLR4_ROOT=~/dev/antlr4/runtime/Cpp/run
INC="-I$ANTLR4_ROOT/usr/local/include/antlr4-runtime -Icpp-gen"
LIBDIRS="-L$ANTLR4_ROOT/usr/local/lib/ -Lcpp-gen"
LIBS="-lantlr4-runtime cpp-gen/RoxalParserBase.a -licuio -licui18n -licuuc -licudata -lpthread"
OPTS="-std=c++17 -DDEBUG_BUILD -fPIC -Wno-attributes -Werror=return-type -g -fmax-errors=1"
g++ ${OPTS} ${INC} ${LIBDIRS} *.cpp ${LIBS} -o roxal

