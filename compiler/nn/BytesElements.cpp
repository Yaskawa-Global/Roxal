#include "BytesElements.h"

#include <cstdint>
#include <stdexcept>

void roxal::nn::appendBytesElement(std::string& out, const void* data, size_t size)
{
    if (size > UINT32_MAX)
        throw std::length_error("a BYTES element of " + std::to_string(size) + " bytes is past 4 GB");
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<char>(size >> (8 * i)));
    out.append(static_cast<const char*>(data), size);
}

bool roxal::nn::splitBytesElements(const void* data, size_t size,
                                   std::vector<std::pair<size_t, size_t>>& elements)
{
    const auto* bytes = static_cast<const unsigned char*>(data);
    size_t at = 0;
    while (at < size) {
        if (size - at < 4)
            return false;
        const uint32_t length = uint32_t(bytes[at]) | uint32_t(bytes[at + 1]) << 8 |
                                uint32_t(bytes[at + 2]) << 16 | uint32_t(bytes[at + 3]) << 24;
        at += 4;
        if (length > size - at)
            return false;
        elements.emplace_back(at, length);
        at += length;
    }
    return true;
}

bool roxal::nn::isValidUtf8(const void* data, size_t size)
{
    const auto* s = static_cast<const unsigned char*>(data);
    size_t i = 0;
    while (i < size) {
        const unsigned char c = s[i];
        if (c < 0x80) {
            ++i;
            continue;
        }
        size_t length;
        uint32_t cp;
        if ((c & 0xE0) == 0xC0) { length = 2; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { length = 3; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { length = 4; cp = c & 0x07; }
        else return false;                       // a continuation byte, or 0xF8..0xFF
        if (size - i < length)
            return false;
        for (size_t k = 1; k < length; ++k) {
            if ((s[i + k] & 0xC0) != 0x80)
                return false;
            cp = (cp << 6) | (s[i + k] & 0x3F);
        }
        static constexpr uint32_t smallest[] = {0, 0, 0x80, 0x800, 0x10000};
        if (cp < smallest[length] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return false;                        // overlong, past Unicode, or a surrogate
        i += length;
    }
    return true;
}
