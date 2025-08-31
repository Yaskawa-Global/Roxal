# VxWorks.cmake -- Platform Definitions for VxWorks
# Copyright (c) 2017 Wind River Systems, Inc. All Rights Reserved.
#
# This file is auto-included by cmake, when CMAKE_SYSTEM = VxWorks
# It is included "late" so allows overriding improper defaults
# from the cmake auto-discovery process where appropriate.
#
# modification history
# --------------------
# 18oct16,mob  written
# 13Nov17,ryan Override the CMAKE_ASM_COMPILE_OBJECT
set(VXWORKS TRUE)
set(CMAKE_SYSTEM_NAME VxWorks)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# Wind River compiler wrappers
set(CMAKE_C_COMPILER   wr-cc)
set(CMAKE_CXX_COMPILER wr-c++)
set(CMAKE_AR           wr-ar)
set(CMAKE_RANLIB       wr-ranlib)


set(CMAKE_C_OUTPUT_EXTENSION .o)
set(CMAKE_C_OUTPUT_EXTENSION_REPLACE 1)
set(CMAKE_CXX_OUTPUT_EXTENSION .o)
set(CMAKE_CXX_OUTPUT_EXTENSION_REPLACE 1)
set(CMAKE_ASM_OUTPUT_EXTENSION .o)
set(CMAKE_ASM_OUTPUT_EXTENSION_REPLACE 1)
# .vxe or .out is set in the toolchain file - users may override
set(CMAKE_EXECUTABLE_SUFFIX ${WIND_EXECUTABLE_SUFFIX})


set(CMAKE_C_COMPILER_FRONTEND_VARIANT   GNU)
set(CMAKE_CXX_COMPILER_FRONTEND_VARIANT GNU)


set(CMAKE_ASM_COMPILER                  wr-cc)
set(CMAKE_ASM_COMPILER_FRONTEND_VARIANT GNU)
set(CMAKE_ASM_FLAGS_INIT "-x assembler-with-cpp")
set(CMAKE_ASM_OUTPUT_EXTENSION ".o")


# The GNU Language discovery process for C and CXX overrides our
# CMAKE_C_FLAGS_INIT and related settings from the Toolchain file.
# Since we really need OUR cross flags and not the discovered ones,
# these are force-overridden back here. Note: This may also mean
# that environment variables are not considered for the _INIT flags.
foreach(lang C CXX ASM)
  foreach(c "" _DEBUG _RELEASE _MINSIZEREL _RELWITHDEBINFO)
    set(CMAKE_${lang}_FLAGS${c}_INIT "${WIND_${lang}_FLAGS${c}}")
  endforeach()
endforeach()

set(CMAKE_ASM_COMPILE_OBJECT "${CMAKE_ASM_COMPILER} ${CMAKE_ASM_FLAGS_INIT} -o <OBJECT> -c <SOURCE>")

# For Auto-discovery purpopses, VxWorks is similar enough to UNIX
# This helps porting software in most cases. It can be turned off
# in a custom toolchain with WIND_VXWORKS_IS_NOT_UNIX if necessary.
set(VXWORKS TRUE)

# Workaround for FindOpenSSL.cmake not knowing VxWorks library names,
# and even failing when the CRYPTO library is not there.
# TODO The proper openssl library name should be contributed to cmake.
find_library(OPENSSL_SSL_LIBRARY OPENSSL)
find_library(OPENSSL_CRYPTO_LIBRARY OPENSSL)
mark_as_advanced(OPENSSL_SSL_LIBRARY OPENSSL_CRYPTO_LIBRARY)
