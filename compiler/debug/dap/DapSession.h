#pragma once

// DAP protocol session: request dispatch, event emission, thread-id
// mapping, and the DebugHostControl / OutputSink integrations.
// Transport-agnostic: the adapter feeds parsed request bodies in (ON THE
// DEBUG WORKER -- the debugger's single controlling thread) and provides
// the two send lanes out.
//
// Send lanes: sendProtocol (responses + lifecycle events -- never dropped;
// the transport may apply backpressure) and sendOutput (program-output
// events -- droppable under a bounded queue; the session emits one
// coalesced truncation warning when drops occurred).
//
// Thread ids: DAP numbers are session-local int32 mappings of Thread::id()
// (uint64 ids would lose precision in JS clients) and persist across stops
// -- unlike frame/scope/variable references, which come from the per-stop
// DebugHandleTable and die on resume, exactly matching DAP's contract.

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <core/Output.h>
#include <core/json5.h>

#include "../DebugHostControl.h"
#include "../DebugInspector.h"
#include "../../Thread.h"

namespace roxal {

using json11::Json;

class StopCoordinator;
class VM;

class DapSession : public DebugHostControl, public OutputSink {
public:
    using SendFn = std::function<void(const std::string& frameBody)>;
    // Output lane returns false when the bounded queue dropped the frame.
    using OutputSendFn = std::function<bool(const std::string& frameBody)>;

    DapSession(VM& vm, StopCoordinator& coord,
               SendFn sendProtocol, OutputSendFn sendOutput);

    // ---- transport-side entry points ----
    // Handle one client request (call on the debug worker only).
    void handleRequest(const Json& req);

    // ---- main-thread lifecycle notifications (posted via the worker) ----
    // After preparation at launch: sends the deferred launch response and,
    // on success, the `initialized` event (breakpoints now verify).
    void onSetupComplete(bool ok, const std::string& error);
    // After execution returns: `exited` + `terminated` events.
    void onProgramEnded(int exitCode);
    // Transport fail-safe (client EOF / framing error / dead pipe): behave
    // as an implicit disconnect -- request exit, release a stopped
    // debuggee, and quit the session.
    void onTransportClosed();

    // ---- adapter queries ----
    bool launchRequested() const { return launchRequested_.load(); }
    bool configurationDone() const { return configurationDone_.load(); }
    bool quitRequested() const { return quitRequested_.load(); }
    const std::string& programPath() const { return programPath_; }
    bool stopOnEntry() const { return stopOnEntry_.load(); }
    void setStopOnEntry(bool v) { stopOnEntry_.store(v); }
    // Armed by the adapter right before a stop-on-entry run starts: the
    // first Step stop is then reported with reason "entry".
    void markEntryPending() { entryPending_.store(true); }
    // The main script thread's id, captured after setup (fallback for stop
    // events whose discovering thread is unknown).
    void setMainThreadId(uint64_t id) { mainThreadId_.store(id); }
    // How to end the debuggee on `terminate` / transport loss.  Default:
    // VM::requestExit(0), the process-owning CLI shape.  An embedding host
    // whose debuggee is one driven run installs its own (e.g. the run's
    // cooperative exit) so terminating the session never exits the host.
    // Set before the session handles requests.
    void setTerminateHandler(std::function<void()> fn) { terminate_ = std::move(fn); }

    // ---- DebugHostControl (called by the coordinator/worker) ----
    DebugHostResult onDebugHoldRequested(const DebugHoldRequest&) noexcept override;
    void onDebugStopped(const DebugStopNotice&) noexcept override;
    void onDebugContinueRequested(const DebugContinueNotice&) noexcept override;
    void onDebugStopFailed(const DebugStopFailure&) noexcept override;
    void onDebugSessionEnded(const DebugSessionEnd&) noexcept override;

    // ---- OutputSink (program output -> DAP output events) ----
    OutputResult emit(const OutputEventView& event) override;

private:
    void dispatchRequest(const Json& req);
    // request handlers (all run on the debug worker)
    Json reqInitialize(const Json& args);
    Json reqLaunch(const Json& req);            // response deferred
    Json reqSetBreakpoints(const Json& args);
    Json reqBreakpointLocations(const Json& args);
    Json reqThreads();
    Json reqStackTrace(const Json& args);
    Json reqScopes(const Json& args);
    Json reqVariables(const Json& args);
    Json reqContinue();
    Json reqStep(const Json& args, Thread::DebugStepMode mode, bool& ok, std::string& err);
    Json reqPause(bool& ok, std::string& err);

    void sendResponse(const Json& req, const Json& body,
                      bool success = true, const std::string& message = {});
    void sendEvent(const std::string& name, const Json& body);
    int nextSeq() { return seq_.fetch_add(1, std::memory_order_relaxed); }

    int32_t dapThreadId(uint64_t threadId);
    uint64_t roxalThreadId(int32_t dapId);
    void requestTerminate();

    VM& vm_;
    StopCoordinator& coord_;
    DebugInspector inspector_;
    SendFn sendProtocol_;
    OutputSendFn sendOutput_;

    std::atomic<int> seq_ { 1 };
    std::atomic<bool> launchRequested_ { false };
    std::atomic<bool> configurationDone_ { false };
    std::atomic<bool> quitRequested_ { false };
    std::atomic<bool> stopOnEntry_ { false };
    std::atomic<bool> entryPending_ { false };
    std::atomic<bool> terminatedSent_ { false };
    std::atomic<uint64_t> mainThreadId_ { 0 };
    std::string programPath_;
    std::function<void()> terminate_;
    int deferredLaunchSeq_ { 0 };   // worker-only state

    // client conventions from initialize
    bool clientColumnsStartAt1_ { true };
    bool clientLinesStartAt1_ { true };
    int toClientLine(int line) const { return clientLinesStartAt1_ ? line : line - 1; }
    int fromClientLine(int line) const { return clientLinesStartAt1_ ? line : line + 1; }

    std::mutex threadMapMutex_;
    std::unordered_map<uint64_t, int32_t> threadToDap_;
    std::unordered_map<int32_t, uint64_t> dapToThread_;
    int32_t nextDapThreadId_ { 1 };

    // last-known thread list (served while running, when inspection is off)
    std::mutex threadsCacheMutex_;
    std::vector<std::pair<int32_t, std::string>> threadsCache_;

    std::atomic<uint64_t> outputDropCount_ { 0 };
};

} // namespace roxal
