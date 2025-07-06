#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <sstream>
#include <ostream>
#include <algorithm>

namespace roxal {

inline std::string humanDurationString(int64_t microSecs)
{
    constexpr int64_t kMicroSecond = 1;
    constexpr int64_t kMilliSecond = 1000 * kMicroSecond;
    constexpr int64_t kSecond = 1000 * kMilliSecond;

    if (microSecs == 0)
        return "0s";

    bool negative = microSecs < 0;
    uint64_t u = negative ? static_cast<uint64_t>(-microSecs) : static_cast<uint64_t>(microSecs);

    auto fmtInt = [](uint64_t v) -> std::string {
        if (v == 0)
            return "0";
        std::string s;
        while (v > 0) {
            s.push_back('0' + (v % 10));
            v /= 10;
        }
        std::reverse(s.begin(), s.end());
        return s;
    };

    auto fmtFrac = [](uint64_t &v, int prec) -> std::string {
        std::string digits;
        bool print = false;
        for (int i = 0; i < prec; ++i) {
            int digit = v % 10;
            print = print || digit != 0;
            if (print)
                digits.push_back('0' + digit);
            v /= 10;
        }
        if (!print)
            return "";
        std::reverse(digits.begin(), digits.end());
        return std::string(".") + digits;
    };

    std::string out;
    if (negative)
        out.push_back('-');

    if (u < static_cast<uint64_t>(kSecond)) {
        if (u < static_cast<uint64_t>(kMilliSecond)) {
            uint64_t val = u;
            std::string frac = fmtFrac(val, 0);
            out += fmtInt(val) + frac + "us";
        } else {
            uint64_t val = u;
            std::string frac = fmtFrac(val, 3);
            out += fmtInt(val) + frac + "ms";
        }
    } else {
        uint64_t val = u;
        std::string frac = fmtFrac(val, 6);
        uint64_t sec = val % 60;
        std::string result = fmtInt(sec) + frac + "s";
        val /= 60;
        if (val > 0) {
            uint64_t min = val % 60;
            result = fmtInt(min) + "m" + result;
            val /= 60;
            if (val > 0)
                result = fmtInt(val) + "h" + result;
        }
        out += result;
    }

    return out;
}

class TimeDuration {
    int64_t m_microSecs;
public:
    constexpr TimeDuration() : m_microSecs(0) {}
    explicit constexpr TimeDuration(int64_t us) : m_microSecs(us) {}

    static constexpr TimeDuration microSecs(int64_t us) { return TimeDuration(us); }
    static constexpr TimeDuration milliSecs(int64_t ms) { return TimeDuration(ms*1000); }
    static constexpr TimeDuration secs(double s) { return TimeDuration(static_cast<int64_t>(s*1000000.0)); }

    static constexpr TimeDuration zero() { return TimeDuration(0); }
    static constexpr TimeDuration max() { return TimeDuration(std::numeric_limits<int64_t>::max()); }

    constexpr int64_t microSecs() const { return m_microSecs; }
    constexpr double seconds() const { return static_cast<double>(m_microSecs)/1000000.0; }
    inline double frequency() const { return m_microSecs ? 1000000.0/static_cast<double>(m_microSecs) : 0.0; }

    inline std::string humanString() const {
        return humanDurationString(m_microSecs);
    }
};

inline constexpr TimeDuration operator+(TimeDuration a, TimeDuration b) { return TimeDuration::microSecs(a.microSecs()+b.microSecs()); }
inline constexpr TimeDuration operator-(TimeDuration a, TimeDuration b) { return TimeDuration::microSecs(a.microSecs()-b.microSecs()); }
inline constexpr TimeDuration operator*(TimeDuration a, int64_t m) { return TimeDuration::microSecs(a.microSecs()*m); }
inline constexpr TimeDuration operator*(int64_t m, TimeDuration a) { return TimeDuration::microSecs(a.microSecs()*m); }
inline constexpr TimeDuration operator/(TimeDuration a, int64_t d) { return TimeDuration::microSecs(a.microSecs()/d); }
inline constexpr TimeDuration operator%(TimeDuration a, TimeDuration b) { return TimeDuration::microSecs(a.microSecs()%b.microSecs()); }

inline constexpr bool operator==(TimeDuration a, TimeDuration b) { return a.microSecs()==b.microSecs(); }
inline constexpr bool operator!=(TimeDuration a, TimeDuration b) { return a.microSecs()!=b.microSecs(); }
inline constexpr bool operator<(TimeDuration a, TimeDuration b) { return a.microSecs()<b.microSecs(); }
inline constexpr bool operator>(TimeDuration a, TimeDuration b) { return a.microSecs()>b.microSecs(); }
inline constexpr bool operator<=(TimeDuration a, TimeDuration b) { return a.microSecs()<=b.microSecs(); }
inline constexpr bool operator>=(TimeDuration a, TimeDuration b) { return a.microSecs()>=b.microSecs(); }

inline std::ostream& operator<<(std::ostream& os, const TimeDuration& d) {
    os << d.humanString();
    return os;
}

} // namespace roxal
