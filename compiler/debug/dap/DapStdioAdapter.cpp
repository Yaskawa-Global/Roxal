#include "DapStdioAdapter.h"

#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>

#include <core/Output.h>
#include <core/json5.h>

#include "DapCodec.h"
#include "DapSession.h"
#include "../StopCoordinator.h"
#include "../../Thread.h"
#include "../../VM.h"

namespace roxal {

DapStdioAdapter::DapStdioAdapter(std::string programPath, bool stopOnEntry,
                                 std::vector<std::string> modulePaths)
    : programPath_(std::move(programPath)), stopOnEntry_(stopOnEntry),
      modulePaths_(std::move(modulePaths))
{
    // stdio purity: the protocol owns a PRIVATE duplicate of the original
    // stdout; fd 1 is then pointed at stderr so any stray direct write
    // (std::cout, printf, a library) lands there instead of corrupting a
    // frame.
    protocolOutFd_ = ::dup(1);
    ::dup2(2, 1);
    // The dup'd descriptor is the protocol's private open file description
    // (the original fd 1 was repointed), so O_NONBLOCK affects nobody else;
    // the writer needs it so a full pipe surfaces as EAGAIN under poll
    // instead of an uninterruptible block.
    if (protocolOutFd_ >= 0)
        ::fcntl(protocolOutFd_, F_SETFL,
                ::fcntl(protocolOutFd_, F_GETFL) | O_NONBLOCK);
    // A closed client pipe must surface as EPIPE, not kill the process.
    std::signal(SIGPIPE, SIG_IGN);
    if (::pipe(wakePipe_) != 0)
        wakePipe_[0] = wakePipe_[1] = -1;
    if (::pipe(writerWakePipe_) != 0)
        writerWakePipe_[0] = writerWakePipe_[1] = -1;
}

DapStdioAdapter::~DapStdioAdapter()
{
    if (protocolOutFd_ >= 0)
        ::close(protocolOutFd_);
    for (int fd : wakePipe_)
        if (fd >= 0)
            ::close(fd);
    for (int fd : writerWakePipe_)
        if (fd >= 0)
            ::close(fd);
}

void DapStdioAdapter::enqueueProtocol(std::string frame)
{
    if (transportDead_.load(std::memory_order_acquire))
        return;
    std::unique_lock<std::mutex> lk(queueMutex_);
    // Bounded backpressure: a client that stops reading must not wedge the
    // debug worker (and thereby VM shutdown) forever.  After the grace
    // period the transport is declared dead and the session fail-safes
    // like a disconnect.
    if (!queueSpaceCv_.wait_for(lk, std::chrono::seconds(5), [&] {
            return queue_.size() < kQueueCap || writerStop_;
        })) {
        lk.unlock();
        if (!transportDead_.exchange(true) && session_)
            session_->onTransportClosed();
        return;
    }
    if (writerStop_)
        return;
    queue_.push_back(std::move(frame));
    queueCv_.notify_one();
}

bool DapStdioAdapter::enqueueOutput(std::string frame)
{
    std::lock_guard<std::mutex> lk(queueMutex_);
    if (writerStop_ || queue_.size() >= kQueueCap)
        return false;   // producers never block on output
    queue_.push_back(std::move(frame));
    queueCv_.notify_one();
    return true;
}

void DapStdioAdapter::writerLoop()
{
    for (;;) {
        std::string frame;
        {
            std::unique_lock<std::mutex> lk(queueMutex_);
            queueCv_.wait(lk, [&] { return !queue_.empty() || writerStop_; });
            if (queue_.empty() && writerStop_)
                return;
            frame = std::move(queue_.front());
            queue_.pop_front();
            queueSpaceCv_.notify_one();
        }
        const std::string framed = dapFrame(frame);
        size_t off = 0;
        while (off < framed.size()) {
            struct pollfd fds[2] = {
                { protocolOutFd_, POLLOUT, 0 },
                { writerWakePipe_[0], POLLIN, 0 },
            };
            if (::poll(fds, writerWakePipe_[0] >= 0 ? 2 : 1, -1) < 0)
                return;
            if (writerWakePipe_[0] >= 0 && (fds[1].revents & POLLIN))
                return;   // teardown wake
            if (fds[0].revents & (POLLERR | POLLHUP))
                return;   // client pipe gone
            if (!(fds[0].revents & POLLOUT))
                continue;
            const ssize_t n = ::write(protocolOutFd_, framed.data() + off,
                                      framed.size() - off);
            if (n < 0) {
                if (errno == EAGAIN || errno == EINTR)
                    continue;
                return;   // EPIPE etc.: client gone
            }
            off += size_t(n);
        }
    }
}

void DapStdioAdapter::readerLoop()
{
    DapFrameParser parser;
    char buf[16384];
    std::string body;
    for (;;) {
        // Wait on stdin OR the teardown self-pipe: a blocked read(0) is not
        // interruptible by closing fd 0, so teardown writes a wake byte.
        struct pollfd fds[2] = {
            { 0, POLLIN, 0 },
            { wakePipe_[0], POLLIN, 0 },
        };
        if (::poll(fds, wakePipe_[0] >= 0 ? 2 : 1, -1) < 0)
            break;
        if (wakePipe_[0] >= 0 && (fds[1].revents & POLLIN))
            break;   // teardown wake
        if ((fds[0].revents & (POLLIN | POLLHUP | POLLERR)) == 0)
            continue;
        const ssize_t n = ::read(0, buf, sizeof buf);
        if (n <= 0)
            break;   // EOF or stdin closed
        parser.append(buf, size_t(n));
        if (parser.error())
            break;   // unrecoverable framing: treat as disconnect
        while (parser.next(body)) {
            std::string err;
            Json msg = Json::parse(body, err);
            if (!err.empty() || !msg.is_object())
                continue;   // malformed JSON frame: skip
            if (msg["type"].string_value() != "request")
                continue;
            DapSession* session = session_.get();
            VM::instance().stopCoordinator().postToWorker(
                [session, msg] { session->handleRequest(msg); });
        }
        if (parser.error())
            break;
    }
    const bool teardown = tearingDown_.load(std::memory_order_acquire);
    {
        std::lock_guard<std::mutex> lk(gateMutex_);
        readerEof_ = true;
    }
    gateCv_.notify_all();
    if (!teardown) {
        // Genuine client EOF or framing failure with no polite disconnect:
        // fail-safe -- request exit and release a stopped debuggee so the
        // process cannot be orphaned.
        DapSession* session = session_.get();
        VM::instance().stopCoordinator().postToWorker(
            [session] { session->onTransportClosed(); });
    }
}

int DapStdioAdapter::run()
{
    VM& vm = VM::instance();
    auto& coord = vm.stopCoordinator();
    coord.ensureWorker();   // the controlling thread for every DAP command

    session_ = std::make_unique<DapSession>(
        vm, coord,
        [this](const std::string& f) { enqueueProtocol(f); },
        [this](const std::string& f) { return enqueueOutput(f); });
    if (stopOnEntry_)
        session_->setStopOnEntry(true);

    coord.setHostControl(session_.get());
    previousSink_ = OutputRouter::sink();   // restore at teardown (null =
                                            // builtin console sink)
    OutputRouter::setSink(session_.get());

    writer_ = std::thread([this] { writerLoop(); });
    reader_ = std::thread([this] { readerLoop(); });

    // The session flags flip on the debug worker with no gate notification:
    // a 100ms poll is plenty for these once-per-session transitions.
    auto waitGate = [&](auto pred) {
        std::unique_lock<std::mutex> lk(gateMutex_);
        while (!pred() && !session_->quitRequested() && !readerEof_)
            gateCv_.wait_for(lk, std::chrono::milliseconds(100));
        return pred();
    };

    int exitCode = 0;
    bool ran = false;

    // 1. launch -> compile on THIS (main) thread so VM::thread lands here.
    if (waitGate([&] { return session_->launchRequested(); })) {
        std::string program = session_->programPath().empty()
            ? programPath_ : session_->programPath();
        std::filesystem::path filePath(program);
        std::ifstream stream(filePath);
        if (!stream.is_open()) {
            for (const auto& mp : modulePaths_) {
                auto candidate = std::filesystem::path(mp) / filePath;
                stream.open(candidate);
                if (stream.is_open()) { filePath = candidate; break; }
            }
        }
        bool setupOk = false;
        std::string error;
        if (!stream.is_open()) {
            error = "file not found: " + program;
        } else {
            std::error_code ec;
            auto parent = std::filesystem::weakly_canonical(filePath, ec).parent_path();
            if (!ec)
                vm.appendModulePaths({ std::filesystem::relative(
                    parent, std::filesystem::current_path()).string() });
            vm.appendModulePaths(modulePaths_);
            try {
                // Prepared, not run: breakpoints are configured between
                // preparation and the first statement, which is the whole
                // reason a launch splits these two steps.
                ProgramOptions options;
                options.sourceName = filePath.string();
                setupOk = vm.stageProgramSync(stream, std::move(options))
                          == ExecutionStatus::OK;
                if (!setupOk)
                    error = "compilation failed";
            } catch (std::exception& e) {
                error = e.what();
            }
        }
        if (setupOk && VM::thread)
            session_->setMainThreadId(VM::thread->id());
        DapSession* session = session_.get();
        coord.postToWorker([session, setupOk, error] {
            session->onSetupComplete(setupOk, error);
        });

        // 2. configurationDone -> execute.
        if (setupOk && waitGate([&] { return session_->configurationDone(); })) {
            if (session_->stopOnEntry() && VM::thread) {
                // Arm a step-In before the first statement: the first
                // boundary publishes a Step stop reported as "entry".
                VM::thread->debugStepMode = Thread::DebugStepMode::In;
                VM::thread->debugStepActivation = 0;
                VM::thread->debugStepOriginIndex = 0;
                coord.addSlowPathDemand();
                session_->markEntryPending();
            }
            const ExecutionStatus res = vm.executeStagedSync();
            ran = true;
            exitCode = vm.isExitRequested() ? vm.exitCode()
                     : (res == ExecutionStatus::OK ? 0 : 1);
            DapSession* s2 = session_.get();
            coord.postToWorker([s2, exitCode] { s2->onProgramEnded(exitCode); });
        }
    }
    if (!ran && session_->launchRequested())
        exitCode = 1;

    // 3. Serve until the client disconnects (or its pipe closes); bounded so
    //    a vanished client cannot wedge the process.
    {
        std::unique_lock<std::mutex> lk(gateMutex_);
        gateCv_.wait_for(lk, std::chrono::seconds(10), [&] {
            return readerEof_ || session_->quitRequested();
        });
    }

    // 4. Teardown, in order: silence the INPUT side first -- once the
    //    reader is joined, no new command can reach the worker, so VM
    //    shutdown cannot race a freshly posted request (and the
    //    coordinator's retirement latch guards against worker re-creation
    //    regardless).
    tearingDown_.store(true, std::memory_order_release);
    if (wakePipe_[1] >= 0)
        (void)!::write(wakePipe_[1], "x", 1);   // wake a reader parked in poll
    if (reader_.joinable())
        reader_.join();

    //    Full VM shutdown next: resumes any stop, drains breakpoints, joins
    //    the debug worker (queued commands ran or were dropped), and
    //    delivers the session-end notice through the still-registered host
    //    control -- the writer is still alive to carry it.
    VM::shutdownIfConstructed();
    coord.setHostControl(nullptr);
    OutputRouter::setSink(previousSink_);

    //    Finally the writer: drain, then stop.
    {
        std::lock_guard<std::mutex> lk(queueMutex_);
        writerStop_ = true;
    }
    queueCv_.notify_all();
    queueSpaceCv_.notify_all();
    if (writerWakePipe_[1] >= 0)
        (void)!::write(writerWakePipe_[1], "x", 1);
    if (writer_.joinable())
        writer_.join();
    return exitCode;
}

} // namespace roxal
