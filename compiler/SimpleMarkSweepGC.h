#pragma once

#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ObjControl.h"

namespace roxal {

class Value;
class Thread;
class VM;

class ValueVisitor;

class SimpleMarkSweepGC {
public:
    static constexpr std::uint64_t kDefaultAutoTriggerThreshold = 64ull * 1024ull * 1024ull; // 64 MiB

    class ExternalRootProvider {
    public:
        virtual ~ExternalRootProvider() = default;
        virtual void visitRoots(ValueVisitor& visitor) = 0;
    };

    class ExternalParticipant {
    public:
        explicit ExternalParticipant(SimpleMarkSweepGC& gc);
        ~ExternalParticipant();

        ExternalParticipant(const ExternalParticipant&) = delete;
        ExternalParticipant& operator=(const ExternalParticipant&) = delete;

        void setRootVisitor(std::function<void(ValueVisitor&)> visitor);
        void clearRootVisitor();
        void pollSafepointIfRequested();

    private:
        SimpleMarkSweepGC* gc_ { nullptr };
        std::uint64_t id_ { 0 };
    };

    static SimpleMarkSweepGC& instance();

    void registerAllocation(ObjControl* control);
    void unregisterAllocation(ObjControl* control);

    void requestCollect();
    void safepoint(Thread& currentThread);

    void setVM(VM* vm);
    void notifyCleanupPending();

    void setAutoTriggerThreshold(std::uint64_t threshold);
    std::uint64_t autoTriggerThreshold() const noexcept;

    void setEnabled(bool enabled) noexcept;
    bool isEnabled() const noexcept;

    void onThreadEnter();
    void onThreadExit();

    void registerExternalRootProvider(ExternalRootProvider* provider);
    void unregisterExternalRootProvider(ExternalRootProvider* provider);

    bool isCollectionRequested() const noexcept;
    std::uint64_t currentEpoch() const noexcept;
    size_t lastCollectionFreed() const noexcept;
    bool isCollectionInProgress() const noexcept;

    void visitRoots(ValueVisitor& visitor);

    // Run a synchronous collection assuming all mutator threads have already
    // parked (e.g. during VM shutdown). Returns the number of objects queued
    // for destruction. The caller is responsible for draining
    // Obj::unrefedObjs afterwards via VM::freeObjects().
    size_t collectNowForShutdown();

private:
    SimpleMarkSweepGC() = default;

    struct CollectionResult {
        std::vector<Obj*> unreachable;
        std::uint64_t freedBytes = 0;
    };

    struct ExternalParticipantState {
        bool atSafepoint { false };
        std::function<void(ValueVisitor&)> rootVisitor;
    };

    CollectionResult performCollection(std::unique_lock<std::mutex>& lock);
    void externalParticipantEnter(std::uint64_t id);
    void externalParticipantExit(std::uint64_t id);
    void externalParticipantSetVisitor(std::uint64_t id, std::function<void(ValueVisitor&)> visitor);
    void externalParticipantClearVisitor(std::uint64_t id);
    void externalParticipantSafepoint(std::uint64_t id);

    mutable std::mutex mutex_;
    std::unordered_set<ObjControl*> controls_;
    std::unordered_set<ExternalRootProvider*> externalRootProviders_;
    std::unordered_map<std::uint64_t, ExternalParticipantState> externalParticipants_;
    std::condition_variable safepointCv_;
    std::atomic<std::uint64_t> epoch_{1};
    std::atomic<std::uint64_t> nextExternalParticipantId_{1};
    std::atomic<bool> collectionRequested_{false};
    std::atomic<bool> collectionInProgress_{false};
    std::atomic<size_t> lastFreedCount_{0};
    std::atomic<std::uint64_t> lastFreedBytes_{0};
    std::atomic<std::uint64_t> lastRequestedBytes_{0};
    std::atomic<std::uint64_t> bytesAllocatedSinceLastCollect_{0};
    std::atomic<std::uint64_t> currentAllocatedBytes_{0};
    std::atomic<std::uint64_t> autoTriggerThreshold_{kDefaultAutoTriggerThreshold};
    std::atomic<bool> gcEnabled_{true};
    size_t activeThreads_{0};
    size_t threadsAtSafepoint_{0};
    Thread* collector_{nullptr};
    bool externalCollectorActive_{false};
    std::thread::id externalCollectorThread_{};
    std::atomic<VM*> vm_{nullptr};

    friend class ExternalParticipant;
};

} // namespace roxal
