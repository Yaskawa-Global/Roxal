#include "ModuleFileIO.h"
#include "AsyncIOManager.h"
#include "FileOps.h"
#include "VM.h"
#include "Object.h"
#include <sstream>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <optional>
#include <algorithm>
#include <system_error>

using namespace roxal;

ModuleFileIO::ModuleFileIO()
{
    moduleTypeValue = Value::objVal(newModuleTypeObj(toUnicodeString("fileio")));
    ObjModuleType::allModules.push_back(moduleTypeValue);
}

ModuleFileIO::~ModuleFileIO()
{
    destroyModuleType(moduleTypeValue);
}

void ModuleFileIO::registerBuiltins(VM& vm)
{
    setVM(vm);
    // resolve arg 0 (path/file) for all functions, and arg 1 (data) for write
    link("open", [this](VM&, ArgsView a){ return fileio_open_builtin(a); }, {}, 0x1);
    link("close", [this](VM&, ArgsView a){ return fileio_close_builtin(a); }, {}, 0x1);
    link("flush", [this](VM&, ArgsView a){ return fileio_flush_builtin(a); }, {}, 0x1);
    link("is_open", [this](VM&, ArgsView a){ return fileio_is_open_builtin(a); }, {}, 0x1);
    link("more_data", [this](VM&, ArgsView a){ return fileio_more_data_builtin(a); }, {}, 0x1);
    link("read", [this](VM&, ArgsView a){ return fileio_read_builtin(a); }, {}, 0x1);
    link("read_line", [this](VM&, ArgsView a){ return fileio_read_line_builtin(a); }, {}, 0x1);
    link("read_file", [this](VM&, ArgsView a){ return fileio_read_file_builtin(a); }, {}, 0x1);
    link("write", [this](VM&, ArgsView a){ return fileio_write_builtin(a); }, {}, 0x3);  // resolve file and data
    link("file_exists", [this](VM&, ArgsView a){ return fileio_file_exists_builtin(a); }, {}, 0x1);
    link("list_dir", [this](VM&, ArgsView a){ return fileio_list_dir_builtin(a); }, {}, 0x1);
    link("delete_file", [this](VM&, ArgsView a){ return fileio_delete_file_builtin(a); }, {}, 0x1);
    link("create_dir", [this](VM&, ArgsView a){ return fileio_create_dir_builtin(a); }, {}, 0x1);
    link("dir_exists", [this](VM&, ArgsView a){ return fileio_dir_exists_builtin(a); }, {}, 0x1);
    link("delete_dir", [this](VM&, ArgsView a){ return fileio_delete_dir_builtin(a); }, {}, 0x1);
    link("file_size", [this](VM&, ArgsView a){ return fileio_file_size_builtin(a); }, {}, 0x1);
    link("absolute_file_path", [this](VM&, ArgsView a){ return fileio_absolute_file_path_builtin(a); }, {}, 0x1);
    link("path_directory", [this](VM&, ArgsView a){ return fileio_path_directory_builtin(a); }, {}, 0x1);
    link("path_file", [this](VM&, ArgsView a){ return fileio_path_file_builtin(a); }, {}, 0x1);
    link("file_extension", [this](VM&, ArgsView a){ return fileio_file_extension_builtin(a); }, {}, 0x1);
    link("file_without_extension", [this](VM&, ArgsView a){ return fileio_file_without_extension_builtin(a); }, {}, 0x1);
    link("rename", [this](VM&, ArgsView a){ return fileio_rename_builtin(a); }, {}, 0x3);
    link("copy", [this](VM&, ArgsView a){ return fileio_copy_builtin(a); }, {}, 0x3);
    link("remove", [this](VM&, ArgsView a){ return fileio_remove_builtin(a); }, {}, 0x1);
    link("stat", [this](VM&, ArgsView a){ return fileio_stat_builtin(a); }, {}, 0x1);
    link("write_file", [this](VM&, ArgsView a){ return fileio_write_file_builtin(a); }, {}, 0x3);
    link("file_tag", [this](VM&, ArgsView a){ return fileio_file_tag_builtin(a); }, {}, 0x1);
    link("content_tag", [this](VM&, ArgsView a){ return fileio_content_tag_builtin(a); }, {}, 0x1);
}

namespace {

Value str(const std::string& s)
{
    return Value::stringVal(toUnicodeString(s));
}

// The script form of a stat result: {'kind', 'size', 'mtime'}, with 'name'
// first when listing a directory.
Value statDict(const fileops::StatInfo& info, const std::string* name = nullptr)
{
    Value dict = Value::dictVal();
    ObjDict* d = asDict(dict);
    if (name)
        d->store(str("name"), str(*name));
    d->store(str("kind"), str(fileops::kindName(info.kind)));
    d->store(str("size"), Value::intVal(static_cast<int64_t>(info.size)));
    d->store(str("mtime"), info.mtime ? Value::realVal(*info.mtime) : Value::nilVal());
    return dict;
}

// The bytes `data` stands for, as write()/write_file() store them: in binary
// mode a list of bytes (or ints 0-255), in text mode the value's string form
// as UTF-8. Returns an error message, or nullptr on success.
const char* toBytes(const Value& data, bool binary, std::string& out)
{
    if (!binary) {
        out = toString(data);
        return nullptr;
    }
    if (!isList(data))
        return "expects list of bytes in binary mode";
    ObjList* lst = asList(data);
    // Fast path: a packed byte list copies out in one shot.
    if (const std::vector<uint8_t>* pb = lst->packedBytes()) {
        out.assign(pb->begin(), pb->end());
        return nullptr;
    }
    out.clear();
    out.reserve(static_cast<size_t>(lst->length()));
    for (int i = 0; i < lst->length(); ++i) {
        const Value& v = lst->getElement(i);
        uint8_t b;
        if (v.isByte())
            b = v.asByte();
        else if (v.isInt()) {
            int64_t iv = v.asInt();
            if (iv < 0 || iv > 255)
                return "int out of byte range";
            b = static_cast<uint8_t>(iv);
        } else {
            return "expects list of bytes or ints";
        }
        out.push_back(static_cast<char>(b));
    }
    return nullptr;
}

}  // namespace

Value ModuleFileIO::fileio_open_builtin(ArgsView args)
{
    if (args.size() < 1 || args.size() > 4 || !isString(args[0]))
        throw std::invalid_argument("fileio.open expects path string and optional append bool, format string, and write bool");
    bool append = false;
    if (args.size() >= 2)
        append = args[1].asBool();

    bool write = false;
    bool writeProvided = false;
    bool formatProvided = false;
    std::string format = "text";
    if (args.size() >= 3) {
        if (isString(args[2])) {
            format = toUTF8StdString(asStringObj(args[2])->s);
            formatProvided = true;
        } else {
            write = args[2].asBool();
            writeProvided = true;
        }
    }
    if (args.size() == 4) {
        if (!formatProvided) {
            if (!isString(args[3]))
                throw std::invalid_argument("fileio.open format must be 'text' or 'binary'");
            format = toUTF8StdString(asStringObj(args[3])->s);
            formatProvided = true;
        } else if (!writeProvided) {
            write = args[3].asBool();
            writeProvided = true;
        } else {
            throw std::invalid_argument("fileio.open received too many arguments");
        }
    }

    if (append) {
        write = true;
        writeProvided = true;
    }

    bool binary = false;
    std::filesystem::path path = std::filesystem::path(toUTF8StdString(asStringObj(args[0])->s));
    ptr<std::fstream> f = roxal::make_ptr<std::fstream>();
    std::ios_base::openmode mode;
    if (write) {
        // Allow read/write, create if missing, and truncate unless appending.
        mode = std::ios::in | std::ios::out;
        if (append)
            mode |= std::ios::app;
        else
            mode |= std::ios::trunc;
    } else {
        mode = std::ios::in;
    }
    if (format == "binary") {
        mode |= std::ios::binary;
        binary = true;
    } else if (format != "text") {
        throw std::invalid_argument("fileio.open format must be 'text' or 'binary'");
    }
    f->open(path, mode);
    if (!f->is_open()) {
        return Value::falseVal();
    }
    return Value::fileVal(f, binary);
}

// The async= convention, shared by every operation that goes through the I/O
// worker: the op is ALWAYS submitted to the worker queue (one queue, FIFO, so
// read-after-write ordering holds regardless of mode). async=false — the
// default — then awaits the future inside the VM dispatcher, so the script
// sees a plain synchronous call while the OS thread stays unblocked (an RT
// the slice returns immediately, a host UI loop keeps pumping). async=true
// returns the future for explicit pipelining (fire-and-forget writes from an
// RT loop), to be consumed with wait(for=...).
static bool asyncArg(const ArgsView& args, size_t index)
{
    return args.size() > index && args[index].isBool() && args[index].asBool();
}

Value ModuleFileIO::awaitInVM(Value future)
{
    // Hoisted to VM::awaitFutureInVM so other builtins (ai.nn's Model.init)
    // can use the same synchronous-looking-await machinery.
    return vm().awaitFutureInVM(std::move(future));
}

Value ModuleFileIO::fileio_close_builtin(ArgsView args)
{
    if (args.size() < 1 || args.size() > 2 || !isFile(args[0]))
        throw std::invalid_argument("fileio.close expects file handle");

    const Value& fileValue = args[0];
    ObjFile* f = asFile(fileValue);

    // Check if there are pending async operations - returns future if pending, nil if not
    Value pendingFuture = AsyncIOManager::instance().getPendingFuture(fileValue);

    if (pendingFuture.isNonNil()) {
        // There are pending operations - submit async close that waits for them
        PendingIOOp op;
        op.type = PendingIOOp::Type::FileClose;
        op.file = f;
        op.fileValue = fileValue;
        // The pending future will be waited on by the async worker
        op.pendingFutures.push_back(asFuture(pendingFuture)->future);
        Value fut = AsyncIOManager::instance().submit(std::move(op));
        return asyncArg(args, 1) ? fut : awaitInVM(fut);
    }

    // No pending operations - close synchronously
    {
        std::lock_guard<std::mutex> lock(f->mutex);
        if (f->file && f->file->is_open())
            f->file->close();
    }
    return Value::trueVal();
}

Value ModuleFileIO::fileio_is_open_builtin(ArgsView args)
{
    if (args.size() != 1 || !isFile(args[0]))
        throw std::invalid_argument("fileio.is_open expects file handle");
    ObjFile* f = asFile(args[0]);
    std::lock_guard<std::mutex> lock(f->mutex);
    return f->file && f->file->is_open() ? Value::trueVal() : Value::falseVal();
}

Value ModuleFileIO::fileio_more_data_builtin(ArgsView args)
{
    if (args.size() != 1 || !isFile(args[0]))
        throw std::invalid_argument("fileio.more_data expects file handle");
    ObjFile* f = asFile(args[0]);
    std::lock_guard<std::mutex> lock(f->mutex);
    if (!f->file || !f->file->is_open()) return Value::falseVal();
    int c = f->file->peek();
    return (c == std::char_traits<char>::eof()) ? Value::falseVal() : Value::trueVal();
}

Value ModuleFileIO::fileio_read_builtin(ArgsView args)
{
    if (args.size() < 1 || args.size() > 2 || !isFile(args[0]))
        throw std::invalid_argument("fileio.read expects file handle");
    ObjFile* f = asFile(args[0]);

    // Submit async read operation
    PendingIOOp op;
    op.type = PendingIOOp::Type::FileRead;
    op.file = f;
    op.fileValue = args[0];
    op.maxBytes = 4096;
    op.binary = f->binary;

    Value fut = AsyncIOManager::instance().submit(std::move(op));
    return asyncArg(args, 1) ? fut : awaitInVM(fut);
}

Value ModuleFileIO::fileio_read_line_builtin(ArgsView args)
{
    if (args.size() < 1 || args.size() > 2 || !isFile(args[0]))
        throw std::invalid_argument("fileio.read_line expects file handle");
    ObjFile* f = asFile(args[0]);

    // Check binary mode synchronously (throws exception)
    if (f->binary) {
        Value exType = vm().loadGlobal(toUnicodeString("FileIOException")).value();
        Value msg = Value::stringVal(toUnicodeString("read_line requires text mode"));
        Value exc = Value::exceptionVal(msg, exType);
        vm().raiseException(exc);
        return Value::nilVal();
    }

    // Submit async read line operation
    PendingIOOp op;
    op.type = PendingIOOp::Type::FileReadLine;
    op.file = f;
    op.fileValue = args[0];
    op.binary = false;

    Value fut = AsyncIOManager::instance().submit(std::move(op));
    return asyncArg(args, 1) ? fut : awaitInVM(fut);
}

Value ModuleFileIO::fileio_read_file_builtin(ArgsView args)
{
    if (args.size() < 1 || args.size() > 3 || !isString(args[0]))
        throw std::invalid_argument("fileio.read_file expects path string and optional format");
    std::string format = "text";
    if (args.size() >= 2) {
        if (!isString(args[1]))
            throw std::invalid_argument("fileio.read_file format must be 'text' or 'binary'");
        format = toUTF8StdString(asStringObj(args[1])->s);
    }
    if (format != "text" && format != "binary")
        throw std::invalid_argument("fileio.read_file format must be 'text' or 'binary'");

    std::string path = toUTF8StdString(asStringObj(args[0])->s);

    // Submit async read all operation
    PendingIOOp op;
    op.type = PendingIOOp::Type::FileReadAll;
    op.path = path;
    op.binary = (format == "binary");

    Value fut = AsyncIOManager::instance().submit(std::move(op));
    return asyncArg(args, 2) ? fut : awaitInVM(fut);
}

Value ModuleFileIO::fileio_write_builtin(ArgsView args)
{
    if (args.size() < 2 || args.size() > 3 || !isFile(args[0]))
        throw std::invalid_argument("fileio.write expects file handle and data");
    ObjFile* f = asFile(args[0]);

    // Prepare write data synchronously (validation happens here)
    std::string writeData;
    if (const char* error = toBytes(args[1], f->binary, writeData))
        throw std::invalid_argument(std::string("fileio.write ") + error);

    // Submit async write operation
    PendingIOOp op;
    op.type = PendingIOOp::Type::FileWrite;
    op.file = f;
    op.fileValue = args[0];
    op.writeData = std::move(writeData);
    op.binary = f->binary;

    Value fut = AsyncIOManager::instance().submit(std::move(op));
    return asyncArg(args, 2) ? fut : awaitInVM(fut);
}

Value ModuleFileIO::fileio_flush_builtin(ArgsView args)
{
    if (args.size() < 1 || args.size() > 2 || !isFile(args[0]))
        throw std::invalid_argument("fileio.flush expects file handle");

    const Value& fileValue = args[0];
    ObjFile* f = asFile(fileValue);

    // Check if there are pending async operations - returns future if pending, nil if not
    Value pendingFuture = AsyncIOManager::instance().getPendingFuture(fileValue);

    if (pendingFuture.isNonNil()) {
        // There are pending operations - submit async flush that waits for them
        PendingIOOp op;
        op.type = PendingIOOp::Type::FileSyncFlush;
        op.file = f;
        op.fileValue = fileValue;
        // The pending future will be waited on by the async worker
        op.pendingFutures.push_back(asFuture(pendingFuture)->future);
        Value fut = AsyncIOManager::instance().submit(std::move(op));
        return asyncArg(args, 1) ? fut : awaitInVM(fut);
    }

    // No pending operations - flush synchronously
    std::lock_guard<std::mutex> lock(f->mutex);
    if (!f->file || !f->file->is_open()) return Value::falseVal();
    f->file->flush();
    return f->file->good() ? Value::trueVal() : Value::falseVal();
}

Value ModuleFileIO::fileio_file_exists_builtin(ArgsView args)
{
    if (args.size() != 1 || !isString(args[0]))
        throw std::invalid_argument("fileio.file_exists expects path string");
    std::filesystem::path p(toUTF8StdString(asStringObj(args[0])->s));
    std::error_code ec;
    return std::filesystem::is_regular_file(p, ec) ? Value::trueVal() : Value::falseVal();
}

Value ModuleFileIO::fileio_list_dir_builtin(ArgsView args)
{
    if (args.size() < 1 || args.size() > 2 || !isString(args[0]))
        throw std::invalid_argument("fileio.list_dir expects path string and optional details bool");
    const std::string path = toUTF8StdString(asStringObj(args[0])->s);

    if (args.size() == 2 && args[1].isBool() && args[1].asBool()) {
        // The details form is new, so it follows the new error model: nil
        // still means "not a directory", anything else raises.
        try {
            auto entries = fileops::listDetails(path);
            if (!entries)
                return Value::nilVal();
            Value resultVal = Value::listVal();
            ObjList* result = asList(resultVal);
            for (const auto& e : *entries)
                result->append(statDict(e.info, &e.name));
            return resultVal;
        } catch (const fileops::FileOpError& e) {
            vm().raiseException(fileOpErrorValue(e, fileIOExceptionType()));
            return Value::nilVal();
        }
    }

    std::filesystem::path p(path);
    std::error_code ec;
    if (!std::filesystem::is_directory(p, ec))
        return Value::nilVal();

    // Names sorted for a deterministic listing (directory_iterator order is
    // filesystem-dependent); directories get a trailing '/', so one list
    // carries the shape of the directory without a second stat pass.
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(p, ec)) {
        std::string name = entry.path().filename().string();
        if (entry.is_directory(ec))
            name += "/";
        names.push_back(std::move(name));
    }
    std::sort(names.begin(), names.end());

    Value resultVal = Value::listVal();
    ObjList* result = asList(resultVal);
    for (const auto& n : names)
        result->append(Value::stringVal(toUnicodeString(n)));
    return resultVal;
}

Value ModuleFileIO::fileio_delete_file_builtin(ArgsView args)
{
    if (args.size() != 1 || !isString(args[0]))
        throw std::invalid_argument("fileio.delete_file expects path string");
    std::filesystem::path p(toUTF8StdString(asStringObj(args[0])->s));
    std::error_code ec;
    if (!std::filesystem::exists(p, ec))
        return Value::falseVal();
    if (ec)
        return Value::falseVal();
    bool isFile = std::filesystem::is_regular_file(p, ec);
    if (ec || !isFile)
        return Value::falseVal();
    bool removed = std::filesystem::remove(p, ec);
    if (ec)
        return Value::falseVal();
    return removed ? Value::trueVal() : Value::falseVal();
}

Value ModuleFileIO::fileio_create_dir_builtin(ArgsView args)
{
    if (args.size() < 1 || args.size() > 2 || !isString(args[0]))
        throw std::invalid_argument("fileio.create_dir expects path string and optional recurse bool");
    bool recurse = false;
    if (args.size() == 2)
        recurse = args[1].asBool();
    std::filesystem::path p(toUTF8StdString(asStringObj(args[0])->s));
    std::error_code ec;
    bool created = recurse ? std::filesystem::create_directories(p, ec)
                           : std::filesystem::create_directory(p, ec);
    if (ec)
        return Value::falseVal();
    if (created)
        return Value::trueVal();
    ec.clear();
    bool exists = std::filesystem::exists(p, ec);
    if (ec || !exists)
        return Value::falseVal();
    ec.clear();
    bool isDir = std::filesystem::is_directory(p, ec);
    if (ec || !isDir)
        return Value::falseVal();
    return Value::trueVal();
}

Value ModuleFileIO::fileio_dir_exists_builtin(ArgsView args)
{
    if (args.size() != 1 || !isString(args[0]))
        throw std::invalid_argument("fileio.dir_exists expects path string");
    std::filesystem::path p(toUTF8StdString(asStringObj(args[0])->s));
    std::error_code ec;
    return std::filesystem::is_directory(p, ec) ? Value::trueVal() : Value::falseVal();
}

Value ModuleFileIO::fileio_delete_dir_builtin(ArgsView args)
{
    if (args.size() < 1 || args.size() > 2 || !isString(args[0]))
        throw std::invalid_argument("fileio.delete_dir expects path string and optional recurse bool");
    bool recurse = false;
    if (args.size() == 2)
        recurse = args[1].asBool();
    std::filesystem::path p(toUTF8StdString(asStringObj(args[0])->s));
    std::error_code ec;
    if (!std::filesystem::exists(p, ec))
        return Value::falseVal();
    if (ec)
        return Value::falseVal();
    ec.clear();
    bool isDir = std::filesystem::is_directory(p, ec);
    if (ec || !isDir)
        return Value::falseVal();
    if (recurse) {
        uintmax_t removed = std::filesystem::remove_all(p, ec);
        if (ec)
            return Value::falseVal();
        return removed > 0 ? Value::trueVal() : Value::falseVal();
    }
    bool removed = std::filesystem::remove(p, ec);
    if (ec)
        return Value::falseVal();
    return removed ? Value::trueVal() : Value::falseVal();
}

Value ModuleFileIO::fileio_file_size_builtin(ArgsView args)
{
    if (args.size() != 1 || !isString(args[0]))
        throw std::invalid_argument("fileio.file_size expects path string");
    std::filesystem::path p(toUTF8StdString(asStringObj(args[0])->s));
    std::error_code ec;
    if (!std::filesystem::is_regular_file(p, ec))
        return Value::intVal(0);
    const uintmax_t size = std::filesystem::file_size(p, ec);
    return Value::intVal(ec ? 0 : static_cast<int64_t>(size));
}

Value ModuleFileIO::fileio_absolute_file_path_builtin(ArgsView args)
{
    if (args.size() != 1 || !isString(args[0]))
        throw std::invalid_argument("fileio.absolute_file_path expects path string");
    std::filesystem::path p(toUTF8StdString(asStringObj(args[0])->s));
    auto abs = std::filesystem::absolute(p);
    return Value::stringVal(toUnicodeString(abs.string()));
}

Value ModuleFileIO::fileio_path_directory_builtin(ArgsView args)
{
    if (args.size() != 1 || !isString(args[0]))
        throw std::invalid_argument("fileio.path_directory expects path string");
    std::filesystem::path p(toUTF8StdString(asStringObj(args[0])->s));
    return Value::stringVal(toUnicodeString(p.parent_path().string()));
}

Value ModuleFileIO::fileio_path_file_builtin(ArgsView args)
{
    if (args.size() != 1 || !isString(args[0]))
        throw std::invalid_argument("fileio.path_file expects path string");
    std::filesystem::path p(toUTF8StdString(asStringObj(args[0])->s));
    return Value::stringVal(toUnicodeString(p.filename().string()));
}

Value ModuleFileIO::fileio_file_extension_builtin(ArgsView args)
{
    if (args.size() != 1 || !isString(args[0]))
        throw std::invalid_argument("fileio.file_extension expects path string");
    std::filesystem::path p(toUTF8StdString(asStringObj(args[0])->s));
    auto ext = p.extension().string();
    if (!ext.empty() && ext[0] == '.') ext.erase(0,1);
    return Value::stringVal(toUnicodeString(ext));
}

Value ModuleFileIO::fileio_file_without_extension_builtin(ArgsView args)
{
    if (args.size() != 1 || !isString(args[0]))
        throw std::invalid_argument("fileio.file_without_extension expects path string");
    std::filesystem::path p(toUTF8StdString(asStringObj(args[0])->s));
    return Value::stringVal(toUnicodeString(p.replace_extension().string()));
}

// ---------------------------------------------------------------------------
// Path operations. Each marshals its arguments here, on the calling thread,
// then runs a fileops call on the I/O worker (the same FIFO as the handle
// ops, so it is ordered after writes already queued). Failures resolve the
// future to a FileIOException -- see PendingIOOp::task.

Value ModuleFileIO::fileIOExceptionType()
{
    return vm().loadGlobal(toUnicodeString("FileIOException")).value();
}

Value ModuleFileIO::raiseInvalid(const std::string& op, const std::string& reason)
{
    Value detail = Value::dictVal();
    asDict(detail)->store(str("code"), str("EINVAL"));
    asDict(detail)->store(str("op"), str(op));
    asDict(detail)->store(str("reason"), str(reason));
    Value exc = Value::exceptionVal(str("EINVAL: " + op + ": " + reason),
                                    fileIOExceptionType(), Value::nilVal(), detail);
    vm().raiseException(exc);
    return Value::nilVal();
}

Value ModuleFileIO::submitPathTask(std::string opName, std::function<Value()> task, bool async)
{
    PendingIOOp op;
    op.type = PendingIOOp::Type::PathTask;
    op.opName = std::move(opName);
    op.task = std::move(task);
    op.errorType = fileIOExceptionType();
    Value fut = AsyncIOManager::instance().submit(std::move(op));
    return async ? fut : awaitInVM(fut);
}

namespace {

// A path argument, or nullopt if it is not a (non-empty) string.
std::optional<std::string> pathArg(const ArgsView& args, size_t i)
{
    if (!args.has(i) || !isString(args[i]))
        return std::nullopt;
    std::string p = toUTF8StdString(asStringObj(args[i])->s);
    if (p.empty())
        return std::nullopt;
    return p;
}

bool boolArg(const ArgsView& args, size_t i)
{
    return args.has(i) && args[i].isBool() && args[i].asBool();
}

}  // namespace

Value ModuleFileIO::fileio_rename_builtin(ArgsView args)
{
    auto from = pathArg(args, 0), to = pathArg(args, 1);
    if (!from || !to)
        return raiseInvalid("rename", "expects two non-empty path strings");
    const bool replace = boolArg(args, 2);
    return submitPathTask("rename", [from = *from, to = *to, replace] {
        fileops::rename(from, to, replace);
        return Value::nilVal();
    }, asyncArg(args, 3));
}

Value ModuleFileIO::fileio_copy_builtin(ArgsView args)
{
    auto from = pathArg(args, 0), to = pathArg(args, 1);
    if (!from || !to)
        return raiseInvalid("copy", "expects two non-empty path strings");
    const bool recurse = boolArg(args, 2);
    const bool replace = boolArg(args, 3);
    return submitPathTask("copy", [from = *from, to = *to, recurse, replace] {
        fileops::copy(from, to, recurse, replace);
        return Value::nilVal();
    }, asyncArg(args, 4));
}

Value ModuleFileIO::fileio_remove_builtin(ArgsView args)
{
    auto path = pathArg(args, 0);
    if (!path)
        return raiseInvalid("remove", "expects a non-empty path string");
    const bool recurse = boolArg(args, 1);
    return submitPathTask("remove", [path = *path, recurse] {
        fileops::remove(path, recurse);
        return Value::nilVal();
    }, asyncArg(args, 2));
}

Value ModuleFileIO::fileio_stat_builtin(ArgsView args)
{
    auto path = pathArg(args, 0);
    if (!path)
        return raiseInvalid("stat", "expects a non-empty path string");
    return submitPathTask("stat", [path = *path] {
        auto info = fileops::stat(path);
        return info ? statDict(*info) : Value::nilVal();
    }, asyncArg(args, 1));
}

Value ModuleFileIO::fileio_write_file_builtin(ArgsView args)
{
    auto path = pathArg(args, 0);
    if (!path || !args.has(1))
        return raiseInvalid("write_file", "expects a non-empty path string and data");
    std::string format = "text";
    if (args.has(2) && isString(args[2]))
        format = toUTF8StdString(asStringObj(args[2])->s);
    if (format != "text" && format != "binary")
        return raiseInvalid("write_file", "format must be 'text' or 'binary'");
    const bool atomic = !args.has(3) || !args[3].isBool() || args[3].asBool();

    // Marshal here: the Value must not reach the worker.
    std::string bytes;
    if (const char* error = toBytes(args[1], format == "binary", bytes))
        return raiseInvalid("write_file", error);
    return submitPathTask("write_file", [path = *path, bytes = std::move(bytes), atomic] {
        fileops::writeFile(path, bytes, atomic);
        return Value::nilVal();
    }, asyncArg(args, 4));
}

Value ModuleFileIO::fileio_file_tag_builtin(ArgsView args)
{
    auto path = pathArg(args, 0);
    if (!path)
        return raiseInvalid("file_tag", "expects a non-empty path string");
    return submitPathTask("file_tag", [path = *path] {
        return str(fileops::fileTag(path));
    }, asyncArg(args, 1));
}

Value ModuleFileIO::fileio_content_tag_builtin(ArgsView args)
{
    // Pure computation: no I/O, so no worker round trip.
    if (!args.has(0))
        return raiseInvalid("content_tag", "expects a string or a list of bytes");
    std::string bytes;
    if (isString(args[0])) {
        bytes = toUTF8StdString(asStringObj(args[0])->s);
    } else if (isList(args[0])) {
        if (const char* error = toBytes(args[0], /*binary=*/true, bytes))
            return raiseInvalid("content_tag", error);
    } else {
        return raiseInvalid("content_tag", "expects a string or a list of bytes");
    }
    return str(fileops::contentTag(bytes));
}
