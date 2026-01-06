#ifndef REGEX_WRAPPER_H
#define REGEX_WRAPPER_H

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

namespace roxal {

// RegexWrapper - RAII wrapper around PCRE2 compiled regex
struct RegexWrapper {
    pcre2_code* code = nullptr;
    bool global = false;  // 'g' flag - affects iteration behavior

    RegexWrapper() = default;
    RegexWrapper(pcre2_code* c, bool g) : code(c), global(g) {}

    ~RegexWrapper() {
        if (code) {
            pcre2_code_free(code);
            code = nullptr;
        }
    }

    // Non-copyable
    RegexWrapper(const RegexWrapper&) = delete;
    RegexWrapper& operator=(const RegexWrapper&) = delete;
};

} // namespace roxal

#endif // REGEX_WRAPPER_H
