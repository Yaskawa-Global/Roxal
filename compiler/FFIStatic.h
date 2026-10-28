#pragma once

// Statically linked FFI libraries.
//
// A wasm build has no dlopen, so a library that Roxal code reaches through
// sys.loadlib/@cfunc is linked into the executable instead and registered here
// under the file name the .rox code loads (e.g. "libcvxshim.so").  sys.loadlib
// looks a path's file name up in this registry before trying dlopen, and @cfunc
// then resolves symbols from the registered table -- so the module's .rox is the
// same in both builds.  A native build may register libraries too.
//
// Deliberately free of VM headers: the generated registration tables
// (tools/ffi_static_registry.py) include it from the library's own translation
// unit, which has only the library's headers to hand.

#include <cstddef>

namespace roxal {

struct StaticFFISymbol {
    const char* name;
    void* address;
};

// Register (or replace) the symbols of the library `fileName` ("libfoo.so").
// Call before any Roxal code loads it -- the host does so at startup.
void registerStaticLibrary(const char* fileName, const StaticFFISymbol* symbols, size_t count);

}
