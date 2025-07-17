#pragma once

#include "BuiltinModule.h"
#include "Value.h"
#include "Object.h"

namespace roxal {

class ModuleFileIO : public BuiltinModule {
public:
    ModuleFileIO();

    void registerBuiltins(VM& vm) override;

    inline ObjModuleType* moduleType() const { return asModuleType(moduleTypeValue); }

    // builtin function implementations
    Value fileio_open_builtin(VM& vm, int argCount, Value* args);
    Value fileio_close_builtin(VM& vm, int argCount, Value* args);
    Value fileio_isopen_builtin(VM& vm, int argCount, Value* args);
    Value fileio_moredata_builtin(VM& vm, int argCount, Value* args);
    Value fileio_read_builtin(VM& vm, int argCount, Value* args);
    Value fileio_readline_builtin(VM& vm, int argCount, Value* args);
    Value fileio_readfile_builtin(VM& vm, int argCount, Value* args);
    Value fileio_write_builtin(VM& vm, int argCount, Value* args);

private:
    Value moduleTypeValue; // ObjModuleType*
};

}
