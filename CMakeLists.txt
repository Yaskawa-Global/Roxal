cmake_minimum_required(VERSION 3.10)

project(roxal VERSION 1.0 LANGUAGES CXX C)

enable_testing()


add_compile_definitions(
#    DEBUG_OUTPUT_LEXER_TOKENS
#    DEBUG_OUTPUT_PARSE_TREE
#    DEBUG_TRACE_PARSE
#    DEBUG_TRACE_SCOPES
#    DEBUG_TRACE_NAME_RESOLUTION
    DEBUG_TRACE_MEMORY
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

if(EXISTS "${VCPKG_ROOT}")
    # Point CMake to the vcpkg toolchain file
    set(CMAKE_TOOLCHAIN_FILE "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "Vcpkg toolchain file")
    # Add the vcpkg installation prefix to CMAKE_PREFIX_PATH
    list(APPEND CMAKE_PREFIX_PATH "${VCPKG_ROOT}/installed/x64-linux")
endif()


#
# ANTLR4

# Install runtime first via:
# $ ./vcpkg install antlr4
# and tools using: (see https://github.com/antlr/antlr4-tools)
# $ pip install antlr4-tools
# (check they are compatible)

# Attempt to locate the ANTLR4 runtime
# Prefer vcpkg if present, otherwise fall back to common Homebrew locations
set(ANTLR4_ROOT "")
if(EXISTS "${VCPKG_ROOT}/installed/x64-linux")
    set(ANTLR4_ROOT "${VCPKG_ROOT}/installed/x64-linux")
elseif(APPLE)
    if(EXISTS "/opt/homebrew/opt/antlr4-cpp-runtime")
        set(ANTLR4_ROOT "/opt/homebrew/opt/antlr4-cpp-runtime")
    elseif(EXISTS "/usr/local/opt/antlr4-cpp-runtime")
        set(ANTLR4_ROOT "/usr/local/opt/antlr4-cpp-runtime")
    endif()
endif()

if(ANTLR4_ROOT)
    list(APPEND CMAKE_PREFIX_PATH "${ANTLR4_ROOT}")
endif()

# Find ANTLR4 package
find_package(antlr4-runtime 4.13.1 CONFIG REQUIRED)

#
# Eigen
# Use an existing installation if available. Otherwise look for a
# local checkout next to this repository so Eigen does not need to be
# globally installed. Clone it with:
#   git clone https://gitlab.com/libeigen/eigen.git ../eigen
#   cd ../eigen && git checkout 3.4.0

set(EIGEN_ROOT "${CMAKE_SOURCE_DIR}/../eigen")
if(EXISTS "${EIGEN_ROOT}")
    list(APPEND CMAKE_MODULE_PATH "${EIGEN_ROOT}/cmake")
    set(EIGEN3_INCLUDE_DIR "${EIGEN_ROOT}")
endif()

find_package(Eigen3 REQUIRED)
include_directories(${EIGEN3_INCLUDE_DIR})

# Detect libffi via pkg-config so that include and library paths
# are recorded explicitly and the build fails clearly if libffi is
# missing.
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBFFI REQUIRED libffi)

# ICU Unicode library
set(ICU_ROOT "")
if(APPLE)
    if(EXISTS "/opt/homebrew/opt/icu4c")
        set(ICU_ROOT "/opt/homebrew/opt/icu4c")
    elseif(EXISTS "/usr/local/opt/icu4c")
        set(ICU_ROOT "/usr/local/opt/icu4c")
    endif()
endif()

if(ICU_ROOT)
    list(APPEND CMAKE_PREFIX_PATH "${ICU_ROOT}")
endif()

find_package(ICU REQUIRED COMPONENTS i18n io uc data)
include_directories(${ICU_INCLUDE_DIRS})

# Build the dataflow engine library
add_subdirectory(dataflow)



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
    compiler/TypeDeducer.cpp
    compiler/Value.cpp
    compiler/Thread.cpp
    compiler/VM.cpp
    compiler/linenoise.c
    compiler/roxal.cpp
)


#
# Custom command to generate ANTLR4 parser and lexer

set(GENERATED_DIR ${CMAKE_SOURCE_DIR}/compiler/cpp-gen)

# List every file ANTLR will create
set(GENERATED_SOURCES
    ${GENERATED_DIR}/RoxalLexer.cpp
    ${GENERATED_DIR}/RoxalParser.cpp
    ${GENERATED_DIR}/RoxalVisitor.cpp
    ${GENERATED_DIR}/RoxalBaseVisitor.cpp)

# Path to the .g4 file
set(GRAMMAR_FILE ${CMAKE_SOURCE_DIR}/compiler/Roxal.g4)


# Custom command to generate ANTLR4 parser and lexer
add_custom_command(
    OUTPUT ${GENERATED_SOURCES}
    COMMAND antlr4 -Dlanguage=Cpp -package "roxal" -no-listener -visitor ${GRAMMAR_FILE} -o ${GENERATED_DIR}
    DEPENDS ${GRAMMAR_FILE}
    COMMENT "Generating ANTLR4 parser and lexer from Roxal.g4"
)

# Custom target to tie the generated files to a target
add_custom_target(generate_parser
    DEPENDS ${GENERATED_SOURCES}
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
    ${LIBFFI_LIBRARY_DIRS}
)

# Add libffi include directories discovered via pkg-config
include_directories(${LIBFFI_INCLUDE_DIRS})

# Add the executable target
add_executable(roxal ${CORE_SOURCES} ${SOURCES} ${GENERATED_SOURCES})

add_dependencies(roxal generate_parser)


target_link_libraries(roxal
    antlr4-runtime
    ${ICU_LIBRARIES}
    pthread
    boost_program_options
    dataflow
    ${LIBFFI_LIBRARIES}
)


# Set compile options based on the build type
target_compile_options(roxal PRIVATE
    $<$<CONFIG:DEBUG>:-fPIC -Wno-attributes -Werror=return-type -g -fmax-errors=1>
    $<$<CONFIG:RELEASE>:-O3>
)

# Set compile definitions based on the build type
target_compile_definitions(roxal PRIVATE
    $<$<CONFIG:DEBUG>:DEBUG_BUILD>
)


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
