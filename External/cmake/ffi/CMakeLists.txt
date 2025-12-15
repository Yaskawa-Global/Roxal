cmake_minimum_required(VERSION 3.10)
project(libffi C ASM)

option(FFI_USE_SHARED_LIBS "Build libffi as a shared library" OFF)
option(FFI_SKIP_HEADERS   "Skip installing headers"           OFF)

set(VERSION 3.4.2 CACHE STRING "libffi version")

if(NOT FFI_ROOT)
  set(FFI_ROOT ${CMAKE_CURRENT_LIST_DIR})
endif()

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(CMAKE_SYSTEM_NAME STREQUAL "VxWorks")
  set(TARGET ARM64)
endif()

if(NOT DEFINED TARGET)
  if(NOT CMAKE_SYSTEM_PROCESSOR)
    set(CMAKE_SYSTEM_PROCESSOR "${CMAKE_HOST_SYSTEM_PROCESSOR}")
  endif()
  string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" lower_system_processor)

  if(CMAKE_SYSTEM_NAME MATCHES "Windows" AND CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "arm")
    set(TARGET ARM_WIN32)
  elseif(CMAKE_SYSTEM_NAME MATCHES "Windows" AND CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "arm64")
    set(TARGET ARM_WIN64)
  elseif(CMAKE_SYSTEM_NAME MATCHES "Windows" AND CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "x86")
    set(TARGET X86_WIN32)
  elseif(CMAKE_SYSTEM_NAME MATCHES "Windows" AND (CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "x64" OR CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "amd64"))
    set(TARGET X86_WIN64)
  elseif(lower_system_processor MATCHES "arm64|aarch64")
    set(TARGET ARM64)
  elseif(lower_system_processor MATCHES "^arm")
    set(TARGET ARM)
  elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(TARGET X86_64)
  elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(TARGET X86)
  else()
    message(FATAL_ERROR "Unknown processor: ${CMAKE_SYSTEM_PROCESSOR} (set -DTARGET=ARM64 등으로 강제 지정 가능)")
  endif()
endif()

message(STATUS "libffi TARGET = ${TARGET}")

if("${TARGET}" STREQUAL "X86_64")
  set(HAVE_LONG_DOUBLE 1)
else()
  set(HAVE_LONG_DOUBLE 0)
endif()
set(FFI_EXEC_TRAMPOLINE_TABLE 0)

if(NOT FFI_CONFIG_FILE)
  set(FFI_CONFIG_FILE ${FFI_ROOT}/fficonfig.h)
endif()
if(NOT EXISTS "${FFI_CONFIG_FILE}")
  message(FATAL_ERROR "fficonfig.h not found: ${FFI_CONFIG_FILE}")
endif()

file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/include)
configure_file(${FFI_ROOT}/include/ffi.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/include/ffi.h @ONLY)
file(COPY ${FFI_CONFIG_FILE} DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

if("${TARGET}" STREQUAL "ARM64" OR "${TARGET}" STREQUAL "ARM_WIN64")
  file(COPY ${FFI_ROOT}/src/aarch64/ffitarget.h DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/include)
elseif("${TARGET}" STREQUAL "ARM" OR "${TARGET}" STREQUAL "ARM_WIN32")
  file(COPY ${FFI_ROOT}/src/arm/ffitarget.h     DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/include)
else()
  file(COPY ${FFI_ROOT}/src/x86/ffitarget.h     DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/include)
endif()

include_directories(
  ${CMAKE_CURRENT_BINARY_DIR}/include
  ${CMAKE_CURRENT_BINARY_DIR}
  ${FFI_ROOT}/include
  ${FFI_ROOT}
  ${FFI_ROOT}/src
)

if("${TARGET}" STREQUAL "ARM64" OR "${TARGET}" STREQUAL "ARM_WIN64")
  include_directories(${FFI_ROOT}/src/aarch64)
elseif("${TARGET}" STREQUAL "ARM" OR "${TARGET}" STREQUAL "ARM_WIN32")
  include_directories(${FFI_ROOT}/src/arm)
elseif("${TARGET}" MATCHES "^X86")
  include_directories(${FFI_ROOT}/src/x86)
endif()

set(FFI_SOURCES
  ${FFI_ROOT}/src/closures.c
  ${FFI_ROOT}/src/prep_cif.c
  ${FFI_ROOT}/src/types.c
  ${FFI_ROOT}/src/tramp.c
)

if("${TARGET}" STREQUAL "ARM64" OR "${TARGET}" STREQUAL "ARM_WIN64")
  list(APPEND FFI_SOURCES ${FFI_ROOT}/src/aarch64/ffi.c)
elseif("${TARGET}" STREQUAL "ARM" OR "${TARGET}" STREQUAL "ARM_WIN32")
  list(APPEND FFI_SOURCES ${FFI_ROOT}/src/arm/ffi.c)
else()
  list(APPEND FFI_SOURCES
    ${FFI_ROOT}/src/java_raw_api.c
    ${FFI_ROOT}/src/raw_api.c
  )
  if("${TARGET}" STREQUAL "X86")
    list(APPEND FFI_SOURCES ${FFI_ROOT}/src/x86/ffi.c)
  elseif("${TARGET}" STREQUAL "X86_WIN32")
    list(APPEND FFI_SOURCES ${FFI_ROOT}/src/x86/ffi.c)
  elseif("${TARGET}" STREQUAL "X86_WIN64")
    list(APPEND FFI_SOURCES ${FFI_ROOT}/src/x86/ffiw64.c)
  elseif("${TARGET}" STREQUAL "X86_64")
    list(APPEND FFI_SOURCES
      ${FFI_ROOT}/src/x86/ffi64.c
      ${FFI_ROOT}/src/x86/ffiw64.c
    )
  endif()
endif()

function(add_ffi_asm ASMFILE)
  if(NOT MSVC)
    set_source_files_properties(${ASMFILE} PROPERTIES
      LANGUAGE C
      COMPILE_OPTIONS "-x;assembler-with-cpp"
    )
  endif()
  list(APPEND FFI_SOURCES ${ASMFILE})
  set(FFI_SOURCES "${FFI_SOURCES}" PARENT_SCOPE)
endfunction()

if("${TARGET}" STREQUAL "X86")
  add_ffi_asm(${FFI_ROOT}/src/x86/sysv.S)
elseif("${TARGET}" STREQUAL "X86_64")
  add_ffi_asm(${FFI_ROOT}/src/x86/unix64.S)
  add_ffi_asm(${FFI_ROOT}/src/x86/win64.S)
elseif("${TARGET}" STREQUAL "X86_WIN32")
  add_ffi_asm(${FFI_ROOT}/src/x86/sysv.S)
elseif("${TARGET}" STREQUAL "X86_WIN64")
  add_ffi_asm(${FFI_ROOT}/src/x86/win64.S)
elseif("${TARGET}" STREQUAL "ARM_WIN32")
  add_ffi_asm(${FFI_ROOT}/src/arm/sysv.S)
elseif("${TARGET}" STREQUAL "ARM")
  add_ffi_asm(${FFI_ROOT}/src/arm/sysv.S)
elseif("${TARGET}" STREQUAL "ARM64" OR "${TARGET}" STREQUAL "ARM_WIN64")
  add_ffi_asm(${FFI_ROOT}/src/aarch64/sysv.S)
else()
  message(FATAL_ERROR "No assembly mapping for TARGET=${TARGET}")
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  list(APPEND FFI_SOURCES ${FFI_ROOT}/src/debug.c)
  add_definitions(-DFFI_DEBUG)
endif()

if(NOT FFI_USE_SHARED_LIBS)
  add_definitions(-DFFI_BUILDING)
endif()
if((BUILD_SHARED_LIBS OR FFI_USE_SHARED_LIBS) AND WIN32)
  add_definitions(-DFFI_BUILDING_DLL)
endif()

add_library(ffi STATIC ${FFI_SOURCES})
set_target_properties(ffi PROPERTIES
  OUTPUT_NAME "ffi"                    # => libffi.a
  POSITION_INDEPENDENT_CODE ON         # RTP 등에서 안전
)

install(TARGETS ffi
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION lib
)

if(NOT FFI_SKIP_HEADERS)
  install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/include/ffi.h
    ${CMAKE_CURRENT_BINARY_DIR}/include/ffitarget.h
    DESTINATION include
  )
endif()
