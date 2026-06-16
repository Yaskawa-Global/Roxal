// Plugin entry point for the dynamically-loaded qt module (libroxalqt.so).
//
// The roxal binary itself links no Qt; the qt module and its Qt6 dependencies live
// here and are loaded with dlopen only when a script does `import qt` (see the qt
// factory in VM.cpp). The core resolves this one C-ABI symbol via dlsym, calls it to
// construct the ModuleQt BuiltinModule, then drives it through the usual BuiltinModule
// virtuals. Core symbols (VM/GC/Object/...) referenced by the Qt code are resolved at
// load time from the host executable, which is linked with -rdynamic (ENABLE_EXPORTS).
#ifdef ROXAL_ENABLE_QT

#include "ModuleQt.h"
#include "BuiltinModule.h"

// extern "C" → unmangled, default-visibility export so the core can dlsym it by name.
// Returns a heap BuiltinModule the core adopts into a ptr<> (shared_ptr); ModuleQt has a
// virtual destructor, so its eventual delete dispatches back into this plugin correctly.
extern "C" roxal::BuiltinModule* roxal_qt_create_module()
{
    return new roxal::ModuleQt();
}

#endif // ROXAL_ENABLE_QT
