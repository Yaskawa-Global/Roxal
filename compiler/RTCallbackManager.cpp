#include "RTCallbackManager.h"
#include <iostream>
#include <algorithm>

namespace roxal {

RTCallbackManager& RTCallbackManager::instance()
{
    static RTCallbackManager instance;
    return instance;
}

RTCallbackHandle RTCallbackManager::registerCallback(RTCallback callback, int64_t intervalUs,
                                                     int64_t maxLatenessUs)
{
    if (!callback || intervalUs <= 0) {
        return InvalidRTCallbackHandle;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    Entry entry;
    entry.handle = nextHandle_.fetch_add(1, std::memory_order_relaxed);
    entry.callback = std::move(callback);
    entry.intervalUs = intervalUs;
    entry.maxLatenessUs = maxLatenessUs;
    entry.enabled = true;

    // Align first trigger to interval boundary for phase-consistent timing.
    // This ensures callbacks registered at different times still fire in sync
    // if they have the same interval.
    auto nowUs = TimePoint::currentTime().microSecs();
    // Round up to next interval boundary
    entry.nextTriggerUs = ((nowUs / intervalUs) + 1) * intervalUs;

    callbacks_.push_back(std::move(entry));
    updateCachedDeadline();

    return callbacks_.back().handle;
}

void RTCallbackManager::unregisterCallback(RTCallbackHandle handle)
{
    if (handle == InvalidRTCallbackHandle) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = std::find_if(callbacks_.begin(), callbacks_.end(),
                           [handle](const Entry& e) { return e.handle == handle; });

    if (it != callbacks_.end()) {
        callbacks_.erase(it);
        updateCachedDeadline();
    }
}

void RTCallbackManager::setEnabled(RTCallbackHandle handle, bool enabled)
{
    if (handle == InvalidRTCallbackHandle) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = std::find_if(callbacks_.begin(), callbacks_.end(),
                           [handle](const Entry& e) { return e.handle == handle; });

    if (it != callbacks_.end() && it->enabled != enabled) {
        it->enabled = enabled;
        updateCachedDeadline();
    }
}

bool RTCallbackManager::mayNeedInvocation() const
{
    // Fast path: no callbacks registered
    if (!hasActiveCallbacks_.load(std::memory_order_relaxed)) {
        return false;
    }

    // Fast path: not main thread
    if (!isMainThread()) {
        return false;
    }

    // Check if deadline is approaching
    auto nowUs = TimePoint::currentTime().microSecs();
    auto deadlineUs = cachedNextDeadlineUs_.load(std::memory_order_relaxed);

    return nowUs >= deadlineUs;
}

bool RTCallbackManager::checkAndInvokeCallbacks(TimePoint now)
{
    // Only invoke from main thread
    if (!isMainThread()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    if (callbacks_.empty()) {
        return false;
    }

    bool anyInvoked = false;
    auto nowUs = now.microSecs();

    for (auto& entry : callbacks_) {
        if (!entry.enabled) {
            continue;
        }

        // Check if this callback is due
        if (nowUs >= entry.nextTriggerUs) {
            // Check for missed deadline
            int64_t latenessUs = nowUs - entry.nextTriggerUs;

            if (entry.maxLatenessUs > 0 && latenessUs > entry.maxLatenessUs) {
                handleMissedDeadline(entry.nextTriggerUs, nowUs, entry.maxLatenessUs);
            }

            // Invoke the callback with the scheduled time
            TimePoint scheduledTime = TimePoint::microSecs(entry.nextTriggerUs);

            // Note: We invoke with mutex held. Callbacks should be very fast (<50μs).
            // If this becomes a problem, we could copy callbacks and release lock.
            entry.callback(scheduledTime);

            // Schedule next trigger - advance by intervals until we're in the future
            // This handles the case where we missed multiple intervals
            while (entry.nextTriggerUs <= nowUs) {
                entry.nextTriggerUs += entry.intervalUs;
            }

            anyInvoked = true;
        }
    }

    if (anyInvoked) {
        updateCachedDeadline();
    }

    return anyInvoked;
}

TimePoint RTCallbackManager::nextDeadline() const
{
    return TimePoint::microSecs(cachedNextDeadlineUs_.load(std::memory_order_relaxed));
}

bool RTCallbackManager::hasCallbacks() const
{
    return hasActiveCallbacks_.load(std::memory_order_relaxed);
}

void RTCallbackManager::setMainThread()
{
    // Only set once - first call wins
    bool expected = false;
    if (mainThreadSet_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        mainThreadId_ = std::this_thread::get_id();
    }
}

bool RTCallbackManager::isMainThread() const
{
    if (!mainThreadSet_.load(std::memory_order_acquire)) {
        return false;
    }
    return std::this_thread::get_id() == mainThreadId_;
}

bool RTCallbackManager::isMainThreadSet() const
{
    return mainThreadSet_.load(std::memory_order_acquire);
}

void RTCallbackManager::setAbortOnMissedDeadline(bool abort)
{
    abortOnMissedDeadline_.store(abort, std::memory_order_relaxed);
}

bool RTCallbackManager::getAbortOnMissedDeadline() const
{
    return abortOnMissedDeadline_.load(std::memory_order_relaxed);
}

void RTCallbackManager::updateCachedDeadline()
{
    // Called with mutex_ held

    int64_t minDeadline = INT64_MAX;
    bool hasActive = false;

    for (const auto& entry : callbacks_) {
        if (entry.enabled) {
            hasActive = true;
            if (entry.nextTriggerUs < minDeadline) {
                minDeadline = entry.nextTriggerUs;
            }
        }
    }

    cachedNextDeadlineUs_.store(minDeadline, std::memory_order_relaxed);
    hasActiveCallbacks_.store(hasActive, std::memory_order_relaxed);
}

void RTCallbackManager::handleMissedDeadline(int64_t scheduledUs, int64_t actualUs, int64_t maxLatenessUs)
{
    int64_t latenessUs = actualUs - scheduledUs;

    // Log to stderr for visibility
    std::cerr << "RT DEADLINE MISSED: scheduled=" << scheduledUs
              << "us, actual=" << actualUs
              << "us, lateness=" << latenessUs
              << "us (max allowed=" << maxLatenessUs << "us)" << std::endl;

    if (abortOnMissedDeadline_.load(std::memory_order_relaxed)) {
        std::cerr << "FATAL: Aborting due to missed RT deadline" << std::endl;
        std::abort();
    }
}

} // namespace roxal
