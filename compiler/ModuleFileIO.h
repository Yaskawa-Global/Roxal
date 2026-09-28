#pragma once

#include "BuiltinModule.h"
#include "Value.h"
#include "Object.h"

#include <functional>
#include <string>

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
    Value fileio_list_dir_builtin(ArgsView args);
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

    // Path operations that raise FileIOException (code in .detail) on failure.
    // All but content_tag run on the I/O worker, with the async= convention.
    Value fileio_rename_builtin(ArgsView args);
    Value fileio_copy_builtin(ArgsView args);
    Value fileio_remove_builtin(ArgsView args);
    Value fileio_stat_builtin(ArgsView args);
    Value fileio_write_file_builtin(ArgsView args);
    Value fileio_file_tag_builtin(ArgsView args);
    Value fileio_content_tag_builtin(ArgsView args);

private:
    // Await `future` inside the VM dispatcher (sys.wait(for=)'s machinery):
    // parks the Roxal thread, NOT the OS thread — under a bounded slice the thread
    // reports not-runnable and resumes on a later call; a host UI loop keeps
    // pumping. This is what makes async=false LOOK synchronous to the script
    // while the I/O worker does the actual work. Returns the resolved value
    // when the future is already ready, else suspends and returns nil (the
    // dispatch loop writes the real result into the call's result slot).
    Value awaitInVM(Value future);

    // Queue a path operation on the I/O worker and, unless `async`, await it
    // in the VM. The task must capture only plain C++ data (see PendingIOOp).
    Value submitPathTask(std::string opName, std::function<Value()> task, bool async);

    Value fileIOExceptionType();

    // Raise a FileIOException "EINVAL: <op>: <reason>" for a bad argument value
    // (catchable, unlike a C++ throw out of a builtin). Returns nil for the
    // builtin to return.
    Value raiseInvalid(const std::string& op, const std::string& reason);

    Value moduleTypeValue; // ObjModuleType*
};

}
