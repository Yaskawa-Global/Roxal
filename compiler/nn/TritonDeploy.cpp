#include "TritonDeploy.h"

#include "triton_inference.pb.h"
#include "core/fnv1a.h"

#include <grpcpp/support/slice.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace fs = std::filesystem;
using namespace roxal::nn;

namespace {

void fnv(std::uint64_t& hash, const void* data, std::size_t size)
{
    hash = roxal::fnv1a64(data, size, hash);
}

void fnvU64(std::uint64_t& hash, std::uint64_t value)
{
    unsigned char bytes[8];
    for (int i = 0; i < 8; ++i)
        bytes[i] = static_cast<unsigned char>(value >> (8 * i));
    fnv(hash, bytes, sizeof bytes);
}

// A file mapped read-only; empty for an empty file.  The mapping lives as long
// as any copy of the returned pointer.
std::shared_ptr<const void> mapFile(const fs::path& path, std::uint64_t& size)
{
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        throw std::runtime_error("cannot open '" + path.string() + "': " + std::strerror(errno));
    struct ::stat st;
    if (::fstat(fd, &st) != 0) {
        const int err = errno;
        ::close(fd);
        throw std::runtime_error("cannot read '" + path.string() + "': " + std::strerror(err));
    }
    size = static_cast<std::uint64_t>(st.st_size);
    if (size == 0) {
        ::close(fd);
        return nullptr;
    }
    void* data = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    const int err = errno;
    ::close(fd);
    if (data == MAP_FAILED)
        throw std::runtime_error("cannot map '" + path.string() + "': " + std::strerror(err));
    return std::shared_ptr<const void>(data, [size](const void* p) {
        ::munmap(const_cast<void*>(p), size);
    });
}

// ---- protobuf wire format, without a schema --------------------------------

struct Wire {
    const unsigned char* p;
    const unsigned char* end;

    bool varint(std::uint64_t& value) {
        value = 0;
        for (int shift = 0; shift < 64 && p < end; shift += 7) {
            const unsigned char byte = *p++;
            value |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
            if (!(byte & 0x80))
                return true;
        }
        return false;
    }

    // The next field: its number, and for a length-delimited one its bytes.
    // False at the end; throws on a malformed message.
    bool next(std::uint32_t& number, const unsigned char*& data, std::size_t& length, bool& delimited) {
        if (p >= end)
            return false;
        std::uint64_t tag = 0;
        if (!varint(tag))
            throw std::invalid_argument("malformed");
        number = static_cast<std::uint32_t>(tag >> 3);
        delimited = false;
        std::uint64_t value = 0;
        switch (tag & 7) {
            case 0:
                if (!varint(value))
                    throw std::invalid_argument("malformed");
                return true;
            case 1:
                if (end - p < 8)
                    throw std::invalid_argument("malformed");
                p += 8;
                return true;
            case 5:
                if (end - p < 4)
                    throw std::invalid_argument("malformed");
                p += 4;
                return true;
            case 2:
                if (!varint(value) || value > static_cast<std::uint64_t>(end - p))
                    throw std::invalid_argument("malformed");
                data = p;
                length = static_cast<std::size_t>(value);
                p += length;
                delimited = true;
                return true;
            default:
                throw std::invalid_argument("malformed");
        }
    }
};

// onnx.proto's messages, as far as tensors with external data go.
struct ExternalDataScan {
    std::vector<std::string> locations;
    int depth = 0;

    template <typename Fn>
    void fields(const unsigned char* data, std::size_t length, Fn&& onField) {
        if (++depth > 64)
            throw std::invalid_argument("nested too deeply");
        Wire wire{data, data + length};
        std::uint32_t number = 0;
        const unsigned char* field = nullptr;
        std::size_t size = 0;
        bool delimited = false;
        while (wire.next(number, field, size, delimited))
            if (delimited)
                onField(number, field, size);
        --depth;
    }

    void model(const unsigned char* d, std::size_t n) {           // ModelProto
        fields(d, n, [&](auto f, auto* fd, auto fn) { if (f == 7) graph(fd, fn); });
    }
    void graph(const unsigned char* d, std::size_t n) {           // GraphProto
        fields(d, n, [&](auto f, auto* fd, auto fn) {
            if (f == 1) node(fd, fn);
            else if (f == 5) tensor(fd, fn);
            else if (f == 15) sparse(fd, fn);
        });
    }
    void node(const unsigned char* d, std::size_t n) {            // NodeProto
        fields(d, n, [&](auto f, auto* fd, auto fn) { if (f == 5) attribute(fd, fn); });
    }
    void attribute(const unsigned char* d, std::size_t n) {       // AttributeProto
        fields(d, n, [&](auto f, auto* fd, auto fn) {
            if (f == 5 || f == 10) tensor(fd, fn);
            else if (f == 6 || f == 11) graph(fd, fn);
            else if (f == 22 || f == 23) sparse(fd, fn);
        });
    }
    void sparse(const unsigned char* d, std::size_t n) {          // SparseTensorProto
        fields(d, n, [&](auto f, auto* fd, auto fn) { if (f == 1 || f == 2) tensor(fd, fn); });
    }
    void tensor(const unsigned char* d, std::size_t n) {          // TensorProto
        fields(d, n, [&](auto f, auto* fd, auto fn) { if (f == 13) entry(fd, fn); });
    }
    void entry(const unsigned char* d, std::size_t n) {           // StringStringEntryProto
        std::string key, value;
        fields(d, n, [&](auto f, auto* fd, auto fn) {
            if (f == 1) key.assign(reinterpret_cast<const char*>(fd), fn);
            else if (f == 2) value.assign(reinterpret_cast<const char*>(fd), fn);
        });
        if (key == "location" && std::find(locations.begin(), locations.end(), value) == locations.end())
            locations.push_back(value);
    }
};

std::string lowercase(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string sanitizeStem(const std::string& stem)
{
    std::string out;
    for (unsigned char c : stem) {
        if (out.size() >= 48)
            break;
        out.push_back(std::isalnum(c) || c == '_' || c == '-' ? static_cast<char>(c) : '_');
    }
    return out.empty() ? "model" : out;
}

void addFile(ModelFileSet& set, const fs::path& source, const std::string& name)
{
    for (const auto& f : set.files)
        if (f.name == name)
            throw std::invalid_argument("'" + source.string() + "' and '" + f.source.string() +
                                        "' would both be the model's '" + name + "'");
    // Canonical: a hard link to a symlink would link the symlink itself.
    const fs::path real = fs::canonical(source);
    set.files.push_back({real, name, fs::file_size(real)});
}

std::uint64_t contentHash(const fs::path& path)
{
    struct ::stat st;
    if (::stat(path.c_str(), &st) != 0)
        throw std::runtime_error("cannot read '" + path.string() + "': " + std::strerror(errno));
    const std::string key = path.string() + '\0' + std::to_string(st.st_size) + '\0' +
                            std::to_string(st.st_mtim.tv_sec) + '.' + std::to_string(st.st_mtim.tv_nsec) +
                            '\0' + std::to_string(st.st_ino);
    static std::mutex mutex;
    static std::unordered_map<std::string, std::uint64_t> memo;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = memo.find(key);
        if (it != memo.end())
            return it->second;
    }
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        throw std::runtime_error("cannot open '" + path.string() + "': " + std::strerror(errno));
    std::uint64_t hash = roxal::fnv1a64Basis;
    std::vector<unsigned char> buffer(1 << 20);
    for (;;) {
        const ssize_t n = ::read(fd, buffer.data(), buffer.size());
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            const int err = errno;
            ::close(fd);
            throw std::runtime_error("cannot read '" + path.string() + "': " + std::strerror(err));
        }
        if (n == 0)
            break;
        fnv(hash, buffer.data(), static_cast<std::size_t>(n));
    }
    ::close(fd);
    std::lock_guard<std::mutex> lock(mutex);
    memo[key] = hash;
    return hash;
}

bool holdsSymlink(const fs::path& modelDir)
{
    std::error_code ec;
    for (fs::recursive_directory_iterator it(modelDir / "1", ec), end; !ec && it != end; it.increment(ec))
        if (it->is_symlink())
            return true;
    return false;
}

void appendVarint(std::string& out, std::uint64_t value)
{
    while (value >= 0x80) {
        out.push_back(static_cast<char>(value | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<char>(value));
}

std::uint64_t varintSize(std::uint64_t value)
{
    std::uint64_t size = 1;
    while (value >= 0x80) {
        value >>= 7;
        ++size;
    }
    return size;
}

void releaseMapping(void* mapping)
{
    delete static_cast<std::shared_ptr<const void>*>(mapping);
}

} // namespace

std::vector<std::string> roxal::nn::onnxExternalData(const fs::path& onnxFile)
{
    std::uint64_t size = 0;
    const auto mapping = mapFile(onnxFile, size);
    ExternalDataScan scan;
    try {
        if (mapping)
            scan.model(static_cast<const unsigned char*>(mapping.get()), static_cast<std::size_t>(size));
    } catch (const std::invalid_argument&) {
        throw std::invalid_argument("'" + onnxFile.string() + "' is not an ONNX model");
    }
    for (const auto& location : scan.locations) {
        const fs::path rel(location);
        bool escapes = rel.empty() || rel.is_absolute();
        for (const auto& part : rel)
            escapes = escapes || part == "..";
        if (escapes)
            throw std::invalid_argument("'" + onnxFile.string() + "' names external data '" + location +
                                        "' outside its own directory");
    }
    return scan.locations;
}

ModelFileSet roxal::nn::modelFiles(const fs::path& path)
{
    const fs::path given = fs::absolute(path);
    if (!fs::exists(given))
        throw std::invalid_argument("there is no model at '" + path.string() + "'");
    ModelFileSet set;

    if (fs::is_directory(given)) {
        const fs::path dir = fs::canonical(given);
        set.stem = sanitizeStem(dir.filename().string());
        for (auto it = fs::recursive_directory_iterator(dir); it != fs::recursive_directory_iterator(); ++it) {
            const std::string leaf = it->path().filename().string();
            if (!leaf.empty() && leaf[0] == '.') {
                if (it->is_directory())
                    it.disable_recursion_pending();
                continue;
            }
            if (it->is_regular_file())
                addFile(set, it->path(), it->path().lexically_relative(dir).generic_string());
        }
        if (set.files.empty())
            throw std::invalid_argument("the directory '" + path.string() + "' holds no model files");
    } else {
        const std::string ext = lowercase(given.extension().string());
        set.stem = sanitizeStem(given.stem().string());
        if (ext == ".onnx") {
            addFile(set, given, "model.onnx");
            for (const auto& location : onnxExternalData(given)) {
                const fs::path data = given.parent_path() / location;
                if (!fs::is_regular_file(data))
                    throw std::invalid_argument("'" + path.string() + "' keeps weights in '" + location +
                                                "', which is not there");
                addFile(set, data, fs::path(location).generic_string());
            }
        } else if (ext == ".plan" || ext == ".engine") {
            addFile(set, given, "model.plan");
        } else if (ext == ".pt") {
            addFile(set, given, "model.pt");
        } else if (ext == ".xml") {
            const fs::path weights = fs::path(given).replace_extension(".bin");
            if (!fs::is_regular_file(weights))
                throw std::invalid_argument("the OpenVINO model '" + path.string() + "' has no '" +
                                            weights.filename().string() + "' beside it");
            addFile(set, given, "model.xml");
            addFile(set, weights, "model.bin");
        } else {
            throw std::invalid_argument("which Triton backend serves a '" + given.extension().string() +
                                        "' file is not known ('" + path.string() + "'): deploy the "
                                        "model's directory instead, its files named as that backend "
                                        "expects");
        }
    }
    std::sort(set.files.begin(), set.files.end(),
              [](const ModelFile& a, const ModelFile& b) { return a.name < b.name; });
    for (const auto& f : set.files)
        set.totalBytes += f.size;
    return set;
}

std::string roxal::nn::deploymentName(const ModelFileSet& files, const std::string& configJson)
{
    std::uint64_t hash = roxal::fnv1a64Basis;
    for (const auto& f : files.files) {
        fnv(hash, f.name.data(), f.name.size() + 1);   // with its terminating NUL
        fnvU64(hash, f.size);
        fnvU64(hash, contentHash(f.source));
    }
    fnv(hash, "config", 7);
    fnv(hash, configJson.data(), configJson.size());
    static const char digits[] = "0123456789abcdef";
    std::string hex(16, '0');
    for (int i = 15; i >= 0; --i, hash >>= 4)
        hex[static_cast<std::size_t>(i)] = digits[hash & 0xf];
    return "roxal." + files.stem + "." + hex;
}

std::string roxal::nn::normalizeAddress(std::string address)
{
    const auto bracket = address.rfind(']');
    const auto colon = address.rfind(':');
    if (colon == std::string::npos || (bracket != std::string::npos && colon < bracket))
        address += ":8001";
    return address;
}

fs::path roxal::nn::repositoryDirectory(std::string directory)
{
    if (directory == "~" || directory.rfind("~/", 0) == 0) {
        const char* home = std::getenv("HOME");
        if (!home)
            throw std::invalid_argument("HOME is not set, so '~' means nothing");
        directory = std::string(home) + directory.substr(1);
    }
    if (directory.empty())
        throw std::invalid_argument("no directory given");
    return fs::absolute(directory).lexically_normal();
}

std::map<std::string, fs::path> roxal::nn::parseRepositoryList(const std::string& list)
{
    auto trim = [](std::string s) {
        const auto first = s.find_first_not_of(" \t");
        const auto last = s.find_last_not_of(" \t");
        return first == std::string::npos ? std::string() : s.substr(first, last - first + 1);
    };
    std::map<std::string, fs::path> out;
    std::size_t at = 0;
    while (at <= list.size()) {
        const auto semi = list.find(';', at);
        const std::string entry = trim(list.substr(at, semi == std::string::npos ? std::string::npos : semi - at));
        at = semi == std::string::npos ? list.size() + 1 : semi + 1;
        if (entry.empty())
            continue;
        const auto eq = entry.find('=');
        std::string server = eq == std::string::npos ? std::string() : trim(entry.substr(0, eq));
        std::string dir = eq == std::string::npos ? std::string() : trim(entry.substr(eq + 1));
        if (server.rfind("triton://", 0) == 0)
            server = server.substr(9);
        if (server.empty() || dir.empty() || server.find('/') != std::string::npos)
            throw std::invalid_argument("'" + entry + "' is not server=directory");
        try {
            out[normalizeAddress(server)] = repositoryDirectory(dir);
        } catch (const std::invalid_argument& e) {
            throw std::invalid_argument("'" + entry + "': " + e.what());
        }
    }
    return out;
}

bool roxal::nn::placeInRepository(const ModelFileSet& files, const fs::path& repository,
                                  const std::string& name, bool allowSymlinks)
{
    const fs::path deployed = repository / name;
    if (fs::exists(deployed))
        return holdsSymlink(deployed);

    std::random_device random;
    const fs::path staging = repository / (".roxal-staging." + name + "." + std::to_string(::getpid()) +
                                           "." + std::to_string(random()));
    try {
        bool anySymlink = false;
        for (const auto& f : files.files) {
            const fs::path dest = staging / "1" / fs::path(f.name);
            fs::create_directories(dest.parent_path());
            std::error_code ec;
            fs::create_hard_link(f.source, dest, ec);
            if (!ec)
                continue;
            if (allowSymlinks) {
                ec.clear();
                fs::create_symlink(f.source, dest, ec);
                if (!ec) {
                    anySymlink = true;
                    continue;
                }
            }
            fs::copy_file(f.source, dest);
        }
        std::error_code ec;
        fs::rename(staging, deployed, ec);
        if (ec) {
            fs::remove_all(staging);
            if (fs::exists(deployed))
                return holdsSymlink(deployed);   // another program got there first
            throw fs::filesystem_error("cannot place the model in the repository", deployed, ec);
        }
        return anySymlink;
    } catch (...) {
        std::error_code ignored;
        fs::remove_all(staging, ignored);
        throw;
    }
}

void roxal::nn::copyOverSymlinks(const fs::path& repository, const std::string& name)
{
    std::vector<fs::path> links;
    for (const auto& entry : fs::recursive_directory_iterator(repository / name / "1"))
        if (entry.is_symlink())
            links.push_back(entry.path());
    for (const auto& link : links) {
        const fs::path copy = link.string() + ".roxal-copy";
        fs::copy_file(fs::read_symlink(link), copy, fs::copy_options::overwrite_existing);
        fs::rename(copy, link);
    }
}

grpc::ByteBuffer roxal::nn::encodeLoadRequest(const std::string& name, const std::string& configJson,
                                              const ModelFileSet* upload)
{
    roxal::triton::RepositoryModelLoadRequest header;
    header.set_model_name(name);
    // Triton reads uploaded files only when a configuration comes with them
    // (otherwise it looks for the model on disk); "{}" lets it fill in the
    // whole configuration from the model, as for a repository model.
    if (!configJson.empty() || upload)
        (*header.mutable_parameters())["config"].set_string_param(configJson.empty() ? "{}" : configJson);

    // The files go after the header as further entries of the `parameters`
    // map (field 3: an entry is {key = 1, value = 2}, and a value's bytes
    // are ModelRepositoryParameter field 4), each file's bytes a slice over
    // its mapping.
    std::string pending = header.SerializeAsString();
    std::vector<grpc::Slice> slices;
    if (upload) {
        slices.reserve(2 * upload->files.size() + 1);
        for (const auto& f : upload->files) {
            std::uint64_t size = 0;
            auto mapping = mapFile(f.source, size);
            if (size != f.size)
                throw std::runtime_error("'" + f.source.string() + "' changed while it was being deployed");
            const std::string key = "file:1/" + f.name;
            const std::uint64_t paramLength = 1 + varintSize(size) + size;
            const std::uint64_t entryLength = 1 + varintSize(key.size()) + key.size() +
                                              1 + varintSize(paramLength) + paramLength;
            pending.push_back(static_cast<char>((3 << 3) | 2));
            appendVarint(pending, entryLength);
            pending.push_back(static_cast<char>((1 << 3) | 2));
            appendVarint(pending, key.size());
            pending += key;
            pending.push_back(static_cast<char>((2 << 3) | 2));
            appendVarint(pending, paramLength);
            pending.push_back(static_cast<char>((4 << 3) | 2));
            appendVarint(pending, size);
            if (size == 0)
                continue;
            slices.emplace_back(pending);
            pending.clear();
            auto* keep = new std::shared_ptr<const void>(std::move(mapping));
            slices.emplace_back(const_cast<void*>(keep->get()), static_cast<std::size_t>(size),
                                &releaseMapping, keep);
        }
    }
    if (!pending.empty())
        slices.emplace_back(pending);
    return grpc::ByteBuffer(slices.data(), slices.size());
}
