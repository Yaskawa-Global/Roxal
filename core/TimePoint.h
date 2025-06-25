#pragma once

#include <cstdint>
#include <string>
#include <sstream>
#include "TimeDuration.h"
#include <ostream>
#include <chrono>
#include <ctime>
#include <thread>
#include <iomanip>
#include <stdexcept>
#include <cstring>

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

    static TimePoint currentTime();
    void sleepUntil() const;

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

inline void sleepUntil(TimePoint futureTime) { futureTime.sleepUntil(); }

inline uint64_t microSecsSinceBoot()
{
    struct timespec tp;
    if (clock_gettime(CLOCK_MONOTONIC,&tp) != 0)
        throw std::runtime_error("Error querying system monotonic clock:"+std::string(strerror(errno)));
    return (uint64_t(tp.tv_sec)*1000000ull)+uint64_t(tp.tv_nsec/1000);
}

inline TimePoint TimePoint::currentTime()
{
    static uint64_t usOffset = microSecsSinceBoot();
    return TimePoint::microSecs(uint64_t(microSecsSinceBoot() - usOffset));
}

inline void TimePoint::sleepUntil() const
{
    auto now = TimePoint::currentTime();
    if (*this <= now)
        return;
    auto microSecsFromNow = microSecs() - now.microSecs();
    std::chrono::microseconds microSecsFromNowDuration(microSecsFromNow);
    auto current_time = std::chrono::steady_clock::now();
    auto future_time = current_time + microSecsFromNowDuration;
    std::this_thread::sleep_until(future_time);
}

inline std::string toHumanReadableTime(const std::chrono::steady_clock::time_point& tp)
{
    auto system_now = std::chrono::system_clock::now();
    auto steady_now = std::chrono::steady_clock::now();
    auto duration_since_steady_epoch = tp - steady_now;
    auto system_time = system_now + duration_since_steady_epoch;

    std::time_t time = std::chrono::system_clock::to_time_t(system_time);
    auto milliseconds_part = std::chrono::duration_cast<std::chrono::milliseconds>(system_time.time_since_epoch()) % 1000;

    std::tm tm = *std::localtime(&time);
    std::stringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    ss << '.' << std::setfill('0') << std::setw(3) << milliseconds_part.count();
    return ss.str();
}

} // namespace roxal
