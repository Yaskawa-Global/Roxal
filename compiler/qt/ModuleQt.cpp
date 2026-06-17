#ifdef ROXAL_ENABLE_QT

#include "ModuleQt.h"
#include "VM.h"
#include "Object.h"
#include "ModuleQtConvert.h"
#include "ObjQtObject.h"
#include "QtSignalHub.h"
#include "QtListModel.h"
#include "QtTreeModel.h"
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
#include <QSortFilterProxyModel>
#include <QModelIndex>
#include <QHash>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QVariantMap>
#include <QTimer>
#include <QElapsedTimer>

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
    ensureQtUiThread(method);   // Engine.* is main-thread only (covers all engine methods)
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
    ensureQtUiThread(method);   // ListModel.* is main-thread only (covers all model methods)
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

// Resolve the RoxalTreeModel a TreeModel receiver (args[0]) wraps (same pattern).
RoxalTreeModel* treeModelFromReceiver(ArgsView args, const char* method)
{
    ensureQtUiThread(method);   // TreeModel.* is main-thread only
    if (args.size() < 1 || !isObjectInstance(args[0]))
        throw std::invalid_argument(std::string("TreeModel.") + method + " expects a receiver");
    ObjectInstance* inst = asObjectInstance(args[0]);
    Value nativeVal = inst->getProperty("_native");
    if (!isForeignPtr(nativeVal))
        throw std::runtime_error(std::string("TreeModel.") + method +
                                 "(): model not initialized (call qt.TreeModel(type) first)");
    auto* handle = static_cast<QPointer<RoxalTreeModel>*>(asForeignPtr(nativeVal)->ptr);
    RoxalTreeModel* model = handle ? handle->data() : nullptr;
    if (!model)
        throw std::runtime_error(std::string("TreeModel.") + method +
                                 "(): model is no longer valid");
    return model;
}

// If `v` is a qt.TreeModel handle, return its backing RoxalTreeModel (else nullptr).
RoxalTreeModel* roxalTreeModelPtr(const Value& v)
{
    if (!isObjectInstance(v)) return nullptr;
    ObjectInstance* inst = asObjectInstance(v);
    if (!isObjectType(inst->instanceType)) return nullptr;
    if (asObjectType(inst->instanceType)->name != icu::UnicodeString("TreeModel")) return nullptr;
    Value nativeVal = inst->getProperty("_native");
    if (!isForeignPtr(nativeVal)) return nullptr;
    auto* handle = static_cast<QPointer<RoxalTreeModel>*>(asForeignPtr(nativeVal)->ptr);
    return handle ? handle->data() : nullptr;
}

// Resolve the QSortFilterProxyModel a SortFilterModel receiver (args[0]) wraps.
QSortFilterProxyModel* sortFilterFromReceiver(ArgsView args, const char* method)
{
    ensureQtUiThread(method);   // SortFilterModel.* is main-thread only
    if (args.size() < 1 || !isObjectInstance(args[0]))
        throw std::invalid_argument(std::string("SortFilterModel.") + method + " expects a receiver");
    ObjectInstance* inst = asObjectInstance(args[0]);
    Value nativeVal = inst->getProperty("_native");
    if (!isForeignPtr(nativeVal))
        throw std::runtime_error(std::string("SortFilterModel.") + method +
                                 "(): not initialized (call qt.SortFilterModel(model) first)");
    auto* handle = static_cast<QPointer<QSortFilterProxyModel>*>(asForeignPtr(nativeVal)->ptr);
    QSortFilterProxyModel* proxy = handle ? handle->data() : nullptr;
    if (!proxy)
        throw std::runtime_error(std::string("SortFilterModel.") + method + "(): no longer valid");
    return proxy;
}

// If `v` is a qt.SortFilterModel handle, return its QSortFilterProxyModel (else nullptr).
QSortFilterProxyModel* roxalSortFilterPtr(const Value& v)
{
    if (!isObjectInstance(v)) return nullptr;
    ObjectInstance* inst = asObjectInstance(v);
    if (!isObjectType(inst->instanceType)) return nullptr;
    if (asObjectType(inst->instanceType)->name != icu::UnicodeString("SortFilterModel")) return nullptr;
    Value nativeVal = inst->getProperty("_native");
    if (!isForeignPtr(nativeVal)) return nullptr;
    auto* handle = static_cast<QPointer<QSortFilterProxyModel>*>(asForeignPtr(nativeVal)->ptr);
    return handle ? handle->data() : nullptr;
}

// Map a role NAME to its Qt role int via the proxy's source model roleNames(); -1 if absent.
static int sortFilterRoleInt(QSortFilterProxyModel* proxy, const QByteArray& name)
{
    if (!proxy->sourceModel()) return -1;
    const QHash<int, QByteArray> roles = proxy->sourceModel()->roleNames();
    for (auto it = roles.constBegin(); it != roles.constEnd(); ++it)
        if (it.value() == name) return it.key();
    return -1;
}

// Resolve the QQmlComponent a Component receiver (args[0]) wraps (same _native /
// QPointer-in-ForeignPtr pattern; the component is parented to its engine). Returns
// a guaranteed-non-null component or throws.
QQmlComponent* componentFromReceiver(ArgsView args, const char* method)
{
    ensureQtUiThread(method);   // Component.* is main-thread only
    if (args.size() < 1 || !isObjectInstance(args[0]))
        throw std::invalid_argument(std::string("Component.") + method + " expects a receiver");
    ObjectInstance* inst = asObjectInstance(args[0]);
    Value nativeVal = inst->getProperty("_native");
    if (!isForeignPtr(nativeVal))
        throw std::runtime_error(std::string("Component.") + method +
                                 "(): not a component (create one via engine.create_component())");
    auto* handle = static_cast<QPointer<QQmlComponent>*>(asForeignPtr(nativeVal)->ptr);
    QQmlComponent* comp = handle ? handle->data() : nullptr;
    if (!comp)
        throw std::runtime_error(std::string("Component.") + method +
                                 "(): component is no longer valid (its engine was destroyed)");
    return comp;
}

// Mint a fresh qt.Component instance wrapping `comp`. `componentType` is the
// module's Component ObjObjectType. The QQmlComponent is owned by Qt (parented to
// its engine); _native is a non-owning, auto-nulling QPointer inside a ForeignPtr
// (mirrors Engine/model handles), so GC collecting the wrapper never deletes it.
Value makeComponentValue(const Value& componentType, QQmlComponent* comp)
{
    auto inst = newObjectInstance(componentType);
    auto* handle = new QPointer<QQmlComponent>(comp);
    auto fp = newForeignPtrObj(static_cast<void*>(handle));
    fp->registerCleanup([](void* p) {
        delete static_cast<QPointer<QQmlComponent>*>(p);
    });
    inst->setProperty("_native", Value::objVal(std::move(fp)));
    return Value::objVal(std::move(inst));
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
    linkMethod("Engine", "run_for",     [this](VM&, ArgsView a) { return engine_run_for_builtin(a); });
    linkMethod("Engine", "quit",        [this](VM&, ArgsView a) { return engine_quit_builtin(a); });
    linkMethod("Engine", "find",        [this](VM&, ArgsView a) { return engine_find_builtin(a); });
    linkMethod("Engine", "root",        [this](VM&, ArgsView a) { return engine_root_builtin(a); });
    linkMethod("Engine", "set_context_property", [this](VM&, ArgsView a) { return engine_set_context_property_builtin(a); });
    linkMethod("Engine", "create_component",        [this](VM&, ArgsView a) { return engine_create_component_builtin(a); });
    linkMethod("Engine", "create_component_string", [this](VM&, ArgsView a) { return engine_create_component_string_builtin(a); });

    linkMethod("Component", "create", [this](VM&, ArgsView a) { return component_create_builtin(a); });

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

    linkMethod("TreeModel", "init",        [this](VM&, ArgsView a) { return treemodel_init_builtin(a); });
    linkMethod("TreeModel", "count",       [this](VM&, ArgsView a) { return treemodel_count_builtin(a); });
    linkMethod("TreeModel", "child",       [this](VM&, ArgsView a) { return treemodel_child_builtin(a); });
    linkMethod("TreeModel", "parent_of",   [this](VM&, ArgsView a) { return treemodel_parent_of_builtin(a); });
    linkMethod("TreeModel", "append",      [this](VM&, ArgsView a) { return treemodel_append_builtin(a); });
    linkMethod("TreeModel", "insert",      [this](VM&, ArgsView a) { return treemodel_insert_builtin(a); });
    linkMethod("TreeModel", "remove",      [this](VM&, ArgsView a) { return treemodel_remove_builtin(a); });
    linkMethod("TreeModel", "move",        [this](VM&, ArgsView a) { return treemodel_move_builtin(a); });
    linkMethod("TreeModel", "clear",       [this](VM&, ArgsView a) { return treemodel_clear_builtin(a); });
    linkMethod("TreeModel", "begin_reset", [this](VM&, ArgsView a) { return treemodel_begin_reset_builtin(a); });
    linkMethod("TreeModel", "end_reset",   [this](VM&, ArgsView a) { return treemodel_end_reset_builtin(a); });
    linkMethod("TreeModel", "node_changed",[this](VM&, ArgsView a) { return treemodel_node_changed_builtin(a); });
    linkMethod("TreeModel", "cell_changed",[this](VM&, ArgsView a) { return treemodel_cell_changed_builtin(a); });
    linkMethod("TreeModel", "set",         [this](VM&, ArgsView a) { return treemodel_set_builtin(a); });

    linkMethod("SortFilterModel", "init",         [this](VM&, ArgsView a) { return sortfilter_init_builtin(a); });
    linkMethod("SortFilterModel", "sort_by",      [this](VM&, ArgsView a) { return sortfilter_sort_by_builtin(a); });
    linkMethod("SortFilterModel", "filter",       [this](VM&, ArgsView a) { return sortfilter_filter_builtin(a); });
    linkMethod("SortFilterModel", "clear_filter", [this](VM&, ArgsView a) { return sortfilter_clear_filter_builtin(a); });
    linkMethod("SortFilterModel", "count",        [this](VM&, ArgsView a) { return sortfilter_count_builtin(a); });
    linkMethod("SortFilterModel", "row",          [this](VM&, ArgsView a) { return sortfilter_row_builtin(a); });

    link("get",  [this](VM&, ArgsView a) { return qt_get_builtin(a); });
    link("set",  [this](VM&, ArgsView a) { return qt_set_builtin(a); });
    link("call", [this](VM&, ArgsView a) { return qt_call_builtin(a); });

    link("connect",    [this](VM&, ArgsView a) { return qt_connect_builtin(a); });
    link("on",         [this](VM&, ArgsView a) { return qt_on_builtin(a); });
    link("disconnect", [this](VM&, ArgsView a) { return qt_disconnect_builtin(a); });

    link("process_events", [this](VM&, ArgsView a) { return qt_process_events_builtin(a); });
    link("every",          [this](VM&, ArgsView a) { return qt_every_builtin(a); });

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
    ensureQtUiThread("Engine");   // constructing an engine off the UI thread is a bug too
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

Value ModuleQt::engine_run_for_builtin(ArgsView args)
{
    engineFromReceiver(args, "run_for"); // validate receiver + UI-thread guard
    if (args.size() < 2 || !args[1].isNumber())
        throw std::invalid_argument("Engine.run_for(ms) expects a number of milliseconds");
    int ms = args[1].isInt() ? static_cast<int>(args[1].asInt())
                             : static_cast<int>(args[1].asReal());

    // Bounded cooperative pump: service Qt events (repaints, queued signals, qt.every
    // timers) for up to `ms`, then return — no parking. For interactive/REPL use, so
    // the script keeps control. Returns early if a quit (window close) is requested.
    if (ms <= 0) {
        QCoreApplication::processEvents(QEventLoop::AllEvents);
        return Value::nilVal();
    }
    QElapsedTimer elapsed;
    elapsed.start();
    while (!impl_->quitRequested) {
        qint64 remaining = ms - elapsed.elapsed();
        if (remaining <= 0) break;
        QCoreApplication::processEvents(QEventLoop::AllEvents | QEventLoop::WaitForMoreEvents,
                                        static_cast<int>(remaining));
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

// ---- Dynamic object creation: engine.create_component(_string) + Component.create ----

namespace {
// Look up the module's `Component` ObjObjectType (declared in qt.rox).
Value componentTypeOf(const Value& moduleTypeVal)
{
    auto typeOpt = asModuleType(moduleTypeVal)->vars.load(toUnicodeString("Component"));
    if (!typeOpt.has_value() || !isObjectType(typeOpt.value()))
        throw std::runtime_error("qt: the Component type is unavailable (qt.rox not loaded?)");
    return typeOpt.value();
}
} // namespace

Value ModuleQt::engine_create_component_builtin(ArgsView args)
{
    QQmlApplicationEngine* engine = engineFromReceiver(args, "create_component");
    if (args.size() < 2 || !isString(args[1]))
        throw std::invalid_argument("Engine.create_component(path) expects a string path");

    std::string path = toUTF8StdString(asStringObj(args[1])->s);
    QUrl url = resolveQmlUrl(path, vm().getModulePaths());
    // Parent the component to the engine so Qt owns it (it dies with the engine).
    auto* comp = new QQmlComponent(engine, url, engine);
    if (comp->isError())
        throw std::runtime_error("Engine.create_component(): " + comp->errorString().toStdString());
    return makeComponentValue(componentTypeOf(moduleType()), comp);
}

Value ModuleQt::engine_create_component_string_builtin(ArgsView args)
{
    QQmlApplicationEngine* engine = engineFromReceiver(args, "create_component_string");
    if (args.size() < 2 || !isString(args[1]))
        throw std::invalid_argument("Engine.create_component_string(qml) expects a string");

    std::string qml = toUTF8StdString(asStringObj(args[1])->s);
    // Base URL = the calling script's dir, so the snippet's relative asset URLs and
    // imports resolve like load_string().
    QUrl baseUrl;
    const std::filesystem::path dir = currentScriptDir();
    if (!dir.empty())
        baseUrl = QUrl::fromLocalFile(QString::fromStdString((dir / "inline.qml").string()));
    auto* comp = new QQmlComponent(engine, engine);   // parented to the engine
    comp->setData(QByteArray::fromStdString(qml), baseUrl);
    if (comp->isError())
        throw std::runtime_error("Engine.create_component_string(): " + comp->errorString().toStdString());
    return makeComponentValue(componentTypeOf(moduleType()), comp);
}

Value ModuleQt::component_create_builtin(ArgsView args)
{
    QQmlComponent* comp = componentFromReceiver(args, "create");
    Value parentVal = (args.size() >= 2) ? args[1] : Value::nilVal();
    Value propsVal  = (args.size() >= 3) ? args[2] : Value::nilVal();

    if (!parentVal.isNil() && !isQtObject(parentVal))
        throw std::invalid_argument("Component.create(parent, props): parent must be a Qt item handle or nil");
    if (!propsVal.isNil() && !isDict(propsVal))
        throw std::invalid_argument("Component.create(parent, props): props must be a dict or nil");

    // Initial property values are applied before completion, so bindings see them.
    QVariantMap initialProps;
    if (isDict(propsVal))
        initialProps = toQVariant(propsVal).toMap();

    QQmlContext* ctx = comp->engine() ? comp->engine()->rootContext() : nullptr;
    QObject* obj = comp->createWithInitialProperties(initialProps, ctx);
    if (!obj) {
        std::string detail = comp->isError() ? comp->errorString().toStdString()
                                             : std::string("createWithInitialProperties returned null");
        throw std::runtime_error("Component.create(): " + detail);
    }

    // Keep the object on the C++ side: QML's JS GC must never delete it under us.
    QQmlEngine::setObjectOwnership(obj, QQmlEngine::CppOwnership);

    if (isQtObject(parentVal)) {
        QObject* parentObj = asQtObject(parentVal)->deref("Component.create");
        obj->setParent(parentObj);                              // ownership: freed with parent
        if (auto* pItem = qobject_cast<QQuickItem*>(parentObj))
            if (auto* cItem = qobject_cast<QQuickItem*>(obj))
                cItem->setParentItem(pItem);                    // layout: appears under parent
    } else {
        // Detached: owned by the engine so it doesn't leak (lives until teardown).
        obj->setParent(comp->engine());
    }
    return qtObjectValue(obj);
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

// ---- Cooperative pumping for interactive / REPL use ----
// Without run()'s park, the Qt event loop never spins, so a loaded UI doesn't paint
// and queued signals/timers don't fire. These let a script (or the REPL) advance Qt
// on demand: a one-shot drain, a bounded pump, and a periodic Roxal timer.

Value ModuleQt::qt_process_events_builtin(ArgsView args)
{
    ensureQtUiThread("qt.process_events");
    // Drain all currently-pending Qt events (repaints, queued signals/callbacks) and
    // return. With an optional max_ms, process for up to that many milliseconds.
    if (args.size() >= 1 && !args[0].isNil()) {
        if (!args[0].isNumber())
            throw std::invalid_argument("qt.process_events(max_ms) expects a number of milliseconds or nil");
        int maxMs = args[0].isInt() ? static_cast<int>(args[0].asInt())
                                    : static_cast<int>(args[0].asReal());
        QCoreApplication::processEvents(QEventLoop::AllEvents, maxMs);
    } else {
        QCoreApplication::processEvents(QEventLoop::AllEvents);
    }
    return Value::nilVal();
}

Value ModuleQt::qt_every_builtin(ArgsView args)
{
    ensureQtUiThread("qt.every");
    if (args.size() < 2 || !args[0].isNumber() || !isClosure(args[1]))
        throw std::invalid_argument("qt.every(ms, handler) expects an interval in ms and a callable");
    if (!impl_->app)
        throw std::runtime_error("qt.every(): Qt application not initialized");
    int ms = args[0].isInt() ? static_cast<int>(args[0].asInt())
                             : static_cast<int>(args[0].asReal());
    if (ms < 0)
        throw std::invalid_argument("qt.every(ms, handler): ms must be >= 0");

    // The QTimer is owned by Qt (parented to the application); the returned item handle
    // is non-owning, so the user can timer.stop()/start() or set timer.interval, and it
    // is destroyed at teardown. The handler fires (on this UI thread) whenever Qt is
    // pumped — under run(), run_for(), or process_events(). The signal hub keeps the
    // closure alive while connected.
    auto* timer = new QTimer(impl_->app);
    timer->setInterval(ms);
    QMetaMethod sig = findQtSignal(timer, "timeout");
    QtSignalHub::instance().connectCallback(timer, sig, args[1]);
    timer->start();
    return qtObjectValue(timer);
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
    } else if (RoxalTreeModel* tree = roxalTreeModelPtr(v)) {
        engine->rootContext()->setContextProperty(name, static_cast<QObject*>(tree));
    } else if (QSortFilterProxyModel* proxy = roxalSortFilterPtr(v)) {
        engine->rootContext()->setContextProperty(name, static_cast<QObject*>(proxy));
    } else if (isQtObject(v)) {
        engine->rootContext()->setContextProperty(name, asQtObject(v)->deref("set_context_property"));
    } else if (isObjectInstance(v)) {
        // A plain Roxal object → a bindable QQmlPropertyMap (its public properties
        // become QML-bindable, read/write/notify). (ListModel/Engine handles are
        // caught above; this is for ordinary objects exposed as a backend/state.)
        RoxalPropertyMap* map = QtBindHub::instance().wrap(v);
        map->installMethods(engine);   // expose the object's public methods as callable values
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

// ---- qt.TreeModel builtins (by node object; nil parent = root level) ----

Value ModuleQt::treemodel_init_builtin(ArgsView args)
{
    if (args.size() < 2 || !isObjectInstance(args[0]))
        throw std::invalid_argument("qt.TreeModel(node_type) expects a receiver and a node type");
    ObjectInstance* inst = asObjectInstance(args[0]);

    Value rowTypeVal = args[1];
    if (!isObjectType(rowTypeVal))
        throw std::invalid_argument("qt.TreeModel(node_type): the argument must be a type");
    ObjObjectType* rowType = asObjectType(rowTypeVal);
    if (rowType->isActor || rowType->isEnumeration)
        throw std::invalid_argument("qt.TreeModel(node_type): node type must be an object type or interface "
                                    "(not an actor or enum)");

    // The model owns a root list, optionally seeded with `roots` (each a node instance;
    // its `children` subtree comes along in the node objects).
    auto listObj = newListObj();
    ObjList* rootList = listObj.get();
    Value roots = Value::objVal(std::move(listObj));
    if (args.size() >= 3 && !args[2].isNil()) {
        if (!isList(args[2]))
            throw std::invalid_argument("qt.TreeModel(node_type, roots): roots must be a list of nodes");
        ObjList* seed = asList(args[2]);
        for (int32_t i = 0; i < seed->length(); ++i) {
            Value e = seed->getElement(static_cast<size_t>(i));
            if (!isObjectInstance(e) ||
                !isSubtypeOf(asObjectType(asObjectInstance(e)->instanceType), rowType))
                throw std::runtime_error("qt.TreeModel: a root node does not match the node type");
            rootList->append(e);
        }
    }

    RoxalTreeModel* model = QtModelHub::instance().createTree(rowTypeVal, roots);
    auto* handle = new QPointer<RoxalTreeModel>(model);
    auto fp = newForeignPtrObj(static_cast<void*>(handle));
    fp->registerCleanup([](void* p) {
        delete static_cast<QPointer<RoxalTreeModel>*>(p);
    });
    inst->setProperty("_native", Value::objVal(std::move(fp)));
    return Value::nilVal();
}

Value ModuleQt::treemodel_count_builtin(ArgsView args)
{
    RoxalTreeModel* m = treeModelFromReceiver(args, "count");
    return Value::intVal(m->childCount(args.size() >= 2 ? args[1] : Value::nilVal()));
}

Value ModuleQt::treemodel_child_builtin(ArgsView args)
{
    RoxalTreeModel* m = treeModelFromReceiver(args, "child");
    if (args.size() < 3 || !args[2].isInt())
        throw std::invalid_argument("TreeModel.child(parent, i) expects a parent (or nil) and an int index");
    return m->childAt(args[1], static_cast<int>(args[2].asInt()));
}

Value ModuleQt::treemodel_parent_of_builtin(ArgsView args)
{
    RoxalTreeModel* m = treeModelFromReceiver(args, "parent_of");
    if (args.size() < 2)
        throw std::invalid_argument("TreeModel.parent_of(node) expects a node");
    return m->parentOf(args[1]);
}

Value ModuleQt::treemodel_append_builtin(ArgsView args)
{
    RoxalTreeModel* m = treeModelFromReceiver(args, "append");
    if (args.size() < 3 || !isObjectInstance(args[2]))
        throw std::invalid_argument("TreeModel.append(parent, node) expects a parent (or nil) and a node object");
    if (!m->admits(args[2]))
        throw std::runtime_error("TreeModel.append: the node does not match the model's node type");
    m->appendChild(args[1], args[2]);
    return Value::nilVal();
}

Value ModuleQt::treemodel_insert_builtin(ArgsView args)
{
    RoxalTreeModel* m = treeModelFromReceiver(args, "insert");
    if (args.size() < 4 || !args[2].isInt() || !isObjectInstance(args[3]))
        throw std::invalid_argument("TreeModel.insert(parent, i, node) expects a parent (or nil), an int index, and a node");
    if (!m->admits(args[3]))
        throw std::runtime_error("TreeModel.insert: the node does not match the model's node type");
    m->insertChild(args[1], static_cast<int>(args[2].asInt()), args[3]);
    return Value::nilVal();
}

Value ModuleQt::treemodel_remove_builtin(ArgsView args)
{
    RoxalTreeModel* m = treeModelFromReceiver(args, "remove");
    if (args.size() < 3 || !args[2].isInt())
        throw std::invalid_argument("TreeModel.remove(parent, i) expects a parent (or nil) and an int index");
    m->removeChild(args[1], static_cast<int>(args[2].asInt()));
    return Value::nilVal();
}

Value ModuleQt::treemodel_move_builtin(ArgsView args)
{
    RoxalTreeModel* m = treeModelFromReceiver(args, "move");
    if (args.size() < 4 || !args[2].isInt() || !args[3].isInt())
        throw std::invalid_argument("TreeModel.move(parent, from, to) expects a parent (or nil) and two int indices");
    m->moveChild(args[1], static_cast<int>(args[2].asInt()), static_cast<int>(args[3].asInt()));
    return Value::nilVal();
}

Value ModuleQt::treemodel_clear_builtin(ArgsView args)
{
    RoxalTreeModel* m = treeModelFromReceiver(args, "clear");
    m->clearChildren(args.size() >= 2 ? args[1] : Value::nilVal());
    return Value::nilVal();
}

Value ModuleQt::treemodel_begin_reset_builtin(ArgsView args)
{
    treeModelFromReceiver(args, "begin_reset")->beginResetBatch();
    return Value::nilVal();
}

Value ModuleQt::treemodel_end_reset_builtin(ArgsView args)
{
    treeModelFromReceiver(args, "end_reset")->endResetBatch();
    return Value::nilVal();
}

Value ModuleQt::treemodel_node_changed_builtin(ArgsView args)
{
    RoxalTreeModel* m = treeModelFromReceiver(args, "node_changed");
    if (args.size() < 2)
        throw std::invalid_argument("TreeModel.node_changed(node) expects a node");
    m->nodeChanged(args[1]);
    return Value::nilVal();
}

Value ModuleQt::treemodel_cell_changed_builtin(ArgsView args)
{
    RoxalTreeModel* m = treeModelFromReceiver(args, "cell_changed");
    if (args.size() < 3 || !isString(args[2]))
        throw std::invalid_argument("TreeModel.cell_changed(node, role) expects a node and a string role");
    m->cellChanged(args[1], QByteArray::fromStdString(toUTF8StdString(asStringObj(args[2])->s)));
    return Value::nilVal();
}

Value ModuleQt::treemodel_set_builtin(ArgsView args)
{
    RoxalTreeModel* m = treeModelFromReceiver(args, "set");
    if (args.size() < 4 || !isString(args[2]))
        throw std::invalid_argument("TreeModel.set(node, role, value) expects a node, a string role, and a value");
    m->setCell(args[1], QByteArray::fromStdString(toUTF8StdString(asStringObj(args[2])->s)), args[3]);
    return Value::nilVal();
}

// ---- qt.SortFilterModel builtins (a QSortFilterProxyModel over a qt.ListModel) ----

Value ModuleQt::sortfilter_init_builtin(ArgsView args)
{
    if (args.size() < 2 || !isObjectInstance(args[0]))
        throw std::invalid_argument("qt.SortFilterModel(source) expects a receiver and a source model");
    ObjectInstance* inst = asObjectInstance(args[0]);
    RoxalListModel* source = roxalListModelPtr(args[1]);
    if (!source)
        throw std::invalid_argument("qt.SortFilterModel(source): source must be a qt.ListModel");

    QSortFilterProxyModel* proxy = QtModelHub::instance().createSortFilter(source);
    auto* handle = new QPointer<QSortFilterProxyModel>(proxy);
    auto fp = newForeignPtrObj(static_cast<void*>(handle));
    fp->registerCleanup([](void* p) {
        delete static_cast<QPointer<QSortFilterProxyModel>*>(p);
    });
    inst->setProperty("_native", Value::objVal(std::move(fp)));
    return Value::nilVal();
}

Value ModuleQt::sortfilter_sort_by_builtin(ArgsView args)
{
    QSortFilterProxyModel* proxy = sortFilterFromReceiver(args, "sort_by");
    if (args.size() < 2 || !isString(args[1]))
        throw std::invalid_argument("SortFilterModel.sort_by(role, descending=false) expects a string role");
    QByteArray role = QByteArray::fromStdString(toUTF8StdString(asStringObj(args[1])->s));
    const int roleInt = sortFilterRoleInt(proxy, role);
    if (roleInt < 0)
        throw std::runtime_error("SortFilterModel.sort_by: no role '" + std::string(role.constData()) + "'");
    const bool desc = args.size() >= 3 && args[2].isBool() && args[2].asBool();
    proxy->setSortRole(roleInt);
    proxy->sort(0, desc ? Qt::DescendingOrder : Qt::AscendingOrder);
    return Value::nilVal();
}

Value ModuleQt::sortfilter_filter_builtin(ArgsView args)
{
    QSortFilterProxyModel* proxy = sortFilterFromReceiver(args, "filter");
    if (args.size() < 3 || !isString(args[1]) || !isString(args[2]))
        throw std::invalid_argument("SortFilterModel.filter(role, text) expects a string role and string text");
    QByteArray role = QByteArray::fromStdString(toUTF8StdString(asStringObj(args[1])->s));
    const int roleInt = sortFilterRoleInt(proxy, role);
    if (roleInt < 0)
        throw std::runtime_error("SortFilterModel.filter: no role '" + std::string(role.constData()) + "'");
    proxy->setFilterRole(roleInt);
    proxy->setFilterFixedString(QString::fromStdString(toUTF8StdString(asStringObj(args[2])->s)));
    return Value::nilVal();
}

Value ModuleQt::sortfilter_clear_filter_builtin(ArgsView args)
{
    sortFilterFromReceiver(args, "clear_filter")->setFilterFixedString(QString());
    return Value::nilVal();
}

Value ModuleQt::sortfilter_count_builtin(ArgsView args)
{
    return Value::intVal(sortFilterFromReceiver(args, "count")->rowCount());
}

Value ModuleQt::sortfilter_row_builtin(ArgsView args)
{
    QSortFilterProxyModel* proxy = sortFilterFromReceiver(args, "row");
    if (args.size() < 2 || !args[1].isInt())
        throw std::invalid_argument("SortFilterModel.row(i) expects an int index");
    const int i = static_cast<int>(args[1].asInt());
    if (i < 0 || i >= proxy->rowCount()) return Value::nilVal();
    const QModelIndex sidx = proxy->mapToSource(proxy->index(i, 0));
    RoxalListModel* source = static_cast<RoxalListModel*>(proxy->sourceModel());
    return source ? source->rowAt(sidx.row()) : Value::nilVal();
}

Value ModuleQt::qt_notify_builtin(ArgsView args)
{
    ensureQtUiThread("notify");   // pushes to the QML property map (a UI-thread QObject)
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
