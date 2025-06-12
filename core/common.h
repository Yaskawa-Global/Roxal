#pragma once

#include <cstdint>
#include <cstdarg>

#include <string>
#include <memory>
#include <vector>
#include <queue>
#include <iostream>
#include <stdexcept>
#include <limits>


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


namespace roxal {


template<class T>
using ptr = std::shared_ptr<T>;


#define _CRT_NO_VA_START_VALIDATION

// inline to avoid linker error (?)
inline std::string format(const std::string& fmt...)
{
    va_list args;
    va_start(args, fmt);
    size_t len = std::vsnprintf(NULL, 0, fmt.c_str(), args);
    va_end(args);
    std::vector<char> vec(len + 1);
    va_start(args, fmt);
    std::vsnprintf(&vec[0], len + 1, fmt.c_str(), args);
    va_end(args);
    return &vec[0];
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


}

namespace std {
    template<>
    struct hash<icu::UnicodeString>
    {
        size_t operator()(const icu::UnicodeString& s) const noexcept {
            return static_cast<size_t>(s.hashCode());
        }
    };
}
