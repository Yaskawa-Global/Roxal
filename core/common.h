#pragma once

#include <cstdint>
#include <cstdarg> 

#include <string>
#include <memory>
#include <vector>
#include <iostream>
#include <stdexcept>

// ICU
#include <unicode/unistr.h>

//#define DEBUG_OUTPUT_LEXER_TOKENS
//#define DEBUG_OUTPUT_PARSE_TREE
//#define DEBUG_TRACE_PARSE
#define DEBUG_OUTPUT_CHUNK
//#define DEBUG_TRACE_MEMORY
//#define DEBUG_TRACE_EXECUTION


//#define NAN_TAGGING


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


inline std::string toUTF8StdString(const icu::UnicodeString& us) {
    std::string s {};
    us.toUTF8String(s);
    return s;
}

// assumes UTF8 encoded std::string
inline icu::UnicodeString toUnicodeString(const std::string& s) {
    return icu::UnicodeString::fromUTF8(s);
}


// demangle typeid(T).name() strings to be more human readable
std::string demangle(const char* name);


inline std::string spaces(int n) {
    return std::string( n, ' ' );
}

}