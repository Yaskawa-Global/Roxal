#include "ModuleFileIO.h"
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
    {
        link("open", [this](VM& vm, ArgsView a){ return fileio_open_builtin(vm,a); });
    }
    {
        link("close", [this](VM& vm, ArgsView a){ return fileio_close_builtin(vm,a); });
    }
    {
        link("flush", [this](VM& vm, ArgsView a){ return fileio_flush_builtin(vm,a); });
    }
    {
        link("isOpen", [this](VM& vm, ArgsView a){ return fileio_isopen_builtin(vm,a); });
    }
    {
        link("moreData", [this](VM& vm, ArgsView a){ return fileio_moredata_builtin(vm,a); });
    }
    {
        link("read", [this](VM& vm, ArgsView a){ return fileio_read_builtin(vm,a); });
    }
    {
        link("readLine", [this](VM& vm, ArgsView a){ return fileio_readline_builtin(vm,a); });
    }
    {
        link("readFile", [this](VM& vm, ArgsView a){ return fileio_readfile_builtin(vm,a); });
    }
    {
        link("write", [this](VM& vm, ArgsView a){ return fileio_write_builtin(vm,a); });
    }
    {
        link("fileExists", [this](VM& vm, ArgsView a){ return fileio_fileexists_builtin(vm,a); });
    }
    {
        link("deleteFile", [this](VM& vm, ArgsView a){ return fileio_deletefile_builtin(vm,a); });
    }
    {
        link("createDir", [this](VM& vm, ArgsView a){ return fileio_createdir_builtin(vm,a); });
    }
    {
        link("dirExists", [this](VM& vm, ArgsView a){ return fileio_direxists_builtin(vm,a); });
    }
    {
        link("deleteDir", [this](VM& vm, ArgsView a){ return fileio_deletedir_builtin(vm,a); });
    }
    {
        link("fileSize", [this](VM& vm, ArgsView a){ return fileio_filesize_builtin(vm,a); });
    }
    {
        link("absoluteFilePath", [this](VM& vm, ArgsView a){ return fileio_abspathfile_builtin(vm,a); });
    }
    {
        link("pathDirectory", [this](VM& vm, ArgsView a){ return fileio_pathdir_builtin(vm,a); });
    }
    {
        link("pathFile", [this](VM& vm, ArgsView a){ return fileio_pathfile_builtin(vm,a); });
    }
    {
        link("fileExtension", [this](VM& vm, ArgsView a){ return fileio_fileext_builtin(vm,a); });
    }
    {
        link("fileWithoutExtension", [this](VM& vm, ArgsView a){ return fileio_filewoext_builtin(vm,a); });
    }
}

Value ModuleFileIO::fileio_open_builtin(VM& vm, ArgsView args)
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
    std::ios_base::openmode mode = write ? (std::ios::in | std::ios::out) : std::ios::in;
    if (append)
        mode |= std::ios::app;
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

Value ModuleFileIO::fileio_close_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 1 || !isFile(args[0]))
        throw std::invalid_argument("fileio.close expects file handle");
    ObjFile* f = asFile(args[0]);
    if (f->file && f->file->is_open())
        f->file->close();
    return Value::nilVal();
}

Value ModuleFileIO::fileio_isopen_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 1 || !isFile(args[0]))
        throw std::invalid_argument("fileio.isOpen expects file handle");
    ObjFile* f = asFile(args[0]);
    return f->file && f->file->is_open() ? Value::trueVal() : Value::falseVal();
}

Value ModuleFileIO::fileio_moredata_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 1 || !isFile(args[0]))
        throw std::invalid_argument("fileio.moreData expects file handle");
    ObjFile* f = asFile(args[0]);
    if (!f->file || !f->file->is_open()) return Value::falseVal();
    int c = f->file->peek();
    return (c == std::char_traits<char>::eof()) ? Value::falseVal() : Value::trueVal();
}

Value ModuleFileIO::fileio_read_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 1 || !isFile(args[0]))
        throw std::invalid_argument("fileio.read expects file handle");
    ObjFile* f = asFile(args[0]);
    if (!f->file || !f->file->is_open()) return Value::nilVal();
    char buf[4096];
    f->file->read(buf, sizeof(buf));
    std::streamsize n = f->file->gcount();
    if (f->binary) {
        Value lst { Value::listVal() };
        asList(lst)->elts.reserve(static_cast<size_t>(n));
        for (std::streamsize i = 0; i < n; ++i)
            asList(lst)->elts.push_back(Value::byteVal(static_cast<uint8_t>(buf[i])));
        return lst;
    }
    std::string s(buf, static_cast<size_t>(n));
    return Value::stringVal(toUnicodeString(s));
}

Value ModuleFileIO::fileio_readline_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 1 || !isFile(args[0]))
        throw std::invalid_argument("fileio.readLine expects file handle");
    ObjFile* f = asFile(args[0]);
    if (!f->file || !f->file->is_open()) return Value::nilVal();
    if (f->binary) {
        Value exType = vm.loadGlobal(toUnicodeString("FileIOException")).value();
        Value msg = Value::stringVal(toUnicodeString("readLine requires text mode"));
        Value exc = Value::exceptionVal(msg, exType);
        vm.raiseException(exc);
        return Value::nilVal();
    }
    std::string line;
    if (!std::getline(*f->file, line))
        return Value::nilVal();
    return Value::stringVal(toUnicodeString(line));
}

Value ModuleFileIO::fileio_readfile_builtin(VM& vm, ArgsView args)
{
    if (args.size() < 1 || args.size() > 2 || !isString(args[0]))
        throw std::invalid_argument("fileio.readFile expects path string and optional format");
    std::string format = "text";
    if (args.size() == 2) {
        if (!isString(args[1]))
            throw std::invalid_argument("fileio.readFile format must be 'text' or 'binary'");
        format = toUTF8StdString(asStringObj(args[1])->s);
    }
    std::ios_base::openmode mode = std::ios::in;
    if (format == "binary")
        mode |= std::ios::binary;
    else if (format != "text")
        throw std::invalid_argument("fileio.readFile format must be 'text' or 'binary'");
    std::filesystem::path path = std::filesystem::path(toUTF8StdString(asStringObj(args[0])->s));
    std::ifstream in(path, mode);
    if (!in.is_open()) {
        Value exType = vm.loadGlobal(toUnicodeString("FileIOException")).value();
        Value msg = Value::stringVal(toUnicodeString("open failed"));
        Value exc = Value::exceptionVal(msg, exType);
        vm.raiseException(exc);
        return Value::nilVal();
    }
    std::stringstream ss;
    ss << in.rdbuf();
    std::string data = ss.str();
    if (format == "binary") {
        Value lst { Value::listVal() };
        asList(lst)->elts.reserve(data.size());
        for (char c : data)
            asList(lst)->elts.push_back(Value::byteVal(static_cast<uint8_t>(c)));
        return lst;
    }
    return Value::stringVal(toUnicodeString(data));
}

Value ModuleFileIO::fileio_write_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 2 || !isFile(args[0]))
        throw std::invalid_argument("fileio.write expects file handle and data");
    ObjFile* f = asFile(args[0]);
    if (!f->file || !f->file->is_open()) return Value::nilVal();
    if (f->binary) {
        if (!isList(args[1]))
            throw std::invalid_argument("fileio.write expects list of bytes in binary mode");
        ObjList* lst = asList(args[1]);
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
            f->file->put(static_cast<char>(b));
        }
    } else {
        std::string s = toString(args[1]);
        (*f->file) << s;
    }
    return Value::nilVal();
}

Value ModuleFileIO::fileio_flush_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 1 || !isFile(args[0]))
        throw std::invalid_argument("fileio.flush expects file handle");
    ObjFile* f = asFile(args[0]);
    if (!f->file || !f->file->is_open()) return Value::falseVal();
    f->file->flush();
    return f->file->good() ? Value::trueVal() : Value::falseVal();
}

Value ModuleFileIO::fileio_fileexists_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 1 || !isString(args[0]))
        throw std::invalid_argument("fileio.fileExists expects path string");
    std::filesystem::path p(toUTF8StdString(asStringObj(args[0])->s));
    return std::filesystem::exists(p) && std::filesystem::is_regular_file(p) ? Value::trueVal() : Value::falseVal();
}

Value ModuleFileIO::fileio_deletefile_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 1 || !isString(args[0]))
        throw std::invalid_argument("fileio.deleteFile expects path string");
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

Value ModuleFileIO::fileio_createdir_builtin(VM& vm, ArgsView args)
{
    if (args.size() < 1 || args.size() > 2 || !isString(args[0]))
        throw std::invalid_argument("fileio.createDir expects path string and optional recurse bool");
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

Value ModuleFileIO::fileio_direxists_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 1 || !isString(args[0]))
        throw std::invalid_argument("fileio.dirExists expects path string");
    std::filesystem::path p(toUTF8StdString(asStringObj(args[0])->s));
    return std::filesystem::exists(p) && std::filesystem::is_directory(p) ? Value::trueVal() : Value::falseVal();
}

Value ModuleFileIO::fileio_deletedir_builtin(VM& vm, ArgsView args)
{
    if (args.size() < 1 || args.size() > 2 || !isString(args[0]))
        throw std::invalid_argument("fileio.deleteDir expects path string and optional recurse bool");
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

Value ModuleFileIO::fileio_filesize_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 1 || !isString(args[0]))
        throw std::invalid_argument("fileio.fileSize expects path string");
    std::filesystem::path p(toUTF8StdString(asStringObj(args[0])->s));
    if (!std::filesystem::exists(p) || !std::filesystem::is_regular_file(p))
        return Value::intVal(0);
    return Value::intVal(static_cast<int32_t>(std::filesystem::file_size(p)));
}

Value ModuleFileIO::fileio_abspathfile_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 1 || !isString(args[0]))
        throw std::invalid_argument("fileio.absoluteFilePath expects path string");
    std::filesystem::path p(toUTF8StdString(asStringObj(args[0])->s));
    auto abs = std::filesystem::absolute(p);
    return Value::stringVal(toUnicodeString(abs.string()));
}

Value ModuleFileIO::fileio_pathdir_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 1 || !isString(args[0]))
        throw std::invalid_argument("fileio.pathDirectory expects path string");
    std::filesystem::path p(toUTF8StdString(asStringObj(args[0])->s));
    return Value::stringVal(toUnicodeString(p.parent_path().string()));
}

Value ModuleFileIO::fileio_pathfile_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 1 || !isString(args[0]))
        throw std::invalid_argument("fileio.pathFile expects path string");
    std::filesystem::path p(toUTF8StdString(asStringObj(args[0])->s));
    return Value::stringVal(toUnicodeString(p.filename().string()));
}

Value ModuleFileIO::fileio_fileext_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 1 || !isString(args[0]))
        throw std::invalid_argument("fileio.fileExtension expects path string");
    std::filesystem::path p(toUTF8StdString(asStringObj(args[0])->s));
    auto ext = p.extension().string();
    if (!ext.empty() && ext[0] == '.') ext.erase(0,1);
    return Value::stringVal(toUnicodeString(ext));
}

Value ModuleFileIO::fileio_filewoext_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 1 || !isString(args[0]))
        throw std::invalid_argument("fileio.fileWithoutExtension expects path string");
    std::filesystem::path p(toUTF8StdString(asStringObj(args[0])->s));
    return Value::stringVal(toUnicodeString(p.replace_extension().string()));
}
