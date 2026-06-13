#pragma once
//
// CudaRuntime — lazy dlopen wrapper for CUDA runtime API.
//
// Loads libcudart.so at first use so that the roxal binary has no
// build-time or link-time dependency on the CUDA toolkit.  When CUDA
// is not installed the functions simply report "not available".
//

#include <cstddef>
#include <string>
#include <dlfcn.h>

class CudaRuntime {
public:
    static CudaRuntime& instance() {
        static CudaRuntime rt;
        return rt;
    }

    bool available() const { return handle_ != nullptr; }

    // cudaMemcpy(dst, src, count, kind)  — kind: 1=H2D, 2=D2H
    int memcpy(void* dst, const void* src, size_t count, int kind) const {
        if (!memcpy_) return -1;
        return memcpy_(dst, src, count, kind);
    }

    // cudaMemGetInfo(&free, &total)
    int memGetInfo(size_t* free, size_t* total) const {
        if (!memGetInfo_) return -1;
        return memGetInfo_(free, total);
    }

    const char* getErrorString(int error) const {
        if (!getErrorString_) return "CUDA runtime not loaded";
        return getErrorString_(error);
    }

private:
    CudaRuntime() {
        // Try versioned names first, then unversioned
        const char* names[] = {
            "libcudart.so.12", "libcudart.so.11", "libcudart.so"
        };
        for (auto name : names) {
            handle_ = dlopen(name, RTLD_NOW);
            if (handle_) break;
        }
        if (handle_) {
            memcpy_        = reinterpret_cast<memcpy_fn>(dlsym(handle_, "cudaMemcpy"));
            memGetInfo_    = reinterpret_cast<memGetInfo_fn>(dlsym(handle_, "cudaMemGetInfo"));
            getErrorString_= reinterpret_cast<getErrorString_fn>(dlsym(handle_, "cudaGetErrorString"));
        }
    }

    ~CudaRuntime() = default;
    CudaRuntime(const CudaRuntime&) = delete;
    CudaRuntime& operator=(const CudaRuntime&) = delete;

    using memcpy_fn         = int(*)(void*, const void*, size_t, int);
    using memGetInfo_fn     = int(*)(size_t*, size_t*);
    using getErrorString_fn = const char*(*)(int);

    void*            handle_         = nullptr;
    memcpy_fn        memcpy_         = nullptr;
    memGetInfo_fn    memGetInfo_     = nullptr;
    getErrorString_fn getErrorString_ = nullptr;
};
