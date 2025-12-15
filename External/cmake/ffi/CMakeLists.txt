cmake_minimum_required(VERSION 3.10)
project(libffi C ASM)

# ------------------------------------------------------------
# 기본 옵션
# ------------------------------------------------------------
option(FFI_USE_SHARED_LIBS "Build libffi as a shared library" OFF)
option(FFI_SKIP_HEADERS   "Skip installing headers"           OFF)

# libffi 버전(ffi.h.in 치환용)
set(VERSION 3.4.2 CACHE STRING "libffi version")

# libffi 소스 루트(외부에서 넘길 수 있음)
if(NOT FFI_ROOT)
  set(FFI_ROOT ${CMAKE_CURRENT_LIST_DIR})
endif()

# C 표준(필요시)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ------------------------------------------------------------
# 타깃 판별 (VxWorks이면 ARM64로 강제)
# ------------------------------------------------------------
if(CMAKE_SYSTEM_NAME STREQUAL "VxWorks")
  set(TARGET ARM64)
endif()

if(NOT DEFINED TARGET)
  # 일반 플랫폼 자동 판별
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
# 일부 플랫폼에서 쓰는 매크로들
if("${TARGET}" STREQUAL "X86_64")
  set(HAVE_LONG_DOUBLE 1)
else()
  set(HAVE_LONG_DOUBLE 0)
endif()
set(FFI_EXEC_TRAMPOLINE_TABLE 0)

# ------------------------------------------------------------
# 헤더/설정 준비 (ffi.h, ffitarget.h, fficonfig.h)
# ------------------------------------------------------------
# fficonfig.h 경로(외부에서 넘길 수 있음). 없으면 FFI_ROOT 밑을 시도.
if(NOT FFI_CONFIG_FILE)
  set(FFI_CONFIG_FILE ${FFI_ROOT}/fficonfig.h)
endif()
if(NOT EXISTS "${FFI_CONFIG_FILE}")
  message(FATAL_ERROR "fficonfig.h not found: ${FFI_CONFIG_FILE}")
endif()

# 바이너리 출력 디렉터리 밑에 구성 헤더 배치
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/include)
configure_file(${FFI_ROOT}/include/ffi.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/include/ffi.h @ONLY)
file(COPY ${FFI_CONFIG_FILE} DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

# 타깃별 ffitarget.h 복사
if("${TARGET}" STREQUAL "ARM64" OR "${TARGET}" STREQUAL "ARM_WIN64")
  file(COPY ${FFI_ROOT}/src/aarch64/ffitarget.h DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/include)
elseif("${TARGET}" STREQUAL "ARM" OR "${TARGET}" STREQUAL "ARM_WIN32")
  file(COPY ${FFI_ROOT}/src/arm/ffitarget.h     DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/include)
else()
  file(COPY ${FFI_ROOT}/src/x86/ffitarget.h     DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/include)
endif()

# 인클루드 경로
include_directories(
  ${CMAKE_CURRENT_BINARY_DIR}/include
  ${CMAKE_CURRENT_BINARY_DIR}
  ${FFI_ROOT}/include
  ${FFI_ROOT}
  ${FFI_ROOT}/src
)

# aarch64 어셈블리에서 내부 헤더를 찾도록 보강
if("${TARGET}" STREQUAL "ARM64" OR "${TARGET}" STREQUAL "ARM_WIN64")
  include_directories(${FFI_ROOT}/src/aarch64)
elseif("${TARGET}" STREQUAL "ARM" OR "${TARGET}" STREQUAL "ARM_WIN32")
  include_directories(${FFI_ROOT}/src/arm)
elseif("${TARGET}" MATCHES "^X86")
  include_directories(${FFI_ROOT}/src/x86)
endif()

# ------------------------------------------------------------
# 소스 목록
# ------------------------------------------------------------
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
  # x86 계열
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

# ------------------------------------------------------------
# 어셈블리(.S) 추가 (전처리 포함)
# ------------------------------------------------------------
function(add_ffi_asm ASMFILE)
  # 비-MSVC: -x assembler-with-cpp 로 전처리 포함
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

# Debug 빌드 시 디버그 소스/매크로
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  list(APPEND FFI_SOURCES ${FFI_ROOT}/src/debug.c)
  add_definitions(-DFFI_DEBUG)
endif()

# libffi 내부 빌드 매크로
if(NOT FFI_USE_SHARED_LIBS)
  add_definitions(-DFFI_BUILDING)
endif()
if((BUILD_SHARED_LIBS OR FFI_USE_SHARED_LIBS) AND WIN32)
  add_definitions(-DFFI_BUILDING_DLL)
endif()

# ------------------------------------------------------------
# 라이브러리 생성 (정적) - 항상 libffi.a 로
# ------------------------------------------------------------
add_library(ffi STATIC ${FFI_SOURCES})
set_target_properties(ffi PROPERTIES
  OUTPUT_NAME "ffi"                    # => libffi.a
  POSITION_INDEPENDENT_CODE ON         # RTP 등에서 안전
)

# ------------------------------------------------------------
# 설치(선택)
# ------------------------------------------------------------
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
