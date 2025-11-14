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
    Value fileio_is_open_builtin(ArgsView args);
    Value fileio_more_data_builtin(ArgsView args);
    Value fileio_read_builtin(ArgsView args);
    Value fileio_read_line_builtin(ArgsView args);
    Value fileio_read_file_builtin(ArgsView args);
    Value fileio_write_builtin(ArgsView args);
    Value fileio_flush_builtin(ArgsView args);
    Value fileio_file_exists_builtin(ArgsView args);
    Value fileio_delete_file_builtin(ArgsView args);
    Value fileio_create_dir_builtin(ArgsView args);
    Value fileio_dir_exists_builtin(ArgsView args);
    Value fileio_delete_dir_builtin(ArgsView args);
    Value fileio_file_size_builtin(ArgsView args);
    Value fileio_absolute_file_path_builtin(ArgsView args);
    Value fileio_path_directory_builtin(ArgsView args);
    Value fileio_path_file_builtin(ArgsView args);
    Value fileio_file_extension_builtin(ArgsView args);
    Value fileio_file_without_extension_builtin(ArgsView args);

private:
    Value moduleTypeValue; // ObjModuleType*
};

}
