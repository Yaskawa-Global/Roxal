cmake_minimum_required(VERSION 3.10)

project(roxal VERSION 1.0 LANGUAGES CXX C)
enable_testing()

# --- Platform detect (VxWorks) ---
if(CMAKE_SYSTEM_NAME STREQUAL "VxWorks")
  set(VXWORKS TRUE)
  add_definitions(-DVXWORKS_BUILD)
endif()

# --- Build options ---
if(NOT VXWORKS)
  option(ROXAL_BUILD_MAIN "Build the roxal executable" ON)
  option(ROXAL_ENABLE_FILEIO "Enable builtin file I/O module" ON)
else()
  option(ROXAL_BUILD_MAIN "Build the roxal executable" OFF)
  option(ROXAL_ENABLE_FILEIO "Enable builtin file I/O module" OFF)
endif()
option(USE_GC_SGCL "Use the SGCL garbage collector" OFF)

add_compile_definitions(
#  DEBUG_OUTPUT_LEXER_TOKENS
#  DEBUG_OUTPUT_PARSE_TREE
#  DEBUG_TRACE_PARSE
#  DEBUG_TRACE_SCOPES
#  DEBUG_TRACE_NAME_RESOLUTION
   DEBUG_TRACE_MEMORY
#  DEBUG_TRACE_EXECUTION
)

# --- C++ standard ---
if(VXWORKS)
  add_compile_options(-fno-char8_t)
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

# --- vcpkg only for non-VxWorks ---
if(NOT VXWORKS)
  if(DEFINED ENV{VCPKG_ROOT})
    set(VCPKG_ROOT $ENV{VCPKG_ROOT})
  else()
    set(VCPKG_ROOT "${CMAKE_SOURCE_DIR}/../vcpkg")
  endif()
  set(CMAKE_TOOLCHAIN_FILE "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
  list(APPEND CMAKE_PREFIX_PATH "${VCPKG_ROOT}/installed/x64-linux")
endif()

# =========================
# Third-party: ANTLR4 / FFI / ICU
# =========================
if(VXWORKS)
  set(VX_LIB_DIR "${CMAKE_SOURCE_DIR}/libraries")

  add_library(antlr4-runtime STATIC IMPORTED GLOBAL)
  set_target_properties(antlr4-runtime PROPERTIES
    IMPORTED_LOCATION             "${VX_LIB_DIR}/antlr4/libantlr4-runtime.a"
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/External/antlr4-cpp-runtime/runtime/src"
  )

  add_library(ffi STATIC IMPORTED GLOBAL)
  set_target_properties(ffi PROPERTIES
    IMPORTED_LOCATION             "${VX_LIB_DIR}/ffi/libffi.a"
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/External/ffi/include"
  )

  add_library(icuio STATIC IMPORTED GLOBAL)
  set_target_properties(icuio PROPERTIES
    IMPORTED_LOCATION             "${VX_LIB_DIR}/icu/libicuio.a"
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/External/icu/icu4c/source/io"
  )
  add_library(icui18n STATIC IMPORTED GLOBAL)
  set_target_properties(icui18n PROPERTIES
    IMPORTED_LOCATION             "${VX_LIB_DIR}/icu/libicui18n.a"
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/External/icu/icu4c/source/i18n"
  )
  add_library(icuuc STATIC IMPORTED GLOBAL)
  set_target_properties(icuuc PROPERTIES
    IMPORTED_LOCATION             "${VX_LIB_DIR}/icu/libicuuc.a"
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/External/icu/icu4c/source/common;${CMAKE_SOURCE_DIR}/External/icu/icu4c/source/common/unicode"
  )
  add_library(icudata STATIC IMPORTED GLOBAL)
  set_target_properties(icudata PROPERTIES
    IMPORTED_LOCATION             "${VX_LIB_DIR}/icu/libicudata.a"
  )
else()
  set(ANTLR4_ROOT "${VCPKG_ROOT}/installed/x64-linux")
  find_package(antlr4-runtime 4.13.2 CONFIG REQUIRED)

  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBFFI REQUIRED libffi)
endif()

# =========================
# Eigen (header-only)
# =========================
if(VXWORKS)
  set(EIGEN3_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/External/eigen"
      CACHE PATH "Eigen include dir" FORCE)
  if(NOT EXISTS "${EIGEN3_INCLUDE_DIR}/Eigen/Dense")
    file(GLOB _eigen_candidates "${CMAKE_SOURCE_DIR}/External/eigen*/Eigen")
    list(LENGTH _eigen_candidates _n)
    if(_n GREATER 0)
      list(GET _eigen_candidates 0 _eigen_Eigen_dir)
      get_filename_component(_eigen_root "${_eigen_Eigen_dir}" DIRECTORY)
      set(EIGEN3_INCLUDE_DIR "${_eigen_root}" CACHE PATH "Eigen include dir" FORCE)
    endif()
  endif()
  if(NOT EXISTS "${EIGEN3_INCLUDE_DIR}/Eigen/Dense")
    message(FATAL_ERROR "Eigen headers not found. Expected: ${EIGEN3_INCLUDE_DIR}/Eigen/Dense")
  endif()
  add_library(Eigen3::Eigen INTERFACE IMPORTED)
  set_target_properties(Eigen3::Eigen PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${EIGEN3_INCLUDE_DIR}"
  )
else()
  find_package(Eigen3 REQUIRED NO_MODULE)
endif()

# =========================
# Subdir: dataflow
# =========================
add_subdirectory(dataflow)
target_compile_definitions(dataflow PRIVATE $<$<BOOL:${USE_GC_SGCL}>:USE_GC_SGCL>)

# =========================
# Sources
# =========================
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
  compiler/ModuleMath.cpp
  compiler/ModuleSys.cpp
)
if(ROXAL_ENABLE_FILEIO)
  list(APPEND COMPILER_SOURCES compiler/ModuleFileIO.cpp)
endif()

set(MAIN_SOURCE compiler/roxal.cpp)

# =========================
# ANTLR4 codegen
# =========================
set(GENERATED_DIR ${CMAKE_SOURCE_DIR}/compiler/cpp-gen)
set(GENERATED_SOURCES
  ${GENERATED_DIR}/RoxalLexer.cpp
  ${GENERATED_DIR}/RoxalParser.cpp
  ${GENERATED_DIR}/RoxalVisitor.cpp
  ${GENERATED_DIR}/RoxalBaseVisitor.cpp
)
set(GRAMMAR_FILE ${CMAKE_SOURCE_DIR}/compiler/Roxal.g4)

add_custom_command(
  OUTPUT  ${GENERATED_SOURCES}
  COMMAND antlr4 -Dlanguage=Cpp -package "roxal" -no-listener -visitor ${GRAMMAR_FILE} -o ${GENERATED_DIR}
  DEPENDS ${GRAMMAR_FILE}
  COMMENT "Generating ANTLR4 parser and lexer from Roxal.g4"
)
add_custom_target(generate_parser DEPENDS ${GENERATED_SOURCES})

# =========================
# Library: roxalcore
# =========================
add_library(roxalcore STATIC ${CORE_SOURCES} ${COMPILER_SOURCES} ${GENERATED_SOURCES})
set_target_properties(roxalcore PROPERTIES OUTPUT_NAME roxal)
add_dependencies(roxalcore generate_parser)

target_include_directories(roxalcore PRIVATE
  ${CMAKE_SOURCE_DIR}
  ${GENERATED_DIR}
)

# 공용 호환 헤더
target_include_directories(roxalcore PUBLIC ${CMAKE_SOURCE_DIR}/External/compat)
target_include_directories(dataflow  PUBLIC ${CMAKE_SOURCE_DIR}/External/compat)

# --- VxWorks: roxalcore가 직접 사용하는 외부 헤더를 명시적으로 포함 ---
if(VXWORKS)
  target_include_directories(roxalcore PRIVATE
    ${CMAKE_SOURCE_DIR}/External/antlr4-cpp-runtime/runtime/src
    ${CMAKE_SOURCE_DIR}/External/icu/icu4c/source/common
    ${CMAKE_SOURCE_DIR}/External/icu/icu4c/source/common/unicode
    ${CMAKE_SOURCE_DIR}/External/icu/icu4c/source/i18n
    ${CMAKE_SOURCE_DIR}/External/icu/icu4c/source/io
    ${CMAKE_SOURCE_DIR}/External/ffi/include
  )
endif()

# Eigen
if(VXWORKS)
  if(DEFINED EIGEN3_INCLUDE_DIR)
    target_include_directories(roxalcore PRIVATE ${EIGEN3_INCLUDE_DIR})
  endif()
else()
  if(TARGET Eigen3::Eigen)
    target_link_libraries(roxalcore PRIVATE Eigen3::Eigen)
  elseif(DEFINED EIGEN3_INCLUDE_DIRS)
    target_include_directories(roxalcore PRIVATE ${EIGEN3_INCLUDE_DIRS})
  endif()
endif()

# Threads / dl
find_package(Threads REQUIRED)

# Link to third-parties (usage reqs)
if(VXWORKS)
  target_link_libraries(roxalcore PUBLIC
    antlr4-runtime
    icuio icui18n icuuc icudata
    ffi
    dataflow
    Threads::Threads
  )
  if(CMAKE_DL_LIBS)
    target_link_libraries(roxalcore PUBLIC ${CMAKE_DL_LIBS})
  endif()
else()
  target_link_libraries(roxalcore PUBLIC
    antlr4-runtime
    icuio icui18n icuuc icudata
    dataflow
    Threads::Threads
    ${CMAKE_DL_LIBS}
    ${LIBFFI_LIBRARIES}
  )
endif()

# Compile options/defs
target_compile_options(roxalcore PRIVATE
  $<$<CONFIG:DEBUG>:-fPIC -Wno-attributes -Werror=return-type -g -fmax-errors=1>
  $<$<CONFIG:RELEASE>:-O3>
)
target_compile_definitions(roxalcore PRIVATE
  $<$<CONFIG:DEBUG>:DEBUG_BUILD>
  $<$<BOOL:${ROXAL_ENABLE_FILEIO}>:ROXAL_ENABLE_FILEIO>
  $<$<BOOL:${USE_GC_SGCL}>:USE_GC_SGCL>
)

# =========================
# (옵션) 리눅스 실행파일
# =========================
if(ROXAL_BUILD_MAIN)
  add_executable(roxal ${MAIN_SOURCE}   ${CMAKE_SOURCE_DIR}/main.cpp)
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


if(VXWORKS)
  # VxWorks 전용 실행파일
  add_executable(roxal_vxe main.cpp)

  # (선택) 뭐가 들어갔는지 보려면 링크맵 생성
  target_link_options(roxal_vxe PRIVATE
    -Wl,-Map,${CMAKE_BINARY_DIR}/roxal_link.map)

  # ★ 특정 아카이브는 전부 포함
  target_link_libraries(roxal_vxe PRIVATE
    -Wl,--whole-archive
      $<TARGET_FILE:roxalcore>
      $<TARGET_FILE:dataflow>
      ${VX_LIB_DIR}/antlr4/libantlr4-runtime.a
      ${VX_LIB_DIR}/icu/libicuio.a
      ${VX_LIB_DIR}/icu/libicui18n.a
      ${VX_LIB_DIR}/icu/libicuuc.a
      ${VX_LIB_DIR}/icu/libicudata.a
      ${VX_LIB_DIR}/ffi/libffi.a
    -Wl,--no-whole-archive
    Threads::Threads
  )
  set_target_properties(roxal_vxe PROPERTIES OUTPUT_NAME "roxal" SUFFIX ".vxe")
endif()


add_custom_target(check
  COMMAND ${CMAKE_COMMAND} --build . --target all
  COMMAND ${CMAKE_CTEST_COMMAND}
)
