#pragma once

#include "BuiltinModule.h"
#include "Value.h"
#include "Object.h"

namespace roxal {

class ModuleFileIO : public BuiltinModule {
public:
    ModuleFileIO();
    virtual ~ModuleFileIO();

    void registerBuiltins(VM& vm) override;

    inline Value moduleType() const { return moduleTypeValue; }

    // builtin function implementations
    Value fileio_open_builtin(ArgsView args);
    Value fileio_close_builtin(ArgsView args);
    Value fileio_isopen_builtin(ArgsView args);
    Value fileio_moredata_builtin(ArgsView args);
    Value fileio_read_builtin(ArgsView args);
    Value fileio_readline_builtin(ArgsView args);
    Value fileio_readfile_builtin(ArgsView args);
    Value fileio_write_builtin(ArgsView args);
    Value fileio_flush_builtin(ArgsView args);
    Value fileio_fileexists_builtin(ArgsView args);
    Value fileio_deletefile_builtin(ArgsView args);
    Value fileio_createdir_builtin(ArgsView args);
    Value fileio_direxists_builtin(ArgsView args);
    Value fileio_deletedir_builtin(ArgsView args);
    Value fileio_filesize_builtin(ArgsView args);
    Value fileio_abspathfile_builtin(ArgsView args);
    Value fileio_pathdir_builtin(ArgsView args);
    Value fileio_pathfile_builtin(ArgsView args);
    Value fileio_fileext_builtin(ArgsView args);
    Value fileio_filewoext_builtin(ArgsView args);

private:
    Value moduleTypeValue; // ObjModuleType*
};

}
