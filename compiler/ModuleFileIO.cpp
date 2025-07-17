#include "ModuleFileIO.h"
#include "VM.h"
#include "Object.h"
#include <sstream>
#include <fstream>
#include <filesystem>

using namespace roxal;

ModuleFileIO::ModuleFileIO()
{
    moduleTypeValue = objVal(moduleTypeVal(toUnicodeString("fileio")));
}

void ModuleFileIO::registerBuiltins(VM& vm)
{
    auto addFile = [&](const std::string& name, NativeFn fn,
                       ptr<type::Type> funcType = nullptr,
                       std::vector<Value> defaults = {}){
        vm.defineNative(name, fn, funcType, defaults);
        moduleType()->vars.store(toUnicodeString(name),
            objVal(nativeVal(fn, nullptr, funcType, defaults)));
    };

    addFile("open", [this](VM& vm, int c, Value* a){ return fileio_open_builtin(vm,c,a); });
    addFile("close", [this](VM& vm, int c, Value* a){ return fileio_close_builtin(vm,c,a); });
    addFile("isOpen", [this](VM& vm, int c, Value* a){ return fileio_isopen_builtin(vm,c,a); });
    addFile("moreData", [this](VM& vm, int c, Value* a){ return fileio_moredata_builtin(vm,c,a); });
    addFile("read", [this](VM& vm, int c, Value* a){ return fileio_read_builtin(vm,c,a); });
    addFile("readLine", [this](VM& vm, int c, Value* a){ return fileio_readline_builtin(vm,c,a); });
    addFile("readFile", [this](VM& vm, int c, Value* a){ return fileio_readfile_builtin(vm,c,a); });
    addFile("write", [this](VM& vm, int c, Value* a){ return fileio_write_builtin(vm,c,a); });
}

Value ModuleFileIO::fileio_open_builtin(VM& vm, int argCount, Value* args)
{
    if (argCount < 1 || argCount > 3 || !isString(args[0]))
        throw std::invalid_argument("fileio.open expects path string, optional append bool and format string");
    bool append = false;
    if (argCount >= 2)
        append = args[1].asBool();
    std::string format = "text";
    if (argCount == 3) {
        if (!isString(args[2]))
            throw std::invalid_argument("fileio.open format must be 'text' or 'binary'");
        format = toUTF8StdString(asString(args[2])->s);
    }
    bool binary = false;
    std::filesystem::path path = std::filesystem::path(toUTF8StdString(asString(args[0])->s));
    auto f = roxal::make_ptr<std::fstream>();
    std::ios_base::openmode mode = std::ios::in | std::ios::out;
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
        return falseVal();
    }
    return objVal(fileVal(f, binary));
}

Value ModuleFileIO::fileio_close_builtin(VM& vm, int argCount, Value* args)
{
    if (argCount != 1 || !isFile(args[0]))
        throw std::invalid_argument("fileio.close expects file handle");
    ObjFile* f = asFile(args[0]);
    if (f->file && f->file->is_open())
        f->file->close();
    return nilVal();
}

Value ModuleFileIO::fileio_isopen_builtin(VM& vm, int argCount, Value* args)
{
    if (argCount != 1 || !isFile(args[0]))
        throw std::invalid_argument("fileio.isOpen expects file handle");
    ObjFile* f = asFile(args[0]);
    return f->file && f->file->is_open() ? trueVal() : falseVal();
}

Value ModuleFileIO::fileio_moredata_builtin(VM& vm, int argCount, Value* args)
{
    if (argCount != 1 || !isFile(args[0]))
        throw std::invalid_argument("fileio.moreData expects file handle");
    ObjFile* f = asFile(args[0]);
    if (!f->file || !f->file->is_open()) return falseVal();
    int c = f->file->peek();
    return (c == std::char_traits<char>::eof()) ? falseVal() : trueVal();
}

Value ModuleFileIO::fileio_read_builtin(VM& vm, int argCount, Value* args)
{
    if (argCount != 1 || !isFile(args[0]))
        throw std::invalid_argument("fileio.read expects file handle");
    ObjFile* f = asFile(args[0]);
    if (!f->file || !f->file->is_open()) return nilVal();
    char buf[4096];
    f->file->read(buf, sizeof(buf));
    std::streamsize n = f->file->gcount();
    if (f->binary) {
        ObjList* lst = listVal();
        lst->elts.reserve(static_cast<size_t>(n));
        for (std::streamsize i = 0; i < n; ++i)
            lst->elts.push_back(byteVal(static_cast<uint8_t>(buf[i])));
        return objVal(lst);
    }
    std::string s(buf, static_cast<size_t>(n));
    return objVal(stringVal(toUnicodeString(s)));
}

Value ModuleFileIO::fileio_readline_builtin(VM& vm, int argCount, Value* args)
{
    if (argCount != 1 || !isFile(args[0]))
        throw std::invalid_argument("fileio.readLine expects file handle");
    ObjFile* f = asFile(args[0]);
    if (!f->file || !f->file->is_open()) return nilVal();
    if (f->binary) {
        Value exType = vm.loadGlobal(toUnicodeString("FileIOException")).value();
        Value msg = objVal(stringVal(toUnicodeString("readLine requires text mode")));
        Value exc = objVal(exceptionVal(msg, exType));
        vm.raiseException(exc);
        return nilVal();
    }
    std::string line;
    if (!std::getline(*f->file, line))
        return nilVal();
    return objVal(stringVal(toUnicodeString(line)));
}

Value ModuleFileIO::fileio_readfile_builtin(VM& vm, int argCount, Value* args)
{
    if (argCount < 1 || argCount > 2 || !isString(args[0]))
        throw std::invalid_argument("fileio.readFile expects path string and optional format");
    std::string format = "text";
    if (argCount == 2) {
        if (!isString(args[1]))
            throw std::invalid_argument("fileio.readFile format must be 'text' or 'binary'");
        format = toUTF8StdString(asString(args[1])->s);
    }
    std::ios_base::openmode mode = std::ios::in;
    if (format == "binary")
        mode |= std::ios::binary;
    else if (format != "text")
        throw std::invalid_argument("fileio.readFile format must be 'text' or 'binary'");
    std::filesystem::path path = std::filesystem::path(toUTF8StdString(asString(args[0])->s));
    std::ifstream in(path, mode);
    if (!in.is_open()) {
        Value exType = vm.loadGlobal(toUnicodeString("FileIOException")).value();
        Value msg = objVal(stringVal(toUnicodeString("open failed")));
        Value exc = objVal(exceptionVal(msg, exType));
        vm.raiseException(exc);
        return nilVal();
    }
    std::stringstream ss;
    ss << in.rdbuf();
    std::string data = ss.str();
    if (format == "binary") {
        ObjList* lst = listVal();
        lst->elts.reserve(data.size());
        for (char c : data)
            lst->elts.push_back(byteVal(static_cast<uint8_t>(c)));
        return objVal(lst);
    }
    return objVal(stringVal(toUnicodeString(data)));
}

Value ModuleFileIO::fileio_write_builtin(VM& vm, int argCount, Value* args)
{
    if (argCount != 2 || !isFile(args[0]))
        throw std::invalid_argument("fileio.write expects file handle and data");
    ObjFile* f = asFile(args[0]);
    if (!f->file || !f->file->is_open()) return nilVal();
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
    return nilVal();
}

