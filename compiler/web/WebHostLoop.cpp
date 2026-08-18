#ifdef __EMSCRIPTEN__

#include "WebHostLoop.h"
#include "JsBridge.h"
#include "RoxalStore.h"

#include "VM.h"
#include "SimpleMarkSweepGC.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

using namespace roxal;
using namespace roxal::web;

namespace {

std::atomic<bool> g_stopRequested{false};
bool g_installed = false;

struct WebHostLoop : HostEventLoop {
    // Re-entrancy gate, the QtSignalHub/QtHostLoop precedent: a handler that
    // yields lands back in waitForEvents -> pump() with the outer turn's
    // containers mid-mutation. One turn at a time; the nested attempt is a
    // no-op and the outer turn finishes first. VM-thread-only, so plain int.
    int pumpDepth_ = 0;

    void pump() override
    {
        if (pumpDepth_ > 0) return;
        ++pumpDepth_;
        roxal::web::g_pumpCount.fetch_add(1, std::memory_order_relaxed);
        // Wasm: native pump frames hold Values while store handlers re-enter
        // the interpreter (which polls safepoints); the conservative scan
        // cannot see wasm locals, so a collection mid-pump would sweep them.
        // No-park makes the whole pump turn collection-atomic.
        SimpleMarkSweepGC::GCNoParkScope nativeCover;
        // Containment: pump() runs from the dispatch loop, which has no call site
        // to propagate to, so an escaping exception would kill the VM thread.
        try {
            // Order matters. Publish state changes BEFORE running inbound work, so
            // a handler that reads the UI sees what the last turn produced; then
            // flush again below to push whatever the handler just changed.
            WebStoreHub::instance().flushAll();
            drainInbound();
            // Settle actor-store calls whose futures have landed. After the
            // drain, so a call queued this turn that finishes instantly still
            // resolves this turn rather than next.
            WebStoreHub::instance().pollPendingCalls();
            WebStoreHub::instance().flushAll();
            flush();          // one proxied batch for everything above
        } catch (const std::exception& e) {
            std::cerr << "web: " << e.what() << std::endl;
        } catch (...) {
        }
        --pumpDepth_;
    }

    void waitForEvents(TimeDuration maxWait) override
    {
        // Nothing to block on: browser events arrive by another thread pushing onto
        // the inbound queue, and the browser main thread cannot be waited on from
        // here. Poll at a UI-appropriate granularity instead.
        pump();

        // Un-park here, after any handler has returned -- doing it inside the
        // handler would be undone by the scope that restores the park on return.
        if (g_stopRequested.load(std::memory_order_acquire)) {
            if (Thread* t = VM::thread.get()) t->threadSleep = false;
            return;
        }

        // Cap the nap at roughly one UI frame, so queued work never sits longer
        // than a user would notice.
        constexpr int64_t capUs = 8000;
        const int64_t us = maxWait.microSecs() < capUs ? maxWait.microSecs() : capUs;
        if (us > 0) std::this_thread::sleep_for(std::chrono::microseconds(us));
    }
};

} // namespace

std::atomic<std::uint64_t> roxal::web::g_pumpCount{0};

void roxal::web::installHostLoop(VM& vm)
{
    if (g_installed) return;
    vm.setHostEventLoop(make_ptr<WebHostLoop>());
    g_installed = true;
}

void roxal::web::uninstallHostLoop(VM& vm)
{
    if (!g_installed) return;
    vm.setHostEventLoop(nullptr);
    g_installed = false;
}

void roxal::web::parkCurrentThread(bool timed, double seconds)
{
    Thread* thread = VM::thread.get();
    if (!thread)
        throw std::runtime_error("web: no current thread to park");

    g_stopRequested.store(false, std::memory_order_release);
    thread->threadSleepUntil = timed
        ? TimePoint::currentTime() + TimeDuration::secs(seconds)
        : TimePoint::max();
    thread->threadSleep = true;
}

void roxal::web::requestStop()
{
    g_stopRequested.store(true, std::memory_order_release);
}

#endif // __EMSCRIPTEN__
