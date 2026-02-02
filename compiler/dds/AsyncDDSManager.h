#pragma once

#ifdef ROXAL_ENABLE_DDS

#include "Value.h"
#include "core/memory.h"

#include <mutex>
#include <thread>
#include <atomic>
#include <list>
#include <future>
#include <functional>
#include <condition_variable>
#include <dds/dds.h>

namespace roxal {

// Pending async DDS operation
struct PendingDDSOp {
    enum class Type {
        DdsWrite
    };

    Type type;
    ptr<std::promise<Value>> promise;
    std::shared_future<Value> future;

    // DDS write operation data
    dds_entity_t writer { 0 };
    void* sample { nullptr };
    std::shared_ptr<dds_topic_descriptor_t> descriptor;  // For sample cleanup
};

class AsyncDDSManager {
public:
    static AsyncDDSManager& instance();

    // Submit an operation, returns future Value for VM
    Value submit(PendingDDSOp op);

    // Lifecycle
    void start();
    void stop();
    bool isRunning() const { return running.load(); }

    // Destructor ensures thread is stopped
    ~AsyncDDSManager();

private:
    AsyncDDSManager() = default;
    AsyncDDSManager(const AsyncDDSManager&) = delete;
    AsyncDDSManager& operator=(const AsyncDDSManager&) = delete;

    void workerLoop();
    Value executeOp(PendingDDSOp& op);

    // Operation executors
    Value executeDdsWrite(PendingDDSOp& op);

    std::thread workerThread;
    std::atomic<bool> running{false};
    std::mutex queueMutex;
    std::condition_variable queueCV;
    std::list<PendingDDSOp> pendingOps;
};

} // namespace roxal

#endif // ROXAL_ENABLE_DDS
