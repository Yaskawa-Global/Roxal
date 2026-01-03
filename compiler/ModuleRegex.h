#pragma once

#include "BuiltinModule.h"
#include "Value.h"
#include "Object.h"

namespace roxal {

// Forward declaration - defined in ModuleRegex.cpp
struct RegexWrapper;

class ModuleRegex : public BuiltinModule {
public:
    ModuleRegex();
    virtual ~ModuleRegex();

    void registerBuiltins(VM& vm) override;

    inline Value moduleType() const { return moduleTypeValue; }

    // Module-level functions
    Value regex_compile_builtin(ArgsView args);

    // Regex object methods
    Value regex_init_builtin(ArgsView args);
    Value regex_test_builtin(ArgsView args);
    Value regex_exec_builtin(ArgsView args);

    // Helper to compile a pattern with flags
    static RegexWrapper* compilePattern(const std::string& pattern,
                                        const std::string& flags);

    // Helper to get RegexWrapper from an object instance
    static RegexWrapper* getWrapper(ObjectInstance* inst);

private:
    Value moduleTypeValue; // ObjModuleType*
};

} // namespace roxal
