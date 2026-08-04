#include <core/ustring.h>

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

using roxal::code_point;
using roxal::ustring;

namespace {

void check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "ustring test failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

template <typename Operation>
void checkUnsupported(Operation operation, const char* message)
{
    try {
        operation();
    } catch (const std::runtime_error&) {
        return;
    }
    check(false, message);
}

} // namespace

int main()
{
    const std::string robot = "\xf0\x9f\xa4\x96";
    const ustring text = ustring::fromUTF8("A" + robot + "Z");
    check(text.toUTF8String() == "A" + robot + "Z", "UTF-8 round trip");
    check(text.length() == 4, "UTF-16 code-unit length");
    check(text.char32At(1) == static_cast<code_point>(0x1f916), "astral code point");

    ustring edited = text;
    edited.setCharAt(0, u'a');
    check(edited.toUTF8String() == "a" + robot + "Z", "UTF-16 code-unit mutation");

    ustring source("operator-");
    const ustring suffix = source.tempSubString(8);
    source = ustring("uoperator-");
    check(suffix == "-", "substring is independent of its source");
    check(ustring("abcdef").tempSubStringBetween(2, 5) == "cde", "substring bounds");

    const ustring escaped = ustring::fromUTF8("\\u03BC\\x{1F916}\\n").unescape();
    check(escaped.toUTF8String() == "\xce\xbc" + robot + "\n", "Unicode escape decoding");

    check(ustring("abc").hashCode() == 136518, "stable UTF-16 hash");
    check(ustring::fromUTF8(robot).compareCodePointOrder(ustring("z")) > 0,
          "code-point ordering");
    check(ustring("prefix-value").startsWith("prefix"), "prefix matching");

#ifdef ROXAL_UNICODE_BACKEND_ICU
    ustring upper = ustring::fromUTF8("stra\xc3\x9f" "e");
    upper.toUpper();
    check(upper.toUTF8String() == "STRASSE", "ICU Unicode upper case");
#else
    checkUnsupported([] { ustring("a").toUpper(); }, "builtin upper case is unsupported");
    checkUnsupported([] { ustring("A").toLower(); }, "builtin lower case is unsupported");
    checkUnsupported([] { ustring("hello").toTitle(); }, "builtin title case is unsupported");
#endif

    return EXIT_SUCCESS;
}
