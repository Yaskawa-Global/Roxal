#pragma once

#include "BuiltinModule.h"
#include "Value.h"
#include "Object.h"

namespace roxal {

class ModuleFileIO : public BuiltinModule {
public:
    ModuleFileIO();
    virtual ~ModuleFileIO() {
        if (!moduleTypeValue.isNil())
            asModuleType(moduleTypeValue)->vars.clear();
        moduleTypeValue = Value::nilVal();
    }

    void registerBuiltins(VM& vm) override;

    inline Value moduleType() const { return moduleTypeValue; }

    // builtin function implementations
    Value fileio_open_builtin(VM& vm, ArgsView args);
    Value fileio_close_builtin(VM& vm, ArgsView args);
    Value fileio_isopen_builtin(VM& vm, ArgsView args);
    Value fileio_moredata_builtin(VM& vm, ArgsView args);
    Value fileio_read_builtin(VM& vm, ArgsView args);
    Value fileio_readline_builtin(VM& vm, ArgsView args);
    Value fileio_readfile_builtin(VM& vm, ArgsView args);
    Value fileio_write_builtin(VM& vm, ArgsView args);
    Value fileio_fileexists_builtin(VM& vm, ArgsView args);
    Value fileio_direxists_builtin(VM& vm, ArgsView args);
    Value fileio_filesize_builtin(VM& vm, ArgsView args);
    Value fileio_abspathfile_builtin(VM& vm, ArgsView args);
    Value fileio_pathdir_builtin(VM& vm, ArgsView args);
    Value fileio_pathfile_builtin(VM& vm, ArgsView args);
    Value fileio_fileext_builtin(VM& vm, ArgsView args);
    Value fileio_filewoext_builtin(VM& vm, ArgsView args);

private:
    Value moduleTypeValue; // ObjModuleType*
};

}
