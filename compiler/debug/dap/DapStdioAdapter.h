#pragma once

// Stdio DAP transport.
//
// Threads:
//  - reader: blocking reads of fd 0, incremental frame parsing (DapCodec);
//    each request is POSTED TO THE DEBUG WORKER, the debugger's single
//    controlling thread, where DapSession handles it.
//  - writer: drains a bounded frame queue to the protocol fd.  Protocol
//    frames (responses/lifecycle events) apply backpressure when full;
//    output frames DROP (producers never block) with a coalesced count.
//  - main (the caller of run()): owns the program lifecycle -- setup at
//    launch, execution at configurationDone, exited/terminated at return.
//
// stdio purity: the constructor duplicates fd 1 for the protocol's private
// use and points fd 1 at stderr, so no stray std::cout/printf can ever
// corrupt a frame; program print() flows through the OutputSink into DAP
// output events instead.

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace roxal {

class DapSession;
class OutputSink;
class VM;

class DapStdioAdapter {
public:
    // programPath: from the CLI when given; launch arguments.program
    // overrides.  stopOnEntry: the --dap-wait flag shape.
    DapStdioAdapter(std::string programPath, bool stopOnEntry,
                    std::vector<std::string> modulePaths);
    ~DapStdioAdapter();

    // Serve the session to completion; returns the process exit code.
    int run();

private:
    void readerLoop();
    void writerLoop();
    void enqueueProtocol(std::string frame);   // backpressure when full
    bool enqueueOutput(std::string frame);     // false = dropped

    std::string programPath_;
    bool stopOnEntry_ { false };
    std::vector<std::string> modulePaths_;

    int protocolOutFd_ { -1 };
    int wakePipe_[2] { -1, -1 };        // reader teardown self-pipe (closing
                                        // fd 0 does NOT wake a blocked read)
    int writerWakePipe_[2] { -1, -1 };  // writer teardown self-pipe (a write
                                        // blocked on a full pipe is likewise
                                        // uninterruptible without one)
    std::atomic<bool> tearingDown_ { false };   // distinguishes the teardown
                                                // wake from genuine client EOF
    std::atomic<bool> transportDead_ { false }; // protocol lane gave up
    OutputSink* previousSink_ { nullptr };

    std::unique_ptr<DapSession> session_;

    std::thread reader_;
    std::thread writer_;

    std::mutex queueMutex_;
    std::condition_variable queueCv_;      // writer wake
    std::condition_variable queueSpaceCv_; // protocol backpressure
    std::deque<std::string> queue_;
    bool writerStop_ { false };
    static constexpr size_t kQueueCap = 4096;

    std::mutex gateMutex_;
    std::condition_variable gateCv_;
    bool readerEof_ { false };
};

} // namespace roxal
