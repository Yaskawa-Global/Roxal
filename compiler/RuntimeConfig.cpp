#include "RuntimeConfig.h"
#include <algorithm>
#include <cctype>

namespace roxal {

std::unordered_map<std::string, std::string> RuntimeConfig::config_;
std::mutex RuntimeConfig::mutex_;

void RuntimeConfig::set(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_[key] = value;
}

std::optional<std::string> RuntimeConfig::get(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = config_.find(key);
    if (it != config_.end()) {
        return it->second;
    }
    return std::nullopt;
}

bool RuntimeConfig::getBool(const std::string& key, bool defaultVal) {
    auto val = get(key);
    if (!val.has_value()) {
        return defaultVal;
    }

    std::string s = val.value();
    // Convert to lowercase for comparison
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    return (s == "true" || s == "1" || s == "yes" || s == "on");
}

int RuntimeConfig::getInt(const std::string& key, int defaultVal) {
    auto val = get(key);
    if (!val.has_value()) {
        return defaultVal;
    }

    try {
        return std::stoi(val.value());
    } catch (...) {
        return defaultVal;
    }
}

bool RuntimeConfig::has(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_.find(key) != config_.end();
}

void RuntimeConfig::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    config_.clear();
}

} // namespace roxal
