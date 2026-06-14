#ifdef ROXAL_ENABLE_QT

#include "ModuleQt.h"
#include "VM.h"
#include "Object.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QCoreApplication>
#include <QEventLoop>
#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace roxal;

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
}

void ModuleQt::teardownQt(VM& vm)
{
    if (!impl_->app && impl_->engines.empty())
        return; // already torn down

    vm.setHostEventLoop(nullptr);
    impl_->hostLoop.reset();
    impl_->engines.clear();             // destroy windows/engines while platform alive
    if (impl_->app && impl_->ownsApp)
        delete impl_->app.data();       // QPointer auto-nulls impl_->app afterwards
    impl_->ownsApp = false;
}

// ============================================================
// Helpers
// ============================================================

void ModuleQt::requestQuit()
{
    impl_->quitRequested = true;
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
    engine->load(QUrl::fromLocalFile(QString::fromStdString(path)));
    if (engine->rootObjects().isEmpty())
        throw std::runtime_error("Engine.load(): failed to load QML from '" + path +
                                 "' (root must be a Window)");
    connectRootWindowClose(engine, [this] { requestQuit(); });
    return Value::nilVal();
}

Value ModuleQt::engine_load_string_builtin(ArgsView args)
{
    QQmlApplicationEngine* engine = engineFromReceiver(args, "load_string");
    if (args.size() < 2 || !isString(args[1]))
        throw std::invalid_argument("Engine.load_string(qml) expects a string");

    std::string qml = toUTF8StdString(asStringObj(args[1])->s);
    engine->loadData(QByteArray::fromStdString(qml));
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

#endif // ROXAL_ENABLE_QT
