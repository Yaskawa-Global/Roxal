#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

#if defined(ROXAL_UNICODE_BACKEND_ICU) == defined(ROXAL_UNICODE_BACKEND_BUILTIN)
#error "Define exactly one ROXAL_UNICODE_BACKEND_* backend"
#endif

#ifdef ROXAL_UNICODE_BACKEND_ICU
#include <unicode/unistr.h>
#endif

namespace roxal {

using code_point = char32_t;

constexpr int32_t utf16_code_unit_count(code_point cp) noexcept
{
    return cp > 0xffff ? 2 : 1;
}

// Roxal-owned UTF-16 string abstraction.  Its public operations deliberately
// model the subset of ICU UnicodeString used by the VM/compiler, but none of
// the rest of the codebase is allowed to depend on an ICU type.
//
// The ICU build is a single-member wrapper with all hot operations inline.  In
// the builtin build the same operations use std::u16string; only Unicode-data
// operations (case and title mapping) are unavailable until a later backend
// supplies them.
class ustring {
public:
    ustring() = default;

    // Source literals in Roxal's C++ implementation are UTF-8.  Existing
    // callers using plain ASCII retain their old result, while this also makes
    // UTF-8 literals unambiguous for the non-ICU backend.
    ustring(const char* utf8) : ustring(fromUTF8(std::string(utf8 ? utf8 : ""))) {}
    ustring(const std::string& utf8) : ustring(fromUTF8(utf8)) {}

    // Kept for the existing Concat preallocation call pattern.
    ustring(int32_t capacity, int32_t, int32_t) { reserve(capacity); }

    ustring(const ustring& source, int32_t start)
        : ustring(source.tempSubString(start)) {}
    ustring(const ustring& source, int32_t start, int32_t count)
        : ustring(source.tempSubString(start, count)) {}

    static ustring fromUTF8(const std::string& utf8)
    {
        ustring result;
#ifdef ROXAL_UNICODE_BACKEND_ICU
        result.value_ = icu::UnicodeString::fromUTF8(utf8);
#else
        result.value_ = decodeUTF8(utf8);
#endif
        return result;
    }

    static ustring fromUTF8(const char* utf8)
    {
        return fromUTF8(std::string(utf8 ? utf8 : ""));
    }

    void toUTF8String(std::string& out) const
    {
#ifdef ROXAL_UNICODE_BACKEND_ICU
        value_.toUTF8String(out);
#else
        out.append(encodeUTF8(value_));
#endif
    }

    std::string toUTF8String() const
    {
        std::string out;
        toUTF8String(out);
        return out;
    }

    int32_t length() const noexcept
    {
#ifdef ROXAL_UNICODE_BACKEND_ICU
        return value_.length();
#else
        return static_cast<int32_t>(value_.size());
#endif
    }

    bool isEmpty() const noexcept { return length() == 0; }
    bool isBogus() const noexcept { return false; }

    char16_t charAt(int32_t index) const noexcept
    {
        if (index < 0 || index >= length()) return 0;
#ifdef ROXAL_UNICODE_BACKEND_ICU
        return value_.charAt(index);
#else
        return value_[static_cast<size_t>(index)];
#endif
    }

    code_point char32At(int32_t index) const noexcept
    {
        const char16_t first = charAt(index);
        if (first >= 0xd800 && first <= 0xdbff && index + 1 < length()) {
            const char16_t second = charAt(index + 1);
            if (second >= 0xdc00 && second <= 0xdfff)
                return 0x10000 + ((static_cast<code_point>(first) - 0xd800) << 10)
                       + (static_cast<code_point>(second) - 0xdc00);
        }
        return first;
    }

    void setCharAt(int32_t index, char16_t unit)
    {
        if (index < 0 || index >= length()) return;
#ifdef ROXAL_UNICODE_BACKEND_ICU
        value_.setCharAt(index, static_cast<UChar>(unit));
#else
        value_[static_cast<size_t>(index)] = unit;
#endif
    }

    const char16_t* getBuffer() const noexcept
    {
#ifdef ROXAL_UNICODE_BACKEND_ICU
        return reinterpret_cast<const char16_t*>(value_.getBuffer());
#else
        return value_.data();
#endif
    }

    ustring tempSubString(int32_t start) const { return tempSubString(start, length() - start); }

    ustring tempSubString(int32_t start, int32_t count) const
    {
        const int32_t begin = clampIndex(start);
        const int32_t end = clampIndex(start + (count < 0 ? 0 : count));
        ustring result;
#ifdef ROXAL_UNICODE_BACKEND_ICU
        // ICU's tempSubString() returns a read-only alias into value_.  The
        // Roxal abstraction promises a value result, matching std::u16string
        // and preventing later mutation of the source from changing it.
        result.value_.append(value_, begin, end - begin);
#else
        result.value_ = value_.substr(static_cast<size_t>(begin), static_cast<size_t>(end - begin));
#endif
        return result;
    }

    ustring tempSubStringBetween(int32_t start, int32_t limit) const
    {
        return tempSubString(start, limit - start);
    }

    void extract(int32_t start, int32_t count, ustring& out) const
    {
        out = tempSubString(start, count);
    }

    ustring unescape() const
    {
#ifdef ROXAL_UNICODE_BACKEND_ICU
        ustring result;
        result.value_ = value_.unescape();
        return result;
#else
        ustring result;
        for (int32_t i = 0; i < length(); ++i) {
            const char16_t unit = charAt(i);
            if (unit != u'\\') {
                result.append(unit);
                continue;
            }
            if (++i >= length()) return {};
            const char16_t escape = charAt(i);
            switch (escape) {
                case u'a': result.append(u'\a'); break;
                case u'b': result.append(u'\b'); break;
                case u'e': result.append(static_cast<char16_t>(0x1b)); break;
                case u'f': result.append(u'\f'); break;
                case u'n': result.append(u'\n'); break;
                case u'r': result.append(u'\r'); break;
                case u't': result.append(u'\t'); break;
                case u'v': result.append(u'\v'); break;
                case u'\\': result.append(u'\\'); break;
                case u'\'': result.append(u'\''); break;
                case u'"': result.append(u'"'); break;
                case u'u':
                    if (!appendEscapedCodePoint(result, *this, i, 4)) return {};
                    i += 4;
                    break;
                case u'U':
                    if (!appendEscapedCodePoint(result, *this, i, 8)) return {};
                    i += 8;
                    break;
                case u'x': {
                    if (i + 1 < length() && charAt(i + 1) == u'{') {
                        int32_t close = i + 2;
                        while (close < length() && charAt(close) != u'}') ++close;
                        const int32_t digits = close - (i + 2);
                        if (close >= length() || digits < 1 || digits > 6
                            || !appendEscapedCodePoint(result, *this, i + 1, digits)) return {};
                        i = close;
                    } else {
                        if (!appendEscapedCodePoint(result, *this, i, 2)) return {};
                        i += 2;
                    }
                    break;
                }
                default:
                    // ICU's unescape accepts a non-escape character literally.
                    result.append(escape);
                    break;
            }
        }
        return result;
#endif
    }

    int32_t indexOf(const ustring& needle) const noexcept
    {
#ifdef ROXAL_UNICODE_BACKEND_ICU
        return value_.indexOf(needle.value_);
#else
        const auto pos = value_.find(needle.value_);
        return pos == std::u16string::npos ? -1 : static_cast<int32_t>(pos);
#endif
    }

    int32_t indexOf(char16_t needle) const noexcept
    {
#ifdef ROXAL_UNICODE_BACKEND_ICU
        return value_.indexOf(needle);
#else
        const auto pos = value_.find(needle);
        return pos == std::u16string::npos ? -1 : static_cast<int32_t>(pos);
#endif
    }

    int32_t lastIndexOf(char16_t needle) const noexcept
    {
#ifdef ROXAL_UNICODE_BACKEND_ICU
        return value_.lastIndexOf(needle);
#else
        const auto pos = value_.rfind(needle);
        return pos == std::u16string::npos ? -1 : static_cast<int32_t>(pos);
#endif
    }

    int32_t compare(int32_t start, int32_t count, const ustring& other) const noexcept
    {
        const ustring part = tempSubString(start, count);
        if (part < other) return -1;
        if (other < part) return 1;
        return 0;
    }

    int32_t compareCodePointOrder(const ustring& other) const noexcept
    {
#ifdef ROXAL_UNICODE_BACKEND_ICU
        return value_.compareCodePointOrder(other.value_);
#else
        int32_t lhsIndex = 0;
        int32_t rhsIndex = 0;
        while (lhsIndex < length() && rhsIndex < other.length()) {
            const code_point lhs = char32At(lhsIndex);
            const code_point rhs = other.char32At(rhsIndex);
            if (lhs < rhs) return -1;
            if (lhs > rhs) return 1;
            lhsIndex += utf16_code_unit_count(lhs);
            rhsIndex += utf16_code_unit_count(rhs);
        }
        return lhsIndex < length() ? 1 : rhsIndex < other.length() ? -1 : 0;
#endif
    }

    bool startsWith(const ustring& prefix) const noexcept
    {
        return length() >= prefix.length() && compare(0, prefix.length(), prefix) == 0;
    }

    void reserve(int32_t capacity)
    {
        if (capacity <= 0) return;
#ifdef ROXAL_UNICODE_BACKEND_ICU
        // UnicodeString has no reserve API.  This is intentionally a no-op;
        // preserving its old allocation behavior matters more than forcing a
        // copy solely to reserve capacity.
        (void)capacity;
#else
        value_.reserve(static_cast<size_t>(capacity));
#endif
    }

    ustring& append(const ustring& other)
    {
#ifdef ROXAL_UNICODE_BACKEND_ICU
        value_.append(other.value_);
#else
        value_.append(other.value_);
#endif
        return *this;
    }

    ustring& append(char16_t unit)
    {
#ifdef ROXAL_UNICODE_BACKEND_ICU
        value_.append(static_cast<UChar>(unit));
#else
        value_.push_back(unit);
#endif
        return *this;
    }

    ustring& operator+=(const ustring& other) { return append(other); }
    ustring& operator+=(const char* utf8) { return append(ustring(utf8)); }
    ustring& operator+=(char16_t unit) { return append(unit); }

    void toUpper()
    {
#ifdef ROXAL_UNICODE_BACKEND_ICU
        value_.toUpper();
#else
        unicodeOperationUnavailable("upper");
#endif
    }

    void toLower()
    {
#ifdef ROXAL_UNICODE_BACKEND_ICU
        value_.toLower();
#else
        unicodeOperationUnavailable("lower");
#endif
    }

    void toTitle()
    {
#ifdef ROXAL_UNICODE_BACKEND_ICU
        value_.toTitle(nullptr);
#else
        unicodeOperationUnavailable("title");
#endif
    }

    int32_t hashCode() const noexcept
    {
#ifdef ROXAL_UNICODE_BACKEND_ICU
        return value_.hashCode();
#else
        // ICU UnicodeString uses a 37-based hash over UTF-16 units, with 1
        // specifically for an empty string.  Keeping it makes bytecode/name
        // hashes stable when switching backends.
        if (value_.empty()) return 1;
        uint32_t hash = 0;
        for (char16_t unit : value_)
            hash = hash * 37u + static_cast<uint16_t>(unit);
        return static_cast<int32_t>(hash);
#endif
    }

    friend bool operator==(const ustring& lhs, const ustring& rhs) noexcept
    {
#ifdef ROXAL_UNICODE_BACKEND_ICU
        return lhs.value_ == rhs.value_;
#else
        return lhs.value_ == rhs.value_;
#endif
    }
    friend bool operator!=(const ustring& lhs, const ustring& rhs) noexcept { return !(lhs == rhs); }
    friend bool operator<(const ustring& lhs, const ustring& rhs) noexcept
    {
#ifdef ROXAL_UNICODE_BACKEND_ICU
        return lhs.value_ < rhs.value_;
#else
        return lhs.value_ < rhs.value_;
#endif
    }
    friend bool operator==(const ustring& lhs, const char* rhs) { return lhs == ustring(rhs); }
    friend bool operator==(const char* lhs, const ustring& rhs) { return ustring(lhs) == rhs; }
    friend bool operator!=(const ustring& lhs, const char* rhs) { return !(lhs == rhs); }
    friend bool operator!=(const char* lhs, const ustring& rhs) { return !(lhs == rhs); }
    friend ustring operator+(ustring lhs, const ustring& rhs) { return lhs.append(rhs); }
    friend ustring operator+(ustring lhs, const char* rhs) { return lhs += rhs; }
    friend ustring operator+(const char* lhs, const ustring& rhs) { return ustring(lhs) + rhs; }

private:
    int32_t clampIndex(int32_t index) const noexcept
    {
        if (index <= 0) return 0;
        return index >= length() ? length() : index;
    }

    [[noreturn]] static void unicodeOperationUnavailable(const char* operation)
    {
        throw std::runtime_error(std::string("Unicode backend does not support string.") + operation + "()");
    }

    static int hexDigit(char16_t unit) noexcept
    {
        if (unit >= u'0' && unit <= u'9') return unit - u'0';
        if (unit >= u'a' && unit <= u'f') return unit - u'a' + 10;
        if (unit >= u'A' && unit <= u'F') return unit - u'A' + 10;
        return -1;
    }

    static bool appendEscapedCodePoint(ustring& out, const ustring& source,
                                       int32_t markerIndex, int32_t digits)
    {
        code_point cp = 0;
        const int32_t first = markerIndex + 1;
        if (first + digits > source.length()) return false;
        for (int32_t i = 0; i < digits; ++i) {
            const int value = hexDigit(source.charAt(first + i));
            if (value < 0) return false;
            cp = (cp << 4) | static_cast<code_point>(value);
        }
        if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
        if (cp <= 0xffff) {
            out.append(static_cast<char16_t>(cp));
        } else {
            cp -= 0x10000;
            out.append(static_cast<char16_t>(0xd800 + (cp >> 10)));
            out.append(static_cast<char16_t>(0xdc00 + (cp & 0x3ff)));
        }
        return true;
    }

#ifndef ROXAL_UNICODE_BACKEND_ICU
    static std::u16string decodeUTF8(const std::string& utf8)
    {
        std::u16string out;
        for (size_t i = 0; i < utf8.size();) {
            const uint8_t first = static_cast<uint8_t>(utf8[i]);
            code_point cp = 0xfffd;
            size_t width = 1;
            if (first < 0x80) {
                cp = first;
            } else if (first >= 0xc2 && first <= 0xdf && i + 1 < utf8.size()) {
                const uint8_t b1 = static_cast<uint8_t>(utf8[i + 1]);
                if ((b1 & 0xc0) == 0x80) { cp = ((first & 0x1f) << 6) | (b1 & 0x3f); width = 2; }
            } else if (first >= 0xe0 && first <= 0xef && i + 2 < utf8.size()) {
                const uint8_t b1 = static_cast<uint8_t>(utf8[i + 1]);
                const uint8_t b2 = static_cast<uint8_t>(utf8[i + 2]);
                if ((b1 & 0xc0) == 0x80 && (b2 & 0xc0) == 0x80
                    && !(first == 0xe0 && b1 < 0xa0) && !(first == 0xed && b1 >= 0xa0)) {
                    cp = ((first & 0x0f) << 12) | ((b1 & 0x3f) << 6) | (b2 & 0x3f);
                    width = 3;
                }
            } else if (first >= 0xf0 && first <= 0xf4 && i + 3 < utf8.size()) {
                const uint8_t b1 = static_cast<uint8_t>(utf8[i + 1]);
                const uint8_t b2 = static_cast<uint8_t>(utf8[i + 2]);
                const uint8_t b3 = static_cast<uint8_t>(utf8[i + 3]);
                if ((b1 & 0xc0) == 0x80 && (b2 & 0xc0) == 0x80 && (b3 & 0xc0) == 0x80
                    && !(first == 0xf0 && b1 < 0x90) && !(first == 0xf4 && b1 > 0x8f)) {
                    cp = ((first & 0x07) << 18) | ((b1 & 0x3f) << 12)
                       | ((b2 & 0x3f) << 6) | (b3 & 0x3f);
                    width = 4;
                }
            }
            if (cp <= 0xffff) {
                out.push_back(static_cast<char16_t>(cp));
            } else {
                cp -= 0x10000;
                out.push_back(static_cast<char16_t>(0xd800 + (cp >> 10)));
                out.push_back(static_cast<char16_t>(0xdc00 + (cp & 0x3ff)));
            }
            i += width;
        }
        return out;
    }

    static std::string encodeUTF8(const std::u16string& input)
    {
        std::string out;
        for (size_t i = 0; i < input.size(); ++i) {
            code_point cp = input[i];
            if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < input.size()
                && input[i + 1] >= 0xdc00 && input[i + 1] <= 0xdfff) {
                cp = 0x10000 + ((cp - 0xd800) << 10) + (input[++i] - 0xdc00);
            } else if (cp >= 0xd800 && cp <= 0xdfff) {
                cp = 0xfffd;
            }
            if (cp <= 0x7f) out.push_back(static_cast<char>(cp));
            else if (cp <= 0x7ff) {
                out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
            } else if (cp <= 0xffff) {
                out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
            } else {
                out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
            }
        }
        return out;
    }
#endif

#ifdef ROXAL_UNICODE_BACKEND_ICU
    icu::UnicodeString value_;
#else
    std::u16string value_;
#endif
};

#ifdef ROXAL_UNICODE_BACKEND_ICU
static_assert(sizeof(icu::UnicodeString) == sizeof(ustring),
              "The ICU ustring backend must remain a zero-overhead wrapper");
#endif

} // namespace roxal
