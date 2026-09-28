// FileOps: the path-level operations behind fileio's rename/copy/remove/stat/
// write_file/file_tag. Covers what a .rox test cannot reach: the fallback paths
// (a backend that cannot rename directories, a cross-filesystem move) and
// failures injected between the steps of a multi-step operation.

#include "FileOps.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;
using namespace roxal::fileops;

int failures = 0;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FileOps test failed: " << message << '\n';
        ++failures;
    }
}

std::string root;

std::string at(const std::string& rel) { return root + "/" + rel; }

void put(const std::string& rel, const std::string& text)
{
    std::ofstream out(at(rel), std::ios::binary | std::ios::trunc);
    out << text;
}

std::string get(const std::string& rel)
{
    std::ifstream in(at(rel), std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool exists(const std::string& rel) { return fs::exists(at(rel)); }
bool isDir(const std::string& rel) { return fs::is_directory(at(rel)); }

void mkdirs(const std::string& rel) { fs::create_directories(at(rel)); }

void reset()
{
    fs::remove_all(root);
    fs::create_directories(root);
}

// No temporary sibling of `name` left behind in `dirRel`.
bool noTemps(const std::string& dirRel, const std::string& name)
{
    const std::string prefix = "." + name + ".tmp-";
    for (const auto& e : fs::directory_iterator(at(dirRel)))
        if (e.path().filename().string().rfind(prefix, 0) == 0)
            return false;
    return true;
}

// Run `fn`, expecting a FileOpError with `code`.
void expectCode(const std::string& code, const std::function<void()>& fn,
                const std::string& what)
{
    try {
        fn();
        check(false, what + ": expected " + code + ", succeeded");
    } catch (const FileOpError& e) {
        check(e.code == code, what + ": expected " + code + ", got " + e.what());
        check(std::string(e.what()).rfind(code + ": ", 0) == 0,
              what + ": message should start with the code: " + e.what());
    }
}

// Fault injection: throw at the n-th arrival at `faultAt`.
std::string faultAt;
int faultCountdown = 0;

void injectFault(const char* where)
{
    if (faultAt == where && --faultCountdown <= 0) {
        faultAt.clear();
        throw FileOpError("EIO", "test", "", "", 0, std::string("injected at ") + where);
    }
}

void armFault(const std::string& where, int nth = 1)
{
    faultAt = where;
    faultCountdown = nth;
    testing::faultPoint = &injectFault;
}

void disarmFault()
{
    faultAt.clear();
    testing::faultPoint = nullptr;
}

// Rename seam: EXDEV for one chosen source path, the real call otherwise.
std::string exdevFrom;
int renameCalls = 0;

int seamRename(const char* from, const char* to, bool)
{
    ++renameCalls;
    if (!exdevFrom.empty() && exdevFrom == from) {
        errno = EXDEV;
        return -1;
    }
    return ::rename(from, to);
}

BackendTraits noDirRename(const std::string&)
{
    BackendTraits t;
    t.canRenameDirs = false;
    t.reliableMTime = false;
    return t;
}

BackendTraits lockAsEIO(const std::string&)
{
    BackendTraits t;
    t.lockFailuresAreEIO = true;
    return t;
}

// Rename seam: EIO for every call, as WASMFS reports a failed OPFS move.
int eioRename(const char*, const char*, bool)
{
    errno = EIO;
    return -1;
}

void testTags()
{
    reset();
    check(contentTag("") == "cbf29ce484222325-0", "empty content tag");
    check(contentTag("a") == "af63dc4c8601ec8c-1", "tag of 'a'");
    // Pins the DECIMAL length: a hex length would give "...-b".
    check(contentTag("hello world") == "779a65e7023cd2e7-11", "tag of 'hello world'");
    check(contentTag("print('h\xc3\xa9llo w\xc3\xb6rld \xe2\x9c\x93')\n") == "2f52c98658ac8139-27",
          "tag of a UTF-8 sample");

    put("empty", "");
    check(fileTag(at("empty")) == "cbf29ce484222325-0", "empty file tag");
    put("hw", "hello world");
    check(fileTag(at("hw")) == "779a65e7023cd2e7-11", "file tag of 'hello world'");

    // Across the 64 KiB read-chunk boundary.
    std::string big(200 * 1024 + 7, '\0');
    for (size_t i = 0; i < big.size(); ++i)
        big[i] = char(i * 31 + 7);
    put("big", big);
    check(fileTag(at("big")) == contentTag(big), "file tag == content tag across chunks");

    expectCode("ENOENT", [] { fileTag(at("missing")); }, "file_tag missing");
    mkdirs("d");
    expectCode("EISDIR", [] { fileTag(at("d")); }, "file_tag directory");
}

void testStatAndList()
{
    reset();
    put("f.txt", "12345");
    mkdirs("sub");
    auto f = stat(at("f.txt"));
    check(f && f->kind == StatInfo::Kind::File && f->size == 5 && f->mtime.has_value(),
          "stat file");
    auto d = stat(at("sub"));
    check(d && d->kind == StatInfo::Kind::Dir && d->size == 0, "stat dir");
    check(!stat(at("missing")), "stat missing is empty");
    check(!stat(at("f.txt/below")), "stat below a file is empty");

    backendTraitsHook = &noDirRename;
    check(!stat(at("f.txt"))->mtime.has_value(), "mtime withheld when unreliable");
    backendTraitsHook = nullptr;

    put("b.rox", "x");
    auto list = listDetails(root);
    check(list && list->size() == 3, "list size");
    if (list && list->size() == 3) {
        check((*list)[0].name == "b.rox" && (*list)[1].name == "f.txt" && (*list)[2].name == "sub",
              "list sorted by name");
        check((*list)[2].info.kind == StatInfo::Kind::Dir, "list kind");
    }
    check(!listDetails(at("f.txt")), "list of a file is empty");
    check(!listDetails(at("missing")), "list of missing is empty");
}

void testRenameRules()
{
    reset();
    put("a", "A");
    rename(at("a"), at("b"), false);
    check(!exists("a") && get("b") == "A", "rename file");

    put("c", "C");
    expectCode("EEXIST", [] { rename(at("b"), at("c"), false); }, "rename onto file");
    check(get("b") == "A" && get("c") == "C", "EEXIST leaves both");
    rename(at("b"), at("c"), true);
    check(!exists("b") && get("c") == "A", "rename replace file");

    mkdirs("dir");
    expectCode("EEXIST", [] { rename(at("c"), at("dir"), false); }, "file onto dir");
    expectCode("EISDIR", [] { rename(at("c"), at("dir"), true); }, "file onto dir, replace");
    put("dir/inner.txt", "I");
    expectCode("ENOTDIR", [] { rename(at("dir"), at("c"), true); }, "dir onto file, replace");
    mkdirs("dir2");
    expectCode("EISDIR", [] { rename(at("dir"), at("dir2"), true); }, "dir onto dir, replace");
    expectCode("EINVAL", [] { rename(at("dir"), at("dir/deeper"), false); }, "dir into itself");
    expectCode("ENOENT", [] { rename(at("nope"), at("x"), false); }, "missing source");
    expectCode("ENOENT", [] { rename(at("c"), at("no/such/x"), false); }, "missing target parent");

    rename(at("dir"), at("moved"), false);
    check(!exists("dir") && get("moved/inner.txt") == "I", "rename dir");
    rename(at("c"), at("c"), false);
    check(get("c") == "A", "rename onto itself is a no-op");
}

void testRenameFallbacks()
{
    reset();
    testing::renameFn = &seamRename;

    // A backend that cannot rename directories: never asks it to.
    mkdirs("tree/sub");
    put("tree/sub/x.txt", "X");
    put("tree/y.txt", "Y");
    backendTraitsHook = &noDirRename;
    renameCalls = 0;
    rename(at("tree"), at("tree2"), false);
    check(renameCalls == 0, "dir fallback never calls rename");
    check(!exists("tree") && get("tree2/sub/x.txt") == "X" && get("tree2/y.txt") == "Y",
          "dir fallback moves the tree");

    // The copy fails part way: the source stays, the partial target goes.
    armFault("copy-tree-entry", 2);
    expectCode("EIO", [] { rename(at("tree2"), at("tree3"), false); }, "dir fallback copy fails");
    disarmFault();
    check(get("tree2/sub/x.txt") == "X" && get("tree2/y.txt") == "Y", "failed copy keeps source");
    check(!exists("tree3"), "failed copy removes the partial target");

    // The copy succeeds but the source cannot be removed: EIO, target complete.
    armFault("before-remove-source");
    expectCode("EIO", [] { rename(at("tree2"), at("tree4"), false); }, "source removal fails");
    disarmFault();
    check(get("tree4/sub/x.txt") == "X" && exists("tree2"), "target complete, source kept");
    backendTraitsHook = nullptr;

    // EXDEV: a file moves by copy, and so does a directory.
    put("f", "F");
    exdevFrom = at("f");
    rename(at("f"), at("g"), false);
    check(!exists("f") && get("g") == "F" && noTemps("", "g"), "EXDEV file move");
    put("h", "old");
    exdevFrom = at("g");
    rename(at("g"), at("h"), true);
    check(!exists("g") && get("h") == "F", "EXDEV file move with replace");
    exdevFrom = at("tree4");
    rename(at("tree4"), at("tree5"), false);
    check(!exists("tree4") && get("tree5/y.txt") == "Y", "EXDEV dir move");
    exdevFrom.clear();

    testing::renameFn = nullptr;
}

void testCopy()
{
    reset();
    put("a", "A");
    copy(at("a"), at("b"), false, false);
    check(get("a") == "A" && get("b") == "A" && noTemps("", "b"), "copy file");
    put("c", "C");
    expectCode("EEXIST", [] { copy(at("a"), at("c"), false, false); }, "copy onto file");
    copy(at("a"), at("c"), false, true);
    check(get("c") == "A", "copy replace");
    copy(at("a"), at("a"), false, true);
    check(get("a") == "A", "copy file onto itself");

    mkdirs("t/u");
    put("t/u/v.txt", "V");
    put("t/w.txt", "W");
    expectCode("EISDIR", [] { copy(at("t"), at("t2"), false, false); }, "copy dir without recurse");
    copy(at("t"), at("t2"), true, false);
    check(get("t2/u/v.txt") == "V" && get("t2/w.txt") == "W" && get("t/w.txt") == "W",
          "copy tree");
    expectCode("EEXIST", [] { copy(at("t"), at("t2"), true, false); }, "copy tree onto dir");
    expectCode("EISDIR", [] { copy(at("t"), at("t2"), true, true); }, "copy tree onto dir, replace");
    expectCode("EINVAL", [] { copy(at("t"), at("t/u/inner"), true, false); }, "copy tree into itself");
    expectCode("ENOENT", [] { copy(at("missing"), at("z"), false, false); }, "copy missing");

    // A failure before the final rename leaves the old target and no temp.
    armFault("before-rename");
    expectCode("EIO", [] { copy(at("b"), at("c"), false, true); }, "copy fails before rename");
    disarmFault();
    check(get("c") == "A" && noTemps("", "c"), "failed copy leaves target and no temp");
}

void testRemove()
{
    reset();
    put("f", "F");
    remove(at("f"), false);
    check(!exists("f"), "remove file");
    expectCode("ENOENT", [] { remove(at("f"), false); }, "remove missing");
    mkdirs("empty");
    remove(at("empty"), false);
    check(!exists("empty"), "remove empty dir");
    mkdirs("full/sub");
    put("full/sub/x", "X");
    expectCode("ENOTEMPTY", [] { remove(at("full"), false); }, "remove non-empty dir");
    check(exists("full/sub/x"), "ENOTEMPTY removes nothing");
    remove(at("full"), true);
    check(!exists("full"), "remove recursively");
}

void testWriteFile()
{
    reset();
    writeFile(at("new.txt"), "one", true);
    check(get("new.txt") == "one" && noTemps("", "new.txt"), "atomic write new");
    writeFile(at("new.txt"), "two", true);
    check(get("new.txt") == "two" && noTemps("", "new.txt"), "atomic overwrite");
    writeFile(at("plain.txt"), "p", false);
    writeFile(at("plain.txt"), "q", false);
    check(get("plain.txt") == "q", "non-atomic write");
    writeFile(at("empty.txt"), "", true);
    check(exists("empty.txt") && get("empty.txt").empty(), "atomic write of nothing");

    mkdirs("adir");
    expectCode("EISDIR", [] { writeFile(at("adir"), "x", true); }, "write onto a directory");
    check(isDir("adir") && noTemps("", "adir"), "directory target intact");
    expectCode("ENOENT", [] { writeFile(at("no/parent.txt"), "x", true); }, "missing parent");

    for (const char* where : {"after-temp-write", "before-rename"}) {
        armFault(where);
        expectCode("EIO", [] { writeFile(at("new.txt"), "three", true); },
                   std::string("write fails at ") + where);
        disarmFault();
        check(get("new.txt") == "two" && noTemps("", "new.txt"),
              std::string("failed write at ") + where + " leaves target and no temp");
    }

    ::chmod(at("new.txt").c_str(), 0640);
    writeFile(at("new.txt"), "four", true);
    struct ::stat st;
    check(::stat(at("new.txt").c_str(), &st) == 0 && (st.st_mode & 0777) == 0640,
          "atomic write keeps permissions");
}

// A backend where a locked file fails to be removed or renamed with a bare
// EIO (WASMFS on OPFS): that EIO, for an existing regular file, is reported as
// EACCES -- in use. A directory, or no such trait, keeps EIO.
void testLockFailuresAreEIO()
{
    reset();
    put("f", "F");
    mkdirs("d");
    testing::renameFn = &eioRename;
    expectCode("EIO", [] { rename(at("f"), at("x"), false); },
               "EIO stays EIO without the trait");
    backendTraitsHook = &lockAsEIO;
    expectCode("EACCES", [] { rename(at("f"), at("x"), false); },
               "EIO renaming a file is EACCES under the trait");
    put("g", "G");
    expectCode("EACCES", [] { rename(at("g"), at("f"), true); },
               "EIO replacing a file is EACCES under the trait");
    expectCode("EACCES", [] { writeFile(at("f"), "new", true); },
               "an atomic write that cannot replace the file is EACCES");
    check(get("f") == "F" && noTemps("", "f"), "the in-use file is untouched");
    expectCode("EIO", [] { rename(at("d"), at("d2"), false); },
               "EIO renaming a directory stays EIO");
    backendTraitsHook = nullptr;
    testing::renameFn = nullptr;
}

void testTempNamesAndCodes()
{
    const std::string t = tempSiblingName("/some/dir/file.rox");
    const std::string prefix = "/some/dir/.file.rox.tmp-";
    check(t.rfind(prefix, 0) == 0, "temp name is a dot-sibling: " + t);
    const std::string rest = t.substr(prefix.size());
    const size_t dash = rest.find('-');
    check(dash != std::string::npos && dash > 0
              && rest.find_first_not_of("0123456789") == dash
              && rest.size() - dash - 1 == 8
              && rest.find_first_not_of("0123456789abcdef", dash + 1) == std::string::npos,
          "temp name carries <ms>-<8 hex>: " + t);
    check(tempSiblingName("/d/f") != tempSiblingName("/d/f"), "temp names are unique");

    check(std::strcmp(codeForErrno(EPERM), "EACCES") == 0, "EPERM reports as EACCES");
    check(std::strcmp(codeForErrno(ENAMETOOLONG), "EINVAL") == 0, "ENAMETOOLONG as EINVAL");
    check(std::strcmp(codeForErrno(ENOSPC), "EIO") == 0, "others as EIO");
}

} // namespace

int main()
{
    char tmpl[] = "/tmp/roxal_fileops_XXXXXX";
    if (!::mkdtemp(tmpl)) {
        std::cerr << "FileOps test: cannot create a temp directory\n";
        return EXIT_FAILURE;
    }
    root = tmpl;

    testTags();
    testStatAndList();
    testRenameRules();
    testRenameFallbacks();
    testCopy();
    testRemove();
    testWriteFile();
    testLockFailuresAreEIO();
    testTempNamesAndCodes();

    fs::remove_all(root);
    if (failures) {
        std::cerr << failures << " FileOps check(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
