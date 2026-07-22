#include "AsyncIOManager.h"
#include "Object.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdlib>
#include <iostream>

using namespace roxal;

namespace {

std::atomic<AsyncIOManager*> s_instance{nullptr};

const char* opTypeName(PendingIOOp::Type t)
{
    switch (t) {
        case PendingIOOp::Type::FileRead:      return "read";
        case PendingIOOp::Type::FileReadLine:  return "read_line";
        case PendingIOOp::Type::FileReadAll:   return "read_file";
        case PendingIOOp::Type::FileWrite:     return "write";
        case PendingIOOp::Type::FileFlush:     return "flush";
        case PendingIOOp::Type::FileClose:     return "close";
        case PendingIOOp::Type::FileSyncFlush: return "flush";
    }
    return "op";
}

// Script-observable failure result: false for the bool-returning mutation
// ops, nil for reads (matching their "no data" convention).
Value opFailureValue(PendingIOOp::Type t)
{
    switch (t) {
        case PendingIOOp::Type::FileWrite:
        case PendingIOOp::Type::FileFlush:
        case PendingIOOp::Type::FileClose:
        case PendingIOOp::Type::FileSyncFlush:
            return Value::falseVal();
        default:
            return Value::nilVal();
    }
}

}  // namespace

AsyncIOManager& AsyncIOManager::instance()
{
    static AsyncIOManager inst;
    s_instance.store(&inst, std::memory_order_release);
    return inst;
}

AsyncIOManager* AsyncIOManager::instanceIfCreated()
{
    return s_instance.load(std::memory_order_acquire);
}

AsyncIOManager::~AsyncIOManager()
{
    s_instance.store(nullptr, std::memory_order_release);
    stop();
}

void AsyncIOManager::tracePending(ValueVisitor& visitor)
{
    auto visitStrong = [&visitor](const Value& v) {
        if (v.isObj() && !v.isWeak())
            visitor.visit(v);
    };
    auto visitOps = [&](const std::list<PendingIOOp>& ops) {
        for (const PendingIOOp& op : ops) {
            visitStrong(op.fileValue);
            // An op resolved but not yet discarded still holds its result in
            // the shared state (e.g. a read's byte list) — root it too.
            if (op.future.valid() &&
                op.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
                visitStrong(op.future.get());
        }
    };
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        visitOps(pendingOps);
        visitOps(processingOps);
    }
    {
        std::lock_guard<std::mutex> lock(fileFuturesMutex);
        for (auto& entry : fileFutures) {
            for (auto& fut : entry.second) {
                if (fut.valid() &&
                    fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
                    visitStrong(fut.get());
            }
        }
    }
}

void AsyncIOManager::start()
{
    if (!running.load()) {
        running = true;
        workerThread = std::thread(&AsyncIOManager::workerLoop, this);
    }
}

void AsyncIOManager::stop()
{
    if (running.load()) {
        auto grace = std::chrono::milliseconds(500);
        if (const char* env = std::getenv("ROXAL_FILEIO_SHUTDOWN_GRACE_MS")) {
            char* end = nullptr;
            long ms = std::strtol(env, &end, 10);
            if (end != env && ms >= 0)
                grace = std::chrono::milliseconds(ms);
        }
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            shutdownDeadline = std::chrono::steady_clock::now() + grace;
        }
        running = false;
        queueCV.notify_all();
        if (workerThread.joinable()) {
            workerThread.join();
        }
    }
}

Value AsyncIOManager::submit(PendingIOOp op)
{
    // Ensure worker is running
    if (!running.load()) {
        start();
    }

    ptr<std::promise<Value>> promise = make_ptr<std::promise<Value>>();
    op.promise = promise;
    std::shared_future<Value> future = promise->get_future().share();
    op.future = future;

    // Track future for file operations (so waitForFile can wait for them)
    if (op.file) {
        std::lock_guard<std::mutex> lock(fileFuturesMutex);
        fileFutures[op.file].push_back(future);
    }

    {
        std::lock_guard<std::mutex> lock(queueMutex);
        pendingOps.push_back(std::move(op));
    }
    queueCV.notify_one();

    return Value::futureVal(future);
}

Value AsyncIOManager::getPendingFuture(Value fileValue)
{
    if (!isFile(fileValue)) return Value::nilVal();
    ObjFile* file = asFile(fileValue);

    std::vector<std::shared_future<Value>> futures;
    {
        std::lock_guard<std::mutex> lock(fileFuturesMutex);
        auto it = fileFutures.find(file);
        if (it != fileFutures.end()) {
            // Remove completed futures, keep pending ones
            auto& vec = it->second;
            vec.erase(std::remove_if(vec.begin(), vec.end(), [](const std::shared_future<Value>& f) {
                return !f.valid() || f.wait_for(std::chrono::microseconds(0)) == std::future_status::ready;
            }), vec.end());

            if (vec.empty()) {
                fileFutures.erase(it);
                return Value::nilVal();
            }

            futures = vec;  // Copy, don't move - we still track them
        }
    }

    if (futures.empty()) {
        return Value::nilVal();
    }

    // Create a combined future that waits for all pending ops
    ptr<std::promise<Value>> promise = make_ptr<std::promise<Value>>();
    std::shared_future<Value> combinedFuture = promise->get_future().share();

    // Submit a task that waits for all futures then signals completion
    PendingIOOp waitOp;
    waitOp.type = PendingIOOp::Type::FileFlush;  // Reuse flush type for wait-only
    waitOp.file = file;
    waitOp.pendingFutures = std::move(futures);
    waitOp.promise = promise;
    waitOp.future = combinedFuture;

    {
        std::lock_guard<std::mutex> lock(queueMutex);
        pendingOps.push_back(std::move(waitOp));
    }
    queueCV.notify_one();

    return Value::futureVal(combinedFuture);
}

void AsyncIOManager::waitForFile(Value fileValue)
{
    if (!isFile(fileValue)) return;
    ObjFile* file = asFile(fileValue);

    std::vector<std::shared_future<Value>> futures;
    {
        std::lock_guard<std::mutex> lock(fileFuturesMutex);
        auto it = fileFutures.find(file);
        if (it != fileFutures.end()) {
            futures = std::move(it->second);
            fileFutures.erase(it);
        }
    }

    // Blocking wait for all pending operations on this file
    for (auto& fut : futures) {
        if (fut.valid()) {
            fut.wait();
        }
    }
}

void AsyncIOManager::workerLoop()
{
    while (running.load()) {
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            // Wait for work or shutdown
            queueCV.wait_for(lock, std::chrono::milliseconds(10), [this] {
                return !pendingOps.empty() || !running.load();
            });

            if (!running.load() && pendingOps.empty()) {
                break;
            }

            // Move the batch into the member list (not a local) so the GC
            // root tracer can still reach these ops while they execute.
            processingOps.splice(processingOps.end(), pendingOps);
        }

        // Process all pending operations
        size_t abandoned = 0;
        size_t abandonedBytes = 0;
        for (auto& op : processingOps) {
            // Shutdown drain is bounded: past the grace deadline, abandon
            // the remainder loudly rather than stall process exit behind a
            // slow device. Scripts that need durability must consume the
            // close()/flush() future.
            if (!running.load() &&
                std::chrono::steady_clock::now() > shutdownDeadline) {
                op.promise->set_value(opFailureValue(op.type));
                ++abandoned;
                abandonedBytes += op.writeData.size();
                continue;
            }
            try {
                Value result = executeOp(op);
                op.promise->set_value(result);
            } catch (const std::exception& e) {
                // A failed async op must not be silent: report it, and
                // resolve the future to a script-observable failure value.
                std::cerr << "fileio: async " << opTypeName(op.type)
                          << (op.path.empty() ? std::string() : " '" + op.path + "'")
                          << " failed: " << e.what() << std::endl;
                op.promise->set_value(opFailureValue(op.type));
            } catch (...) {
                std::cerr << "fileio: async " << opTypeName(op.type)
                          << " failed with unknown error" << std::endl;
                op.promise->set_value(opFailureValue(op.type));
            }
        }
        if (abandoned) {
            std::cerr << "fileio: " << abandoned << " pending op(s) abandoned at shutdown, "
                      << abandonedBytes << " bytes unwritten (wait on close()'s"
                      << " future to guarantee completion)" << std::endl;
        }

        {
            std::lock_guard<std::mutex> lock(queueMutex);
            processingOps.clear();
        }
    }

    // Shutdown: resolve anything queued after the final batch was taken so
    // no future is left holding a broken promise.
    std::lock_guard<std::mutex> lock(queueMutex);
    if (!pendingOps.empty()) {
        std::cerr << "fileio: " << pendingOps.size()
                  << " op(s) abandoned at shutdown" << std::endl;
        for (auto& op : pendingOps)
            op.promise->set_value(opFailureValue(op.type));
        pendingOps.clear();
    }
}

Value AsyncIOManager::executeOp(PendingIOOp& op)
{
    // If this op has pending futures to wait for, wait for them first
    for (auto& fut : op.pendingFutures) {
        if (fut.valid()) {
            fut.wait();
        }
    }

    switch (op.type) {
        case PendingIOOp::Type::FileRead:
            return executeFileRead(op);
        case PendingIOOp::Type::FileReadLine:
            return executeFileReadLine(op);
        case PendingIOOp::Type::FileReadAll:
            return executeFileReadAll(op);
        case PendingIOOp::Type::FileWrite:
            return executeFileWrite(op);
        case PendingIOOp::Type::FileFlush:
            return executeFileFlush(op);
        case PendingIOOp::Type::FileClose:
            return executeFileClose(op);
        case PendingIOOp::Type::FileSyncFlush:
            return executeFileSyncFlush(op);
    }
    return Value::nilVal();
}

Value AsyncIOManager::executeFileRead(PendingIOOp& op)
{
    if (!op.file) return Value::nilVal();

    std::lock_guard<std::mutex> lock(op.file->mutex);
    if (!op.file->file || !op.file->file->is_open())
        return Value::nilVal();

    std::vector<char> buf(op.maxBytes);
    op.file->file->read(buf.data(), static_cast<std::streamsize>(op.maxBytes));
    std::streamsize n = op.file->file->gcount();

    if (op.binary) {
        // Binary reads land as a packed byte list (one memcpy, ~1 byte/elem).
        std::vector<uint8_t> bytes(buf.data(), buf.data() + n);
        return Value::listVal(std::move(bytes));
    }

    std::string s(buf.data(), static_cast<size_t>(n));
    return Value::stringVal(toUnicodeString(s));
}

Value AsyncIOManager::executeFileReadLine(PendingIOOp& op)
{
    if (!op.file) return Value::nilVal();

    std::lock_guard<std::mutex> lock(op.file->mutex);
    if (!op.file->file || !op.file->file->is_open())
        return Value::nilVal();

    // Binary mode shouldn't use read_line
    if (op.binary)
        return Value::nilVal();

    std::string line;
    if (!std::getline(*op.file->file, line))
        return Value::nilVal();

    return Value::stringVal(toUnicodeString(line));
}

Value AsyncIOManager::executeFileReadAll(PendingIOOp& op)
{
    std::ios_base::openmode mode = std::ios::in;
    if (op.binary)
        mode |= std::ios::binary;

    std::ifstream in(op.path, mode);
    if (!in.is_open())
        return Value::nilVal();

    std::stringstream ss;
    ss << in.rdbuf();
    std::string data = ss.str();

    if (op.binary) {
        // Whole-file binary read lands as a packed byte list.
        std::vector<uint8_t> bytes(data.begin(), data.end());
        return Value::listVal(std::move(bytes));
    }

    return Value::stringVal(toUnicodeString(data));
}

Value AsyncIOManager::executeFileWrite(PendingIOOp& op)
{
    if (!op.file) return Value::falseVal();

    std::lock_guard<std::mutex> lock(op.file->mutex);
    if (!op.file->file || !op.file->file->is_open()) {
        std::cerr << "fileio: async write dropped -- file already closed ("
                  << op.writeData.size() << " bytes)" << std::endl;
        return Value::falseVal();
    }

    op.file->file->write(op.writeData.data(),
                         static_cast<std::streamsize>(op.writeData.size()));
    if (!op.file->file->good()) {
        std::cerr << "fileio: async write failed ("
                  << op.writeData.size() << " bytes)" << std::endl;
        return Value::falseVal();
    }

    return Value::trueVal();
}

Value AsyncIOManager::executeFileFlush(PendingIOOp& op)
{
    // If pendingFutures is non-empty, this is just a "wait for pending ops" task
    // The waiting was already done in executeOp, so just return success
    if (!op.pendingFutures.empty()) {
        return Value::nilVal();
    }

    if (!op.file) return Value::nilVal();

    std::lock_guard<std::mutex> lock(op.file->mutex);
    if (!op.file->file || !op.file->file->is_open())
        return Value::falseVal();

    op.file->file->flush();
    return op.file->file->good() ? Value::trueVal() : Value::falseVal();
}

Value AsyncIOManager::executeFileClose(PendingIOOp& op)
{
    // pendingFutures already waited in executeOp
    if (!op.file) return Value::falseVal();

    std::lock_guard<std::mutex> lock(op.file->mutex);
    if (op.file->file && op.file->file->is_open()) {
        op.file->file->close();
    }
    return Value::trueVal();
}

Value AsyncIOManager::executeFileSyncFlush(PendingIOOp& op)
{
    // pendingFutures already waited in executeOp
    if (!op.file) return Value::falseVal();

    std::lock_guard<std::mutex> lock(op.file->mutex);
    if (!op.file->file || !op.file->file->is_open())
        return Value::falseVal();

    op.file->file->flush();
    return op.file->file->good() ? Value::trueVal() : Value::falseVal();
}
