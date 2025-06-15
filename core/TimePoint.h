#pragma once

#include "TimeDuration.h"

namespace core {


class TimePoint {
public:
    TimePoint() : t(0) {}
    TimePoint(const TimePoint& other) : t(other.t) {}
    ~TimePoint() {}

    static TimePoint zero() { return TimePoint(0); }
    static TimePoint microSecs(int64_t microSecs) { return TimePoint(microSecs); }

    TimePoint& operator=(const TimePoint& other) { t = other.t; return *this; }
    bool operator==(const TimePoint& other) const { return t == other.t; }
    bool operator!=(const TimePoint& other) const { return t != other.t; }

    TimePoint& operator+=(TimeDuration delta) { t += delta.microSecs(); return *this; }
    TimePoint& operator-=(TimeDuration delta) { t -= delta.microSecs(); return *this; }

    bool operator<(TimePoint other) const { return t < other.t; }
    bool operator>(TimePoint other) const { return t > other.t; }
    bool operator>=(TimePoint other) const { return t >= other.t; }
    bool operator<=(TimePoint other) const { return t <= other.t; }
    TimePoint operator+(TimeDuration delta) const {
        TimePoint result(*this);
        result += delta;
        return result;
    }
    TimePoint operator-(TimeDuration delta) const {
        TimePoint result(*this);
        #ifdef DEBUG_BUILD
        assert(result.t >= delta.microSecs());
        #endif
        result -= delta;
        return result;
    }
    TimeDuration operator-(TimePoint time) const {
        if (t > time.t)
            return TimeDuration::microSecs(t - time.t);
        else {
            #ifdef DEBUG_BUILD
            // since difference is negative, have to use signed arithmetic, hence
            //  TimePoints must be within signed range
            assert(t < std::numeric_limits<int64_t>::max());
            assert(time.t < std::numeric_limits<int64_t>::max());
            #endif
            return TimeDuration::microSecs(int64_t(t) - int64_t(time.t));
        }
    }

    TimeDuration operator%(TimeDuration divisor) const { return TimeDuration::microSecs(t % divisor.microSecs()); }

    uint64_t microSecs() const { return t; }
    double seconds() const { return double(t)/1000000.0; }

    std::string humanString() const { // TODO: move to .cpp
        if (t == 0) return "0";
        std::stringstream ss;
        const uint64_t day_us = 1000000ull*60*60*24;
        const uint64_t hour_us = 1000000ull*60*60;
        const uint64_t minute_us = 1000000ull*60;
        const uint64_t second_us = 1000000;
        const uint64_t milli_us = 1000;
        uint64_t rem = t;
        if (rem >= day_us) {
            ss << rem/day_us << "d";
            rem %= day_us;
        }
        if (rem >= hour_us) {
            ss << rem/hour_us << "h";
            rem %= hour_us;
        }
        if (rem >= minute_us) {
            ss << rem/minute_us << "m";
            rem %= minute_us;
        }
        if (rem >= second_us) {
            ss << rem/second_us << "s";
            rem %= second_us;
        }
        if (rem >= milli_us) {
            ss << rem/milli_us << "ms";
            rem %= milli_us;
        }
        if (rem > 0) {
            ss << rem << "us";
        }

        return ss.str();
    }

    friend std::ostream& operator<<(std::ostream& os, const TimePoint& timePoint) {
        os << timePoint.t << "us";
        return os;
    }
private:
    explicit TimePoint(uint64_t t) : t(t) {}
    uint64_t t; // microseconds (since arbitrary clock time)
};

inline TimePoint min(TimePoint a, TimePoint b) { return a.microSecs() < b.microSecs() ? a : b; }


} // namespace core
