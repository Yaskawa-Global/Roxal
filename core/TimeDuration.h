#pragma once

namespace core {


class TimeDuration {
public:
    TimeDuration() : dt(0) {}
    TimeDuration(const TimeDuration& other) : dt(other.dt) {}
    ~TimeDuration() {}

    static TimeDuration zero() { return TimeDuration(0); }
    static TimeDuration max() { return TimeDuration(std::numeric_limits<int64_t>::max()/2); } // >100k years

    static TimeDuration microSecs(int64_t microSecsDelta) { return TimeDuration(microSecsDelta); }
    static TimeDuration milliSecs(int64_t milliSecsDelta) { return TimeDuration(milliSecsDelta * 1000); }
    static TimeDuration secs(int64_t secsDelta) { return TimeDuration(secsDelta * 1000000); }
    static TimeDuration mins(int64_t minsDelta) { return TimeDuration(minsDelta * 60000000); }
    static TimeDuration hours(int64_t hoursDelta) { return TimeDuration(hoursDelta * 3600000000); }
    static TimeDuration days(int64_t daysDelta) { return TimeDuration(daysDelta * 86400000000); }


    TimeDuration& operator=(const TimeDuration& other) { dt = other.dt; return *this; }
    bool operator==(const TimeDuration& other) const { return dt == other.dt; }
    bool operator!=(const TimeDuration& other) const { return dt != other.dt; }

    bool operator<(TimeDuration other) const { return dt < other.dt; }
    bool operator>(TimeDuration other) const { return dt > other.dt; }

    TimeDuration& operator+=(TimeDuration delta) { dt += delta.dt; return *this; }
    TimeDuration& operator-=(TimeDuration delta) { dt -= delta.dt; return *this; }
    TimeDuration& operator*=(int scale) { dt = int64_t(scale * dt); return *this; }
    TimeDuration& operator*=(uint64_t scale) { dt = scale * dt; return *this; }

    TimeDuration operator*(int scale) const {return TimeDuration(dt * scale); }
    TimeDuration operator*(int64_t scale) const {return TimeDuration(dt * scale); }
    TimeDuration operator*(uint64_t scale) const {return TimeDuration(dt * scale); }

    TimeDuration operator%(TimeDuration divisor) const { return TimeDuration(dt % divisor.dt); }
    int64_t operator/(TimeDuration divisor) const { return dt / divisor.dt; }

    int64_t microSecs() const { return dt; }

    // frequency corresponding to a period of this duration
    double frequency() const { return 1.0e+6 / dt; }

    std::string humanString() const { // TODO: move to .cpp
        if (dt == 0) return "0";
        std::stringstream ss;
        const uint64_t day_us = 1000000ull*60*60*24;
        const uint64_t hour_us = 1000000ull*60*60;
        const uint64_t minute_us = 1000000ull*60;
        const uint64_t second_us = 1000000;
        const uint64_t milli_us = 1000;
        uint64_t rem = dt;
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

    friend std::ostream& operator<<(std::ostream& os, const TimeDuration& duration) {
        os << duration.dt << "us";
        return os;
    }
private:
    TimeDuration(int64_t microSecsDelta) : dt(microSecsDelta) {}

    int64_t dt; // microseconds
};


inline TimeDuration min(TimeDuration a, TimeDuration b) { return a.microSecs() < b.microSecs() ? a : b; }



} // namespace core
