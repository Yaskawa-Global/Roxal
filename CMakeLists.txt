cmake_minimum_required(VERSION 3.10)

project(roxal VERSION 1.0 LANGUAGES CXX C)

enable_testing()


add_compile_definitions(
#    DEBUG_OUTPUT_LEXER_TOKENS
#    DEBUG_OUTPUT_PARSE_TREE
#    DEBUG_TRACE_PARSE
#    DEBUG_TRACE_SCOPES
#    DEBUG_TRACE_NAME_RESOLUTION
#    DEBUG_TRACE_MEMORY
#    DEBUG_TRACE_EXECUTION
)

# Set the C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

#
# vcpkg
# (install at same level as project folder)

# Set the relative path to vcpkg (assumed to be at same level as project folder)
set(VCPKG_ROOT "${CMAKE_SOURCE_DIR}/../vcpkg")

# Point CMake to the vcpkg toolchain file
set(CMAKE_TOOLCHAIN_FILE "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "Vcpkg toolchain file")

# Add the vcpkg installation prefix to CMAKE_PREFIX_PATH
list(APPEND CMAKE_PREFIX_PATH "${VCPKG_ROOT}/installed/x64-linux")


#
# ANTLR4

# Install runtime first via:
# $ ./vcpkg install antlr4
# and tools using: (see https://github.com/antlr/antlr4-tools)
# $ pip install antlr4-tools
# (check they are compatible)

# Set ANTLR4_ROOT to the location where ANTLR4 is installed by vcpkg
set(ANTLR4_ROOT "${VCPKG_ROOT}/installed/x64-linux")

# Find ANTLR4 package
find_package(antlr4-runtime 4.13.1 CONFIG REQUIRED)


# Add the source files
set(CORE_SOURCES
    core/AST.cpp
    core/atomic.cpp
    core/common.cpp
    core/types.cpp
)

set(SOURCES
    compiler/ASTEditor.cpp
    compiler/ASTGenerator.cpp
    compiler/ASTGraphviz.cpp
    compiler/Chunk.cpp
    compiler/Object.cpp
    compiler/RoxalCompiler.cpp
    compiler/RoxalIndentationLexer.cpp
    compiler/Stream.cpp
    compiler/TypeDeducer.cpp
    compiler/Value.cpp
    compiler/VM.cpp
    compiler/linenoise.c
    compiler/roxal.cpp
    compiler/cpp-gen/RoxalBaseVisitor.cpp
    compiler/cpp-gen/RoxalLexer.cpp
    compiler/cpp-gen/RoxalParser.cpp
    compiler/cpp-gen/RoxalVisitor.cpp
)


#
# Custom command to generate ANTLR4 parser and lexer

# Path to the .g4 file
set(GRAMMAR_FILE ${CMAKE_SOURCE_DIR}/compiler/Roxal.g4)

set(GENERATED_DIR ${CMAKE_SOURCE_DIR}/compiler/cpp-gen)

# Custom command to generate ANTLR4 parser and lexer
add_custom_command(
    OUTPUT ${GENERATED_DIR}/RoxalLexer.cpp ${GENERATED_DIR}/RoxalParser.cpp
    COMMAND antlr4 -Dlanguage=Cpp -package "roxal" -no-listener -visitor ${GRAMMAR_FILE} -o ${GENERATED_DIR}
    DEPENDS ${GRAMMAR_FILE}
    COMMENT "Generating ANTLR4 parser and lexer from Roxal.g4"
)

# Custom target to tie the generated files to a target
add_custom_target(generate_parser
    DEPENDS ${GENERATED_DIR}/RoxalLexer.cpp ${GENERATED_DIR}/RoxalParser.cpp
)



# include the project root directory
include_directories(${CMAKE_HOME_DIRECTORY})

# Add the include directories
include_directories(
    ${ANTLR4_ROOT}/include/antlr4-runtime
    compiler/cpp-gen
)

link_directories(
    ${ANTLR4_ROOT}/lib
    ${CMAKE_SOURCE_DIR}/cpp-gen
)

# Add the executable target
add_executable(roxal ${CORE_SOURCES} ${SOURCES})

add_dependencies(roxal generate_parser)


target_link_libraries(roxal
    antlr4-runtime
    icuio
    icui18n
    icuuc
    icudata
    pthread
    boost_program_options
)


target_compile_options(roxal PRIVATE
    -fPIC
    -Wno-attributes
    -Werror=return-type
    -g
    -fmax-errors=1
)

target_compile_definitions(roxal PRIVATE DEBUG_BUILD)


#
# Testing

add_test(NAME RoxalSourceTests
    COMMAND python3 ${CMAKE_SOURCE_DIR}/runtests.py
)

# Set properties to always show output
set_tests_properties(RoxalSourceTests PROPERTIES
    PASS_REGULAR_EXPRESSION ".*"
    FAIL_REGULAR_EXPRESSION ".*FAIL.*"
)


add_custom_target(check
    COMMAND ${CMAKE_COMMAND} --build . --target all
    COMMAND ${CMAKE_CTEST_COMMAND}
)
