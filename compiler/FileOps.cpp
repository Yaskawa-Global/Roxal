#include "FileOps.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <filesystem>
#include <random>
#include <system_error>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace roxal::fileops {

namespace {

std::string describe(const std::string& code, const std::string& op,
                     const std::string& path, const std::string& path2,
                     const std::string& reason)
{
    std::string s = code + ": " + op + " '" + path + "'";
    if (!path2.empty())
        s += " -> '" + path2 + "'";
    if (!reason.empty())
        s += ": " + reason;
    return s;
}

// generic_category().message() rather than strerror(): thread-safe, and the
// worker thread is where most of these are raised.
[[noreturn]] void fail(int err, const char* op, const std::string& path,
                       const std::string& path2 = {})
{
    throw FileOpError(codeForErrno(err), op, path, path2, err,
                      std::generic_category().message(err));
}

[[noreturn]] void failCode(const char* code, const char* op, const std::string& path,
                           const std::string& path2, const std::string& reason)
{
    throw FileOpError(code, op, path, path2, 0, reason);
}

void point(const char* where)
{
    if (testing::faultPoint)
        testing::faultPoint(where);
}

BackendTraits traitsFor(const std::string& path)
{
    return backendTraitsHook ? backendTraitsHook(path) : BackendTraits{};
}

// A remove or rename failed. On a backend where a locked file fails that way
// with a bare EIO (BackendTraits::lockFailuresAreEIO), an EIO involving an
// existing regular file is reported as EACCES: the file is in use.
[[noreturn]] void failMutation(int err, const char* op, const std::string& path,
                               const std::string& path2 = {},
                               const std::string& probe = {})
{
    if (err == EIO) {
        const std::string& a = probe.empty() ? path : probe;
        const std::string& b = probe.empty() ? path2 : std::string();
        for (const std::string* p : {&a, &b}) {
            struct ::stat st;
            if (!p->empty() && traitsFor(*p).lockFailuresAreEIO
                && ::stat(p->c_str(), &st) == 0 && S_ISREG(st.st_mode))
                throw FileOpError("EACCES", op, path, path2, err,
                                  "in use (open for writing)");
        }
    }
    fail(err, op, path, path2);
}

// stat(2), with "nothing there" (ENOENT, or a non-directory component) as
// false and every other failure thrown.
bool statRaw(const std::string& path, struct ::stat& st, const char* op)
{
    if (::stat(path.c_str(), &st) == 0)
        return true;
    const int err = errno;
    if (err == ENOENT || err == ENOTDIR)
        return false;
    fail(err, op, path);
}

std::optional<double> mtimeOf(const struct ::stat& st)
{
#if defined(__APPLE__)
    return double(st.st_mtimespec.tv_sec) + double(st.st_mtimespec.tv_nsec) / 1e9;
#else
    return double(st.st_mtim.tv_sec) + double(st.st_mtim.tv_nsec) / 1e9;
#endif
}

StatInfo infoFrom(const struct ::stat& st, const std::string& path)
{
    StatInfo info;
    if (S_ISREG(st.st_mode))
        info.kind = StatInfo::Kind::File;
    else if (S_ISDIR(st.st_mode))
        info.kind = StatInfo::Kind::Dir;
    info.size = info.kind == StatInfo::Kind::Dir ? 0 : uint64_t(st.st_size);
    info.mode = uint32_t(st.st_mode & 07777);
    if (traitsFor(path).reliableMTime)
        info.mtime = mtimeOf(st);
    return info;
}

// Both paths name the same existing file or directory.
bool sameEntry(const std::string& a, const std::string& b)
{
    struct ::stat sa, sb;
    return ::stat(a.c_str(), &sa) == 0 && ::stat(b.c_str(), &sb) == 0
        && sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

std::string normalizedAbsolute(const std::string& p)
{
    std::error_code ec;
    fs::path abs = fs::absolute(fs::path(p), ec);
    if (ec)
        abs = fs::path(p);
    std::string s = abs.lexically_normal().string();
    while (s.size() > 1 && s.back() == '/')
        s.pop_back();
    return s;
}

// `path` lies strictly inside the tree rooted at `dir` (lexically).
bool isInside(const std::string& dir, const std::string& path)
{
    const std::string d = normalizedAbsolute(dir);
    const std::string p = normalizedAbsolute(path);
    if (d == "/")
        return p.size() > 1;
    return p.size() > d.size() && p.compare(0, d.size(), d) == 0 && p[d.size()] == '/';
}

int rawRename(const char* from, const char* to, bool noReplace)
{
    if (testing::renameFn)
        return testing::renameFn(from, to, noReplace);
#if defined(__linux__) && !defined(__EMSCRIPTEN__) && defined(RENAME_NOREPLACE)
    if (noReplace) {
        // Atomic against processes outside this VM too. Some filesystems do
        // not support the flag; the caller has already checked for a target,
        // so plain rename is the fallback there.
        const int r = ::renameat2(AT_FDCWD, from, AT_FDCWD, to, RENAME_NOREPLACE);
        if (r == 0 || (errno != EINVAL && errno != ENOSYS))
            return r;
    }
#else
    (void)noReplace;
#endif
    return ::rename(from, to);
}

// An existing target is allowed only for a file replacing a file, and only
// with `replace`.
void ensureTargetAllowed(const char* op, const std::string& from, const std::string& to,
                         bool srcDir, const std::optional<StatInfo>& dst, bool replace)
{
    if (!dst)
        return;
    if (!replace)
        failCode("EEXIST", op, from, to, "target exists");
    if (dst->kind == StatInfo::Kind::Dir)
        failCode("EISDIR", op, from, to, "will not replace a directory");
    if (srcDir)
        failCode("ENOTDIR", op, from, to, "will not replace a file with a directory");
}

class Fd {
public:
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd() { if (fd_ >= 0) ::close(fd_); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;

    bool ok() const { return fd_ >= 0; }
    int get() const { return fd_; }

    void closeOrFail(const char* op, const std::string& path)
    {
        const int fd = fd_;
        fd_ = -1;
        if (::close(fd) != 0)
            fail(errno, op, path);
    }

private:
    int fd_;
};

// A temporary file removed on scope exit unless released.
class TempFile {
public:
    explicit TempFile(std::string path) : path(std::move(path)) {}
    ~TempFile() { if (created) ::unlink(path.c_str()); }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    void release() { created = false; }

    const std::string path;
    bool created = false;
};

void writeAll(int fd, const char* data, size_t size, const char* op, const std::string& path)
{
    while (size > 0) {
        const ssize_t n = ::write(fd, data, size);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            fail(errno, op, path);
        }
        data += n;
        size -= size_t(n);
    }
}

void syncOrFail(int fd, const char* op, const std::string& path)
{
    if (::fsync(fd) == 0)
        return;
    const int err = errno;
    if (err == EINVAL || err == ENOTSUP || err == EROFS)
        return;                         // this file cannot be synced; not an error
    fail(err, op, path);
}

// After a rename, sync the directory so the new name itself is durable.
// Best effort, and only where directories can be opened for that.
void syncParentDir(const std::string& path)
{
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
    std::string parent = fs::path(path).parent_path().string();
    if (parent.empty())
        parent = ".";
    const int fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        (void)::fsync(fd);
        ::close(fd);
    }
#else
    (void)path;
#endif
}

void copyContents(int in, int out, const char* op, const std::string& from,
                  const std::string& to)
{
    std::vector<char> buf(64 * 1024);
    for (;;) {
        const ssize_t n = ::read(in, buf.data(), buf.size());
        if (n < 0) {
            if (errno == EINTR)
                continue;
            fail(errno, op, from, to);
        }
        if (n == 0)
            return;
        writeAll(out, buf.data(), size_t(n), op, to);
    }
}

// Copy a file to a path that must not exist (inside a tree we are creating).
void copyFileDirect(const std::string& from, const std::string& to, const char* op)
{
    Fd in(::open(from.c_str(), O_RDONLY | O_CLOEXEC));
    if (!in.ok())
        fail(errno, op, from, to);
    struct ::stat st;
    const mode_t mode = ::fstat(in.get(), &st) == 0 ? (st.st_mode & 07777) : 0666;
    Fd out(::open(to.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode));
    if (!out.ok())
        fail(errno, op, from, to);
    copyContents(in.get(), out.get(), op, from, to);
    out.closeOrFail(op, to);
}

// Copy a file through a temporary sibling of `to`, renamed into place: a
// partial copy is never visible at `to`. `noReplace` when `to` was absent.
void copyFileViaTemp(const std::string& from, const std::string& to, bool noReplace,
                     const char* op)
{
    Fd in(::open(from.c_str(), O_RDONLY | O_CLOEXEC));
    if (!in.ok())
        fail(errno, op, from, to);
    struct ::stat st;
    const mode_t mode = ::fstat(in.get(), &st) == 0 ? (st.st_mode & 07777) : 0666;

    TempFile tmp(tempSiblingName(to));
    Fd out(::open(tmp.path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode));
    if (!out.ok())
        fail(errno, op, from, to);
    tmp.created = true;
    copyContents(in.get(), out.get(), op, from, to);
    syncOrFail(out.get(), op, to);
    out.closeOrFail(op, to);
    point("before-rename");
    if (rawRename(tmp.path.c_str(), to.c_str(), noReplace) != 0)
        failMutation(errno, op, from, to, to);
    tmp.release();
    syncParentDir(to);
}

std::vector<std::string> childNames(const std::string& dir, const char* op)
{
    std::vector<std::string> names;
    std::error_code ec;
    fs::directory_iterator it(dir, ec);
    const fs::directory_iterator end;
    for (; !ec && it != end; it.increment(ec))
        names.push_back(it->path().filename().string());
    if (ec)
        fail(ec.value(), op, dir);
    return names;
}

std::string child(const std::string& dir, const std::string& name)
{
    return (fs::path(dir) / name).string();
}

// Remove a directory tree, reporting the entry that could not be removed.
// Symbolic links are removed, never followed.
void removeTree(const std::string& dir, const char* op)
{
    for (const std::string& name : childNames(dir, op)) {
        const std::string p = child(dir, name);
        struct ::stat st;
        if (::lstat(p.c_str(), &st) != 0) {
            if (errno == ENOENT)
                continue;                   // gone meanwhile
            fail(errno, op, p);
        }
        if (S_ISDIR(st.st_mode))
            removeTree(p, op);
        else if (::unlink(p.c_str()) != 0 && errno != ENOENT)
            failMutation(errno, op, p);
    }
    if (::rmdir(dir.c_str()) != 0) {
        const int err = errno;
        fail(err == EEXIST ? ENOTEMPTY : err, op, dir);
    }
}

void removeTreeQuietly(const std::string& dir)
{
    try {
        removeTree(dir, "remove");
    } catch (...) {
        // Rolling back a failed copy: the original error is what gets reported.
    }
}

void copyTreeContents(const std::string& from, const std::string& to, const char* op)
{
    for (const std::string& name : childNames(from, op)) {
        point("copy-tree-entry");
        const std::string src = child(from, name);
        const std::string dst = child(to, name);
        struct ::stat st;
        if (!statRaw(src, st, op))
            continue;                       // gone meanwhile
        if (S_ISDIR(st.st_mode)) {
            if (::mkdir(dst.c_str(), 0777) != 0)
                fail(errno, op, src, dst);
            copyTreeContents(src, dst, op);
        } else if (S_ISREG(st.st_mode)) {
            copyFileDirect(src, dst, op);
        } else {
            failCode("EINVAL", op, src, dst, "not a regular file or directory");
        }
    }
}

// Copy a directory tree to `to`, which must not exist. On failure, whatever
// was created is removed again -- but never a `to` this call did not create.
void copyTree(const std::string& from, const std::string& to, const char* op)
{
    if (::mkdir(to.c_str(), 0777) != 0)
        fail(errno, op, from, to);
    try {
        copyTreeContents(from, to, op);
    } catch (...) {
        removeTreeQuietly(to);
        throw;
    }
}

// The move fallback: copy, then delete the source. Not atomic. A failed copy
// leaves the source intact; a failed delete leaves a complete copy at `to`
// and reports what remains of the source.
void moveByCopy(const std::string& from, const std::string& to, bool srcDir,
                bool targetAbsent, const char* op)
{
    if (srcDir)
        copyTree(from, to, op);
    else
        copyFileViaTemp(from, to, targetAbsent, op);
    try {
        point("before-remove-source");
        if (srcDir)
            removeTree(from, op);
        else if (::unlink(from.c_str()) != 0)
            failMutation(errno, op, from);
    } catch (const std::exception& e) {
        failCode("EIO", op, from, to,
                 std::string("copied, but the source could not be fully removed (")
                     + e.what() + ")");
    }
}

constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ull;
constexpr uint64_t kFnvPrime = 0x100000001b3ull;

std::string formatTag(uint64_t hash, uint64_t size)
{
    char buf[48];
    std::snprintf(buf, sizeof buf, "%" PRIx64 "-%" PRIu64, hash, size);
    return buf;
}

uint64_t randomSeed()
{
    uint64_t seed = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
    seed ^= uint64_t(std::hash<std::thread::id>{}(std::this_thread::get_id())) << 1;
    try {
        std::random_device rd;
        seed ^= (uint64_t(rd()) << 32) | rd();
    } catch (...) {
        // No entropy source: the time and thread id still make the name unique
        // enough, and O_EXCL catches the rest.
    }
    return seed;
}

} // namespace

FileOpError::FileOpError(std::string code_, std::string op_, std::string path_,
                         std::string path2_, int err_, const std::string& reason)
    : std::runtime_error(describe(code_, op_, path_, path2_, reason))
    , code(std::move(code_))
    , op(std::move(op_))
    , path(std::move(path_))
    , path2(std::move(path2_))
    , err(err_)
    , reason(reason)
{
}

const char* codeForErrno(int err)
{
    switch (err) {
        case ENOENT:        return "ENOENT";
        case EEXIST:        return "EEXIST";
        case ENOTDIR:       return "ENOTDIR";
        case EISDIR:        return "EISDIR";
        case ENOTEMPTY:     return "ENOTEMPTY";
        case EACCES:
        case EPERM:
        case EROFS:         return "EACCES";
        case EBUSY:         return "EBUSY";
        case EXDEV:         return "EXDEV";
        case EINVAL:
        case ENAMETOOLONG:  return "EINVAL";
        default:            return "EIO";
    }
}

const char* kindName(StatInfo::Kind kind)
{
    switch (kind) {
        case StatInfo::Kind::File: return "file";
        case StatInfo::Kind::Dir:  return "dir";
        default:                   return "other";
    }
}

std::optional<StatInfo> stat(const std::string& path)
{
    struct ::stat st;
    if (!statRaw(path, st, "stat"))
        return std::nullopt;
    return infoFrom(st, path);
}

std::optional<std::vector<DirEntry>> listDetails(const std::string& path)
{
    const char* op = "list_dir";
    struct ::stat st;
    if (!statRaw(path, st, op) || !S_ISDIR(st.st_mode))
        return std::nullopt;

    std::vector<DirEntry> entries;
    for (const std::string& name : childNames(path, op)) {
        struct ::stat est;
        const std::string p = child(path, name);
        if (!statRaw(p, est, op))
            continue;                       // vanished between listing and stat
        entries.push_back({name, infoFrom(est, p)});
    }
    std::sort(entries.begin(), entries.end(),
              [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });
    return entries;
}

void rename(const std::string& from, const std::string& to, bool replace)
{
    const char* op = "rename";
    const auto src = stat(from);
    if (!src)
        fail(ENOENT, op, from, to);

    if (sameEntry(from, to)) {
        // Nothing to move -- or a case-only rename on a case-insensitive
        // filesystem, which rename(2) handles.
        if (rawRename(from.c_str(), to.c_str(), false) != 0)
            fail(errno, op, from, to);
        return;
    }

    const bool srcDir = src->kind == StatInfo::Kind::Dir;
    if (srcDir && isInside(from, to))
        failCode("EINVAL", op, from, to, "cannot move a directory inside itself");
    const auto dst = stat(to);
    ensureTargetAllowed(op, from, to, srcDir, dst, replace);

    // Asked up front, so an EBUSY from a native mount point is never taken
    // for "this backend cannot move directories".
    if (srcDir && !traitsFor(from).canRenameDirs) {
        moveByCopy(from, to, srcDir, !dst, op);
        return;
    }
    if (rawRename(from.c_str(), to.c_str(), /*noReplace=*/!dst) == 0) {
        syncParentDir(to);
        return;
    }
    const int err = errno;
    if (err == EXDEV) {
        moveByCopy(from, to, srcDir, !dst, op);
        return;
    }
    failMutation(err, op, from, to);
}

void copy(const std::string& from, const std::string& to, bool recurse, bool replace)
{
    const char* op = "copy";
    const auto src = stat(from);
    if (!src)
        fail(ENOENT, op, from, to);
    if (src->kind == StatInfo::Kind::Other)
        failCode("EINVAL", op, from, to, "not a regular file or directory");

    const bool srcDir = src->kind == StatInfo::Kind::Dir;
    if (srcDir && !recurse)
        failCode("EISDIR", op, from, to, "is a directory (pass recurse=true)");
    if (srcDir && (sameEntry(from, to) || isInside(from, to)))
        failCode("EINVAL", op, from, to, "cannot copy a directory inside itself");

    const auto dst = stat(to);
    ensureTargetAllowed(op, from, to, srcDir, dst, replace);
    if (srcDir)
        copyTree(from, to, op);
    else if (!(dst && sameEntry(from, to)))     // a file onto itself: nothing to do
        copyFileViaTemp(from, to, /*noReplace=*/!dst, op);
}

void remove(const std::string& path, bool recurse)
{
    const char* op = "remove";
    struct ::stat st;
    if (::lstat(path.c_str(), &st) != 0)
        fail(errno, op, path);
    if (S_ISDIR(st.st_mode)) {
        if (recurse) {
            removeTree(path, op);
        } else if (::rmdir(path.c_str()) != 0) {
            const int err = errno;
            fail(err == EEXIST ? ENOTEMPTY : err, op, path);
        }
    } else if (::unlink(path.c_str()) != 0) {
        failMutation(errno, op, path);
    }
}

void writeFile(const std::string& path, std::string_view bytes, bool atomic)
{
    const char* op = "write_file";
    struct ::stat st;
    const bool exists = statRaw(path, st, op);
    if (exists && S_ISDIR(st.st_mode))
        failCode("EISDIR", op, path, "", "is a directory");

    if (!atomic) {
        Fd fd(::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666));
        if (!fd.ok())
            fail(errno, op, path);
        writeAll(fd.get(), bytes.data(), bytes.size(), op, path);
        syncOrFail(fd.get(), op, path);
        fd.closeOrFail(op, path);
        return;
    }

    TempFile tmp(tempSiblingName(path));
    Fd fd(::open(tmp.path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666));
    if (!fd.ok())
        fail(errno, op, path);
    tmp.created = true;
    if (exists)
        (void)::fchmod(fd.get(), st.st_mode & 07777);   // keep the file's permissions
    writeAll(fd.get(), bytes.data(), bytes.size(), op, path);
    point("after-temp-write");
    syncOrFail(fd.get(), op, path);
    fd.closeOrFail(op, path);
    point("before-rename");
    if (rawRename(tmp.path.c_str(), path.c_str(), false) != 0)
        failMutation(errno, op, path);
    tmp.release();
    syncParentDir(path);
}

std::string fileTag(const std::string& path)
{
    const char* op = "file_tag";
    Fd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
    if (!fd.ok())
        fail(errno, op, path);
    struct ::stat st;
    if (::fstat(fd.get(), &st) == 0 && S_ISDIR(st.st_mode))
        failCode("EISDIR", op, path, "", "is a directory");

    uint64_t hash = kFnvOffset;
    uint64_t size = 0;
    std::vector<unsigned char> buf(64 * 1024);
    for (;;) {
        const ssize_t n = ::read(fd.get(), buf.data(), buf.size());
        if (n < 0) {
            if (errno == EINTR)
                continue;
            fail(errno, op, path);
        }
        if (n == 0)
            break;
        for (ssize_t i = 0; i < n; ++i) {
            hash ^= buf[size_t(i)];
            hash *= kFnvPrime;
        }
        size += uint64_t(n);
    }
    return formatTag(hash, size);
}

std::string contentTag(std::string_view bytes)
{
    uint64_t hash = kFnvOffset;
    for (unsigned char c : bytes) {
        hash ^= c;
        hash *= kFnvPrime;
    }
    return formatTag(hash, bytes.size());
}

std::string tempSiblingName(const std::string& target)
{
    thread_local std::mt19937_64 rng(randomSeed());
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    char suffix[16];
    std::snprintf(suffix, sizeof suffix, "%08" PRIx32, uint32_t(rng()));
    const fs::path t(target);
    const std::string name = "." + t.filename().string() + ".tmp-"
                           + std::to_string(ms) + "-" + suffix;
    return (t.parent_path() / name).string();
}

} // namespace roxal::fileops
