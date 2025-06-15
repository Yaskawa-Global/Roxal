#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <sstream>
#include <ostream>

namespace roxal {

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
        std::ostringstream oss;
        oss << seconds() << "s";
        return oss.str();
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

