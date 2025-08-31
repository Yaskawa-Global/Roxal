
# Minimum version
cmake_minimum_required(VERSION 3.10)

# This CMakeLists was originally used to build the dataflow engine as a
# standalone test application. When included from the main Roxal build we
# simply create a library that can be linked against.

project(dataflow LANGUAGES CXX C)
enable_testing()

if(NOT VXWORKS)
   list(APPEND CMAKE_PREFIX_PATH "~/.local")
   list(APPEND CMAKE_PREFIX_PATH "/usr/local")
endif()


if(NOT VXWORKS)
   #
   # vcpkg

   # Set the relative path to vcpkg (assumed to be at same level as project folder)
   set(VCPKG_ROOT "${CMAKE_SOURCE_DIR}/../../../../vcpkg")

   # Point CMake to the vcpkg toolchain file
   set(CMAKE_TOOLCHAIN_FILE "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "Vcpkg toolchain file")

   # Add the vcpkg installation prefix to CMAKE_PREFIX_PATH
   list(APPEND CMAKE_PREFIX_PATH "${VCPKG_ROOT}/installed/x64-linux")
endif()

# Sources
set(DATAFLOW_SOURCES
    Signal.cpp
    FuncNode.cpp
    DataflowEngine.cpp
)

# Library
add_library(dataflow ${DATAFLOW_SOURCES})

# include the project root directory
if(NOT VXWORKS)
   include_directories(${CMAKE_HOME_DIRECTORY})
else()
   target_include_directories(dataflow PUBLIC ${CMAKE_HOME_DIRECTORY})
endif()

#Threads
if(VXWORKS)
   find_package(Threads REQUIRED)
   target_link_libraries(dataflow
      PUBLIC
         icuuc icui18n icudata
         Eigen3::Eigen
         Threads::Threads)
else()
   target_link_libraries(dataflow
       PUBLIC
           pthread
   )
endif()

# Build options
target_compile_options(dataflow PRIVATE
    $<$<CONFIG:DEBUG>:-fPIC -Wno-attributes -Werror=return-type -g -fmax-errors=1>
    $<$<CONFIG:RELEASE>:-O3>
)

# Defines
target_compile_definitions(dataflow PRIVATE
    $<$<CONFIG:DEBUG>:DEBUG_BUILD>
)
