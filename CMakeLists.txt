cmake_minimum_required(VERSION 3.12)

project(roxal VERSION 0.8.33 LANGUAGES CXX C)
set(ROXAL_PRERELEASE "pre" CACHE STRING "SemVer prerelease label (e.g. alpha.1, rc.1)")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# Optimized x86 builds target x86-64-v2 (SSE4.2/POPCNT era — any x86-64 CPU
# from ~2009 on): hot loops vectorize better and popcount becomes a single
# instruction instead of libgcc's __popcountdi2. Set empty (-DROXAL_RELEASE_MARCH=)
# for fully portable x86 binaries; non-x86 hosts (e.g. arm64) are unaffected.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    set(ROXAL_RELEASE_MARCH "x86-64-v2" CACHE STRING
        "Value for -march= in Release/RelWithDebInfo builds (empty to disable)")
    if(ROXAL_RELEASE_MARCH)
        string(APPEND CMAKE_CXX_FLAGS_RELEASE " -march=${ROXAL_RELEASE_MARCH}")
        string(APPEND CMAKE_CXX_FLAGS_RELWITHDEBINFO " -march=${ROXAL_RELEASE_MARCH}")
    endif()
endif()

# Link-time optimization for optimized builds. On by default for everything
# except Debug (LTO ~doubles link time, unwanted while iterating). Turn off
# with -DROXAL_ENABLE_LTO=OFF. Measured ~10% on real workloads; it does not
# help the bare interpreter loop (GCC won't inline the small Value helpers
# into the very large VM::execute regardless).
option(ROXAL_ENABLE_LTO "Link-time optimization for optimized (non-Debug) builds" ON)
if(ROXAL_ENABLE_LTO AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ROXAL_IPO_OK OUTPUT ROXAL_IPO_MSG)
    if(ROXAL_IPO_OK)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
        message(STATUS "LTO enabled for ${CMAKE_BUILD_TYPE} build")
    else()
        message(WARNING "LTO requested but unsupported by the toolchain: ${ROXAL_IPO_MSG}")
    endif()
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

if(EMSCRIPTEN)
    # -fwasm-exceptions and -pthread are ABI-level: they must match across the
    # ANTLR4 runtime, libroxal and the host, and a mismatch is silent class-layout
    # divergence rather than a link error.  Set them once here so a configure that
    # forgets to pass them by hand cannot produce a subtly broken build.
    #
    # pthreads is not optional: SimpleMarkSweepGC's reclaimer and the dataflow
    # engine actor are both created during VM construction, before any script
    # runs.  See wasm/README.md.
    add_compile_options(-fwasm-exceptions -pthread)
    add_link_options(-fwasm-exceptions -pthread)
endif()

# The CLI entry point needs boost::program_options, a compiled Boost library we
# would have to cross-build for wasm; a browser embedding has no argv anyway, so
# the wasm host (wasm/CMakeLists.txt) provides its own entry point instead.
if(EMSCRIPTEN)
    set(_roxal_build_main_default OFF)
else()
    set(_roxal_build_main_default ON)
endif()
option(ROXAL_BUILD_MAIN "Build the roxal executable" ${_roxal_build_main_default})
unset(_roxal_build_main_default)

option(ROXAL_BUILD_WASM_HOST "Build the WebAssembly host (Emscripten builds only)" ON)

# Unicode text storage and operations provider.  Keep this as a backend selector
# rather than a boolean: WebAssembly will eventually add a JS-hosted provider.
set(ROXAL_UNICODE_BACKEND "icu" CACHE STRING
    "Unicode backend: icu (full Unicode operations) or builtin (UTF-16 core only)")
set_property(CACHE ROXAL_UNICODE_BACKEND PROPERTY STRINGS icu builtin)
string(TOLOWER "${ROXAL_UNICODE_BACKEND}" ROXAL_UNICODE_BACKEND)
if(ROXAL_UNICODE_BACKEND STREQUAL "icu")
    add_compile_definitions(ROXAL_UNICODE_BACKEND_ICU)
    set(ROXAL_UNICODE_LIBRARIES icuio icui18n icuuc icudata)
elseif(ROXAL_UNICODE_BACKEND STREQUAL "builtin")
    add_compile_definitions(ROXAL_UNICODE_BACKEND_BUILTIN)
    set(ROXAL_UNICODE_LIBRARIES)
else()
    message(FATAL_ERROR "ROXAL_UNICODE_BACKEND must be 'icu' or 'builtin' (got '${ROXAL_UNICODE_BACKEND}')")
endif()


option(ROXAL_ENABLE_ALL "Enable all optional modules" OFF)
option(ROXAL_ENABLE_FILEIO "Enable builtin file I/O module" ON)
option(ROXAL_ENABLE_GRPC "Enable gRPC/protobuf import support" OFF)
option(ROXAL_ENABLE_DDS "Enable CycloneDDS support" OFF)
option(ROXAL_ENABLE_REGEX "Enable regex module (requires libpcre2)" OFF)
option(ROXAL_ENABLE_XML "Enable XML parsing/writing support" OFF)
option(ROXAL_ENABLE_SOCKET "Enable socket module (POSIX sockets)" ON)
option(ROXAL_ENABLE_FFI "Enable Foreign Function Interface (FFI; requires libffi)" ON)
# Dedicated GC collector thread: runtime collections run on their own non-RT
# thread (demoted to SCHED_OTHER, kept off the RT core) instead of electing a
# mutator inline.  Default ON on Linux (desktop sim and RT hardware hosts
# alike); OFF elsewhere (e.g. VxWorks kernel module / threadless targets,
# which keep the portable any-thread inline election).  The GC class layout
# is identical either way -- only code is gated.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_roxal_gc_dedicated_default ON)
else()
    set(_roxal_gc_dedicated_default OFF)
endif()
option(ROXAL_GC_DEDICATED_THREAD "Run GC collections on a dedicated non-RT thread" ${_roxal_gc_dedicated_default})
option(ROXAL_COMPUTE_SERVER "Enable compute server (roxal --server) and 'at' remote actor support" OFF)
if(ROXAL_COMPUTE_SERVER)
    set(ROXAL_ENABLE_SOCKET ON CACHE BOOL "Enabled implicitly by ROXAL_COMPUTE_SERVER" FORCE)
endif()
option(ROXAL_ENABLE_ONNX "Enable ONNX Runtime backend for tensor type" OFF)
option(ROXAL_ENABLE_AI_NN "Enable ai.nn neural network inference module (requires ONNX Runtime; in wasm builds it bridges to a host NN provider instead)" OFF)
if(ROXAL_ENABLE_AI_NN AND NOT EMSCRIPTEN)
    # Native ai.nn runs on the ONNX Runtime C++ API, and with ONNX on the
    # tensor type itself is ORT-backed. The wasm build keeps builtin tensor
    # storage and delegates inference to the JS host's provider
    # (onnxruntime-web / onnxruntime-node) -- see compiler/web/NnBridge.h.
    set(ROXAL_ENABLE_ONNX ON CACHE BOOL "Enabled implicitly by ROXAL_ENABLE_AI_NN" FORCE)
endif()
option(ROXAL_ENABLE_MEDIA "Enable media module (PNG/JPEG image I/O)" OFF)
# Audio is a separate axis from images: miniaudio drives real playback/capture
# devices, which a browser tab reaches only through Web Audio on the main
# thread. Images are pure computation and port cleanly, so wasm gets those and
# leaves media.Audio out rather than shipping a backend that cannot open a
# device.
if(EMSCRIPTEN)
    set(_roxal_media_audio_default OFF)
else()
    set(_roxal_media_audio_default ON)
endif()
option(ROXAL_ENABLE_MEDIA_AUDIO "Enable the media module's audio half (miniaudio)" ${_roxal_media_audio_default})
unset(_roxal_media_audio_default)
# Audio without the module that hosts it is meaningless, and reporting it as
# "on" in the feature list would be a lie. A plain variable, not a FORCEd cache
# entry: forcing would stick at OFF the next time media IS enabled.
if(ROXAL_ENABLE_MEDIA_AUDIO AND NOT ROXAL_ENABLE_MEDIA)
    set(ROXAL_ENABLE_MEDIA_AUDIO OFF)
endif()
option(ROXAL_ENABLE_QT "Enable qt module (QML/QtQuick UI, requires Qt6)" OFF)
option(ROXAL_ENABLE_INSPECT "Enable inspect module (AST and dataflow introspection)" ON)
if(ROXAL_ENABLE_ALL)
    set(ROXAL_ENABLE_FILEIO ON CACHE BOOL "Enabled by ROXAL_ENABLE_ALL" FORCE)
    set(ROXAL_ENABLE_GRPC ON CACHE BOOL "Enabled by ROXAL_ENABLE_ALL" FORCE)
    set(ROXAL_ENABLE_DDS ON CACHE BOOL "Enabled by ROXAL_ENABLE_ALL" FORCE)
    set(ROXAL_ENABLE_REGEX ON CACHE BOOL "Enabled by ROXAL_ENABLE_ALL" FORCE)
    set(ROXAL_ENABLE_XML ON CACHE BOOL "Enabled by ROXAL_ENABLE_ALL" FORCE)
    set(ROXAL_ENABLE_SOCKET ON CACHE BOOL "Enabled by ROXAL_ENABLE_ALL" FORCE)
    set(ROXAL_ENABLE_FFI ON CACHE BOOL "Enabled by ROXAL_ENABLE_ALL" FORCE)
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
#    DEBUG_BUILTINS  # print one line per BuiltinModule::link / linkMethod call at startup
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
    # pkg-config finds the SYSTEM library, which is native code -- useless to a
    # wasm link. Prefer an explicit prefix (deps/pcre2-wasm by convention, the
    # same shape as antlr4-wasm-mt) whenever one is present, and fall back to
    # pkg-config for ordinary native builds.
    if(NOT DEFINED PCRE2_ROOT AND EXISTS "${ROXAL_DEPS_DIR}/pcre2-wasm/lib/libpcre2-8.a" AND EMSCRIPTEN)
        set(PCRE2_ROOT "${ROXAL_DEPS_DIR}/pcre2-wasm")
    endif()
    if(PCRE2_ROOT)
        set(PCRE2_ROOT "${PCRE2_ROOT}" CACHE PATH "PCRE2 install prefix")
        set(PCRE2_INCLUDE_DIRS "${PCRE2_ROOT}/include")
        # NO_CMAKE_FIND_ROOT_PATH: the Emscripten toolchain re-roots find_library
        # into its sysroot, which would hide the prefix we just named.
        find_library(PCRE2_LIBRARIES NAMES pcre2-8 PATHS "${PCRE2_ROOT}/lib"
                     NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
        message(STATUS "PCRE2: using ${PCRE2_ROOT}")
    else()
        pkg_check_modules(PCRE2 REQUIRED libpcre2-8)
        message(STATUS "PCRE2: using system (pkg-config)")
    endif()
    include_directories(${PCRE2_INCLUDE_DIRS})
endif()

# pugixml for XML builtins
if(ROXAL_ENABLE_XML)
    if(NOT DEFINED pugixml_ROOT)
      # The wasm cross-build first: deps/pugixml is native code and would fail
      # the link (or, worse, be found and produce nothing that runs).
      if(EMSCRIPTEN AND EXISTS "${ROXAL_DEPS_DIR}/pugixml-wasm/lib/cmake/pugixml")
        set(pugixml_ROOT "${ROXAL_DEPS_DIR}/pugixml-wasm")
      elseif(NOT EMSCRIPTEN AND EXISTS "${ROXAL_DEPS_DIR}/pugixml/lib/cmake/pugixml")
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
if(ROXAL_ENABLE_ONNX AND EMSCRIPTEN)
    message(FATAL_ERROR "ROXAL_ENABLE_ONNX (ORT-backed tensors) is not supported in wasm builds; use ROXAL_ENABLE_AI_NN alone, which bridges to the host NN provider")
endif()
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

# PNG / JPEG for media module. Audio (miniaudio) needs no packages: it is a
# vendored single header (compiler/miniaudio.h, like linenoise) whose device
# backend libraries are dlopen'd at runtime.
if(ROXAL_ENABLE_MEDIA)
    if(EMSCRIPTEN)
        # Emscripten ships libpng and libjpeg as PORTS: one flag both supplies
        # the headers at compile time and links the prebuilt archives, so there
        # is nothing to vendor or cross-build. It must be on the COMPILE line
        # too, which is why this is not just a link option.
        add_compile_options(-sUSE_LIBPNG=1 -sUSE_LIBJPEG=1)
        add_link_options(-sUSE_LIBPNG=1 -sUSE_LIBJPEG=1)
        set(ROXAL_MEDIA_LIBRARIES)
    else()
        find_package(PNG REQUIRED)
        find_package(JPEG REQUIRED)
        set(ROXAL_MEDIA_LIBRARIES PNG::PNG JPEG::JPEG)
    endif()
endif()

# Qt6 for the qt module (QML/QtQuick UI). Point find_package at a desktop Qt6
# install via -DCMAKE_PREFIX_PATH=/path/to/Qt/<ver>/gcc_64 (or set Qt6_DIR).
if(ROXAL_ENABLE_QT)
    find_package(Qt6 COMPONENTS Core Gui Qml Quick REQUIRED)
endif()

# Detect libffi via pkg-config so that include and library paths are recorded
# explicitly and the build fails clearly if libffi is missing.  Only probed when
# ROXAL_ENABLE_FFI is ON -- an FFI-less build must not require libffi to be
# installed at all (that is the point of the flag).
if(ROXAL_ENABLE_FFI)
    pkg_check_modules(LIBFFI REQUIRED libffi)
    # Report the resolved library, not LIBFFI_LIBDIR: pkg-config reports the
    # latter as the prefix libdir (/usr/lib) while the actual .so commonly lives
    # in a multiarch subdir (/usr/lib/x86_64-linux-gnu), which is the thing worth
    # seeing when a non-system libffi is picked up via PKG_CONFIG_PATH.
    if(LIBFFI_LINK_LIBRARIES)
        list(GET LIBFFI_LINK_LIBRARIES 0 _roxal_libffi_where)
    else()
        set(_roxal_libffi_where "${LIBFFI_PREFIX}")
    endif()
    message(STATUS "FFI: enabled — libffi ${LIBFFI_VERSION} (${_roxal_libffi_where})")
    unset(_roxal_libffi_where)
else()
    message(STATUS "FFI: disabled — no libffi needed; @cfunc/@cstruct and sys.loadlib unavailable")
endif()

# NOTE: add_subdirectory(dataflow) is deliberately NOT here.  dataflow's sources
# include compiler/Object.h and compiler/Thread.h, so it needs the same include
# set as roxalcore (compiler/grpc, compiler/dds, ...) -- and include_directories()
# only reaches subdirectories added after the call.  It is added below, straight
# after that block.

# Add the source files
set(CORE_SOURCES
    core/AST.cpp
    core/atomic.cpp
    core/common.cpp
    core/types.cpp
    core/json5.cpp
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
    compiler/OverloadResolver.cpp
    compiler/RoxalCompiler.cpp
    compiler/RoxalIndentationLexer.cpp
    compiler/TypeDeducer.cpp
    compiler/Value.cpp
    compiler/SimpleMarkSweepGC.cpp
    compiler/ThreadManager.cpp
    compiler/Thread.cpp
    compiler/VM.cpp
    compiler/linenoise.c
)

list(APPEND COMPILER_SOURCES compiler/ModuleMath.cpp)
list(APPEND COMPILER_SOURCES compiler/ModuleSys.cpp)
list(APPEND COMPILER_SOURCES compiler/TestRefOrient.cpp)
list(APPEND COMPILER_SOURCES compiler/LazyModuleRegistry.cpp)
list(APPEND COMPILER_SOURCES compiler/RuntimeConfig.cpp)

if(EMSCRIPTEN)
    # The dom module has no native equivalent to gate behind a feature flag -- it
    # exists exactly when there is a browser main thread to proxy operations to.
    list(APPEND COMPILER_SOURCES
        compiler/web/JsBridge.cpp
        compiler/web/ObjJsValue.cpp
        compiler/web/WebHostLoop.cpp
        compiler/web/ModuleDom.cpp
        compiler/web/RoxalStore.cpp
        compiler/web/ModuleWeb.cpp
        compiler/web/UnicodeHost.cpp
    )
    if(ROXAL_ENABLE_AI_NN)
        list(APPEND COMPILER_SOURCES compiler/web/NnBridge.cpp)
    endif()
endif()
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
if(ROXAL_ENABLE_INSPECT)
    list(APPEND COMPILER_SOURCES compiler/ModuleInspect.cpp compiler/AstPrinter.cpp)
endif()
if(ROXAL_ENABLE_SOCKET)
    list(APPEND COMPILER_SOURCES compiler/ModuleSocket.cpp)
endif()
if(ROXAL_ENABLE_FFI)
    list(APPEND COMPILER_SOURCES compiler/FFI.cpp)
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
    if(ROXAL_ENABLE_MEDIA_AUDIO)
        list(APPEND COMPILER_SOURCES compiler/ModuleMediaAudio.cpp)
        list(APPEND COMPILER_SOURCES compiler/miniaudio_impl.cpp)
    endif()
endif()
# NOTE: the qt module is NOT compiled into roxalcore — it builds as a separate
# dlopen'd plugin (libroxalqt.so, defined below) so the roxal binary carries no Qt
# dependency. The core only registers a factory that loads that plugin on `import qt`.

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
    ${CMAKE_SOURCE_DIR}/compiler/qt
)

# Build the dataflow engine library.  Must come after the include_directories()
# above: Signal/DataflowEngine/FuncNode include compiler/Object.h + Thread.h, and
# with roxal_abi's feature macros those headers pull in ModuleGrpc.h / ModuleDDS.h
# from compiler/grpc and compiler/dds.
add_subdirectory(dataflow)

link_directories(
    ${antlr4_ROOT}/lib
    ${CMAKE_SOURCE_DIR}/cpp-gen
    ${LIBFFI_LIBRARY_DIRS}
    ${PCRE2_LIBRARY_DIRS}
)

# Add libffi include directories discovered via pkg-config
if(ROXAL_ENABLE_FFI AND LIBFFI_INCLUDE_DIRS)
    include_directories(${LIBFFI_INCLUDE_DIRS})
endif()


# Build a minimal static library containing just the AST + ANTLR-driven
# parser.  Targeted at out-of-tree tools (codegen, linters, formatters)
# that consume Roxal AST without needing the VM, GC, or external-protocol
# modules.  No optional-feature deps (gRPC/DDS/ONNX/etc.).
set(ROXALAST_SOURCES
    core/AST.cpp
    core/common.cpp
    core/types.cpp
    compiler/ASTGenerator.cpp
    compiler/AstPrinter.cpp
    compiler/RoxalIndentationLexer.cpp
)
add_library(roxalast STATIC ${ROXALAST_SOURCES} ${GENERATED_SOURCES})
add_dependencies(roxalast generate_parser)
target_link_libraries(roxalast PUBLIC
    antlr4-runtime
    ${ROXAL_UNICODE_LIBRARIES}
    pthread
)
target_compile_options(roxalast PRIVATE
    $<$<CONFIG:DEBUG>:-fPIC -Wno-attributes -Werror=return-type -g -fmax-errors=1>
    $<$<CONFIG:RELEASE>:-O3>
)
target_compile_definitions(roxalast PRIVATE
    $<$<CONFIG:DEBUG>:DEBUG_BUILD>
)


# ---------------------------------------------------------------------------
# roxal_abi — the SINGLE source of truth for ABI-affecting compile definitions.
# Every target that shares core C++ types across a binary boundary (roxalcore, the
# roxal executable, and the dlopen'd qt plugin) consumes this, so they can never
# disagree on the feature #defines that determine class layout. A divergence would
# silently corrupt memory across the host<->plugin boundary (it did, once, before
# this existed). Add new feature flags HERE, never on an individual target.
# ---------------------------------------------------------------------------
# The ABI-affecting flags as a plain configure-time list.  Built once and fed to
# BOTH roxal_abi (targets inside this project) and roxal_features.cmake (hosts
# like FC that consume libroxal.a as a prebuilt artifact and so cannot link
# roxal_abi).  One list is what makes the "add new flags HERE" rule above hold
# for out-of-tree hosts too -- previously they had to re-derive the set from the
# ROXAL_HAS_* exports, which silently omitted FILEIO/SOCKET/AI_NN and produced
# ODR violations across the host<->libroxal boundary.
set(ROXAL_ABI_DEFINITIONS "")
if(ROXAL_UNICODE_BACKEND STREQUAL "icu")
    list(APPEND ROXAL_ABI_DEFINITIONS ROXAL_UNICODE_BACKEND_ICU)
else()
    list(APPEND ROXAL_ABI_DEFINITIONS ROXAL_UNICODE_BACKEND_BUILTIN)
endif()
set(_roxal_features_on "")
set(_roxal_features_off "")
foreach(_roxal_feature FILEIO GRPC DDS REGEX XML SOCKET FFI ONNX AI_NN MEDIA MEDIA_AUDIO QT INSPECT)
    string(TOLOWER "${_roxal_feature}" _roxal_feature_label)
    if(ROXAL_ENABLE_${_roxal_feature})
        list(APPEND ROXAL_ABI_DEFINITIONS ROXAL_ENABLE_${_roxal_feature})
        list(APPEND _roxal_features_on "${_roxal_feature_label}")
    else()
        list(APPEND _roxal_features_off "${_roxal_feature_label}")
    endif()
endforeach()
unset(_roxal_feature)
unset(_roxal_feature_label)
if(ROXAL_COMPUTE_SERVER)
    list(APPEND ROXAL_ABI_DEFINITIONS ROXAL_COMPUTE_SERVER)
    list(APPEND _roxal_features_on compute_server)
else()
    list(APPEND _roxal_features_off compute_server)
endif()
if(ROXAL_GC_DEDICATED_THREAD)
    list(APPEND ROXAL_ABI_DEFINITIONS ROXAL_GC_DEDICATED_THREAD)
    list(APPEND _roxal_features_on gc_dedicated_thread)
else()
    list(APPEND _roxal_features_off gc_dedicated_thread)
endif()

# Summary of the configured feature set, derived from the very same list that
# builds ROXAL_ABI_DEFINITIONS above so it cannot drift from what is actually
# compiled in.  Each name is the lowercased flag suffix -- i.e. exactly what you
# would pass to -DROXAL_ENABLE_<NAME> (or -DROXAL_<NAME> for the last two) --
# so the "off" line doubles as the answer to "why did my regex script fail?".
foreach(_roxal_list on off)
    if(_roxal_features_${_roxal_list})
        string(REPLACE ";" ", " _roxal_features_${_roxal_list} "${_roxal_features_${_roxal_list}}")
    else()
        set(_roxal_features_${_roxal_list} "(none)")
    endif()
endforeach()
unset(_roxal_list)
message(STATUS "Features on:  ${_roxal_features_on}")
message(STATUS "Features off: ${_roxal_features_off}")
unset(_roxal_features_on)
unset(_roxal_features_off)

add_library(roxal_abi INTERFACE)
target_compile_definitions(roxal_abi INTERFACE
    # DEBUG_BUILD stays a generator expression so multi-config generators still
    # get it per-config; the export below resolves it from CMAKE_BUILD_TYPE.
    $<$<CONFIG:DEBUG>:DEBUG_BUILD>
    ${ROXAL_ABI_DEFINITIONS}
    ROXAL_VERSION="${ROXAL_PROJECT_VERSION}"
    ROXAL_GIT_HASH="${ROXAL_GIT_HASH}"
    ROXAL_PRERELEASE="${ROXAL_PRERELEASE}"
)

# The macros above make VM.h/Object.h #include their module headers (ModuleGrpc.h,
# dds/ModuleDDS.h, onnxruntime_cxx_api.h, ...), which need their dependencies'
# headers in turn.  Those arrive via imported targets, not include_directories --
# CycloneDDS_INCLUDE_DIRS is empty with the CONFIG package.  Carry them here so
# consuming roxal_abi is sufficient to COMPILE the headers the macros select,
# not just to spell the macros.
target_link_libraries(roxal_abi INTERFACE
    $<$<BOOL:${ROXAL_ENABLE_GRPC}>:protobuf::libprotobuf>
    $<$<BOOL:${ROXAL_ENABLE_DDS}>:CycloneDDS::ddsc>
    $<$<BOOL:${ROXAL_ENABLE_XML}>:pugixml::pugixml>
)

# Backend-parity tests for the Roxal-owned text abstraction.  The same target
# is compiled by each CMake backend configuration.
add_executable(roxal_ustring_test core/ustring_test.cpp)
target_link_libraries(roxal_ustring_test PRIVATE roxal_abi ${ROXAL_UNICODE_LIBRARIES})
add_test(NAME RoxalUString COMMAND roxal_ustring_test)

# Build the static library containing the core compiler and VM
add_library(roxalcore STATIC ${CORE_SOURCES} ${COMPILER_SOURCES} ${GENERATED_SOURCES})
set_target_properties(roxalcore PROPERTIES OUTPUT_NAME roxal)

add_dependencies(roxalcore generate_parser)


target_link_libraries(roxalcore
    PUBLIC
        roxal_abi        # shared ABI definitions; PUBLIC so the roxal exe inherits them too
        antlr4-runtime
        ${ROXAL_UNICODE_LIBRARIES}
        pthread
        dataflow
        dl
        $<$<BOOL:${ROXAL_ENABLE_FFI}>:${LIBFFI_LIBRARIES}>
        $<$<BOOL:${ROXAL_ENABLE_GRPC}>:protobuf::libprotobuf>
        $<$<AND:$<BOOL:${ROXAL_ENABLE_GRPC}>,$<BOOL:${ROXAL_GRPC_FROM_SOURCE}>>:gRPC::grpc++>
        $<$<AND:$<BOOL:${ROXAL_ENABLE_GRPC}>,$<NOT:$<BOOL:${ROXAL_GRPC_FROM_SOURCE}>>>:${GRPC_LIBRARIES}>
        $<$<BOOL:${ROXAL_ENABLE_DDS}>:CycloneDDS::ddsc>
        $<$<BOOL:${ROXAL_ENABLE_DDS}>:CycloneDDS::libidlc>
        $<$<BOOL:${ROXAL_ENABLE_DDS}>:CycloneDDS::idl>
        $<$<BOOL:${ROXAL_ENABLE_REGEX}>:${PCRE2_LIBRARIES}>
        $<$<BOOL:${ROXAL_ENABLE_XML}>:pugixml::pugixml>
        $<$<BOOL:${ROXAL_ENABLE_ONNX}>:${ONNXRUNTIME_LIBRARY}>
        ${ROXAL_MEDIA_LIBRARIES}
        # Qt is intentionally NOT linked here — the qt module is a dlopen'd plugin
        # (libroxalqt.so), so roxalcore and the roxal binary carry no Qt dependency.
)


# Set compile options based on the build type
target_compile_options(roxalcore PRIVATE
    $<$<CONFIG:DEBUG>:-fPIC -Wno-attributes -Werror=return-type -g -fmax-errors=1>
    $<$<CONFIG:RELEASE>:-O3>
)

# Compile definitions come from roxal_abi (linked PUBLIC above) — the single shared
# source of truth, so roxalcore, the roxal exe, and the qt plugin can never diverge.


# ---------------------------------------------------------------------------
# qt module plugin (libroxalqt.so) — built only when ROXAL_ENABLE_QT is ON.
# Links Qt6 but NOT roxalcore: the core VM/GC/Object/... symbols it references are
# left undefined at link time (shared libs allow that) and resolved at dlopen time
# from the roxal executable (built with ENABLE_EXPORTS). The roxal binary therefore
# carries no Qt dependency; Qt is only touched when a script does `import qt`.
# ---------------------------------------------------------------------------
if(ROXAL_ENABLE_QT)
    add_library(roxalqt SHARED
        compiler/qt/ModuleQt.cpp
        compiler/qt/ObjQtObject.cpp
        compiler/qt/ModuleQtConvert.cpp
        compiler/qt/QtSignalRelay.cpp
        compiler/qt/QtSignalHub.cpp
        compiler/qt/QtListModel.cpp
        compiler/qt/QtTreeModel.cpp
        compiler/qt/QtTableModel.cpp
        compiler/qt/QtBindable.cpp
        compiler/qt/QtFrameView.cpp
        compiler/qt/QtPlugin.cpp
    )
    add_dependencies(roxalqt generate_parser)
    # roxal_abi carries the SAME ABI definitions roxalcore uses — the single source of
    # truth that makes a host<->plugin class-layout mismatch impossible by construction.
    # Qt6::CorePrivate brings the private headers (QMetaObjectBuilder) used to build the
    # moc-free dynamic method bridge that makes Roxal object methods callable from QML.
    target_link_libraries(roxalqt PRIVATE roxal_abi Qt6::Core Qt6::CorePrivate Qt6::Gui Qt6::Qml Qt6::Quick)
    # roxalqt transitively includes VM.h -> ModuleDDS.h -> <dds/dds.h> when
    # ROXAL_ENABLE_DDS is on, but it intentionally doesn't link roxalcore/ddsc
    # (those symbols resolve at dlopen from the roxal exe -- see the comment
    # above this target).  It still needs the CycloneDDS *headers* to compile,
    # so pull them from the imported target's interface without adding a link
    # dependency.  roxal_abi carries the ROXAL_ENABLE_DDS define but no include
    # dirs, which is why this is needed on top of it.
    if(ROXAL_ENABLE_DDS)
        target_include_directories(roxalqt PRIVATE
            $<TARGET_PROPERTY:CycloneDDS::ddsc,INTERFACE_INCLUDE_DIRECTORIES>)
    endif()
    target_compile_options(roxalqt PRIVATE
        $<$<CONFIG:DEBUG>:-fPIC -Wno-attributes -Werror=return-type -g -fmax-errors=1>
        $<$<CONFIG:RELEASE>:-O3>
    )
    # Build the plugin next to the roxal binary so the dlopen factory (which searches
    # the executable's directory first) finds it without extra configuration.
    set_target_properties(roxalqt PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
endif()


if(ROXAL_BUILD_MAIN)
    add_executable(roxal ${MAIN_SOURCE})
    target_link_libraries(roxal PRIVATE roxalcore boost_program_options)
    # Export the executable's dynamic symbols (-rdynamic) so the dlopen'd qt plugin
    # (libroxalqt.so) resolves the core VM/GC/Object/... symbols from the running
    # binary — keeping a SINGLE copy of the GC/VM singletons across the boundary.
    if(ROXAL_ENABLE_QT)
        set_target_properties(roxal PROPERTIES ENABLE_EXPORTS ON)
    endif()
    target_compile_options(roxal PRIVATE
        $<$<CONFIG:DEBUG>:-fPIC -Wno-attributes -Werror=return-type -g -fmax-errors=1>
        $<$<CONFIG:RELEASE>:-O3>
    )
    # Compile definitions are inherited from roxal_abi via roxalcore (linked PUBLIC),
    # the single shared source of truth — see the roxal_abi target above.

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
#
# roxal_abi carries DEBUG_BUILD as a per-config generator expression; hosts get
# a single resolved answer, so fold it in here from CMAKE_BUILD_TYPE.
set(_roxal_abi_defs_export ${ROXAL_ABI_DEFINITIONS})
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    list(APPEND _roxal_abi_defs_export DEBUG_BUILD)
endif()

# ...and the include dirs those macros make the headers reach for.  Inside this
# project roxal_abi supplies them by linking the imported targets; a host that
# only has libroxal.a needs the resolved paths.  e.g. ROXAL_ENABLE_ONNX makes
# Object.h include <onnxruntime_cxx_api.h> because ObjTensor's storage member is
# literally shared_ptr<Ort::Value> -- there is no way to see the right layout
# without the ONNX headers on the include path.
# Only roxal's own header dirs, plus Eigen (Object.h includes <Eigen/Geometry>
# unconditionally, so that one is not feature-related).  No dependency include
# paths appear here on purpose: the ABI headers forward-declare the optional
# modules and Ort::Value rather than including them, so a host needs the
# ROXAL_ENABLE_* macros -- which select WHICH members are declared -- but never
# the gRPC, CycloneDDS, pugixml or ONNX Runtime headers.  Keep it that way: the
# moment an ABI header includes a dependency's header, every host inherits that
# dependency's include path just to compile a pointer member.
set(_roxal_abi_incs_export
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/core"
    "${CMAKE_SOURCE_DIR}/compiler"
    "${CMAKE_SOURCE_DIR}/compiler/cpp-gen")
if(EIGEN3_INCLUDE_DIR)
    list(APPEND _roxal_abi_incs_export "${EIGEN3_INCLUDE_DIR}")
endif()
list(REMOVE_DUPLICATES _roxal_abi_incs_export)
file(WRITE "${CMAKE_BINARY_DIR}/roxal_features.cmake"
    # Build configuration, not a feature: hosts link libroxal.a as a prebuilt
    # artifact, so they must compile the roxal headers with a compatible
    # configuration.  CMAKE_BUILD_TYPE controls NDEBUG, which can change struct
    # layout across the host<->libroxal boundary just as the feature flags below
    # can -- and the host's sizeof(VM) guard only catches the feature-flag case.
    # Exported so hosts can inherit it, or warn when it disagrees.
    "set(ROXAL_BUILD_TYPE \"${CMAKE_BUILD_TYPE}\")\n"
    # The exact ABI-affecting preprocessor set libroxal.a was compiled with.  A
    # host MUST apply these verbatim when compiling roxal headers: they are the
    # #defines that decide class layout (ActorInstance::isRemote,
    # Thread::remoteComputeCallState, ObjTensor's storage union, ...), and a
    # mismatch is an ODR violation that silently corrupts memory rather than
    # failing to link.  Re-deriving the set from the ROXAL_HAS_* flags below is
    # NOT equivalent -- those are spelled differently (HAS_ vs ENABLE_).
    # Apply with add_compile_definitions().
    "set(ROXAL_ABI_DEFINITIONS ${_roxal_abi_defs_export})\n"
    # Include dirs required to compile the roxal headers with the defines above.
    # Apply with include_directories() wherever ROXAL_ABI_DEFINITIONS is applied.
    "set(ROXAL_ABI_INCLUDE_DIRS ${_roxal_abi_incs_export})\n"
    # LTO leaves GIMPLE bytecode in libroxal.a, so the host's link runs
    # lto-wrapper (serially, unless it passes -flto=auto).  Exported so hosts
    # can explain the resulting link times, or opt out.
    "set(ROXAL_HAS_LTO ${ROXAL_ENABLE_LTO})\n"
    "set(ROXAL_HAS_FILEIO ${ROXAL_ENABLE_FILEIO})\n"
    "set(ROXAL_HAS_SOCKET ${ROXAL_ENABLE_SOCKET})\n"
    "set(ROXAL_HAS_AI_NN ${ROXAL_ENABLE_AI_NN})\n"
    "set(ROXAL_HAS_GRPC ${ROXAL_ENABLE_GRPC})\n"
    "set(ROXAL_HAS_DDS ${ROXAL_ENABLE_DDS})\n"
    "set(ROXAL_HAS_ONNX ${ROXAL_ENABLE_ONNX})\n"
    "set(ROXAL_HAS_REGEX ${ROXAL_ENABLE_REGEX})\n"
    "set(ROXAL_HAS_INSPECT ${ROXAL_ENABLE_INSPECT})\n"
    "set(ROXAL_HAS_FFI ${ROXAL_ENABLE_FFI})\n"
    "set(ROXAL_HAS_XML ${ROXAL_ENABLE_XML})\n"
    "set(ROXAL_HAS_MEDIA ${ROXAL_ENABLE_MEDIA})\n"
    "set(ROXAL_HAS_MEDIA_AUDIO ${ROXAL_ENABLE_MEDIA_AUDIO})\n"
    "set(ROXAL_HAS_COMPUTE_SERVER ${ROXAL_COMPUTE_SERVER})\n"
    "set(ROXAL_HAS_GC_DEDICATED_THREAD ${ROXAL_GC_DEDICATED_THREAD})\n"
    # qt is a dlopen'd plugin (libroxalqt.so), not linked into libroxal.a, so a
    # host that links roxal doesn't need to link Qt.  Exported so hosts can still
    # detect the qt module -- e.g. to place the plugin next to their binary and
    # add -rdynamic so it resolves core symbols at dlopen.
    "set(ROXAL_HAS_QT ${ROXAL_ENABLE_QT})\n"
)

add_custom_target(check
    COMMAND ${CMAKE_COMMAND} --build . --target all
    COMMAND ${CMAKE_CTEST_COMMAND}
)


# ---------------------------------------------------------------------------
# WebAssembly host.  A child of this project rather than a standalone build so
# the ABI-affecting definitions have ONE source of truth: the host links
# roxalcore, which links roxal_abi PUBLIC, so the defines, include dirs, antlr4
# and dataflow all arrive by inheritance.  (The previous standalone link script
# scraped them back out of the generated roxal_features.cmake, which is the same
# information travelling the long way round.)
# ---------------------------------------------------------------------------
if(EMSCRIPTEN AND ROXAL_BUILD_WASM_HOST)
    add_subdirectory(wasm)
endif()
