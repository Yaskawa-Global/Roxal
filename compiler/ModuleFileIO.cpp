#include "ModuleFileIO.h"
#include "AsyncIOManager.h"
#include "VM.h"
#include "Object.h"
#include <sstream>
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
}

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

Value ModuleFileIO::fileio_close_builtin(ArgsView args)
{
    if (args.size() != 1 || !isFile(args[0]))
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
        // The pending future will be waited on by the async worker
        op.pendingFutures.push_back(asFuture(pendingFuture)->future);
        return AsyncIOManager::instance().submit(std::move(op));
    }

    // No pending operations - close synchronously
    {
        std::lock_guard<std::mutex> lock(f->mutex);
        if (f->file && f->file->is_open())
            f->file->close();
    }
    return Value::nilVal();
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
    if (args.size() != 1 || !isFile(args[0]))
        throw std::invalid_argument("fileio.read expects file handle");
    ObjFile* f = asFile(args[0]);

    // Submit async read operation
    PendingIOOp op;
    op.type = PendingIOOp::Type::FileRead;
    op.file = f;
    op.maxBytes = 4096;
    op.binary = f->binary;

    return AsyncIOManager::instance().submit(std::move(op));
}

Value ModuleFileIO::fileio_read_line_builtin(ArgsView args)
{
    if (args.size() != 1 || !isFile(args[0]))
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
    op.binary = false;

    return AsyncIOManager::instance().submit(std::move(op));
}

Value ModuleFileIO::fileio_read_file_builtin(ArgsView args)
{
    if (args.size() < 1 || args.size() > 2 || !isString(args[0]))
        throw std::invalid_argument("fileio.read_file expects path string and optional format");
    std::string format = "text";
    if (args.size() == 2) {
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

    return AsyncIOManager::instance().submit(std::move(op));
}

Value ModuleFileIO::fileio_write_builtin(ArgsView args)
{
    if (args.size() != 2 || !isFile(args[0]))
        throw std::invalid_argument("fileio.write expects file handle and data");
    ObjFile* f = asFile(args[0]);

    // Prepare write data synchronously (validation happens here)
    std::string writeData;
    if (f->binary) {
        if (!isList(args[1]))
            throw std::invalid_argument("fileio.write expects list of bytes in binary mode");
        ObjList* lst = asList(args[1]);
        writeData.reserve(static_cast<size_t>(lst->length()));
        for (int i = 0; i < lst->length(); ++i) {
            const Value& v = lst->elts.at(i);
            uint8_t b;
            if (v.isByte())
                b = v.asByte();
            else if (v.isInt()) {
                int iv = v.asInt();
                if (iv < 0 || iv > 255)
                    throw std::invalid_argument("fileio.write int out of byte range");
                b = static_cast<uint8_t>(iv);
            } else {
                throw std::invalid_argument("fileio.write expects list of bytes or ints");
            }
            writeData.push_back(static_cast<char>(b));
        }
    } else {
        writeData = toString(args[1]);
    }

    // Submit async write operation
    PendingIOOp op;
    op.type = PendingIOOp::Type::FileWrite;
    op.file = f;
    op.writeData = std::move(writeData);
    op.binary = f->binary;

    return AsyncIOManager::instance().submit(std::move(op));
}

Value ModuleFileIO::fileio_flush_builtin(ArgsView args)
{
    if (args.size() != 1 || !isFile(args[0]))
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
        // The pending future will be waited on by the async worker
        op.pendingFutures.push_back(asFuture(pendingFuture)->future);
        return AsyncIOManager::instance().submit(std::move(op));
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
    return std::filesystem::exists(p) && std::filesystem::is_regular_file(p) ? Value::trueVal() : Value::falseVal();
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
    return std::filesystem::exists(p) && std::filesystem::is_directory(p) ? Value::trueVal() : Value::falseVal();
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
    if (!std::filesystem::exists(p) || !std::filesystem::is_regular_file(p))
        return Value::intVal(0);
    return Value::intVal(static_cast<int32_t>(std::filesystem::file_size(p)));
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
