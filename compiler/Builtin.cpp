#include "Builtin.h"
#include "VM.h"
#include "Object.h"
#include <stdexcept>

using namespace roxal;

bool roxal::callBuiltin(ObjClosure* closure, const CallSpec& callSpec)
{
    ObjFunction* function = closure->function;
    BuiltinWrapper* spec = static_cast<BuiltinWrapper*>(function->nativeSpec);

    if (!spec) {
        ptr<ast::Annotation> annot;
        for (const auto& a : function->annotations)
            if (a->name == "builtin") { annot = a; break; }
        if (!annot)
            throw std::runtime_error("builtin annotation missing");

        icu::UnicodeString builtinName = function->name;
        for (const auto& a : annot->args) {
            if (toUTF8StdString(a.first) == "name") {
                if (auto s = std::dynamic_pointer_cast<ast::Str>(a.second))
                    builtinName = s->str;
                else if (auto v = std::dynamic_pointer_cast<ast::Variable>(a.second))
                    builtinName = v->name;
                else
                    throw std::runtime_error("builtin name must be string or identifier");
            }
        }

        ObjModuleType* mod = asModuleType(function->moduleType);
        auto opt = mod->vars.load(builtinName);
        if (!opt.has_value())
            opt = VM::instance().loadGlobal(builtinName);
        if (!opt.has_value())
            throw std::runtime_error("builtin function '" + toUTF8StdString(builtinName) + "' not found");

        Value val = opt.value();
        if (!isNative(val))
            throw std::runtime_error("builtin value '" + toUTF8StdString(builtinName) + "' is not native function");

        ObjNative* nativeObj = asNative(val);
        spec = new BuiltinWrapper{ nativeObj->function, nativeObj->funcType, nativeObj->defaultValues };
        function->nativeSpec = spec;
    }

    return VM::instance().callNativeFn(spec->fn, spec->funcType, spec->defaults, callSpec);
}
