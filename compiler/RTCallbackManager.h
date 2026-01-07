#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <vector>
#include <thread>
#include <cstdint>
#include "core/TimePoint.h"

namespace roxal {

/// Callback function type for RT callbacks.
/// Receives the scheduled time at which the callback should have fired.
using RTCallback = std::function<void(TimePoint scheduledTime)>;

/// Opaque handle for registered callbacks
using RTCallbackHandle = uint64_t;

/// Invalid callback handle sentinel
constexpr RTCallbackHandle InvalidRTCallbackHandle = 0;

/**
 * RTCallbackManager - Singleton that manages real-time callbacks for the VM.
 *
 * This enables external systems (like Future Controller) to register timing-critical
 * callbacks that need to fire at precise intervals (e.g., 1ms with ±10μs jitter).
 *
 * Design principles:
 * - Fast-path checking (~3ns overhead when no callbacks registered)
 * - Uses CLOCK_MONOTONIC via TimePoint for precise timing
 * - Main-thread-only invocation (callbacks only fire from the main VM thread)
 * - Configurable fatal abort on missed deadline (for debugging RT issues)
 * - Fully decoupled: Roxal works standalone without any callbacks registered
 *
 * Usage from Future Controller:
 * @code
 *   auto& rtMgr = roxal::RTCallbackManager::instance();
 *   rtMgr.setAbortOnMissedDeadline(true);  // Enable for debugging
 *
 *   auto handle = rtMgr.registerCallback(
 *       [](roxal::TimePoint scheduled) {
 *           // Fire PDO event
 *           SetEvent(hPDORunEvent);
 *       },
 *       1000,  // 1ms interval
 *       50     // 50μs max lateness
 *   );
 * @endcode
 */
class RTCallbackManager {
public:
    /// Get the singleton instance
    static RTCallbackManager& instance();

    // Non-copyable, non-movable
    RTCallbackManager(const RTCallbackManager&) = delete;
    RTCallbackManager& operator=(const RTCallbackManager&) = delete;

    /**
     * Register a callback to be invoked at regular intervals.
     *
     * @param callback The function to call at each interval
     * @param intervalUs Interval in microseconds (e.g., 1000 for 1ms)
     * @param maxLatenessUs If >0, triggers error/abort if callback is late by more than this.
     *                      If 0, uses the global abortOnMissedDeadline setting with no lateness check.
     * @return Handle to the registered callback (use for unregister/enable/disable)
     */
    RTCallbackHandle registerCallback(RTCallback callback, int64_t intervalUs,
                                      int64_t maxLatenessUs = 0);

    /**
     * Unregister a previously registered callback.
     * Safe to call with InvalidRTCallbackHandle or already-unregistered handles.
     */
    void unregisterCallback(RTCallbackHandle handle);

    /**
     * Enable or disable a callback without unregistering it.
     * Disabled callbacks don't fire but retain their timing state.
     */
    void setEnabled(RTCallbackHandle handle, bool enabled);

    /**
     * Fast-path check if any callbacks might need invocation.
     * Returns false immediately if:
     * - No callbacks are registered
     * - Current thread is not the main thread
     * - Next deadline hasn't been reached
     *
     * Cost: ~3ns when no callbacks, ~25-35ns when callbacks registered (includes clock_gettime)
     */
    bool mayNeedInvocation() const;

    /**
     * Check timing and invoke any due callbacks.
     * Only invokes callbacks if called from the main thread.
     *
     * @param now Current time (pass TimePoint::currentTime())
     * @return true if any callbacks were invoked
     */
    bool checkAndInvokeCallbacks(TimePoint now);

    /**
     * Get the next deadline when a callback needs to fire.
     * Returns TimePoint with INT64_MAX microseconds if no callbacks registered.
     */
    TimePoint nextDeadline() const;

    /**
     * Check if any callbacks are registered and enabled.
     */
    bool hasCallbacks() const;

    /**
     * Call once from the main thread during VM initialization to establish
     * which thread is considered "main" for callback invocation.
     *
     * Thread safety: Safe to call multiple times; only the first call takes effect.
     */
    void setMainThread();

    /**
     * Check if the current thread is the main thread.
     */
    bool isMainThread() const;

    /**
     * Check if the main thread has been set.
     */
    bool isMainThreadSet() const;

    /**
     * Control whether missed deadlines cause a fatal abort.
     * When true, exceeding maxLatenessUs triggers std::abort() after logging.
     * Default: false (just logs the error)
     */
    void setAbortOnMissedDeadline(bool abort);
    bool getAbortOnMissedDeadline() const;

private:
    RTCallbackManager() = default;

    struct Entry {
        RTCallbackHandle handle{0};
        RTCallback callback;
        int64_t intervalUs{0};
        int64_t nextTriggerUs{0};  // Absolute time in microseconds (same epoch as TimePoint)
        int64_t maxLatenessUs{0};  // 0 = use global setting, no lateness check
        bool enabled{true};
    };

    mutable std::mutex mutex_;
    std::vector<Entry> callbacks_;
    std::atomic<uint64_t> nextHandle_{1};

    // Main thread tracking
    std::thread::id mainThreadId_{};
    std::atomic<bool> mainThreadSet_{false};

    // Fast-path atomics (lock-free reads)
    std::atomic<int64_t> cachedNextDeadlineUs_{INT64_MAX};
    std::atomic<bool> hasActiveCallbacks_{false};

    // Deadline enforcement
    std::atomic<bool> abortOnMissedDeadline_{false};

    /// Update cached deadline after callback list changes. Call with mutex held.
    void updateCachedDeadline();

    /// Handle a missed deadline - logs and optionally aborts
    void handleMissedDeadline(int64_t scheduledUs, int64_t actualUs, int64_t maxLatenessUs);
};

} // namespace roxal
