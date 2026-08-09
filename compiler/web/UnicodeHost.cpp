#ifdef __EMSCRIPTEN__

#include "UnicodeHost.h"
#include "JsBridge.h"

#include "Object.h"
#include "Value.h"

#include <string>

using namespace roxal;
using namespace roxal::web;

namespace {

// The bridge speaks UTF-8 strings, ustring's builtin storage is UTF-16, and
// neither conversion belongs in ustring's public API just for this. Both
// routines are the plain textbook ones; lone surrogates are passed through as
// U+FFFD rather than rejected, because a case-mapping call is no place to
// start failing on malformed text the rest of the VM tolerates.
std::string toUtf8(const std::u16string& text)
{
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        char32_t cp = text[i];
        if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < text.size()) {
            const char16_t low = text[i + 1];
            if (low >= 0xdc00 && low <= 0xdfff) {
                cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                ++i;
            } else {
                cp = 0xfffd;
            }
        } else if (cp >= 0xd800 && cp <= 0xdfff) {
            cp = 0xfffd;
        }

        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else if (cp < 0x10000) {
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

// The reply arrives as a Roxal string, whose builtin storage is already
// UTF-16 -- copy its units out rather than round-tripping through UTF-8 again.
std::u16string fromRoxalString(const Value& v)
{
    ObjString* s = asStringObj(v);
    return std::u16string(s->s.getBuffer(), static_cast<size_t>(s->s.length()));
}

bool hostCaseMap(std::u16string& text, CaseMapping mode)
{
    // No bridge on this thread (or the host is gone): report failure so the
    // caller raises "unsupported" instead of silently returning the input.
    if (!canIssueOps()) return false;
    if (text.empty()) return true;

    try {
        Encoder e;
        e.op(Op::UnicodeCase);
        e.u8(static_cast<uint8_t>(mode));
        e.str(toUtf8(text));
        const Value result = exec(e);
        if (!isString(result)) return false;
        text = fromRoxalString(result);
        return true;
    } catch (const std::exception&) {
        // A bridge failure must not escape as an unrelated exception type from
        // deep inside a string operation.
        return false;
    }
}

} // namespace

void roxal::web::installUnicodeHost()
{
    caseMappingHook = &hostCaseMap;
}

#endif // __EMSCRIPTEN__
