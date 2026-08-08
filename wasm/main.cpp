// Minimal wasm host for the Roxal VM.
//
// Deliberately NOT compiler/roxal.cpp: that entry point is the CLI and pulls in
// boost::program_options, a compiled Boost library we would otherwise have to
// cross-build for wasm. A browser embedding has no argv to parse anyway -- the
// host drives the VM directly.
//
// Threading: built with -sPROXY_TO_PTHREAD, so main() runs on its own pthread (a
// Web Worker) and the browser's main thread stays free to service the event loop.
// Roxal scripts block by nature -- sleep, waits, actor sends -- so running the VM
// on the browser main thread would freeze the page. DOM access is proxied back to
// the main thread instead; see web-integration-plan.md.

#include <unistd.h>
#include <sys/stat.h>
#ifdef ROXAL_WASM_WASMFS
#include <emscripten/wasmfs.h>
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <cstdlib>
#include <string>
#include <utility>

#include "VM.h"
#include "ExecutionStatus.h"
#include "web/WebHostLoop.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#else
#define EMSCRIPTEN_KEEPALIVE
static int emscripten_is_main_browser_thread() { return 0; }
#endif

using namespace roxal;

namespace {

// Thread identity as observed by the script thread itself, latched in main().
// Reported by roxal_thread_info() so a harness on ANY thread can assert on it.
//   bit 0: the script thread is the browser's main thread  (must be 0)
//   bit 1: VM::onMainThread() held on the script thread    (must be 1)
//   bit 2: latched -- main() has run far enough to fill this in
std::atomic<int> g_threadInfo{0};

constexpr int kInfoIsBrowserMain = 1 << 0;
constexpr int kInfoVMMainThread  = 1 << 1;
constexpr int kInfoLatched       = 1 << 2;

int runSource(std::istream& source, const std::string& name,
              const std::vector<std::string>& modulePaths) {
    // Rule 1 of the web design: the VM worker may block waiting on the browser
    // main thread, but the main thread must NEVER block waiting on the VM. Running
    // a script here would do exactly that -- and would also latch the VM's notion
    // of its script thread (VM::markMainThread) onto the browser main thread.
    // Under node this "works" and hides the bug; in a browser it freezes the tab.
    if (emscripten_is_main_browser_thread()) {
        std::cerr << "roxal-wasm: refusing to run a script on the browser main thread"
                     " -- dispatch to the VM thread instead" << std::endl;
        return 3;
    }

    const bool trace = std::getenv("ROXAL_WASM_TRACE") != nullptr;
    try {
        if (trace) std::cerr << "[trace] VM::instance()" << std::endl;
        VM& vm = VM::instance();
        if (trace) std::cerr << "[trace] appendModulePaths()" << std::endl;
        vm.appendModulePaths(modulePaths);
        if (trace) std::cerr << "[trace] run()" << std::endl;
        const ExecutionStatus status = vm.run(source, name);
        if (trace) std::cerr << "[trace] run() returned" << std::endl;

        // VM::run() calls markMainThread(), which latches the FIRST caller. Record
        // what it settled on: under PROXY_TO_PTHREAD that must be this worker.
        int info = g_threadInfo.load(std::memory_order_relaxed);
        if (VM::onMainThread()) info |= kInfoVMMainThread;
        g_threadInfo.store(info, std::memory_order_release);

        return status == ExecutionStatus::OK ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "roxal-wasm: " << e.what() << std::endl;
        return 2;
    }
}

// --- inbound script queue ---------------------------------------------------
// The seed of the JS -> Roxal direction. Any thread may submit a script; only the
// VM thread runs one. This is what lets the host keep a VM alive across scripts
// instead of exiting after one, and it is the mechanism web.serve() will sit on.
//
// Note the asymmetry, which is deliberate and permanent (rule 1 of the web
// design): submitting NEVER blocks the caller. The browser main thread cannot
// Atomics.wait, so any design that has it wait on the VM is a deadlock waiting to
// happen. Callers observe completion by polling roxal_completed_count().
std::mutex g_inboxMutex;
std::condition_variable g_inboxCv;
std::deque<std::pair<std::string, std::string>> g_inbox;   // (source, name)
std::atomic<bool> g_quitRequested{false};
std::atomic<int>  g_completedCount{0};
std::atomic<int>  g_lastResult{0};

// Between scripts this thread deliberately services NOTHING. Host event loops
// (the dom module installs one) are pumped by the VM's dispatch loop, which only
// runs while a script does -- and a callback invoked outside that context has no
// Roxal thread to execute on. A UI app therefore keeps a script alive: dom.run()
// parks inside the dispatch loop, exactly as qt's engine.run() does.
//
// Run submitted scripts until roxal_quit(). Called on the VM thread only.
void serveInbox() {
    for (;;) {
        std::pair<std::string, std::string> job;
        {
            std::unique_lock<std::mutex> lock(g_inboxMutex);
            // Bounded wait, because a UI app spends most of its life here with
            // no script running: an event listener installed by a script that
            // has already finished must still fire, and the only thing that can
            // deliver it is this thread servicing the host event loop.
            g_inboxCv.wait(lock, [] {
                return !g_inbox.empty() || g_quitRequested.load(std::memory_order_acquire);
            });
            if (g_inbox.empty() && g_quitRequested.load(std::memory_order_acquire))
                return;
            job = std::move(g_inbox.front());
            g_inbox.pop_front();
        }
        std::istringstream in{job.first};
        const int rc = runSource(in, job.second, {"/stdlib"});
        g_lastResult.store(rc, std::memory_order_relaxed);
        // Release-store LAST: a poller that sees the new count must also see the
        // result and all output written before it.
        g_completedCount.fetch_add(1, std::memory_order_release);
    }
}

} // namespace

extern "C" {

// Run a .rox file already present in the wasm filesystem.
EMSCRIPTEN_KEEPALIVE
int roxal_run_file(const char* path) {
    std::ifstream in(path);
    if (!in) {
        std::cerr << "roxal-wasm: cannot open '" << path << "'" << std::endl;
        return 2;
    }
    return runSource(in, path, {"/stdlib"});
}

// Run a script held in memory -- the path a browser editor actually takes.
EMSCRIPTEN_KEEPALIVE
int roxal_run_source(const char* source, const char* name) {
    std::istringstream in{std::string(source)};
    return runSource(in, name ? name : "<editor>", {"/stdlib"});
}

// Ask a parked Roxal app (dom.run / web.serve) to return, so the script can
// finish and the next submitted script can start. Without this a parked script
// owns the VM thread forever and a re-run would queue behind it.
EMSCRIPTEN_KEEPALIVE
void roxal_request_stop(void) {
#ifdef __EMSCRIPTEN__
    roxal::web::requestStop();
#endif
}

// Thread-model self-report; see g_threadInfo. Callable from any thread.
EMSCRIPTEN_KEEPALIVE
int roxal_thread_info(void) {
    return g_threadInfo.load(std::memory_order_acquire);
}

// --- inbound API: callable from any thread, never blocks ---------------------

// Queue a script for the VM thread to run. Returns immediately.
EMSCRIPTEN_KEEPALIVE
void roxal_submit_source(const char* source, const char* name) {
    {
        std::lock_guard<std::mutex> lock(g_inboxMutex);
        g_inbox.emplace_back(source ? source : "", name ? name : "<editor>");
    }
    g_inboxCv.notify_one();
}

// Ask the serve loop to stop once the queue drains.
EMSCRIPTEN_KEEPALIVE
void roxal_quit(void) {
    g_quitRequested.store(true, std::memory_order_release);
    g_inboxCv.notify_one();
}

// Scripts finished so far. Poll this to detect completion without blocking.
EMSCRIPTEN_KEEPALIVE
int roxal_completed_count(void) {
    return g_completedCount.load(std::memory_order_acquire);
}

// Exit code of the most recently completed script.
EMSCRIPTEN_KEEPALIVE
int roxal_last_result(void) {
    return g_lastResult.load(std::memory_order_relaxed);
}

} // extern "C"

int main(int argc, char** argv) {
    // Under PROXY_TO_PTHREAD this runs on a Worker, not the browser main thread.
    g_threadInfo.store(
        kInfoLatched | (emscripten_is_main_browser_thread() ? kInfoIsBrowserMain : 0),
        std::memory_order_release);

    // /data is where scripts keep files that should outlive them. In a browser
    // with WASMFS it is backed by OPFS -- the origin-private WHATWG File System
    // store -- so fileio writes survive a page reload. Everywhere else (node,
    // or a non-WASMFS build) it is an ordinary in-memory directory with the
    // same path, so scripts never need to care which one they got.
#ifdef ROXAL_WASM_WASMFS
    const bool haveOpfs = EM_ASM_INT({
        return (typeof navigator !== 'undefined'
                && navigator.storage && navigator.storage.getDirectory) ? 1 : 0;
    });
    if (haveOpfs) {
        backend_t opfs = wasmfs_create_opfs_backend();
        if (!opfs || wasmfs_create_directory("/data", 0777, opfs) != 0) {
            std::cerr << "roxal-wasm: OPFS mount failed; /data will not persist" << std::endl;
            ::mkdir("/data", 0777);
        }
    } else {
        ::mkdir("/data", 0777);
    }
#else
    ::mkdir("/data", 0777);
#endif

    // Builtin modules (sys.rox, math.rox, ...) are resolved relative to the
    // working directory, and VM::instance() loads them during construction --
    // before any appendModulePaths() call can take effect. So chdir first.
    if (::chdir("/stdlib") != 0)
        std::cerr << "roxal-wasm: warning: no /stdlib in the wasm FS" << std::endl;

    // One-shot: run the named script and exit (the CLI-shaped path, and what the
    // node test runner uses -- one script per module instance).
    if (argc > 1)
        return roxal_run_file(argv[1]);

    // Otherwise stay alive and serve scripts submitted from JS. main() must not
    // return here: this thread IS the VM, and a UI app outlives any one script.
    serveInbox();
    return g_lastResult.load(std::memory_order_relaxed);
}
