#pragma once

// Path-level file operations for the fileio module: rename, copy, remove, stat,
// whole-file writes and content tags.
//
// Deliberately free of Value and the VM, so it runs on the fileio I/O worker and
// is tested directly (fileops_test.cpp). Failures are thrown as FileOpError,
// carrying a POSIX-style code; ModuleFileIO/AsyncIOManager turn that into a
// FileIOException whose `detail` dict holds the code.
//
// Built on POSIX calls rather than std::fstream: they report errno directly
// (which is what the codes come from), and under WASMFS/OPFS a contended file
// lock arrives as EACCES from open() -- a stream would only say "not open".

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace roxal::fileops {

// A failed operation. what() is "<CODE>: <op> '<path>'[ -> '<path2>']: <reason>".
class FileOpError : public std::runtime_error {
public:
    FileOpError(std::string code, std::string op, std::string path,
                std::string path2, int err, const std::string& reason);

    std::string code;   // ENOENT, EEXIST, ENOTDIR, EISDIR, ENOTEMPTY, EACCES,
                        // EBUSY, EXDEV, EINVAL or EIO
    std::string op;     // the fileio function, e.g. "rename"
    std::string path;
    std::string path2;  // the second path of rename/copy, else empty
    int err;            // the errno behind it, 0 when the check was our own
    std::string reason; // the message without code/op/paths, e.g. "target exists"
};

// The code for an errno value. EPERM and EROFS report as EACCES, ENAMETOOLONG
// as EINVAL; anything without a code of its own is EIO.
const char* codeForErrno(int err);

struct StatInfo {
    enum class Kind { File, Dir, Other };
    Kind kind = Kind::Other;
    uint64_t size = 0;                  // bytes; 0 for directories
    std::optional<double> mtime;        // seconds since the epoch; empty when the
                                        // backend's value is not meaningful
    uint32_t mode = 0;                  // permission bits (to preserve on replace)
};

const char* kindName(StatInfo::Kind kind);   // "file", "dir", "other"

// Nothing at `path` (ENOENT, or a non-directory component) is an empty result;
// other failures (EACCES, ...) throw.
std::optional<StatInfo> stat(const std::string& path);

struct DirEntry {
    std::string name;
    StatInfo info;
};

// The entries of a directory sorted by name, or empty if `path` is not a
// directory. An entry that vanishes between listing and stat is skipped.
std::optional<std::vector<DirEntry>> listDetails(const std::string& path);

// Move a file or directory. See ensureTargetAllowed() in FileOps.cpp for the
// rules on an existing target: `replace` only ever lets a file replace a file.
// A directory on a backend that cannot rename directories, or any move across
// filesystems (EXDEV), falls back to copy-then-delete -- NOT atomic, but the
// source is left intact if the copy fails.
void rename(const std::string& from, const std::string& to, bool replace);

// Copy a file, or (with `recurse`) a directory tree. A file is copied through a
// temporary sibling and renamed into place, so a partial copy is never visible
// at `to`; a failed tree copy removes what it created.
void copy(const std::string& from, const std::string& to, bool recurse, bool replace);

// Remove a file, or a directory -- an empty one, or any one with `recurse`.
void remove(const std::string& path, bool recurse);

// Write `bytes` as the whole content of `path`. Atomic: write a temporary
// sibling (see tempSiblingName), fsync it and rename it over `path`; on failure
// the temporary file is removed and `path` is untouched. Non-atomic: truncate
// and write in place.
void writeFile(const std::string& path, std::string_view bytes, bool atomic);

// Content tags: FNV-1a 64 of the bytes as lowercase hex without padding, '-',
// the byte count in decimal. The empty content is "cbf29ce484222325-0".
std::string fileTag(const std::string& path);
std::string contentTag(std::string_view bytes);

// The temporary sibling used by atomic writes and file copies:
//   <dir>/.<name>.tmp-<epoch ms>-<8 hex digits>
// The timestamp makes the newest of several sort last by name, since mtime is
// not reliable on every backend.
std::string tempSiblingName(const std::string& target);

// What a mounted filesystem can do, as the embedding host knows it. The
// defaults describe an ordinary POSIX filesystem. A host whose mounts lack
// something installs the hook (e.g. the wasm host for its OPFS mount at /data:
// OPFS cannot move directories, WASMFS invents its file times, and a file
// locked by a writer fails to be removed or moved with a bare EIO).
struct BackendTraits {
    bool canRenameDirs = true;
    bool reliableMTime = true;
    // Removing or renaming a file that is open for writing -- by anyone,
    // including this process -- fails with EIO here, and little else does:
    // FileOps reports an EIO from removing or renaming an existing regular
    // file as EACCES ("in use"). (Asking open() instead cannot work: on WASMFS
    // a second open of a file this process holds shares its OPFS handle and
    // succeeds.)
    bool lockFailuresAreEIO = false;
};
using BackendTraitsFn = BackendTraits (*)(const std::string& path);
inline BackendTraitsFn backendTraitsHook = nullptr;

// Seams for fileops_test.cpp only.
namespace testing {
// Replaces the rename syscall (noReplace: fail with EEXIST rather than replace).
// Returns 0 or -1 with errno set, like rename(2).
inline int (*renameFn)(const char* from, const char* to, bool noReplace) = nullptr;
// Called at named points inside multi-step operations; may throw to simulate
// a failure there. Points: "after-temp-write", "before-rename",
// "copy-tree-entry", "before-remove-source".
inline void (*faultPoint)(const char* where) = nullptr;
}

} // namespace roxal::fileops
