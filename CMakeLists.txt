cmake_minimum_required(VERSION 3.12)

project(roxal VERSION 0.8.3 LANGUAGES CXX C)
set(ROXAL_PRERELEASE "pre" CACHE STRING "SemVer prerelease label (e.g. alpha.1, rc.1)")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

enable_testing()

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-fmax-errors=1)
endif()

# AddressSanitizer for debugging
option(ROXAL_ENABLE_ASAN "Enable AddressSanitizer" OFF)
if(ROXAL_ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address)
endif()

option(ROXAL_BUILD_MAIN "Build the roxal executable" ON)


option(ROXAL_ENABLE_ALL "Enable all optional modules" OFF)
option(ROXAL_ENABLE_FILEIO "Enable builtin file I/O module" ON)
option(ROXAL_ENABLE_GRPC "Enable gRPC/protobuf import support" OFF)
option(ROXAL_ENABLE_DDS "Enable CycloneDDS support" OFF)
option(ROXAL_ENABLE_REGEX "Enable regex module (requires libpcre2)" OFF)
option(ROXAL_ENABLE_XML "Enable XML parsing/writing support" OFF)
option(ROXAL_ENABLE_SOCKET "Enable socket module (POSIX sockets)" ON)
option(ROXAL_COMPUTE_SERVER "Enable compute server (roxal --server) and 'at' remote actor support" OFF)
if(ROXAL_COMPUTE_SERVER)
    set(ROXAL_ENABLE_SOCKET ON CACHE BOOL "Enabled implicitly by ROXAL_COMPUTE_SERVER" FORCE)
endif()
option(ROXAL_ENABLE_ONNX "Enable ONNX Runtime backend for tensor type" OFF)
option(ROXAL_ENABLE_AI_NN "Enable ai.nn neural network inference module (requires ONNX Runtime)" OFF)
if(ROXAL_ENABLE_AI_NN)
    set(ROXAL_ENABLE_ONNX ON CACHE BOOL "Enabled implicitly by ROXAL_ENABLE_AI_NN" FORCE)
endif()
option(ROXAL_ENABLE_MEDIA "Enable media module (PNG/JPEG image I/O)" OFF)
if(ROXAL_ENABLE_ALL)
    set(ROXAL_ENABLE_FILEIO ON CACHE BOOL "Enabled by ROXAL_ENABLE_ALL" FORCE)
    set(ROXAL_ENABLE_GRPC ON CACHE BOOL "Enabled by ROXAL_ENABLE_ALL" FORCE)
    set(ROXAL_ENABLE_DDS ON CACHE BOOL "Enabled by ROXAL_ENABLE_ALL" FORCE)
    set(ROXAL_ENABLE_REGEX ON CACHE BOOL "Enabled by ROXAL_ENABLE_ALL" FORCE)
    set(ROXAL_ENABLE_XML ON CACHE BOOL "Enabled by ROXAL_ENABLE_ALL" FORCE)
    set(ROXAL_ENABLE_SOCKET ON CACHE BOOL "Enabled by ROXAL_ENABLE_ALL" FORCE)
    set(ROXAL_COMPUTE_SERVER ON CACHE BOOL "Enabled by ROXAL_ENABLE_ALL" FORCE)
    set(ROXAL_ENABLE_ONNX ON CACHE BOOL "Enabled by ROXAL_ENABLE_ALL" FORCE)
    set(ROXAL_ENABLE_AI_NN ON CACHE BOOL "Enabled by ROXAL_ENABLE_ALL" FORCE)
    set(ROXAL_ENABLE_MEDIA ON CACHE BOOL "Enabled by ROXAL_ENABLE_ALL" FORCE)
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
# From-source dependencies (see install-deps.sh)
#
set(ROXAL_DEPS_DIR "${CMAKE_SOURCE_DIR}/deps")

# Eigen — check: explicit override, then deps/eigen
if(NOT DEFINED Eigen3_ROOT)
  if(EXISTS "${ROXAL_DEPS_DIR}/eigen/share/eigen3/cmake/Eigen3Config.cmake")
    set(Eigen3_ROOT "${ROXAL_DEPS_DIR}/eigen")
  endif()
endif()
if(Eigen3_ROOT)
  set(Eigen3_ROOT "${Eigen3_ROOT}" CACHE PATH "Eigen install prefix")
  list(APPEND CMAKE_PREFIX_PATH "${Eigen3_ROOT}")
  message(STATUS "Eigen: using ${Eigen3_ROOT}")
else()
  message(STATUS "Eigen: using system")
endif()
find_package(Eigen3 REQUIRED)
get_target_property(EIGEN3_INCLUDE_DIR Eigen3::Eigen INTERFACE_INCLUDE_DIRECTORIES)
include_directories(${EIGEN3_INCLUDE_DIR})

# ANTLR4 C++ runtime — check: explicit override, then deps/antlr4
if(NOT DEFINED antlr4_ROOT)
  if(EXISTS "${ROXAL_DEPS_DIR}/antlr4/lib")
    set(antlr4_ROOT "${ROXAL_DEPS_DIR}/antlr4")
  endif()
endif()
if(antlr4_ROOT)
  set(antlr4_ROOT "${antlr4_ROOT}" CACHE PATH "ANTLR4 C++ runtime install prefix")
  message(STATUS "ANTLR4: using ${antlr4_ROOT}")
else()
  message(STATUS "ANTLR4: using system")
endif()
list(APPEND CMAKE_PREFIX_PATH "${antlr4_ROOT}")
# Try CONFIG mode first (available via vcpkg); fall back to manual detection
# for from-source installs which don't ship cmake config files.
find_package(antlr4-runtime 4.13.1 CONFIG QUIET)
if(NOT antlr4-runtime_FOUND)
    find_library(ANTLR4_LIB antlr4-runtime PATHS ${antlr4_ROOT}/lib NO_DEFAULT_PATH)
    find_path(ANTLR4_INCLUDE antlr4-runtime.h PATHS ${antlr4_ROOT}/include/antlr4-runtime NO_DEFAULT_PATH)
    if(NOT ANTLR4_LIB OR NOT ANTLR4_INCLUDE)
        message(FATAL_ERROR "ANTLR4 runtime not found at ${antlr4_ROOT}. "
            "Build from source or set -Dantlr4_ROOT=<prefix>.")
    endif()
endif()

# gRPC — check: explicit override, then deps/grpc, then system packages
if(ROXAL_ENABLE_GRPC)
    if(NOT DEFINED gRPC_ROOT)
      if(EXISTS "${ROXAL_DEPS_DIR}/grpc/lib/cmake/grpc")
        set(gRPC_ROOT "${ROXAL_DEPS_DIR}/grpc")
      endif()
    endif()
    if(gRPC_ROOT)
      set(gRPC_ROOT "${gRPC_ROOT}" CACHE PATH "gRPC/Protobuf install prefix")
      list(APPEND CMAKE_PREFIX_PATH "${gRPC_ROOT}")
      message(STATUS "gRPC: using ${gRPC_ROOT}")

      find_package(Protobuf CONFIG REQUIRED)
      find_package(gRPC CONFIG REQUIRED)

      set(ROXAL_GRPC_FROM_SOURCE TRUE)
    else()
      message(STATUS "gRPC: using system packages")

      find_package(Protobuf REQUIRED)
      pkg_check_modules(GRPC REQUIRED grpc++)
      include_directories(${GRPC_INCLUDE_DIRS})

      set(ROXAL_GRPC_FROM_SOURCE FALSE)
    endif()
endif()

# CycloneDDS — check: explicit override, then deps/cyclonedds
if(ROXAL_ENABLE_DDS)
    if(NOT DEFINED CycloneDDS_ROOT)
      if(EXISTS "${ROXAL_DEPS_DIR}/cyclonedds/lib/cmake/CycloneDDS")
        set(CycloneDDS_ROOT "${ROXAL_DEPS_DIR}/cyclonedds")
      endif()
    endif()
    if(CycloneDDS_ROOT)
      set(CycloneDDS_ROOT "${CycloneDDS_ROOT}" CACHE PATH "CycloneDDS install prefix")
      list(APPEND CMAKE_PREFIX_PATH "${CycloneDDS_ROOT}")
      message(STATUS "CycloneDDS: using ${CycloneDDS_ROOT}")
    else()
      message(STATUS "CycloneDDS: using system")
    endif()
    find_package(CycloneDDS CONFIG REQUIRED)
    include_directories(${CycloneDDS_INCLUDE_DIRS})
endif()

# PCRE2 for regex module
if(ROXAL_ENABLE_REGEX)
    pkg_check_modules(PCRE2 REQUIRED libpcre2-8)
    include_directories(${PCRE2_INCLUDE_DIRS})
endif()

# pugixml for XML builtins
if(ROXAL_ENABLE_XML)
    if(NOT DEFINED pugixml_ROOT)
      if(EXISTS "${ROXAL_DEPS_DIR}/pugixml/lib/cmake/pugixml")
        set(pugixml_ROOT "${ROXAL_DEPS_DIR}/pugixml")
      endif()
    endif()
    if(pugixml_ROOT)
      set(pugixml_ROOT "${pugixml_ROOT}" CACHE PATH "pugixml install prefix")
      list(APPEND CMAKE_PREFIX_PATH "${pugixml_ROOT}")
      message(STATUS "pugixml: using ${pugixml_ROOT}")
    else()
      message(STATUS "pugixml: using system")
    endif()
    find_package(pugixml CONFIG REQUIRED)
endif()

# ONNX Runtime — check: explicit override, then deps/onnxruntime (see install-deps.sh)
if(ROXAL_ENABLE_ONNX)
    if(NOT DEFINED OnnxRuntime_ROOT)
      if(EXISTS "${ROXAL_DEPS_DIR}/onnxruntime/lib")
        set(OnnxRuntime_ROOT "${ROXAL_DEPS_DIR}/onnxruntime")
      endif()
    endif()
    if(OnnxRuntime_ROOT)
      set(OnnxRuntime_ROOT "${OnnxRuntime_ROOT}" CACHE PATH "ONNX Runtime install prefix")
      message(STATUS "ONNX Runtime: using ${OnnxRuntime_ROOT}")
    else()
      message(STATUS "ONNX Runtime: using system")
    endif()

    find_path(ONNXRUNTIME_INCLUDE_DIR
        NAMES onnxruntime_cxx_api.h
        PATHS ${OnnxRuntime_ROOT}/include
              ${OnnxRuntime_ROOT}/include/onnxruntime/core/session
              /usr/local/include/onnxruntime
              /usr/include/onnxruntime
        NO_DEFAULT_PATH
    )
    find_library(ONNXRUNTIME_LIBRARY
        NAMES onnxruntime
        PATHS ${OnnxRuntime_ROOT}/lib
              /usr/local/lib
              /usr/lib
        NO_DEFAULT_PATH
    )
    if(NOT ONNXRUNTIME_INCLUDE_DIR OR NOT ONNXRUNTIME_LIBRARY)
        message(FATAL_ERROR
            "ONNX Runtime not found. Set -DOnnxRuntime_ROOT=<prefix> "
            "or disable with -DROXAL_ENABLE_ONNX=OFF")
    endif()
    include_directories(${ONNXRUNTIME_INCLUDE_DIR})
endif()

# PNG / JPEG for media module
if(ROXAL_ENABLE_MEDIA)
    find_package(PNG REQUIRED)
    find_package(JPEG REQUIRED)
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
list(APPEND COMPILER_SOURCES compiler/TestRefOrient.cpp)
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
if(ROXAL_COMPUTE_SERVER)
    list(APPEND COMPILER_SOURCES compiler/ComputeConnection.cpp)
    list(APPEND COMPILER_SOURCES compiler/ComputeServer.cpp)
endif()
if(ROXAL_ENABLE_AI_NN)
    list(APPEND COMPILER_SOURCES compiler/ModuleNN.cpp)
endif()
if(ROXAL_ENABLE_MEDIA)
    list(APPEND COMPILER_SOURCES compiler/ModuleMedia.cpp)
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
    ${antlr4_ROOT}/include/antlr4-runtime
    compiler/cpp-gen
    ${CMAKE_SOURCE_DIR}/compiler
    ${CMAKE_SOURCE_DIR}/compiler/grpc
    ${CMAKE_SOURCE_DIR}/compiler/dds
)

link_directories(
    ${antlr4_ROOT}/lib
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
        $<$<AND:$<BOOL:${ROXAL_ENABLE_GRPC}>,$<BOOL:${ROXAL_GRPC_FROM_SOURCE}>>:gRPC::grpc++>
        $<$<AND:$<BOOL:${ROXAL_ENABLE_GRPC}>,$<NOT:$<BOOL:${ROXAL_GRPC_FROM_SOURCE}>>>:${GRPC_LIBRARIES}>
        $<$<BOOL:${ROXAL_ENABLE_DDS}>:CycloneDDS::ddsc>
        $<$<BOOL:${ROXAL_ENABLE_DDS}>:CycloneDDS::libidlc>
        $<$<BOOL:${ROXAL_ENABLE_DDS}>:CycloneDDS::idl>
        $<$<BOOL:${ROXAL_ENABLE_REGEX}>:${PCRE2_LIBRARIES}>
        $<$<BOOL:${ROXAL_ENABLE_XML}>:pugixml::pugixml>
        $<$<BOOL:${ROXAL_ENABLE_ONNX}>:${ONNXRUNTIME_LIBRARY}>
        $<$<BOOL:${ROXAL_ENABLE_MEDIA}>:PNG::PNG>
        $<$<BOOL:${ROXAL_ENABLE_MEDIA}>:JPEG::JPEG>
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
    $<$<BOOL:${ROXAL_ENABLE_XML}>:ROXAL_ENABLE_XML>
    $<$<BOOL:${ROXAL_ENABLE_SOCKET}>:ROXAL_ENABLE_SOCKET>
    $<$<BOOL:${ROXAL_ENABLE_ONNX}>:ROXAL_ENABLE_ONNX>
    $<$<BOOL:${ROXAL_ENABLE_AI_NN}>:ROXAL_ENABLE_AI_NN>
    $<$<BOOL:${ROXAL_ENABLE_MEDIA}>:ROXAL_ENABLE_MEDIA>
    $<$<BOOL:${ROXAL_COMPUTE_SERVER}>:ROXAL_COMPUTE_SERVER>
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
        $<$<BOOL:${ROXAL_ENABLE_XML}>:ROXAL_ENABLE_XML>
        $<$<BOOL:${ROXAL_ENABLE_SOCKET}>:ROXAL_ENABLE_SOCKET>
        $<$<BOOL:${ROXAL_ENABLE_ONNX}>:ROXAL_ENABLE_ONNX>
        $<$<BOOL:${ROXAL_ENABLE_AI_NN}>:ROXAL_ENABLE_AI_NN>
        $<$<BOOL:${ROXAL_ENABLE_MEDIA}>:ROXAL_ENABLE_MEDIA>
        $<$<BOOL:${ROXAL_COMPUTE_SERVER}>:ROXAL_COMPUTE_SERVER>
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


# Export enabled features so embedding projects (e.g. FC) can conditionally
# link the transitive dependencies that libroxal.a requires.
file(WRITE "${CMAKE_BINARY_DIR}/roxal_features.cmake"
    "set(ROXAL_HAS_GRPC ${ROXAL_ENABLE_GRPC})\n"
    "set(ROXAL_HAS_DDS ${ROXAL_ENABLE_DDS})\n"
    "set(ROXAL_HAS_ONNX ${ROXAL_ENABLE_ONNX})\n"
    "set(ROXAL_HAS_REGEX ${ROXAL_ENABLE_REGEX})\n"
    "set(ROXAL_HAS_XML ${ROXAL_ENABLE_XML})\n"
    "set(ROXAL_HAS_MEDIA ${ROXAL_ENABLE_MEDIA})\n"
    "set(ROXAL_HAS_COMPUTE_SERVER ${ROXAL_COMPUTE_SERVER})\n"
)

add_custom_target(check
    COMMAND ${CMAKE_COMMAND} --build . --target all
    COMMAND ${CMAKE_CTEST_COMMAND}
)
