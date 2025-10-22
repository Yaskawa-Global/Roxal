#include "ModuleCache.h"

#include <array>
#include <cstring>
#include <fstream>
#include <system_error>

#include "Object.h"

namespace roxal {

namespace {

constexpr std::array<char, 4> kModuleCacheMagic{ {'R', 'O', 'X', 'C'} };

std::string canonicalPathString(const std::filesystem::path& path) {
    std::error_code ec;
    auto canonicalPath = std::filesystem::canonical(path, ec);
    if (ec) {
        return {};
    }
    auto generic = canonicalPath.generic_u8string();
    return std::string(generic.begin(), generic.end());
}

bool validateHeader(std::istream& in,
                    const std::filesystem::path& sourcePath) {
    char magic[4];
    if (!in.read(magic, sizeof(magic))) {
        return false;
    }
    if (std::memcmp(magic, kModuleCacheMagic.data(), kModuleCacheMagic.size()) != 0) {
        return false;
    }

    uint32_t version = 0;
    if (!in.read(reinterpret_cast<char*>(&version), sizeof(version))) {
        return false;
    }
    if (version != kModuleCacheVersion) {
        return false;
    }

    uint32_t pathSize = 0;
    if (!in.read(reinterpret_cast<char*>(&pathSize), sizeof(pathSize))) {
        return false;
    }

    std::string storedPath(pathSize, '\0');
    if (pathSize > 0 && !in.read(storedPath.data(), storedPath.size())) {
        return false;
    }

    const std::string canonicalSource = canonicalPathString(sourcePath);
    if (canonicalSource.empty() || storedPath != canonicalSource) {
        return false;
    }

    return true;
}

void writeHeader(std::ostream& out,
                 const std::string& canonicalSource) {
    out.write(kModuleCacheMagic.data(), kModuleCacheMagic.size());
    uint32_t version = kModuleCacheVersion;
    out.write(reinterpret_cast<const char*>(&version), sizeof(version));
    uint32_t pathSize = static_cast<uint32_t>(canonicalSource.size());
    out.write(reinterpret_cast<const char*>(&pathSize), sizeof(pathSize));
    if (pathSize > 0) {
        out.write(canonicalSource.data(), canonicalSource.size());
    }
}

} // namespace

bool isModuleCacheUpToDate(const std::filesystem::path& sourcePath,
                           const std::filesystem::path& cachePath) {
    std::error_code ec;
    if (!std::filesystem::exists(cachePath, ec)) {
        return false;
    }

    auto cacheTime = std::filesystem::last_write_time(cachePath, ec);
    if (ec) {
        return false;
    }

    auto sourceTime = std::filesystem::last_write_time(sourcePath, ec);
    if (ec) {
        return false;
    }

    return cacheTime >= sourceTime;
}

std::optional<Value> loadModuleCache(const std::filesystem::path& cachePath,
                                     const std::filesystem::path& sourcePath) {
    std::ifstream in(cachePath, std::ios::binary);
    if (!in.is_open()) {
        return std::nullopt;
    }

    if (!validateHeader(in, sourcePath)) {
        return std::nullopt;
    }

    ptr<SerializationContext> ctx = make_ptr<SerializationContext>();
    try {
        Value value = readValue(in, ctx);
        if (!isFunction(value)) {
            return std::nullopt;
        }
        return value;
    } catch (...) {
        return std::nullopt;
    }
}

bool writeModuleCache(const std::filesystem::path& cachePath,
                      const std::filesystem::path& sourcePath,
                      const Value& function) {
    if (function.isNil() || !isFunction(function)) {
        return false;
    }

    const std::string canonicalSource = canonicalPathString(sourcePath);
    if (canonicalSource.empty()) {
        return false;
    }

    std::error_code ec;
    auto parentDir = cachePath.parent_path();
    if (!parentDir.empty()) {
        std::filesystem::create_directories(parentDir, ec);
        if (ec) {
            return false;
        }
    }

    auto tempPath = cachePath;
    tempPath += ".tmp";

    std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }

    writeHeader(out, canonicalSource);

    ptr<SerializationContext> ctx = make_ptr<SerializationContext>();
    try {
        writeValue(out, function, ctx);
    } catch (...) {
        out.close();
        std::filesystem::remove(tempPath, ec);
        return false;
    }

    out.close();

    std::filesystem::rename(tempPath, cachePath, ec);
    if (ec) {
        std::filesystem::remove(tempPath, ec);
        return false;
    }

    return true;
}

} // namespace roxal

