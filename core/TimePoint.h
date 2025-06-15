#pragma once

#include <cstdint>
#include <string>
#include <sstream>
#include "TimeDuration.h"
#include <ostream>

namespace roxal {

class TimePoint {
    int64_t m_microSecs;
public:
    constexpr TimePoint() : m_microSecs(0) {}
    explicit constexpr TimePoint(int64_t us) : m_microSecs(us) {}

    static constexpr TimePoint microSecs(int64_t us) { return TimePoint(us); }
    static constexpr TimePoint milliSecs(int64_t ms) { return TimePoint(ms*1000); }
    static constexpr TimePoint secs(double s) { return TimePoint(static_cast<int64_t>(s*1000000.0)); }

    static constexpr TimePoint zero() { return TimePoint(0); }

    constexpr int64_t microSecs() const { return m_microSecs; }
    constexpr double seconds() const { return static_cast<double>(m_microSecs)/1000000.0; }

    inline std::string humanString() const {
        std::ostringstream oss;
        oss << seconds() << "s";
        return oss.str();
    }
};

inline constexpr bool operator==(TimePoint a, TimePoint b) { return a.microSecs()==b.microSecs(); }
inline constexpr bool operator!=(TimePoint a, TimePoint b) { return a.microSecs()!=b.microSecs(); }
inline constexpr bool operator<(TimePoint a, TimePoint b) { return a.microSecs()<b.microSecs(); }
inline constexpr bool operator>(TimePoint a, TimePoint b) { return a.microSecs()>b.microSecs(); }
inline constexpr bool operator<=(TimePoint a, TimePoint b) { return a.microSecs()<=b.microSecs(); }
inline constexpr bool operator>=(TimePoint a, TimePoint b) { return a.microSecs()>=b.microSecs(); }

inline constexpr TimePoint operator+(TimePoint p, TimeDuration d) { return TimePoint::microSecs(p.microSecs()+d.microSecs()); }
inline constexpr TimePoint operator-(TimePoint p, TimeDuration d) { return TimePoint::microSecs(p.microSecs()-d.microSecs()); }
inline constexpr TimeDuration operator-(TimePoint a, TimePoint b) { return TimeDuration::microSecs(a.microSecs()-b.microSecs()); }
inline constexpr TimeDuration operator%(TimePoint t, TimeDuration d) { return TimeDuration::microSecs(t.microSecs()%d.microSecs()); }

inline std::ostream& operator<<(std::ostream& os, const TimePoint& t) {
    os << t.humanString();
    return os;
}

} // namespace roxal
