#pragma once

#include <cstdint>
#include <cstdarg>

#include <string>
#include <memory>
#include <vector>
#include <queue>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <limits>
#include <atomic>
#include <typeinfo>


// ICU
#include <unicode/unistr.h>

//#define DEBUG_OUTPUT_LEXER_TOKENS
//#define DEBUG_OUTPUT_PARSE_TREE
//#define DEBUG_TRACE_PARSE
//#define DEBUG_TRACE_SCOPES
//#define DEBUG_TRACE_NAME_RESOLUTION
//#define DEBUG_TRACE_MEMORY
//#define DEBUG_TRACE_EXECUTION

#define NAN_TAGGING

#if USE_GC_SGCL
#include <core/sgcl/sgcl.h>
#include <core/sgcl/detail/page.h>
#endif


namespace roxal {

constexpr int hostArch = sizeof(void*) == 8 ? 64 : 32;

#if !USE_GC_SGCL

template<class T>
using ptr = std::shared_ptr<T>;

template<class T>
using weak_ptr = std::weak_ptr<T>;

template<class T, class... Args>
inline ptr<T> make_ptr(Args&&... args) {
    return std::make_shared<T>(std::forward<Args>(args)...);
}

// template<class T>
// ptr<T> ptr_from_this(T* t) { return t->shared_from_this(); }

#else // USE_GC_SGCL

template<class T>
using ptr = sgcl::tracked_ptr<T>;

template<class T>
class weak_ptr {
    T* _ptr = nullptr;

public:
    using element_type = T;

    weak_ptr() noexcept = default;
    weak_ptr(std::nullptr_t) noexcept {}
    weak_ptr(const ptr<T>& p) noexcept : _ptr(p.get()) {}

    weak_ptr(const weak_ptr&) noexcept = default;
    weak_ptr& operator=(const weak_ptr&) noexcept = default;

    weak_ptr& operator=(const ptr<T>& p) noexcept {
        _ptr = p.get();
        return *this;
    }

    T* get() const noexcept { return _ptr; }

    void reset() noexcept { _ptr = nullptr; }

    bool expired() const noexcept {
        if (!_ptr) return true;
        using namespace sgcl::detail;
        auto page = Page::page_of(_ptr);
        auto index = page->index_of(_ptr);
        auto state = page->states()[index].load(std::memory_order_acquire);
        if (state == State::Destroyed || state == State::Reserved || state == State::Unused) {
            return true;
        }
        const std::type_info& ti = Page::metadata_of(_ptr).type_info;
        return ti != typeid(T);
    }

    ptr<T> lock() const noexcept {
        if (expired()) return nullptr;
        return ptr<T>(_ptr);
    }
};

template<class T, class... Args>
inline ptr<T> make_ptr(Args&&... args) {
    return sgcl::make_tracked<T>(std::forward<Args>(args)...);
}

// template<class T>
// ptr<T> ptr_from_this(T* t) { return make_ptr<T>(t); }


#endif


template<class T, class U>
inline ptr<T> dynamic_ptr_cast(const ptr<U>& p) {
#if USE_GC_SGCL
    return sgcl::dynamic_pointer_cast<T>(p);
#else
    return std::dynamic_pointer_cast<T>(p);
#endif
}


// inherit from this class instead of std::enable_shared_from_this<T> directly
template<class T>
struct enable_ptr_from_this
#if !USE_GC_SGCL
    : std::enable_shared_from_this<T>
#endif
{
    ptr<T> ptr_from_this() {
    #if USE_GC_SGCL
        return ptr<T>(static_cast<T*>(this));      // construct sgcl::tracked_ptr
    #else
        return this->shared_from_this();           // std::shared_ptr path
    #endif
    }
};



template<class Map>
inline auto mapValues(const Map& m) {
    using ValueType = typename Map::mapped_type;
    std::vector<ValueType> vec;
    vec.reserve(m.size());
    for (const auto& kv : m) vec.push_back(kv.second);
    return vec;
}


#define _CRT_NO_VA_START_VALIDATION

// inline to avoid linker error (?)
inline std::string format(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    size_t len = std::vsnprintf(nullptr, 0, fmt, args);
    va_end(args);
    std::vector<char> vec(len + 1);
    va_start(args, fmt);
    std::vsnprintf(vec.data(), len + 1, fmt, args);
    va_end(args);
    return vec.data();
}

#undef _CRT_NO_VA_START_VALIDATION

bool startsWith(const std::string& str, const std::string& prefix);
bool startsWith(const icu::UnicodeString& str, const icu::UnicodeString& prefix);

inline std::string toUTF8StdString(const icu::UnicodeString& us) {
    std::string s {};
    us.toUTF8String(s);
    return s;
}

// assumes UTF8 encoded std::string
inline icu::UnicodeString toUnicodeString(const std::string& s) {
    return icu::UnicodeString::fromUTF8(s);
}


uint16_t randomUint16(uint16_t min = 0, uint16_t max = std::numeric_limits<uint16_t>::max());


// demangle typeid(T).name() strings to be more human readable
std::string demangle(const char* name);

inline std::string newlines(int n) {
    return std::string( n, '\n' );
}

inline std::string spaces(int n) {
    return std::string( n, ' ' );
}



// inefficient
std::string stringInterval(const std::string s, size_t startLine, size_t startPos, size_t endLine, size_t endPos);

std::string replace(const std::string& str, const std::string& from, const std::string& to);

//insert new lines (not substrings)
std::string deleteStringLinesAtInterval(const std::string& s, size_t startLine, size_t startPos, size_t endLine, size_t endPos);

//delete lines (not substrings)
std::string insertStringLinesAtInterval(const std::string& s, const std::string& insertS, size_t startLine, size_t startPos);

std::string join(const std::vector<std::string>& strings, const std::string& separator = ", ");

icu::UnicodeString join(const std::vector<icu::UnicodeString>& strings, const std::string& separator = ",");

std::string trim(const std::string& s);
icu::UnicodeString trim(const icu::UnicodeString& s);



inline void assert_msg_impl(bool        expr,
                            const char* expr_str,
                            const char* user_msg,
                            const char* file,
                            int         line,
                            const char* func) {
    if (!expr) {
        std::ostringstream oss;
        oss << "Assertion failed!\n"
            << "  Condition : (" << expr_str << ")\n"
            << "  Message   : " << user_msg  << "\n"
            << "  Location  : " << file << ":" << line
            << " in " << func << "()\n";
        std::cerr << oss.str();
        std::abort();
    }
}


// DEBUG_BUILD-only assertions
#ifdef DEBUG_BUILD
  #define debug_assert_msg(expr, msg) \
    roxal::assert_msg_impl((expr), #expr, (msg), __FILE__, __LINE__, __func__)
#else
  #define debug_assert_msg(expr, msg) ((void)0)
#endif

// always assert, even in release builds
#define assert_msg(expr, msg) \
    roxal::assert_msg_impl((expr), #expr, (msg), __FILE__, __LINE__, __func__)




} // namespace

namespace std {
    template<>
    struct hash<icu::UnicodeString>
    {
        size_t operator()(const icu::UnicodeString& s) const noexcept {
            return static_cast<size_t>(s.hashCode());
        }
    };
}
