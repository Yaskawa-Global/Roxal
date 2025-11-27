cmake_minimum_required(VERSION 3.20)

project(roxal VERSION 1.0.0 LANGUAGES CXX C)
set(ROXAL_PRERELEASE "" CACHE STRING "SemVer prerelease label (e.g. alpha.1, rc.1)")

enable_testing()

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-fmax-errors=1)
endif()

option(ROXAL_BUILD_MAIN "Build the roxal executable" ON)


option(ROXAL_ENABLE_FILEIO "Enable builtin file I/O module" ON)
option(ROXAL_ENABLE_GRPC "Enable gRPC/protobuf import support" OFF)

# Expose the project version and git hash to the codebase
set(ROXAL_PROJECT_VERSION "${PROJECT_VERSION}")
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE ROXAL_GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT ROXAL_GIT_HASH)
    set(ROXAL_GIT_HASH "unknown")
endif()

option(ENABLE_UI "Build with UI support" ON)


add_compile_definitions(
#    DEBUG_OUTPUT_LEXER_TOKENS
#    DEBUG_OUTPUT_PARSE_TREE
#    DEBUG_TRACE_PARSE
#    DEBUG_TRACE_SCOPES
#    DEBUG_TRACE_NAME_RESOLUTION
#    DEBUG_TRACE_MEMORY
#    DEBUG_TRACE_EXECUTION
#    DEBUG_TRACE_GC
)

# Enable verbose GC tracing when requested.
option(ROXAL_DEBUG_TRACE_GC "Emit GC diagnostics while running" OFF)
if(ROXAL_DEBUG_TRACE_GC)
    add_compile_definitions(DEBUG_TRACE_GC)
endif()

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

if(ROXAL_ENABLE_GRPC)
    find_package(Protobuf REQUIRED) # use module or config
    find_package(gRPC CONFIG REQUIRED)
endif()

# Detect libffi via pkg-config so that include and library paths
# are recorded explicitly and the build fails clearly if libffi is
# missing.
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBFFI REQUIRED libffi)



# Build the dataflow engine library
add_subdirectory(dataflow)

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
    compiler/CallableInfo.cpp
    compiler/Introspection.cpp
    compiler/BuiltinModule.cpp
    compiler/Object.cpp
    compiler/FFI.cpp
    compiler/RoxalCompiler.cpp
    compiler/RoxalIndentationLexer.cpp
    compiler/TypeDeducer.cpp
    compiler/Value.cpp
    compiler/SimpleMarkSweepGC.cpp
    compiler/Thread.cpp
    compiler/VM.cpp
    compiler/linenoise.c
)

list(APPEND COMPILER_SOURCES compiler/ModuleMath.cpp)
list(APPEND COMPILER_SOURCES compiler/ModuleSys.cpp)

if(ROXAL_ENABLE_FILEIO)
    list(APPEND COMPILER_SOURCES compiler/ModuleFileIO.cpp)
endif()
if(ROXAL_ENABLE_GRPC)
    list(APPEND COMPILER_SOURCES
        compiler/grpc/ClientCall.cpp
        compiler/grpc/Connector.cpp
        compiler/grpc/ProtoAdapter.cpp
        compiler/grpc/ModuleGrpc.cpp
    )
endif()

if(ENABLE_UI)
    list(APPEND COMPILER_SOURCES ui/ModuleUI.cpp)
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
    ${CMAKE_SOURCE_DIR}/compiler
    ${CMAKE_SOURCE_DIR}/compiler/grpc
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
        dl
        ${LIBFFI_LIBRARIES}
        $<$<BOOL:${ROXAL_ENABLE_GRPC}>:protobuf::libprotobuf>
        $<$<BOOL:${ROXAL_ENABLE_GRPC}>:gRPC::grpc++>
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
    $<$<BOOL:${ROXAL_ENABLE_GRPC}>:ROXAL_ENABLE_GRPC>
    ROXAL_VERSION="${ROXAL_PROJECT_VERSION}"
    ROXAL_GIT_HASH="${ROXAL_GIT_HASH}"
    ROXAL_PRERELEASE="${ROXAL_PRERELEASE}"
)


if(ENABLE_UI)

    list(APPEND COMPILER_SOURCES ui/ModuleUI.cpp)

    set(LV_BUILD_CONF_PATH ${CMAKE_SOURCE_DIR}/ui/lv_conf.h CACHE STRING "" FORCE)
    set(CONFIG_LV_BUILD_EXAMPLES ON CACHE BOOL "" FORCE)
    set(CONFIG_LV_BUILD_DEMOS    ON CACHE BOOL "" FORCE)

    # Prefer config packages (helps with vcpkg)
    set(CMAKE_FIND_PACKAGE_PREFER_CONFIG ON)
    find_package(Freetype REQUIRED)

    find_package(PNG REQUIRED)
    find_package(ZLIB REQUIRED)

    find_package(OpenGL REQUIRED)          # OpenGL::GL
    find_package(GLEW REQUIRED)            # GLEW::GLEW  (via vcpkg)
    find_package(glfw3 CONFIG REQUIRED)    # glfw (or glfw3::glfw depending on port)

    add_subdirectory(../lvgl lvgl)

    # Give FreeType headers/libs to the target that compiles code including them
    target_link_libraries(lvgl PRIVATE Freetype::Freetype)

    # Your library that uses LVGL
    target_link_libraries(roxalcore PUBLIC lvgl PNG::PNG ZLIB::ZLIB OpenGL::GL GLEW::GLEW glfw)

    target_compile_definitions(roxalcore PUBLIC ENABLE_UI=1)
else()
    target_compile_definitions(roxalcore PUBLIC ENABLE_UI=0)
endif()


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
        $<$<BOOL:${ROXAL_ENABLE_GRPC}>:ROXAL_ENABLE_GRPC>
        ROXAL_VERSION="${ROXAL_PROJECT_VERSION}"
        ROXAL_GIT_HASH="${ROXAL_GIT_HASH}"
        ROXAL_PRERELEASE="${ROXAL_PRERELEASE}"
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
