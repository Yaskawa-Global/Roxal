# RoxalStaticFFI.cmake -- link an FFI library into a host instead of dlopen'ing it.
#
# A Roxal FFI module is a .rox that does `lib = sys.loadlib('./libfoo.so')` and
# declares C functions with @cfunc(lib=lib, ...).  A wasm host cannot dlopen, so
# it builds the library's sources into a static library here, and registers it
# at startup; sys.loadlib then finds it by file name (compiler/FFIStatic.h).
# The module's .rox is the same in both builds.
#
#   include(<roxal>/cmake/RoxalStaticFFI.cmake)
#   roxal_add_static_ffi_library(cvxshim_static
#       ROX     <roxal>/modules/opencv/init.rox
#       SOURCES <roxal>/modules/opencv/shim/cvx_shim.cpp
#       LINK_LIBRARIES ${OpenCV_LIBS})
#   target_link_libraries(my_host PRIVATE cvxshim_static)
#   // and once at startup, before Roxal code imports the module:
#   extern "C" void roxal_ffi_register_cvxshim();   roxal_ffi_register_cvxshim();
#
# tools/ffi_static_registry.py generates the registration table from the .rox.
# The library's main source must end with
#     #ifdef ROXAL_FFI_STATIC_TABLE
#     #include ROXAL_FFI_STATIC_TABLE
#     #endif
# so the table's prototypes (spelled from the @cfunc declarations) are compiled
# against the definitions: a .rox/C signature mismatch is then a compile error,
# not a wasm-ld stub that traps when called.  The register function is named
# after the library file: libfoo.so -> roxal_ffi_register_foo().

set(_ROXAL_STATIC_FFI_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

function(roxal_add_static_ffi_library target)
    cmake_parse_arguments(A "" "ROX;LIBRARY"
                          "SOURCES;INCLUDE_DIRS;LINK_LIBRARIES;COMPILE_DEFINITIONS;COMPILE_OPTIONS" ${ARGN})
    if(NOT A_ROX OR NOT A_SOURCES)
        message(FATAL_ERROR "roxal_add_static_ffi_library(${target}): ROX and SOURCES are required")
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)

    set(_gen "${_ROXAL_STATIC_FFI_ROOT}/tools/ffi_static_registry.py")
    set(_table "${CMAKE_CURRENT_BINARY_DIR}/${target}_ffi_table.inc")
    set(_libarg "")
    if(A_LIBRARY)
        set(_libarg --library "${A_LIBRARY}")
    endif()
    add_custom_command(
        OUTPUT "${_table}"
        COMMAND "${Python3_EXECUTABLE}" "${_gen}" "${A_ROX}" -o "${_table}" ${_libarg}
        DEPENDS "${A_ROX}" "${_gen}"
        COMMENT "Static-FFI table for ${target} from ${A_ROX}"
        VERBATIM)

    add_library(${target} STATIC ${A_SOURCES} "${_table}")
    # The table is #included by the sources, not compiled on its own.
    set_source_files_properties("${_table}" PROPERTIES HEADER_FILE_ONLY ON)
    set_source_files_properties(${A_SOURCES} PROPERTIES OBJECT_DEPENDS "${_table}")
    target_compile_definitions(${target} PRIVATE "ROXAL_FFI_STATIC_TABLE=\"${_table}\"" ${A_COMPILE_DEFINITIONS})
    target_include_directories(${target} PRIVATE "${_ROXAL_STATIC_FFI_ROOT}/compiler" ${A_INCLUDE_DIRS})
    if(A_COMPILE_OPTIONS)
        target_compile_options(${target} PRIVATE ${A_COMPILE_OPTIONS})
    endif()
    if(A_LINK_LIBRARIES)
        target_link_libraries(${target} PUBLIC ${A_LINK_LIBRARIES})
    endif()
endfunction()
