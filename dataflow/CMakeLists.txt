
# Minimum version
cmake_minimum_required(VERSION 3.10)

# This CMakeLists was originally used to build the dataflow engine as a
# standalone test application.  When included from the main Roxal build we
# simply create a library that can be linked against.

project(dataflow LANGUAGES CXX C)

enable_testing()

list(APPEND CMAKE_PREFIX_PATH "~/.local")
list(APPEND CMAKE_PREFIX_PATH "/usr/local")

#
# vcpkg

# Set the relative path to vcpkg (assumed to be at same level as project folder)
set(VCPKG_ROOT "${CMAKE_SOURCE_DIR}/../../../../vcpkg")

# Point CMake to the vcpkg toolchain file
set(CMAKE_TOOLCHAIN_FILE "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "Vcpkg toolchain file")

# Add the vcpkg installation prefix to CMAKE_PREFIX_PATH
list(APPEND CMAKE_PREFIX_PATH "${VCPKG_ROOT}/installed/x64-linux")


#find_package( Protobuf REQUIRED CONFIG )
#find_package( gRPC CONFIG REQUIRED )


# Add the source files

set(DATAFLOW_SOURCES
    Signal.cpp
    FuncNode.cpp
    DataflowEngine.cpp
)

# include the project root directory
include_directories(${CMAKE_HOME_DIRECTORY})

# Build the library containing the engine implementation
add_library(dataflow ${DATAFLOW_SOURCES})

target_link_libraries(dataflow
    PUBLIC
        pthread
)


# Set compile options based on the build type
target_compile_options(dataflow PRIVATE
    $<$<CONFIG:DEBUG>:-fPIC -Wno-attributes -Werror=return-type -g -fmax-errors=1>
    $<$<CONFIG:RELEASE>:-O3>
)

# Set compile definitions based on the build type
target_compile_definitions(dataflow PRIVATE
    $<$<CONFIG:DEBUG>:DEBUG_BUILD>
)
