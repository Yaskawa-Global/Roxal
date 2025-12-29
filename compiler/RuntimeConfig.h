#pragma once

#include <string>
#include <unordered_map>
#include <optional>
#include <mutex>

namespace roxal {

/**
 * Global runtime configuration accessible by all modules.
 *
 * Keys use dot-notation namespacing (e.g., "ui.offscreen", "gc.verbose").
 * Values are strings that can be interpreted as bools, ints, etc.
 *
 * Thread-safe for concurrent access.
 */
class RuntimeConfig {
public:
    // Set a configuration value
    static void set(const std::string& key, const std::string& value);

    // Get a configuration value (returns nullopt if not set)
    static std::optional<std::string> get(const std::string& key);

    // Get as bool (interprets "true", "1", "yes" as true; default if not set)
    static bool getBool(const std::string& key, bool defaultVal = false);

    // Get as int (default if not set or not parseable)
    static int getInt(const std::string& key, int defaultVal = 0);

    // Check if a key is set
    static bool has(const std::string& key);

    // Clear all configuration
    static void clear();

private:
    static std::unordered_map<std::string, std::string> config_;
    static std::mutex mutex_;
};

} // namespace roxal
