#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "Value.h"

namespace roxal {

constexpr uint32_t kModuleCacheVersion = 1;

bool isModuleCacheUpToDate(const std::filesystem::path& sourcePath,
                           const std::filesystem::path& cachePath);

std::optional<Value> loadModuleCache(const std::filesystem::path& cachePath,
                                     const std::filesystem::path& sourcePath);

bool writeModuleCache(const std::filesystem::path& cachePath,
                      const std::filesystem::path& sourcePath,
                      const Value& function);

} // namespace roxal

