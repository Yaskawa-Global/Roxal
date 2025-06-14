cmake_minimum_required(VERSION 3.10)

project(roxal VERSION 1.0 LANGUAGES CXX C)

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

set(SOURCES
    Value.cpp
    Signal.cpp
    Func.cpp
    DataflowEngine.cpp
    Tests.cpp
    main.cpp
)



# include the project root directory
include_directories(${CMAKE_HOME_DIRECTORY})

# Add the include directories
include_directories(
    ${PROJECT_SOURCE_DIR}/src/External/yascore/include
)

link_directories(
)

# Add the executable target
add_executable(main ${CORE_SOURCES} ${SOURCES})


target_link_libraries(main
    pthread
    yiax-yascore
    #boost_program_options
)


# Set compile options based on the build type
target_compile_options(main PRIVATE
    $<$<CONFIG:DEBUG>:-fPIC -Wno-attributes -Werror=return-type -g -fmax-errors=1>
    $<$<CONFIG:RELEASE>:-O3>
)

# Set compile definitions based on the build type
target_compile_definitions(main PRIVATE
    $<$<CONFIG:DEBUG>:DEBUG_BUILD>
)
