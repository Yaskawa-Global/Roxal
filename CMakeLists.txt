cmake_minimum_required(VERSION 3.10)

project(roxal VERSION 1.0 LANGUAGES CXX C)

enable_testing()


option(ROXAL_BUILD_MAIN "Build the roxal executable" ON)


option(ROXAL_ENABLE_FILEIO "Enable builtin file I/O module" ON)
option(USE_GC_SGCL "Use the SGCL garbage collector" ON)


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
if(DEFINED ENV{VCPKG_ROOT})
    set(VCPKG_ROOT $ENV{VCPKG_ROOT})
else()
#if VCPKG_ROOT isn't set for the environment then it is assumed to be at same level as project folder
    set(VCPKG_ROOT "${CMAKE_SOURCE_DIR}/../vcpkg")
endif()

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

# Find ANTLR4 package (if updated, adjust the -v command below)
find_package(antlr4-runtime 4.13.1 CONFIG REQUIRED)

#
# Eigen

# Install via:
# git clone https://gitlab.com/libeigen/eigen.git
# git checkout 3.4.0
# mkdir build && cd build && make && sudo make install
find_package( Eigen3 REQUIRED )
set(EIGEN3_INCLUDE_DIR "/usr/local/include/eigen3/")
include_directories(${EIGEN3_INCLUDE_DIR})

# Detect libffi via pkg-config so that include and library paths
# are recorded explicitly and the build fails clearly if libffi is
# missing.
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBFFI REQUIRED libffi)

# Build the dataflow engine library
add_subdirectory(dataflow)

target_compile_definitions(dataflow PRIVATE
    $<$<BOOL:${USE_GC_SGCL}>:USE_GC_SGCL>
)


# Add the source files
set(CORE_SOURCES
    core/AST.cpp
    core/atomic.cpp
    core/common.cpp
    core/types.cpp
    core/json11.cpp
)

set(COMPILER_SOURCES
    compiler/ASTEditor.cpp
    compiler/ASTGenerator.cpp
    compiler/ASTGraphviz.cpp
    compiler/Chunk.cpp
    compiler/Object.cpp
    compiler/FFI.cpp
    compiler/RoxalCompiler.cpp
    compiler/RoxalIndentationLexer.cpp
    compiler/TypeDeducer.cpp
    compiler/Value.cpp
    compiler/Thread.cpp
    compiler/VM.cpp
    compiler/linenoise.c
)

list(APPEND COMPILER_SOURCES compiler/ModuleMath.cpp)
list(APPEND COMPILER_SOURCES compiler/ModuleSys.cpp)

if(ROXAL_ENABLE_FILEIO)
    list(APPEND COMPILER_SOURCES compiler/ModuleFileIO.cpp)
endif()

set(MAIN_SOURCE compiler/roxal.cpp)


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
    COMMAND antlr4 -v 4.13.1 -Dlanguage=Cpp -package "roxal" -no-listener -visitor ${GRAMMAR_FILE} -o ${GENERATED_DIR}
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
    $<$<BOOL:${USE_GC_SGCL}>:${CMAKE_SOURCE_DIR}/core/sgcl>
)

link_directories(
    ${ANTLR4_ROOT}/lib
    ${CMAKE_SOURCE_DIR}/cpp-gen
    ${LIBFFI_LIBRARY_DIRS}
)

# Add libffi include directories discovered via pkg-config
include_directories(${LIBFFI_INCLUDE_DIRS})


# Build the static library containing the core compiler and VM
add_library(roxalcore STATIC ${CORE_SOURCES} ${COMPILER_SOURCES} ${GENERATED_SOURCES})
set_target_properties(roxalcore PROPERTIES OUTPUT_NAME roxal)

add_dependencies(roxalcore generate_parser)


target_link_libraries(roxalcore
    PUBLIC
        antlr4-runtime
        icuio
        icui18n
        icuuc
        icudata
        pthread
        dataflow
        ${LIBFFI_LIBRARIES}
)


# Set compile options based on the build type
target_compile_options(roxalcore PRIVATE
    $<$<CONFIG:DEBUG>:-fPIC -Wno-attributes -Werror=return-type -g -fmax-errors=1>
    $<$<CONFIG:RELEASE>:-O3>
)

# Set compile definitions based on the build type
target_compile_definitions(roxalcore PRIVATE
    $<$<CONFIG:DEBUG>:DEBUG_BUILD>
    $<$<BOOL:${ROXAL_ENABLE_FILEIO}>:ROXAL_ENABLE_FILEIO>
    $<$<BOOL:${USE_GC_SGCL}>:USE_GC_SGCL>
)


if(ROXAL_BUILD_MAIN)
    add_executable(roxal ${MAIN_SOURCE})
    target_link_libraries(roxal PRIVATE roxalcore boost_program_options)
    target_compile_options(roxal PRIVATE
        $<$<CONFIG:DEBUG>:-fPIC -Wno-attributes -Werror=return-type -g -fmax-errors=1>
        $<$<CONFIG:RELEASE>:-O3>
    )
    target_compile_definitions(roxal PRIVATE
        $<$<CONFIG:DEBUG>:DEBUG_BUILD>
        $<$<BOOL:${ROXAL_ENABLE_FILEIO}>:ROXAL_ENABLE_FILEIO>
        $<$<BOOL:${USE_GC_SGCL}>:USE_GC_SGCL>
    )

    add_test(NAME RoxalSourceTests
        COMMAND python3 ${CMAKE_SOURCE_DIR}/runtests.py
    )

    set_tests_properties(RoxalSourceTests PROPERTIES
        PASS_REGULAR_EXPRESSION ".*"
        FAIL_REGULAR_EXPRESSION ".*FAIL.*"
    )
endif()


add_custom_target(check
    COMMAND ${CMAKE_COMMAND} --build . --target all
    COMMAND ${CMAKE_CTEST_COMMAND}
)
