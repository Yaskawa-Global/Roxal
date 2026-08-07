# WebAssembly host for the Roxal VM.
#
# Added by the root CMakeLists when EMSCRIPTEN and ROXAL_BUILD_WASM_HOST.  It is a
# child of that project on purpose: linking roxalcore inherits roxal_abi's compile
# definitions, so the host and libroxal.a cannot disagree about class layouts --
# a disagreement that shows up as silent memory corruption, not a link error.
#
# Configure/build (see also ../install-deps.sh emsdk antlr4-wasm):
#   emcmake cmake -B build-wasm-mt -DCMAKE_BUILD_TYPE=Release \
#     -DROXAL_ENABLE_FFI=OFF -DROXAL_UNICODE_BACKEND=builtin \
#     -DROXAL_ENABLE_SOCKET=OFF -DROXAL_ENABLE_FILEIO=OFF -DROXAL_ENABLE_LTO=OFF \
#     -Dantlr4_ROOT=$PWD/deps/antlr4-wasm-mt \
#     -DEigen3_DIR=$PWD/deps/eigen/share/eigen3/cmake \
#     -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH
#   cmake --build build-wasm-mt -j4 --target roxal_wasm

add_executable(roxal_wasm main.cpp)

# roxalcore links roxal_abi PUBLIC, so the ABI defines, include dirs, the ANTLR4
# runtime and dataflow all arrive transitively.  Nothing to restate here.
target_link_libraries(roxal_wasm PRIVATE roxalcore)

set_target_properties(roxal_wasm PROPERTIES
    OUTPUT_NAME roxal
    SUFFIX ".js"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dist"
)

# --- .rox stdlib preload -----------------------------------------------------
# Only modules/*.rox.  Preloading modules/ wholesale drags in modules/ai (~27MB of
# ONNX model files under git-lfs) plus the opencv/realsense shims, all for features
# this build compiles out: 28MB -> 88KB.
set(_wasm_stdlib_stage "${CMAKE_CURRENT_BINARY_DIR}/stdlib")
set(_wasm_stdlib_stamp "${CMAKE_CURRENT_BINARY_DIR}/stdlib.stamp")
# CONFIGURE_DEPENDS: without it the glob is evaluated once at configure time, so
# a newly added module silently misses the preload until someone reconfigures --
# and the failure looks like "module not found" at runtime, not a build error.
file(GLOB _wasm_stdlib_rox CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/modules/*.rox")

add_custom_command(
    OUTPUT "${_wasm_stdlib_stamp}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_wasm_stdlib_stage}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_wasm_stdlib_rox} "${_wasm_stdlib_stage}"
    COMMAND ${CMAKE_COMMAND} -E touch "${_wasm_stdlib_stamp}"
    DEPENDS ${_wasm_stdlib_rox}
    COMMENT "Staging .rox stdlib for the wasm preload"
    VERBATIM
)
add_custom_target(roxal_wasm_stdlib DEPENDS "${_wasm_stdlib_stamp}")
add_dependencies(roxal_wasm roxal_wasm_stdlib)

# --- link options ------------------------------------------------------------
# -fwasm-exceptions/-pthread come from the root CMakeLists (add_link_options), so
# they stay identical to what libroxal.a and the ANTLR4 runtime were built with.
target_link_options(roxal_wasm PRIVATE
    # main() runs on its own pthread, leaving the browser's main thread free to
    # service the event loop.  This is the whole architecture: Roxal scripts block
    # by nature (sleep, waits, actors), and blocking the browser main thread would
    # freeze the page.  DOM access is proxied back to the main thread instead.
    -sPROXY_TO_PTHREAD=1

    # Pre-spawn Workers: the VM creates the GC reclaimer and the dataflow-engine
    # actor thread during construction, and creating a Worker from a context that
    # then blocks on it can deadlock.
    -sPTHREAD_POOL_SIZE=8

    -sALLOW_MEMORY_GROWTH=1
    -sINITIAL_MEMORY=268435456
    -sSTACK_SIZE=8388608

    # MAXIMUM_MEMORY defaults to 2GB.  With pthreads the maximum is declared up
    # front on the shared memory, so raising it later is a link-time change, not
    # something the runtime can grow into.

    # _roxal_web_queue_inbound is how roxal-bridge.js posts DOM events, store
    # method calls and store writes to the VM thread; _malloc/_free are used by the
    # bridge for its buffers.
    -sEXPORTED_FUNCTIONS=['_main','_roxal_run_file','_roxal_run_source','_roxal_thread_info','_roxal_request_stop','_roxal_submit_source','_roxal_quit','_roxal_completed_count','_roxal_last_result','_roxal_web_queue_inbound','_malloc','_free']
    -sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','FS','ENV','stringToNewUTF8','UTF8ArrayToString','HEAPU8']

    # Main-thread half of the JS bridge. --pre-js so Module.roxalBridge exists
    # before any wasm code can proxy an operation to it.
    "--pre-js=${CMAKE_CURRENT_SOURCE_DIR}/roxal-bridge.js"
    -sMODULARIZE=1
    -sEXPORT_NAME=createRoxal
    -sENVIRONMENT=web,worker,node
    "--preload-file=${_wasm_stdlib_stage}@/stdlib"
)
# --- page assets -------------------------------------------------------------
# The demo pages and the COOP/COEP service worker have to sit NEXT TO roxal.js for
# the browser to load them as one origin, so copy them into the output dir rather
# than expecting anyone to serve two directories.
#
# These are OUTPUT-driven custom commands, not POST_BUILD on roxal_wasm: a
# POST_BUILD step only runs when the target itself relinks, so editing only an
# .html file would leave the stale copy in dist/ while the build still reported
# success -- and the browser would show yesterday's page with no clue why.
set(_wasm_asset_outputs "")
foreach(_asset index.html store-demo.html coi-sw.js)
    add_custom_command(
        OUTPUT "${CMAKE_BINARY_DIR}/dist/${_asset}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/${_asset}" "${CMAKE_BINARY_DIR}/dist/${_asset}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${_asset}"
        COMMENT "Staging ${_asset}"
        VERBATIM)
    list(APPEND _wasm_asset_outputs "${CMAKE_BINARY_DIR}/dist/${_asset}")
endforeach()
add_custom_target(roxal_wasm_assets ALL DEPENDS ${_wasm_asset_outputs})
add_dependencies(roxal_wasm roxal_wasm_assets)

set_property(TARGET roxal_wasm APPEND PROPERTY LINK_DEPENDS "${_wasm_stdlib_stamp}")
set_property(TARGET roxal_wasm APPEND PROPERTY LINK_DEPENDS
             "${CMAKE_CURRENT_SOURCE_DIR}/roxal-bridge.js")
