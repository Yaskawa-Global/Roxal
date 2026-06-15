#ifdef ROXAL_ENABLE_QT

#include "ModuleQt.h"
#include "VM.h"
#include "Object.h"
#include "ModuleQtConvert.h"
#include "ObjQtObject.h"
#include "QtSignalHub.h"
#include "QtListModel.h"
#include "QtBindable.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QCoreApplication>
#include <QEventLoop>
#include <QByteArray>
#include <QObject>
#include <QMetaObject>
#include <QMetaMethod>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <QtGlobal>
#include <QMessageLogContext>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

using namespace roxal;

// True once any quit (window-close / Qt.quit() / engine.quit()) is requested for
// the current run. Process-static; reset on teardown. Read via ModuleQt::quitRequested().
static std::atomic<bool> s_quitRequested { false };

// ============================================================
// Host event-loop integration
// ============================================================
//
// The VM dispatch loop drives this; Qt never runs its own exec(). When the VM
// is idle (parked in Engine.run() / sleep) it blocks in waitForEvents() so a Qt
// event wakes it immediately; while busy it calls pump() at a throttled cadence.
// Both run on the main thread with no VM lock held, so a slot may re-enter the VM.
namespace {

struct QtHostLoop : roxal::HostEventLoop {
    void waitForEvents(TimeDuration maxWait) override {
        // Block until a Qt event arrives or up to `maxWait`, clamped to a sane
        // ceiling so the VM rechecks its own deadlines / quit promptly even if no
        // Qt event arrives. processEvents() returns the instant an event is ready.
        int64_t ms = maxWait.microSecs() / 1000;
        if (ms < 1) ms = 1;
        if (ms > 50) ms = 50;
        QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents,
                                        static_cast<int>(ms));
    }

    void pump() override {
        // Non-blocking: drain whatever is ready and return immediately.
        QCoreApplication::processEvents(QEventLoop::AllEvents);
    }
};

// Resolve the QQmlApplicationEngine the Engine receiver (args[0]) wraps. The
// instance stores a heap QPointer (auto-nulling, non-owning) inside a ForeignPtr;
// returns a guaranteed-non-null engine or throws.
QQmlApplicationEngine* engineFromReceiver(ArgsView args, const char* method)
{
    if (args.size() < 1 || !isObjectInstance(args[0]))
        throw std::invalid_argument(std::string("Engine.") + method + " expects a receiver");
    ObjectInstance* inst = asObjectInstance(args[0]);
    Value nativeVal = inst->getProperty("_native");
    if (!isForeignPtr(nativeVal))
        throw std::runtime_error(std::string("Engine.") + method +
                                 "(): engine not initialized (call Engine() first)");
    auto* handle = static_cast<QPointer<QQmlApplicationEngine>*>(asForeignPtr(nativeVal)->ptr);
    QQmlApplicationEngine* engine = handle ? handle->data() : nullptr;
    if (!engine)
        throw std::runtime_error(std::string("Engine.") + method +
                                 "(): engine is no longer valid");
    return engine;
}

// Resolve the RoxalListModel a ListModel receiver (args[0]) wraps (same _native /
// QPointer-in-ForeignPtr pattern as Engine). Returns a guaranteed-non-null model
// or throws.
RoxalListModel* listModelFromReceiver(ArgsView args, const char* method)
{
    if (args.size() < 1 || !isObjectInstance(args[0]))
        throw std::invalid_argument(std::string("ListModel.") + method + " expects a receiver");
    ObjectInstance* inst = asObjectInstance(args[0]);
    Value nativeVal = inst->getProperty("_native");
    if (!isForeignPtr(nativeVal))
        throw std::runtime_error(std::string("ListModel.") + method +
                                 "(): model not initialized (call qt.ListModel(type) first)");
    auto* handle = static_cast<QPointer<RoxalListModel>*>(asForeignPtr(nativeVal)->ptr);
    RoxalListModel* model = handle ? handle->data() : nullptr;
    if (!model)
        throw std::runtime_error(std::string("ListModel.") + method +
                                 "(): model is no longer valid");
    return model;
}

// If `v` is a qt.ListModel handle, return its backing RoxalListModel (else nullptr).
// Used by set_context_property to hand QML the underlying QAbstractItemModel*.
RoxalListModel* roxalListModelPtr(const Value& v)
{
    if (!isObjectInstance(v)) return nullptr;
    ObjectInstance* inst = asObjectInstance(v);
    if (!isObjectType(inst->instanceType)) return nullptr;
    if (asObjectType(inst->instanceType)->name != icu::UnicodeString("ListModel")) return nullptr;
    Value nativeVal = inst->getProperty("_native");
    if (!isForeignPtr(nativeVal)) return nullptr;
    auto* handle = static_cast<QPointer<RoxalListModel>*>(asForeignPtr(nativeVal)->ptr);
    return handle ? handle->data() : nullptr;
}

// Connect each top-level window's `closing` signal so that closing a window
// unblocks run(). We deliberately do NOT use QGuiApplication::lastWindowClosed:
// it is only emitted while Qt's own exec() loop runs, and we drive Qt via
// processEvents(). A zero-arg functor is fine — Qt drops the QQuickCloseEvent*.
void connectRootWindowClose(QQmlApplicationEngine* engine, std::function<void()> onClose)
{
    if (!engine) return;
    const auto roots = engine->rootObjects();
    for (QObject* obj : roots) {
        if (auto* win = qobject_cast<QQuickWindow*>(obj)) {
            QObject::connect(win, &QQuickWindow::closing,
                             [onClose] { onClose(); });
        }
    }
}

// ---- Qt/QML message handling (qt.log_to_file / log_silence / log_to_stderr) ----
// A process-global qInstallMessageHandler that lets a Roxal script keep Qt/QML
// diagnostics (warnings, QML TypeErrors, …) out of the print()/stdout stream — by
// silencing them or redirecting them to a file. Roxal has no logging module yet, so
// this lives in the qt module. Thread-safe (Qt may log from non-main threads).

enum class QtLogMode { Default, Silence, File };
QtLogMode        s_logMode = QtLogMode::Default;
std::ofstream    s_logFile;
std::mutex       s_logMutex;
QtMessageHandler s_prevHandler   = nullptr;
bool             s_handlerInstalled = false;

std::string formatQtMsg(QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
{
    const char* label = "Message";
    switch (type) {
        case QtDebugMsg:    label = "Debug";    break;
        case QtInfoMsg:     label = "Info";     break;
        case QtWarningMsg:  label = "Warning";  break;
        case QtCriticalMsg: label = "Critical"; break;
        case QtFatalMsg:    label = "Fatal";    break;
    }
    std::string out = std::string(label) + ": " + std::string(msg.toUtf8().constData());
    if (ctx.file && ctx.file[0])
        out += " (" + std::string(ctx.file) + ":" + std::to_string(ctx.line) + ")";
    return out;
}

void roxalQtMessageHandler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
{
    std::lock_guard<std::mutex> lock(s_logMutex);
    const bool fatal = (type == QtFatalMsg);
    if (!fatal && s_logMode == QtLogMode::Silence)
        return;                                   // drop non-fatal messages
    const std::string line = formatQtMsg(type, ctx, msg);
    if (s_logMode == QtLogMode::File && s_logFile.is_open()) {
        s_logFile << line << '\n';
        s_logFile.flush();
        if (fatal) std::fprintf(stderr, "%s\n", line.c_str());  // always surface a fatal
    } else {
        std::fprintf(stderr, "%s\n", line.c_str());
    }
    if (fatal)
        std::abort();                             // preserve qFatal() semantics
}

// Install our handler if not already installed. CALLER MUST HOLD s_logMutex.
void installQtLogHandler()
{
    if (!s_handlerInstalled) {
        s_prevHandler = qInstallMessageHandler(roxalQtMessageHandler);
        s_handlerInstalled = true;
    }
}

// Restore Qt's default message handling and close any log file. Acquires s_logMutex.
void resetQtLogHandler()
{
    std::lock_guard<std::mutex> lock(s_logMutex);
    if (s_handlerInstalled) {
        qInstallMessageHandler(s_prevHandler);
        s_prevHandler = nullptr;
        s_handlerInstalled = false;
    }
    s_logMode = QtLogMode::Default;
    if (s_logFile.is_open())
        s_logFile.close();
}

// ---- QML/asset path resolution (script-relative, with module-path fallback) ----
// engine.load("ui.qml") resolves relative to the .rox script that called it (not the
// process working directory), then falls back to the Roxal module search paths, then
// the cwd. URLs (qrc:/, file:, scheme://) and absolute paths pass through unchanged.

// Directory of the .rox script currently executing the call (empty if unknown).
std::filesystem::path currentScriptDir()
{
    Thread* t = VM::thread.get();
    if (!t || t->frames.empty())
        return {};
    const Value& closure = t->frames.back().closure;
    if (!isClosure(closure))
        return {};
    ObjFunction* fn = asFunction(asClosure(closure)->function);
    if (!fn || !fn->chunk)
        return {};
    std::filesystem::path src(toUTF8StdString(fn->chunk->sourceName));
    return src.has_parent_path() ? src.parent_path() : std::filesystem::path{};
}

bool looksLikeUrl(const std::string& s)
{
    return s.rfind("qrc:", 0) == 0 || s.rfind("file:", 0) == 0 ||
           s.find("://") != std::string::npos;
}

QUrl resolveQmlUrl(const std::string& rawPath, const std::vector<std::string>& modulePaths)
{
    namespace fs = std::filesystem;
    if (looksLikeUrl(rawPath))
        return QUrl(QString::fromStdString(rawPath));          // already a URL
    const fs::path rel(rawPath);
    if (rel.is_absolute())
        return QUrl::fromLocalFile(QString::fromStdString(rawPath));

    const fs::path scriptDir = currentScriptDir();
    std::vector<fs::path> roots;
    if (!scriptDir.empty())
        roots.push_back(scriptDir);                            // (1) the calling script's dir
    for (const auto& mp : modulePaths)
        roots.emplace_back(mp);                                // (2) module search paths

    std::error_code ec;
    for (const auto& root : roots) {
        const fs::path cand = root / rel;
        if (fs::exists(cand, ec))
            return QUrl::fromLocalFile(QString::fromStdString(cand.string()));
    }
    if (fs::exists(rel, ec))                                    // (3) cwd (legacy)
        return QUrl::fromLocalFile(QString::fromStdString(rawPath));
    // Not found anywhere — name the script-relative candidate so the load error is
    // sensible (or the raw path if the script dir is unknown).
    if (!scriptDir.empty())
        return QUrl::fromLocalFile(QString::fromStdString((scriptDir / rel).string()));
    return QUrl::fromLocalFile(QString::fromStdString(rawPath));
}

} // namespace

// ============================================================
// Pimpl: all Qt state (no Qt types in the header)
// ============================================================

struct ModuleQt::Impl {
    QPointer<QGuiApplication> app;                          // non-owning; auto-nulls
    bool ownsApp { false };                                // true if we created it
    ptr<HostEventLoop> hostLoop;                            // QtHostLoop; shared with VM
    std::vector<std::unique_ptr<QQmlApplicationEngine>> engines; // owned
    bool quitRequested { false };                          // set by a slot / quit()
};

// ============================================================
// Module constructor/destructor/registration
// ============================================================

ModuleQt::ModuleQt()
    : impl_(std::make_unique<Impl>())
{
    moduleTypeValue = Value::objVal(newModuleTypeObj(toUnicodeString("qt")));
    ObjModuleType::allModules.push_back(moduleTypeValue);
}

ModuleQt::~ModuleQt()
{
    destroyModuleType(moduleTypeValue);
}

void ModuleQt::registerBuiltins(VM& vm)
{
    setVM(vm);

    linkMethod("Engine", "init",        [this](VM&, ArgsView a) { return engine_init_builtin(a); });
    linkMethod("Engine", "load",        [this](VM&, ArgsView a) { return engine_load_builtin(a); });
    linkMethod("Engine", "load_string", [this](VM&, ArgsView a) { return engine_load_string_builtin(a); });
    linkMethod("Engine", "run",         [this](VM&, ArgsView a) { return engine_run_builtin(a); });
    linkMethod("Engine", "quit",        [this](VM&, ArgsView a) { return engine_quit_builtin(a); });
    linkMethod("Engine", "find",        [this](VM&, ArgsView a) { return engine_find_builtin(a); });
    linkMethod("Engine", "root",        [this](VM&, ArgsView a) { return engine_root_builtin(a); });
    linkMethod("Engine", "set_context_property", [this](VM&, ArgsView a) { return engine_set_context_property_builtin(a); });

    linkMethod("ListModel", "init",        [this](VM&, ArgsView a) { return listmodel_init_builtin(a); });
    linkMethod("ListModel", "count",       [this](VM&, ArgsView a) { return listmodel_count_builtin(a); });
    linkMethod("ListModel", "row",         [this](VM&, ArgsView a) { return listmodel_row_builtin(a); });
    linkMethod("ListModel", "append",      [this](VM&, ArgsView a) { return listmodel_append_builtin(a); });
    linkMethod("ListModel", "insert",      [this](VM&, ArgsView a) { return listmodel_insert_builtin(a); });
    linkMethod("ListModel", "remove",      [this](VM&, ArgsView a) { return listmodel_remove_builtin(a); });
    linkMethod("ListModel", "move",        [this](VM&, ArgsView a) { return listmodel_move_builtin(a); });
    linkMethod("ListModel", "clear",       [this](VM&, ArgsView a) { return listmodel_clear_builtin(a); });
    linkMethod("ListModel", "set_rows",    [this](VM&, ArgsView a) { return listmodel_set_rows_builtin(a); });
    linkMethod("ListModel", "begin_reset", [this](VM&, ArgsView a) { return listmodel_begin_reset_builtin(a); });
    linkMethod("ListModel", "end_reset",   [this](VM&, ArgsView a) { return listmodel_end_reset_builtin(a); });
    linkMethod("ListModel", "row_changed", [this](VM&, ArgsView a) { return listmodel_row_changed_builtin(a); });
    linkMethod("ListModel", "cell_changed",[this](VM&, ArgsView a) { return listmodel_cell_changed_builtin(a); });
    linkMethod("ListModel", "set",         [this](VM&, ArgsView a) { return listmodel_set_builtin(a); });

    link("get",  [this](VM&, ArgsView a) { return qt_get_builtin(a); });
    link("set",  [this](VM&, ArgsView a) { return qt_set_builtin(a); });
    link("call", [this](VM&, ArgsView a) { return qt_call_builtin(a); });

    link("connect",    [this](VM&, ArgsView a) { return qt_connect_builtin(a); });
    link("on",         [this](VM&, ArgsView a) { return qt_on_builtin(a); });
    link("disconnect", [this](VM&, ArgsView a) { return qt_disconnect_builtin(a); });

    link("notify",     [this](VM&, ArgsView a) { return qt_notify_builtin(a); });

    link("log_to_file",   [this](VM&, ArgsView a) { return qt_log_to_file_builtin(a); });
    link("log_silence",   [this](VM&, ArgsView a) { return qt_log_silence_builtin(a); });
    link("log_to_stderr", [this](VM&, ArgsView a) { return qt_log_to_stderr_builtin(a); });
}

// ============================================================
// Lifecycle hooks
// ============================================================

void ModuleQt::onModuleLoaded(VM& vm)
{
    // Create the process-wide QGuiApplication once, before any QML loads. argc/argv
    // must outlive the application, so keep them in static storage.
    if (!QGuiApplication::instance()) {
        static int qtArgc = 1;
        static char qtArg0[] = "roxal";
        static char* qtArgv[] = { qtArg0, nullptr };
        impl_->app = new QGuiApplication(qtArgc, qtArgv);
        impl_->ownsApp = true;
    } else {
        impl_->app = qobject_cast<QGuiApplication*>(QGuiApplication::instance());
        impl_->ownsApp = false;
    }

    // NB: window-close is detected per-window via QQuickWindow::closing (see
    // connectRootWindowClose), not QGuiApplication::lastWindowClosed, which is
    // only emitted under Qt's exec() (we drive Qt via processEvents()).

    // Install the host-loop hook so the VM services Qt cooperatively (shared ptr<>).
    impl_->hostLoop = make_ptr<QtHostLoop>();
    vm.setHostEventLoop(impl_->hostLoop);

    // Register the signal-connection hub's GC root provider (keeps connected
    // Roxal callbacks / event types alive).
    QtSignalHub::instance().init();

    // Register the list-model hub's GC root provider (keeps each model's row list
    // and row type alive while the model is live).
    QtModelHub::instance().init();

    // Register the bindable-object hub's GC root provider (keeps each exposed object
    // alive while its QQmlPropertyMap wrapper is live).
    QtBindHub::instance().init();
}

void ModuleQt::onScriptComplete(VM& vm)
{
    // Deterministic, clean teardown at the end of the script run, while the VM and
    // Qt runtime (platform plugin, thread-local state) are still alive. Destroy the
    // engines (and their windows) and the application here — NOT at atexit.
    teardownQt(vm);
}

void ModuleQt::onModuleUnloading(VM& vm)
{
    // Runs from the VM destructor (atexit). By now onScriptComplete has normally
    // already torn Qt down. If anything remains (e.g. run() never completed), only
    // detach the hook and leak the Qt objects: destroying a QQuickWindow / engine /
    // QGuiApplication during static/atexit teardown crashes inside Qt because its
    // platform plugin and thread-local storage are already gone. Leaking at process
    // exit is safe (the OS reclaims it).
    vm.setHostEventLoop(nullptr);
    impl_->hostLoop.reset();
    // Uninstall our Qt message handler so it isn't called during static teardown
    // (it touches a static std::ofstream).
    resetQtLogHandler();
}

void ModuleQt::teardownQt(VM& vm)
{
    if (!impl_->app && impl_->engines.empty())
        return; // already torn down

    // Disconnect all signal relays first, so no Qt signal can invoke a Roxal closure
    // during the rest of teardown (and drop their Roxal refs while Qt is still alive).
    QtSignalHub::instance().shutdown();

    vm.setHostEventLoop(nullptr);
    impl_->hostLoop.reset();

    // Destroy the windows/engines BEFORE the list-model / bindable context objects.
    // QML re-evaluates some bindings as a window is torn down; if the model or
    // QQmlPropertyMap a binding refers to is already gone, QML logs
    // "TypeError: Cannot read property … of null" at exit. Keeping the context
    // objects alive until the windows are gone avoids those teardown warnings.
    impl_->engines.clear();             // destroy windows/engines while platform alive

    // Views/bindings are gone now — drop the models + bindable wrappers (and their
    // Roxal object refs).
    QtModelHub::instance().shutdown();
    QtBindHub::instance().shutdown();
    s_quitRequested.store(false, std::memory_order_relaxed);

    if (impl_->app && impl_->ownsApp)
        delete impl_->app.data();       // QPointer auto-nulls impl_->app afterwards
    impl_->ownsApp = false;

    // Restore default Qt message handling (uninstall our handler, close any log file).
    resetQtLogHandler();
}

// ============================================================
// Helpers
// ============================================================

bool ModuleQt::quitRequested() { return s_quitRequested.load(std::memory_order_relaxed); }

void ModuleQt::requestQuit()
{
    impl_->quitRequested = true;
    s_quitRequested.store(true, std::memory_order_relaxed);
    // Clear the parked thread's sleep so the dispatch loop resumes after run().
    // Single-threaded: this runs on the main thread, so VM::thread is the run thread.
    if (Thread* t = VM::thread.get())
        t->threadSleep = false;
}

// ============================================================
// Engine methods
// ============================================================

Value ModuleQt::engine_init_builtin(ArgsView args)
{
    if (args.size() < 1 || !isObjectInstance(args[0]))
        throw std::invalid_argument("Engine.init expects a receiver");
    if (!impl_->app)
        throw std::runtime_error("Engine.init(): Qt application not initialized");

    ObjectInstance* inst = asObjectInstance(args[0]);

    auto engine = std::make_unique<QQmlApplicationEngine>();
    QQmlApplicationEngine* enginePtr = engine.get();

    // QML Qt.quit() / Qt.exit() unblock run().
    QObject::connect(enginePtr, &QQmlApplicationEngine::quit,
                     [this] { requestQuit(); });
    QObject::connect(enginePtr, &QQmlApplicationEngine::exit,
                     [this](int) { requestQuit(); });

    impl_->engines.push_back(std::move(engine));

    // Non-owning, auto-nulling handle: a heap QPointer the ForeignPtr owns. GC
    // collecting the wrapper deletes only the QPointer (never the engine, which
    // impl_->engines owns); if the engine is later destroyed the QPointer nulls,
    // so engineFromReceiver fails cleanly instead of dereferencing a dangling ptr.
    auto* handle = new QPointer<QQmlApplicationEngine>(enginePtr);
    auto fp = newForeignPtrObj(static_cast<void*>(handle));
    fp->registerCleanup([](void* p) {
        delete static_cast<QPointer<QQmlApplicationEngine>*>(p);
    });
    inst->setProperty("_native", Value::objVal(std::move(fp)));
    return Value::nilVal();
}

Value ModuleQt::engine_load_builtin(ArgsView args)
{
    QQmlApplicationEngine* engine = engineFromReceiver(args, "load");
    if (args.size() < 2 || !isString(args[1]))
        throw std::invalid_argument("Engine.load(path) expects a string path");

    std::string path = toUTF8StdString(asStringObj(args[1])->s);
    // Resolve relative to the calling .rox script (then module paths, then cwd); URLs
    // and absolute paths pass through.
    QUrl url = resolveQmlUrl(path, vm().getModulePaths());
    engine->load(url);
    if (engine->rootObjects().isEmpty())
        throw std::runtime_error("Engine.load(): failed to load QML from '" + path +
                                 "' (resolved to " + url.toString().toStdString() +
                                 "; root must be a Window)");
    connectRootWindowClose(engine, [this] { requestQuit(); });
    return Value::nilVal();
}

Value ModuleQt::engine_load_string_builtin(ArgsView args)
{
    QQmlApplicationEngine* engine = engineFromReceiver(args, "load_string");
    if (args.size() < 2 || !isString(args[1]))
        throw std::invalid_argument("Engine.load_string(qml) expects a string");

    std::string qml = toUTF8StdString(asStringObj(args[1])->s);
    // Treat the inline QML as if it lived next to the calling script, so its relative
    // asset URLs (images, etc.) resolve relative to the script.
    QUrl baseUrl;
    const std::filesystem::path dir = currentScriptDir();
    if (!dir.empty())
        baseUrl = QUrl::fromLocalFile(QString::fromStdString((dir / "inline.qml").string()));
    engine->loadData(QByteArray::fromStdString(qml), baseUrl);
    if (engine->rootObjects().isEmpty())
        throw std::runtime_error("Engine.load_string(): failed to load QML (root must be a Window)");
    connectRootWindowClose(engine, [this] { requestQuit(); });
    return Value::nilVal();
}

Value ModuleQt::engine_run_builtin(ArgsView args)
{
    QQmlApplicationEngine* engine = engineFromReceiver(args, "run");
    if (engine->rootObjects().isEmpty())
        throw std::runtime_error("Engine.run(): no QML loaded (call load()/load_string() first)");

    Thread* thread = VM::thread.get();
    if (!thread)
        throw std::runtime_error("Engine.run(): no current thread");

    // Park the VM until a Qt slot (window-close / Qt.quit()) calls requestQuit().
    // We don't block in C++ here: setting threadSleep returns control to the
    // dispatch loop, which keeps dispatching Roxal events and pumps Qt via the
    // host-loop wait. If quit was already requested, don't park.
    if (!impl_->quitRequested) {
        thread->threadSleepUntil = TimePoint::max();
        thread->threadSleep = true;
    }
    return Value::nilVal();
}

Value ModuleQt::engine_quit_builtin(ArgsView args)
{
    engineFromReceiver(args, "quit"); // validate receiver
    requestQuit();
    return Value::nilVal();
}

Value ModuleQt::engine_find_builtin(ArgsView args)
{
    QQmlApplicationEngine* engine = engineFromReceiver(args, "find");
    if (args.size() < 2 || !isString(args[1]))
        throw std::invalid_argument("Engine.find(name) expects a string objectName");

    QString name = QString::fromStdString(toUTF8StdString(asStringObj(args[1])->s));
    // Search every root object (and its subtree) by objectName; first match wins.
    const auto roots = engine->rootObjects();
    for (QObject* root : roots) {
        if (!root) continue;
        if (root->objectName() == name)
            return qtObjectValue(root);
        if (QObject* child = root->findChild<QObject*>(name))
            return qtObjectValue(child);
    }
    return Value::nilVal(); // not found is normal, not an error
}

Value ModuleQt::engine_root_builtin(ArgsView args)
{
    QQmlApplicationEngine* engine = engineFromReceiver(args, "root");
    const auto roots = engine->rootObjects();
    if (roots.isEmpty())
        return Value::nilVal();
    return qtObjectValue(roots.first());
}

// ---- Module-level escape hatches: qt.get / qt.set / qt.call ----
// For property names that aren't valid Roxal identifiers (e.g. "z-order") or for
// fully-dynamic access. They reuse the same dynamic dispatch as native syntax.

Value ModuleQt::qt_get_builtin(ArgsView args)
{
    if (args.size() < 2 || !isQtObject(args[0]) || !isString(args[1]))
        throw std::invalid_argument("qt.get(item, name) expects an item and a string name");
    Value out;
    asQtObject(args[0])->tryGetDynamicProperty(args[0], asStringObj(args[1])->s, out);
    return out;
}

Value ModuleQt::qt_set_builtin(ArgsView args)
{
    if (args.size() < 3 || !isQtObject(args[0]) || !isString(args[1]))
        throw std::invalid_argument("qt.set(item, name, value) expects an item, a string name, and a value");
    asQtObject(args[0])->trySetDynamicProperty(asStringObj(args[1])->s, args[2]);
    return Value::nilVal();
}

Value ModuleQt::qt_call_builtin(ArgsView args)
{
    if (args.size() < 2 || !isQtObject(args[0]) || !isString(args[1]))
        throw std::invalid_argument("qt.call(item, name, args) expects an item, a string name, and an optional args list");
    std::vector<Value> callArgs;
    if (args.size() >= 3 && !args[2].isNil()) {
        if (!isList(args[2]))
            throw std::invalid_argument("qt.call(item, name, args): args must be a list");
        ObjList* l = asList(args[2]);
        for (int32_t i = 0; i < l->length(); ++i)
            callArgs.push_back(l->getElement(static_cast<size_t>(i)));
    }
    Value out;
    asQtObject(args[0])->tryInvokeDynamicMethod(asStringObj(args[1])->s,
                                                callArgs.empty() ? nullptr : callArgs.data(),
                                                static_cast<int>(callArgs.size()), out);
    return out;
}

// ---- Signal connections: qt.connect / qt.on / qt.disconnect ----
// For dynamic / non-identifier signal names; native `item.onSig(fn)` and
// `item.sig` (for `when … occurs`) are the preferred forms.

Value ModuleQt::qt_connect_builtin(ArgsView args)
{
    if (args.size() < 3 || !isQtObject(args[0]) || !isString(args[1]) || !isClosure(args[2]))
        throw std::invalid_argument("qt.connect(item, name, handler) expects an item, a string signal name, and a callable");
    QObject* o = asQtObject(args[0])->deref("qt.connect");
    QByteArray name = QByteArray::fromStdString(toUTF8StdString(asStringObj(args[1])->s));
    QMetaMethod sig = findQtSignal(o, name.constData());
    if (!sig.isValid())
        throw std::runtime_error("qt.connect: no signal '" + std::string(name.constData()) +
                                 "' on " + o->metaObject()->className());
    return Value::intVal(QtSignalHub::instance().connectCallback(o, sig, args[2]));
}

Value ModuleQt::qt_on_builtin(ArgsView args)
{
    if (args.size() < 2 || !isQtObject(args[0]) || !isString(args[1]))
        throw std::invalid_argument("qt.on(item, name) expects an item and a string signal name");
    QObject* o = asQtObject(args[0])->deref("qt.on");
    QByteArray name = QByteArray::fromStdString(toUTF8StdString(asStringObj(args[1])->s));
    QMetaMethod sig = findQtSignal(o, name.constData());
    if (!sig.isValid())
        throw std::runtime_error("qt.on: no signal '" + std::string(name.constData()) +
                                 "' on " + o->metaObject()->className());
    return QtSignalHub::instance().eventTypeFor(o, sig);
}

Value ModuleQt::qt_disconnect_builtin(ArgsView args)
{
    if (args.size() < 1 || !args[0].isInt())
        throw std::invalid_argument("qt.disconnect(connection) expects a connection id (from qt.connect / item.onSig)");
    QtSignalHub::instance().disconnectId(static_cast<int>(args[0].asInt()));
    return Value::nilVal();
}

// ============================================================
// Context properties + list models
// ============================================================

Value ModuleQt::engine_set_context_property_builtin(ArgsView args)
{
    QQmlApplicationEngine* engine = engineFromReceiver(args, "set_context_property");
    if (args.size() < 3 || !isString(args[1]))
        throw std::invalid_argument("Engine.set_context_property(name, value) expects a string name and a value");
    QString name = QString::fromStdString(toUTF8StdString(asStringObj(args[1])->s));
    const Value& v = args[2];
    if (RoxalListModel* model = roxalListModelPtr(v)) {
        engine->rootContext()->setContextProperty(name, static_cast<QObject*>(model));
    } else if (isQtObject(v)) {
        engine->rootContext()->setContextProperty(name, asQtObject(v)->deref("set_context_property"));
    } else if (isObjectInstance(v)) {
        // A plain Roxal object → a bindable QQmlPropertyMap (its public properties
        // become QML-bindable, read/write/notify). (ListModel/Engine handles are
        // caught above; this is for ordinary objects exposed as a backend/state.)
        RoxalPropertyMap* map = QtBindHub::instance().wrap(v);
        engine->rootContext()->setContextProperty(name, static_cast<QObject*>(map));
    } else {
        engine->rootContext()->setContextProperty(name, toQVariant(v));
    }
    return Value::nilVal();
}

Value ModuleQt::listmodel_init_builtin(ArgsView args)
{
    if (args.size() < 2 || !isObjectInstance(args[0]))
        throw std::invalid_argument("qt.ListModel(row_type) expects a receiver and a row type");
    ObjectInstance* inst = asObjectInstance(args[0]);

    Value rowTypeVal = args[1];
    if (!isObjectType(rowTypeVal))
        throw std::invalid_argument("qt.ListModel(row_type): the argument must be a type");
    ObjObjectType* rowType = asObjectType(rowTypeVal);
    if (rowType->isActor || rowType->isEnumeration)
        throw std::invalid_argument("qt.ListModel(row_type): row type must be an object type or interface "
                                    "(not an actor or enum)");

    // Build the model's own row list, optionally seeded with `initial` (each row
    // must be an instance of / implement the row type).
    auto listObj = newListObj();
    ObjList* rowList = listObj.get();
    Value rows = Value::objVal(std::move(listObj));
    if (args.size() >= 3 && !args[2].isNil()) {
        if (!isList(args[2]))
            throw std::invalid_argument("qt.ListModel(row_type, initial): initial must be a list of rows");
        ObjList* seed = asList(args[2]);
        for (int32_t i = 0; i < seed->length(); ++i) {
            Value e = seed->getElement(static_cast<size_t>(i));
            if (!isObjectInstance(e) ||
                !isSubtypeOf(asObjectType(asObjectInstance(e)->instanceType), rowType))
                throw std::runtime_error("qt.ListModel: an initial row does not match the row type");
            rowList->append(e);
        }
    }

    RoxalListModel* model = QtModelHub::instance().create(rowTypeVal, rows);

    // Non-owning, auto-nulling handle (the hub owns the model). Mirrors Engine.
    auto* handle = new QPointer<RoxalListModel>(model);
    auto fp = newForeignPtrObj(static_cast<void*>(handle));
    fp->registerCleanup([](void* p) {
        delete static_cast<QPointer<RoxalListModel>*>(p);
    });
    inst->setProperty("_native", Value::objVal(std::move(fp)));
    return Value::nilVal();
}

Value ModuleQt::listmodel_count_builtin(ArgsView args)
{
    return Value::intVal(listModelFromReceiver(args, "count")->count());
}

Value ModuleQt::listmodel_row_builtin(ArgsView args)
{
    RoxalListModel* model = listModelFromReceiver(args, "row");
    if (args.size() < 2 || !args[1].isInt())
        throw std::invalid_argument("ListModel.row(i) expects an int index");
    return model->rowAt(static_cast<int>(args[1].asInt()));
}

Value ModuleQt::listmodel_append_builtin(ArgsView args)
{
    RoxalListModel* model = listModelFromReceiver(args, "append");
    if (args.size() < 2 || !isObjectInstance(args[1]))
        throw std::invalid_argument("ListModel.append(row) expects a row object");
    if (!model->admits(args[1]))
        throw std::runtime_error("ListModel.append: the row does not match the model's row type");
    model->appendRow(args[1]);
    return Value::nilVal();
}

Value ModuleQt::listmodel_insert_builtin(ArgsView args)
{
    RoxalListModel* model = listModelFromReceiver(args, "insert");
    if (args.size() < 3 || !args[1].isInt() || !isObjectInstance(args[2]))
        throw std::invalid_argument("ListModel.insert(i, row) expects an int index and a row object");
    if (!model->admits(args[2]))
        throw std::runtime_error("ListModel.insert: the row does not match the model's row type");
    model->insertRow(static_cast<int>(args[1].asInt()), args[2]);
    return Value::nilVal();
}

Value ModuleQt::listmodel_remove_builtin(ArgsView args)
{
    RoxalListModel* model = listModelFromReceiver(args, "remove");
    if (args.size() < 2 || !args[1].isInt())
        throw std::invalid_argument("ListModel.remove(i) expects an int index");
    model->removeRow(static_cast<int>(args[1].asInt()));
    return Value::nilVal();
}

Value ModuleQt::listmodel_move_builtin(ArgsView args)
{
    RoxalListModel* model = listModelFromReceiver(args, "move");
    if (args.size() < 3 || !args[1].isInt() || !args[2].isInt())
        throw std::invalid_argument("ListModel.move(from, to) expects two int indices");
    model->moveRow(static_cast<int>(args[1].asInt()), static_cast<int>(args[2].asInt()));
    return Value::nilVal();
}

Value ModuleQt::listmodel_clear_builtin(ArgsView args)
{
    listModelFromReceiver(args, "clear")->clearRows();
    return Value::nilVal();
}

Value ModuleQt::listmodel_set_rows_builtin(ArgsView args)
{
    RoxalListModel* model = listModelFromReceiver(args, "set_rows");
    if (args.size() < 2 || !isList(args[1]))
        throw std::invalid_argument("ListModel.set_rows(rows) expects a list of rows");
    ObjList* src = asList(args[1]);
    std::vector<Value> rows;
    rows.reserve(static_cast<size_t>(src->length()));
    for (int32_t i = 0; i < src->length(); ++i) {
        Value e = src->getElement(static_cast<size_t>(i));
        if (!model->admits(e))
            throw std::runtime_error("ListModel.set_rows: a row does not match the model's row type");
        rows.push_back(e);
    }
    model->setRows(rows);
    return Value::nilVal();
}

Value ModuleQt::listmodel_begin_reset_builtin(ArgsView args)
{
    listModelFromReceiver(args, "begin_reset")->beginResetBatch();
    return Value::nilVal();
}

Value ModuleQt::listmodel_end_reset_builtin(ArgsView args)
{
    listModelFromReceiver(args, "end_reset")->endResetBatch();
    return Value::nilVal();
}

Value ModuleQt::listmodel_row_changed_builtin(ArgsView args)
{
    RoxalListModel* model = listModelFromReceiver(args, "row_changed");
    if (args.size() < 2 || !args[1].isInt())
        throw std::invalid_argument("ListModel.row_changed(i) expects an int index");
    model->rowChanged(static_cast<int>(args[1].asInt()));
    return Value::nilVal();
}

Value ModuleQt::listmodel_cell_changed_builtin(ArgsView args)
{
    RoxalListModel* model = listModelFromReceiver(args, "cell_changed");
    if (args.size() < 3 || !args[1].isInt() || !isString(args[2]))
        throw std::invalid_argument("ListModel.cell_changed(i, role) expects an int index and a string role");
    model->cellChanged(static_cast<int>(args[1].asInt()),
                       QByteArray::fromStdString(toUTF8StdString(asStringObj(args[2])->s)));
    return Value::nilVal();
}

Value ModuleQt::listmodel_set_builtin(ArgsView args)
{
    RoxalListModel* model = listModelFromReceiver(args, "set");
    if (args.size() < 4 || !args[1].isInt() || !isString(args[2]))
        throw std::invalid_argument("ListModel.set(i, role, value) expects an int index, a string role, and a value");
    model->setCell(static_cast<int>(args[1].asInt()),
                   QByteArray::fromStdString(toUTF8StdString(asStringObj(args[2])->s)),
                   args[3]);
    return Value::nilVal();
}

Value ModuleQt::qt_notify_builtin(ArgsView args)
{
    if (args.size() < 1 || !isObjectInstance(args[0]))
        throw std::invalid_argument("qt.notify(obj, name=nil) expects a Roxal object exposed via set_context_property");
    RoxalPropertyMap* map = QtBindHub::instance().lookup(asObjectInstance(args[0]));
    if (!map)
        return Value::nilVal();   // not exposed → nothing to push
    if (args.size() >= 2 && isString(args[1]))
        map->pushProperty(QString::fromStdString(toUTF8StdString(asStringObj(args[1])->s)));
    else
        map->pushAll();
    return Value::nilVal();
}

// ---- Qt/QML message logging ----

Value ModuleQt::qt_log_to_file_builtin(ArgsView args)
{
    if (args.size() < 1 || !isString(args[0]))
        throw std::invalid_argument("qt.log_to_file(path) expects a string path");
    std::string path = toUTF8StdString(asStringObj(args[0])->s);
    std::lock_guard<std::mutex> lock(s_logMutex);
    if (s_logFile.is_open())
        s_logFile.close();
    s_logFile.open(path, std::ios::out | std::ios::app);
    if (!s_logFile.is_open())
        throw std::runtime_error("qt.log_to_file: cannot open '" + path + "' for writing");
    s_logMode = QtLogMode::File;
    installQtLogHandler();   // caller holds s_logMutex
    return Value::nilVal();
}

Value ModuleQt::qt_log_silence_builtin(ArgsView)
{
    std::lock_guard<std::mutex> lock(s_logMutex);
    if (s_logFile.is_open())
        s_logFile.close();
    s_logMode = QtLogMode::Silence;
    installQtLogHandler();   // caller holds s_logMutex
    return Value::nilVal();
}

Value ModuleQt::qt_log_to_stderr_builtin(ArgsView)
{
    resetQtLogHandler();     // restores Qt's default handler + closes any log file
    return Value::nilVal();
}

#endif // ROXAL_ENABLE_QT
