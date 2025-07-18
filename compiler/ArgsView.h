#pragma once
#include "Value.h"
#include "Object.h"
#include <span>
#include <string>

namespace roxal {

struct ArgsView {
    Value* data;
    size_t count;

    ArgsView(Value* d = nullptr, size_t c = 0) : data(d), count(c) {}

    Value& operator[](size_t i) const { return data[i]; }
    size_t size() const { return count; }
    bool empty() const { return count == 0; }
    bool has(size_t i) const { return i < count; }

    // Convenience accessors with optional defaults
    bool getBool(size_t i, bool def = false, bool strict = false) const {
        return has(i) ? data[i].asBool(strict) : def;
    }
    int32_t getInt(size_t i, int32_t def = 0, bool strict = false) const {
        return has(i) ? data[i].asInt(strict) : def;
    }
    double getReal(size_t i, double def = 0.0, bool strict = false) const {
        return has(i) ? data[i].asReal(strict) : def;
    }
    std::string getString(size_t i, const std::string& def = "") const {
        if (!has(i)) return def;
        if (!isString(data[i]))
            throw std::invalid_argument("argument is not a string");
        return toUTF8StdString(asString(data[i])->s);
    }
};

}
