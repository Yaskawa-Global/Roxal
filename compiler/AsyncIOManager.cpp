#include "AsyncIOManager.h"
#include "Object.h"

#include <fstream>
#include <sstream>
#include <algorithm>

using namespace roxal;

AsyncIOManager& AsyncIOManager::instance()
{
    static AsyncIOManager inst;
    return inst;
}

AsyncIOManager::~AsyncIOManager()
{
    stop();
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
        std::list<PendingIOOp> toProcess;

        {
            std::unique_lock<std::mutex> lock(queueMutex);
            // Wait for work or shutdown
            queueCV.wait_for(lock, std::chrono::milliseconds(10), [this] {
                return !pendingOps.empty() || !running.load();
            });

            if (!running.load() && pendingOps.empty()) {
                break;
            }

            toProcess = std::move(pendingOps);
            pendingOps.clear();
        }

        // Process all pending operations
        for (auto& op : toProcess) {
            try {
                Value result = executeOp(op);
                op.promise->set_value(result);
            } catch (const std::exception& e) {
                // Set error as nil for now - could create exception value
                op.promise->set_value(Value::nilVal());
            }
        }
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
        Value lst { Value::listVal() };
        asList(lst)->elts.reserve(static_cast<size_t>(n));
        for (std::streamsize i = 0; i < n; ++i)
            asList(lst)->elts.push_back(Value::byteVal(static_cast<uint8_t>(buf[static_cast<size_t>(i)])));
        return lst;
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
        Value lst { Value::listVal() };
        asList(lst)->elts.reserve(data.size());
        for (char c : data)
            asList(lst)->elts.push_back(Value::byteVal(static_cast<uint8_t>(c)));
        return lst;
    }

    return Value::stringVal(toUnicodeString(data));
}

Value AsyncIOManager::executeFileWrite(PendingIOOp& op)
{
    if (!op.file) return Value::nilVal();

    std::lock_guard<std::mutex> lock(op.file->mutex);
    if (!op.file->file || !op.file->file->is_open())
        return Value::nilVal();

    if (op.binary) {
        for (char c : op.writeData) {
            op.file->file->put(c);
        }
    } else {
        (*op.file->file) << op.writeData;
    }

    return Value::nilVal();
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
    if (!op.file) return Value::nilVal();

    std::lock_guard<std::mutex> lock(op.file->mutex);
    if (op.file->file && op.file->file->is_open()) {
        op.file->file->close();
    }
    return Value::nilVal();
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
