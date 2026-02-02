#pragma once

#include "Value.h"
#include "Object.h"
#include "core/memory.h"

#include <mutex>
#include <thread>
#include <atomic>
#include <list>
#include <map>
#include <future>
#include <functional>
#include <condition_variable>

namespace roxal {

// Forward declarations
class ObjFile;

// Pending async I/O operation
struct PendingIOOp {
    enum class Type {
        FileRead,
        FileReadLine,
        FileReadAll,
        FileWrite,
        FileFlush,
        FileClose,          // Wait for pending ops then close
        FileSyncFlush       // Wait for pending ops then flush
    };

    Type type;
    ptr<std::promise<Value>> promise;
    std::shared_future<Value> future;   // Track future for waitForFile

    // File operation data
    ObjFile* file { nullptr };          // For operations on open file handles
    std::string path;                   // For read_file (opens its own handle)
    std::string writeData;              // For write operations
    size_t maxBytes { 4096 };           // For read operations
    bool binary { false };              // Text vs binary mode

    // For close/flush: futures to wait for before executing
    std::vector<std::shared_future<Value>> pendingFutures;
};

class AsyncIOManager {
public:
    static AsyncIOManager& instance();

    // Submit an operation, returns future Value for VM
    Value submit(PendingIOOp op);

    // Check if there are pending operations on a file handle
    // Returns nil if no pending ops (or all are ready), otherwise returns a future
    // that resolves when all pending ops complete
    // Takes Value by copy to ensure GC keeps the file object alive
    Value getPendingFuture(Value fileValue);

    // Blocking wait - only use when not in runFor() context
    void waitForFile(Value fileValue);

    // Lifecycle
    void start();
    void stop();
    bool isRunning() const { return running.load(); }

    // Destructor ensures thread is stopped
    ~AsyncIOManager();

private:
    AsyncIOManager() = default;
    AsyncIOManager(const AsyncIOManager&) = delete;
    AsyncIOManager& operator=(const AsyncIOManager&) = delete;

    void workerLoop();
    Value executeOp(PendingIOOp& op);

    // Operation executors
    Value executeFileRead(PendingIOOp& op);
    Value executeFileReadLine(PendingIOOp& op);
    Value executeFileReadAll(PendingIOOp& op);
    Value executeFileWrite(PendingIOOp& op);
    Value executeFileFlush(PendingIOOp& op);
    Value executeFileClose(PendingIOOp& op);
    Value executeFileSyncFlush(PendingIOOp& op);

    std::thread workerThread;
    std::atomic<bool> running{false};
    std::mutex queueMutex;
    std::condition_variable queueCV;
    std::list<PendingIOOp> pendingOps;

    // Track pending futures per file handle
    std::mutex fileFuturesMutex;
    std::map<ObjFile*, std::vector<std::shared_future<Value>>> fileFutures;
};

} // namespace roxal
