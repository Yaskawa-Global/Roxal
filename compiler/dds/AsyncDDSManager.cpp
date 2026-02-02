#ifdef ROXAL_ENABLE_DDS

#include "AsyncDDSManager.h"

using namespace roxal;

AsyncDDSManager& AsyncDDSManager::instance()
{
    static AsyncDDSManager inst;
    return inst;
}

AsyncDDSManager::~AsyncDDSManager()
{
    stop();
}

void AsyncDDSManager::start()
{
    if (!running.load()) {
        running = true;
        workerThread = std::thread(&AsyncDDSManager::workerLoop, this);
    }
}

void AsyncDDSManager::stop()
{
    if (running.load()) {
        running = false;
        queueCV.notify_all();
        if (workerThread.joinable()) {
            workerThread.join();
        }
    }
}

Value AsyncDDSManager::submit(PendingDDSOp op)
{
    // Ensure worker is running
    if (!running.load()) {
        start();
    }

    ptr<std::promise<Value>> promise = make_ptr<std::promise<Value>>();
    op.promise = promise;
    std::shared_future<Value> future = promise->get_future().share();
    op.future = future;

    {
        std::lock_guard<std::mutex> lock(queueMutex);
        pendingOps.push_back(std::move(op));
    }
    queueCV.notify_one();

    return Value::futureVal(future);
}

void AsyncDDSManager::workerLoop()
{
    while (running.load()) {
        std::list<PendingDDSOp> toProcess;

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

Value AsyncDDSManager::executeOp(PendingDDSOp& op)
{
    switch (op.type) {
        case PendingDDSOp::Type::DdsWrite:
            return executeDdsWrite(op);
    }
    return Value::nilVal();
}

Value AsyncDDSManager::executeDdsWrite(PendingDDSOp& op)
{
    if (op.writer <= 0 || !op.sample)
        return Value::nilVal();

    dds_return_t rc = ::dds_write(op.writer, op.sample);

    // Clean up sample
    if (op.sample && op.descriptor) {
        dds_sample_free(op.sample, op.descriptor.get(), DDS_FREE_ALL);
        op.sample = nullptr;
    }

    if (rc < 0) {
        // Could throw or return error value - for now return nil
        return Value::nilVal();
    }

    return Value::nilVal();
}

#endif // ROXAL_ENABLE_DDS
