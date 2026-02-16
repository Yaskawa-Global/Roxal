cmake_minimum_required(VERSION 3.10)

project(roxal VERSION 0.8.0 LANGUAGES CXX C)
set(ROXAL_PRERELEASE "pre" CACHE STRING "SemVer prerelease label (e.g. alpha.1, rc.1)")

enable_testing()

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-fmax-errors=1)
endif()

option(ROXAL_BUILD_MAIN "Build the roxal executable" ON)


option(ROXAL_ENABLE_FILEIO "Enable builtin file I/O module" ON)
option(ROXAL_ENABLE_GRPC "Enable gRPC/protobuf import support" OFF)
option(ROXAL_ENABLE_DDS "Enable CycloneDDS support" OFF)
set(ROXAL_DDS_ROOT "/usr/share" CACHE PATH "CycloneDDS install prefix when ROXAL_ENABLE_DDS is ON")
option(ROXAL_ENABLE_REGEX "Enable regex module (requires libpcre2)" OFF)
option(ROXAL_ENABLE_SOCKET "Enable socket module (POSIX sockets)" ON)
option(ROXAL_ENABLE_ONNX "Enable ONNX Runtime backend for tensor type" OFF)
set(ROXAL_ONNX_ROOT "${CMAKE_SOURCE_DIR}/deps/onnxruntime" CACHE PATH "ONNX Runtime install prefix")
option(ROXAL_ENABLE_AI_NN "Enable ai.nn neural network inference module (requires ONNX Runtime)" OFF)
if(ROXAL_ENABLE_AI_NN)
    set(ROXAL_ENABLE_ONNX ON CACHE BOOL "Enabled implicitly by ROXAL_ENABLE_AI_NN" FORCE)
endif()

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

find_package(PkgConfig REQUIRED)


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
    pkg_check_modules(GRPC REQUIRED grpc++)
    include_directories(${GRPC_INCLUDE_DIRS})
endif()

# CycloneDDS
#   git clone https://github.com/eclipse-cyclonedds/cyclonedds and configure with
#   cmake -Bbuild -D CMAKE_INSTALL_PREFIX=/usr/share
#   then build and make install
if(ROXAL_ENABLE_DDS)
    if(ROXAL_DDS_ROOT)
        list(APPEND CMAKE_PREFIX_PATH "${ROXAL_DDS_ROOT}")
    endif()
    find_package(CycloneDDS CONFIG REQUIRED)
    include_directories(${CycloneDDS_INCLUDE_DIRS})
endif()

# PCRE2 for regex module
if(ROXAL_ENABLE_REGEX)
    pkg_check_modules(PCRE2 REQUIRED libpcre2-8)
    include_directories(${PCRE2_INCLUDE_DIRS})
endif()

# ONNX Runtime
#   Download a release from https://github.com/microsoft/onnxruntime/releases
#   and extract into deps/onnxruntime (see install-deps.sh).
if(ROXAL_ENABLE_ONNX)
    find_path(ONNXRUNTIME_INCLUDE_DIR
        NAMES onnxruntime_cxx_api.h
        PATHS ${ROXAL_ONNX_ROOT}/include
              /usr/local/include/onnxruntime
              /usr/include/onnxruntime
        NO_DEFAULT_PATH
    )
    find_library(ONNXRUNTIME_LIBRARY
        NAMES onnxruntime
        PATHS ${ROXAL_ONNX_ROOT}/lib
              /usr/local/lib
              /usr/lib
        NO_DEFAULT_PATH
    )
    if(NOT ONNXRUNTIME_INCLUDE_DIR OR NOT ONNXRUNTIME_LIBRARY)
        message(FATAL_ERROR
            "ONNX Runtime not found. Set ROXAL_ONNX_ROOT to the install prefix "
            "(default: deps/onnxruntime) or disable with -DROXAL_ENABLE_ONNX=OFF")
    endif()
    message(STATUS "Found ONNX Runtime: ${ONNXRUNTIME_LIBRARY}")
    message(STATUS "ONNX Runtime include: ${ONNXRUNTIME_INCLUDE_DIR}")
    include_directories(${ONNXRUNTIME_INCLUDE_DIR})

    # CUDA toolkit (optional) — enables GPU→CPU tensor transfer via cudaMemcpy
    find_package(CUDAToolkit QUIET)
    if(CUDAToolkit_FOUND)
        message(STATUS "CUDA toolkit found: ${CUDAToolkit_VERSION} — enabling GPU tensor support")
    endif()
endif()

# Detect libffi via pkg-config so that include and library paths
# are recorded explicitly and the build fails clearly if libffi is
# missing.
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
list(APPEND COMPILER_SOURCES compiler/LazyModuleRegistry.cpp)
list(APPEND COMPILER_SOURCES compiler/RuntimeConfig.cpp)
list(APPEND COMPILER_SOURCES compiler/RTCallbackManager.cpp)

if(ROXAL_ENABLE_FILEIO)
    list(APPEND COMPILER_SOURCES compiler/ModuleFileIO.cpp)
    list(APPEND COMPILER_SOURCES compiler/AsyncIOManager.cpp)
endif()
if(ROXAL_ENABLE_GRPC)
    list(APPEND COMPILER_SOURCES
        compiler/grpc/ClientCall.cpp
        compiler/grpc/Connector.cpp
        compiler/grpc/ProtoAdapter.cpp
        compiler/grpc/ModuleGrpc.cpp
    )
endif()
if(ROXAL_ENABLE_DDS)
    list(APPEND COMPILER_SOURCES
        compiler/dds/DdsAdapter.cpp
        compiler/dds/ModuleDDS.cpp
        compiler/dds/AsyncDDSManager.cpp
    )
endif()
if(ROXAL_ENABLE_REGEX)
    list(APPEND COMPILER_SOURCES compiler/ModuleRegex.cpp)
endif()
if(ROXAL_ENABLE_SOCKET)
    list(APPEND COMPILER_SOURCES compiler/ModuleSocket.cpp)
endif()
if(ROXAL_ENABLE_AI_NN)
    list(APPEND COMPILER_SOURCES compiler/ModuleNN.cpp)
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
find_program(ANTLR4 antlr4)
add_custom_command(
    OUTPUT ${GENERATED_SOURCES}
    COMMAND ${ANTLR4} -v 4.13.1 -Dlanguage=Cpp -package "roxal" -no-listener -visitor ${GRAMMAR_FILE} -o ${GENERATED_DIR}
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
    ${CMAKE_SOURCE_DIR}/compiler/dds
)

link_directories(
    ${ANTLR4_ROOT}/lib
    ${CMAKE_SOURCE_DIR}/cpp-gen
    ${LIBFFI_LIBRARY_DIRS}
    ${PCRE2_LIBRARY_DIRS}
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
        $<$<BOOL:${ROXAL_ENABLE_GRPC}>:${GRPC_LIBRARIES}>
        $<$<BOOL:${ROXAL_ENABLE_DDS}>:CycloneDDS::ddsc>
        $<$<BOOL:${ROXAL_ENABLE_DDS}>:CycloneDDS::libidlc>
        $<$<BOOL:${ROXAL_ENABLE_DDS}>:CycloneDDS::idl>
        $<$<BOOL:${ROXAL_ENABLE_REGEX}>:${PCRE2_LIBRARIES}>
        $<$<BOOL:${ROXAL_ENABLE_ONNX}>:${ONNXRUNTIME_LIBRARY}>
        $<$<AND:$<BOOL:${ROXAL_ENABLE_ONNX}>,$<BOOL:${CUDAToolkit_FOUND}>>:CUDA::cudart>
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
    $<$<BOOL:${ROXAL_ENABLE_DDS}>:ROXAL_ENABLE_DDS>
    $<$<BOOL:${ROXAL_ENABLE_REGEX}>:ROXAL_ENABLE_REGEX>
    $<$<BOOL:${ROXAL_ENABLE_SOCKET}>:ROXAL_ENABLE_SOCKET>
    $<$<BOOL:${ROXAL_ENABLE_ONNX}>:ROXAL_ENABLE_ONNX>
    $<$<AND:$<BOOL:${ROXAL_ENABLE_ONNX}>,$<BOOL:${CUDAToolkit_FOUND}>>:ROXAL_ENABLE_CUDA>
    $<$<BOOL:${ROXAL_ENABLE_AI_NN}>:ROXAL_ENABLE_AI_NN>
    ROXAL_VERSION="${ROXAL_PROJECT_VERSION}"
    ROXAL_GIT_HASH="${ROXAL_GIT_HASH}"
    ROXAL_PRERELEASE="${ROXAL_PRERELEASE}"
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
        $<$<BOOL:${ROXAL_ENABLE_GRPC}>:ROXAL_ENABLE_GRPC>
        $<$<BOOL:${ROXAL_ENABLE_DDS}>:ROXAL_ENABLE_DDS>
        $<$<BOOL:${ROXAL_ENABLE_REGEX}>:ROXAL_ENABLE_REGEX>
        $<$<BOOL:${ROXAL_ENABLE_SOCKET}>:ROXAL_ENABLE_SOCKET>
        $<$<BOOL:${ROXAL_ENABLE_ONNX}>:ROXAL_ENABLE_ONNX>
        $<$<AND:$<BOOL:${ROXAL_ENABLE_ONNX}>,$<BOOL:${CUDAToolkit_FOUND}>>:ROXAL_ENABLE_CUDA>
        $<$<BOOL:${ROXAL_ENABLE_AI_NN}>:ROXAL_ENABLE_AI_NN>
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
