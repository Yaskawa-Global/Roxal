#include "DapSession.h"

#include <filesystem>

#include "../BreakpointManager.h"
#include "../DebugInfo.h"
#include "../ModuleDebugIndex.h"
#include "../StopCoordinator.h"
#include "../../Chunk.h"
#include "../../Object.h"
#include "../../VM.h"

namespace roxal {

DapSession::DapSession(VM& vm, StopCoordinator& coord,
                       SendFn sendProtocol, OutputSendFn sendOutput)
    : vm_(vm), coord_(coord), inspector_(coord),
      sendProtocol_(std::move(sendProtocol)), sendOutput_(std::move(sendOutput))
{
    // A module compiled or imported after its breakpoints were set rebinds
    // them: report the change, so "unverified" turns verified.  Sent at once
    // from the registering (compiling) thread, never via the worker: the
    // module may run -- and stop at one of these breakpoints -- before the
    // worker would get to it, and the client must hear "verified" before
    // "stopped".  (The destructor clears the listener, waiting out a call.)
    BreakpointManager::instance().setRebindListener(
        [this](const std::string& key, const std::vector<BreakpointManager::Bound>& bound) {
            std::vector<std::pair<int, std::pair<bool, int>>> plain;
            for (const auto& b : bound)
                plain.push_back({ b.requestedLine, { b.verified, b.boundLine } });
            onBreakpointsRebound(key, plain);
        });
}

DapSession::~DapSession()
{
    BreakpointManager::instance().setRebindListener(nullptr);
}

// ---- id mapping -------------------------------------------------------------

int32_t DapSession::dapThreadId(uint64_t threadId)
{
    std::lock_guard<std::mutex> lk(threadMapMutex_);
    auto it = threadToDap_.find(threadId);
    if (it != threadToDap_.end())
        return it->second;
    const int32_t id = nextDapThreadId_++;
    threadToDap_.emplace(threadId, id);
    dapToThread_.emplace(id, threadId);
    return id;
}

uint64_t DapSession::roxalThreadId(int32_t dapId)
{
    std::lock_guard<std::mutex> lk(threadMapMutex_);
    auto it = dapToThread_.find(dapId);
    return it == dapToThread_.end() ? 0 : it->second;
}

// ---- send helpers -----------------------------------------------------------

void DapSession::sendResponse(const Json& req, const Json& body,
                              bool success, const std::string& message)
{
    Json::object resp {
        { "type", "response" },
        { "seq", nextSeq() },
        { "request_seq", req["seq"].int_value() },
        { "command", req["command"].string_value() },
        { "success", success },
    };
    if (!body.is_null())
        resp["body"] = body;
    if (!message.empty())
        resp["message"] = message;
    sendProtocol_(Json(std::move(resp)).dump());
}

void DapSession::sendEvent(const std::string& name, const Json& body)
{
    Json::object ev {
        { "type", "event" },
        { "seq", nextSeq() },
        { "event", name },
    };
    if (!body.is_null())
        ev["body"] = body;
    sendProtocol_(Json(std::move(ev)).dump());
}

// ---- request dispatch -------------------------------------------------------

void DapSession::handleRequest(const Json& req)
{
    // Every request gets a response, no matter what: a handler exception
    // must become a failure response, not a silently dropped command in
    // the worker's catch-all.
    try {
        dispatchRequest(req);
    } catch (const std::exception& e) {
        try {
            sendResponse(req, Json(), false,
                         std::string("internal error: ") + e.what());
        } catch (...) {}
    } catch (...) {
        try {
            sendResponse(req, Json(), false, "internal error");
        } catch (...) {}
    }
}

void DapSession::dispatchRequest(const Json& req)
{
    const std::string& cmd = req["command"].string_value();
    const Json& args = req["arguments"];

    if (cmd == "initialize") {
        sendResponse(req, reqInitialize(args));
        return;
    }
    if (cmd == "launch") {
        reqLaunch(req);   // response deferred until setup completes
        return;
    }
    if (cmd == "attach") {
        // The host starts the run (and binds it: bindRun); breakpoints may be
        // set at once -- they bind as the run's modules compile.
        if (launchRequested_.load() || attached_.exchange(true)) {
            sendResponse(req, Json(), false, "already launched or attached");
            return;
        }
        sendResponse(req, Json());
        sendEvent("initialized", Json());
        return;
    }
    if (cmd == "configurationDone") {
        configurationDone_.store(true, std::memory_order_release);
        sendResponse(req, Json());
        return;
    }
    if (cmd == "setBreakpoints") {
        sendResponse(req, reqSetBreakpoints(args));
        return;
    }
    if (cmd == "breakpointLocations") {
        sendResponse(req, reqBreakpointLocations(args));
        return;
    }
    if (cmd == "threads") {
        sendResponse(req, reqThreads());
        return;
    }
    if (cmd == "stackTrace") {
        sendResponse(req, reqStackTrace(args));
        return;
    }
    if (cmd == "scopes") {
        sendResponse(req, reqScopes(args));
        return;
    }
    if (cmd == "variables") {
        sendResponse(req, reqVariables(args));
        return;
    }
    if (cmd == "pause") {
        bool ok = true; std::string err;
        Json body = reqPause(ok, err);
        sendResponse(req, body, ok, err);
        return;
    }
    if (cmd == "continue") {
        sendResponse(req, reqContinue());
        if (coord_.isStopped())
            sendHeldStopped();   // the host's hold keeps it stopped
        return;
    }
    if (cmd == "next" || cmd == "stepIn" || cmd == "stepOut") {
        const auto mode = cmd == "next" ? Thread::DebugStepMode::Next
                        : cmd == "stepIn" ? Thread::DebugStepMode::In
                                          : Thread::DebugStepMode::Out;
        bool ok = true; std::string err;
        Json body = reqStep(args, mode, ok, err);
        sendResponse(req, body, ok, err);
        return;
    }
    if (cmd == "setExceptionBreakpoints") {
        bool stopOnError = false;
        if (args["filters"].is_array())
            for (const auto& fl : args["filters"].array_items())
                if (fl.string_value() == "error")
                    stopOnError = true;
        coord_.setStopOnFatal(stopOnError);
        Json::array bps;
        if (args["filters"].is_array())
            for (size_t i = 0; i < args["filters"].array_items().size(); ++i)
                bps.push_back(Json(Json::object { { "verified", true } }));
        sendResponse(req, Json::object { { "breakpoints", bps } });
        return;
    }
    if (cmd == "exceptionInfo") {
        auto info = coord_.stoppedInfo();
        if (info && info->reason == DebugStopReason::Exception) {
            sendResponse(req, Json::object {
                { "exceptionId", "runtime-error" },
                { "description", info->description },
                { "breakMode", "unhandled" },
            });
        } else {
            sendResponse(req, Json(), false, "no exception is active");
        }
        return;
    }
    if (cmd == "evaluate") {
        // Tier 1 first, under every context: a path is read with no user
        // code.  Hover stops there; watch and repl go on to Tier 2 -- any
        // expression, read-only, in the frame's scope (evaluateExpression).
        const int frameId = args["frameId"].int_value();
        const std::string expr = args["expression"].string_value();
        auto r = inspector_.evaluate(frameId, expr);
        const std::string context = args["context"].string_value();
        if (!r.ok && context != "hover" && coord_.isStopped())
            r = inspector_.evaluateExpression(frameId, expr, TimeDuration::milliSecs(2000),
                                              /*asStatement=*/context == "repl");
        if (r.ok) {
            sendResponse(req, Json::object {
                { "result", r.value },
                { "type", r.type },
                { "variablesReference", r.childHandle },
            });
        } else {
            sendResponse(req, Json(), false, r.error);
        }
        return;
    }
    if (cmd == "source") {
        // The text the VM compiled (ModuleDebugIndex keeps it), by reference
        // (a synthetic source) or by path -- for a read-only view of a frame
        // in a file the client does not have.
        std::string key;
        const int ref = args["sourceReference"].int_value();
        if (ref > 0) {
            auto it = sourceRefs_.find(ref);
            if (it != sourceRefs_.end())
                key = it->second;
        } else {
            key = args["source"]["path"].string_value();
            if (key.empty())
                key = args["source"]["name"].string_value();
            key = vmPath(key);
        }
        std::optional<ModuleDebugIndex::SourceEntry> entry;
        if (!key.empty())
            entry = ModuleDebugIndex::instance().lookupBySource(key);
        if (entry && !entry->sourceText.empty())
            sendResponse(req, Json::object {
                { "content", entry->sourceText },
                { "mimeType", "text/x-roxal" },
            });
        else
            sendResponse(req, Json(), false, "source content not available");
        return;
    }
    if (cmd == "terminate") {
        requestTerminate();
        sendResponse(req, Json());
        return;
    }
    if (cmd == "disconnect") {
        // disconnect(terminateDebuggee=false) is detach, which is
        // unsupported until detach ownership is explicit: reject rather
        // than terminate against the client's request.
        if (args["terminateDebuggee"].is_bool()
            && !args["terminateDebuggee"].bool_value()) {
            sendResponse(req, Json(), false,
                         "disconnect without terminating the debuggee is not supported");
            return;
        }
        requestTerminate();
        sendResponse(req, Json());
        quitRequested_.store(true, std::memory_order_release);
        return;
    }
    // Honest about the rest (no restart / disassembly / setVariable ...).
    sendResponse(req, Json(), false, "unsupported request: " + cmd);
}

Json DapSession::reqInitialize(const Json& args)
{
    if (args["columnsStartAt1"].is_bool())
        clientColumnsStartAt1_ = args["columnsStartAt1"].bool_value();
    if (args["linesStartAt1"].is_bool())
        clientLinesStartAt1_ = args["linesStartAt1"].bool_value();
    // `initialized` is sent after launch's setup succeeds: breakpoints set
    // afterwards then verify against the COMPILED module immediately.
    return Json::object {
        { "supportsConfigurationDoneRequest", true },
        { "supportsBreakpointLocationsRequest", true },
        { "supportsTerminateRequest", true },
        { "supportsRestartRequest", false },
        { "supportsEvaluateForHovers", true },   // Tier 1 paths (safe reads)
        { "supportsExceptionInfoRequest", true },
        { "exceptionBreakpointFilters", Json::array {
            Json(Json::object {
                { "filter", "error" },
                { "label", "Runtime errors (fatal / uncaught)" },
                { "default", true },
            }),
        } },
        { "supportsStepBack", false },
        { "supportsDisassembleRequest", false },
    };
}

Json DapSession::reqLaunch(const Json& req)
{
    // Launch-once: a second launch would mutate programPath_ concurrently
    // with the main thread reading it and clobber the deferred response
    // seq.  Rejected deterministically (no restart until a real
    // ExecutionSession reset boundary exists).
    if (launchRequested_.load(std::memory_order_acquire)) {
        sendResponse(req, Json(), false, "launch already received (restart is not supported)");
        return Json();
    }
    if (attached_.load(std::memory_order_acquire)) {
        sendResponse(req, Json(), false, "attached: the host starts the run");
        return Json();
    }
    const Json& args = req["arguments"];
    if (args["program"].is_string())
        programPath_ = args["program"].string_value();
    if (args["stopOnEntry"].is_bool() && args["stopOnEntry"].bool_value())
        stopOnEntry_.store(true);
    deferredLaunchSeq_ = req["seq"].int_value();
    launchRequested_.store(true, std::memory_order_release);
    return Json();
}

void DapSession::onSetupComplete(bool ok, const std::string& error)
{
    // The deferred launch response.
    Json::object resp {
        { "type", "response" },
        { "seq", nextSeq() },
        { "request_seq", deferredLaunchSeq_ },
        { "command", "launch" },
        { "success", ok },
    };
    if (!ok)
        resp["message"] = error.empty() ? "compilation failed" : error;
    sendProtocol_(Json(std::move(resp)).dump());
    if (ok) {
        sendEvent("initialized", Json());
    } else {
        sendEvent("terminated", Json());
        terminatedSent_.store(true);
    }
}

void DapSession::onTransportClosed()
{
    requestTerminate();
    quitRequested_.store(true, std::memory_order_release);
}

void DapSession::onProgramEnded(int exitCode)
{
    const uint64_t drops = outputDropCount_.exchange(0);
    if (drops > 0)
        sendEvent("output", Json::object {
            { "category", "console" },
            { "output", "[roxal] " + std::to_string(drops)
                        + " output event(s) dropped (bounded queue)\n" },
        });
    sendEvent("exited", Json::object { { "exitCode", exitCode } });
    if (!terminatedSent_.exchange(true))
        sendEvent("terminated", Json());
}

Json DapSession::reqSetBreakpoints(const Json& args)
{
    std::string path = args["source"]["path"].string_value();
    if (path.empty())
        path = args["source"]["name"].string_value();
    path = vmPath(path);
    const std::string key = BreakpointManager::requestKey(path);
    std::vector<int> lines;
    if (args["breakpoints"].is_array()) {
        for (const auto& b : args["breakpoints"].array_items())
            lines.push_back(fromClientLine(b["line"].int_value()));
    } else if (args["lines"].is_array()) {
        for (const auto& l : args["lines"].array_items())
            lines.push_back(fromClientLine(l.int_value()));
    }
    auto bound = BreakpointManager::instance().setBreakpoints(path, lines);
    std::lock_guard<std::mutex> ids(breakpointIdsMutex_);
    Json::array out;
    for (const auto& b : bound) {
        const int line = b.verified ? b.boundLine : b.requestedLine;
        auto [it, fresh] = breakpointIds_.try_emplace({ key, b.requestedLine },
                                                      BpState { nextBreakpointId_, false, 0 });
        if (fresh)
            ++nextBreakpointId_;
        it->second.verified = b.verified;
        it->second.line = line;
        Json::object bp { { "id", it->second.id }, { "verified", b.verified } };
        bp["line"] = toClientLine(line);
        out.push_back(Json(std::move(bp)));
    }
    return Json::object { { "breakpoints", out } };
}

void DapSession::onBreakpointsRebound(
    const std::string& key, const std::vector<std::pair<int, std::pair<bool, int>>>& bound)
{
    std::lock_guard<std::mutex> ids(breakpointIdsMutex_);
    for (const auto& [requested, state] : bound) {
        auto it = breakpointIds_.find({ key, requested });
        if (it == breakpointIds_.end())
            continue;   // not set through this session
        const bool verified = state.first;
        const int line = verified ? state.second : requested;
        if (it->second.verified == verified && it->second.line == line)
            continue;
        it->second.verified = verified;
        it->second.line = line;
        sendEvent("breakpoint", Json::object {
            { "reason", "changed" },
            { "breakpoint", Json::object {
                { "id", it->second.id },
                { "verified", verified },
                { "line", toClientLine(line) },
            } },
        });
    }
}

Json DapSession::clientSource(const std::string& vmSource)
{
    std::error_code ec;
    std::string abs;
    if (std::filesystem::exists(vmSource, ec) && !ec) {
        auto c = std::filesystem::weakly_canonical(vmSource, ec);
        abs = ec ? vmSource : c.string();
    }
    if (mapper_.toClient) {
        if (auto mapped = mapper_.toClient(abs.empty() ? vmSource : abs)) {
            return Json::object {
                { "name", std::filesystem::path(*mapped).filename().string() },
                { "path", *mapped },
            };
        }
    }
    Json::object src;
    if (!abs.empty()) {
        // Only REAL files get a path: canonicalizing a synthetic source name
        // ("cli") would fabricate a filesystem path the client then tries to
        // open.
        src["name"] = std::filesystem::path(vmSource).filename().string();
        src["path"] = abs;
    } else {
        // Synthetic: served by reference (`source`).
        src["name"] = vmSource;
        auto [it, fresh] = sourceRefOf_.try_emplace(vmSource, int(sourceRefOf_.size()) + 1);
        if (fresh)
            sourceRefs_[it->second] = vmSource;
        src["sourceReference"] = it->second;
    }
    if (mapper_.toClient)
        src["presentationHint"] = "deemphasize";   // not one of the client's files
    return src;
}

Json DapSession::reqBreakpointLocations(const Json& args)
{
    Json::array out;
    std::string path = args["source"]["path"].string_value();
    if (path.empty())
        path = args["source"]["name"].string_value();
    path = vmPath(path);
    const int startLine = fromClientLine(args["line"].int_value());
    const int endLine = args["endLine"].is_number()
        ? fromClientLine(args["endLine"].int_value()) : startLine;
    auto entry = ModuleDebugIndex::instance().lookupBySource(path);
    if (entry.has_value() && isList(entry->functions)) {
        std::vector<int> lines;
        ObjList* fns = asList(entry->functions);
        for (int32_t fi = 0; fi < fns->length(); ++fi) {
            Value fv = fns->getElement(size_t(fi));
            if (!isFunction(fv))
                continue;
            const Chunk* ch = asFunction(fv)->chunk.get();
            if (!ch || !ch->debugInfo)
                continue;
            for (const auto& st : ch->debugInfo->stmts) {
                if (st.kind != uint8_t(DebugStmtKind::StatementStart))
                    continue;
                if (st.line >= startLine && st.line <= endLine)
                    lines.push_back(st.line);
            }
        }
        std::sort(lines.begin(), lines.end());
        lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
        for (int l : lines)
            out.push_back(Json(Json::object { { "line", toClientLine(l) } }));
    }
    return Json::object { { "breakpoints", out } };
}

Json DapSession::reqThreads()
{
    Json::array out;
    if (coord_.isStopped()) {
        std::vector<std::pair<int32_t, std::string>> cache;
        for (const auto& td : inspector_.threads()) {
            const int32_t id = dapThreadId(td.threadId);
            const std::string name =
                td.threadId == mainThreadId_.load() ? "main" : td.name;
            cache.emplace_back(id, name);
            out.push_back(Json(Json::object {
                { "id", id }, { "name", name } }));
        }
        std::lock_guard<std::mutex> lk(threadsCacheMutex_);
        threadsCache_ = std::move(cache);
        return Json::object { { "threads", out } };
    }
    // Running: serve the last stop's list (inspection needs a stop); before
    // any stop, the main thread alone.
    std::lock_guard<std::mutex> lk(threadsCacheMutex_);
    if (threadsCache_.empty() && mainThreadId_.load() != 0)
        threadsCache_.emplace_back(dapThreadId(mainThreadId_.load()), "main");
    for (const auto& [id, name] : threadsCache_)
        out.push_back(Json(Json::object { { "id", id }, { "name", name } }));
    return Json::object { { "threads", out } };
}

Json DapSession::reqStackTrace(const Json& args)
{
    const uint64_t tid = roxalThreadId(args["threadId"].int_value());
    const size_t startFrame = args["startFrame"].is_number()
        ? size_t(args["startFrame"].int_value()) : 0;
    const size_t levels = args["levels"].is_number()
        ? size_t(args["levels"].int_value()) : 0;
    Json::array out;
    for (const auto& f : inspector_.stackTrace(tid, startFrame, levels)) {
        Json::object fr {
            { "id", f.handle },
            { "name", f.name },
            { "line", toClientLine(f.line) },
            { "column", clientColumnsStartAt1_ ? f.column + 1 : f.column },
        };
        if (!f.source.empty())
            fr["source"] = clientSource(f.source);
        out.push_back(Json(std::move(fr)));
    }
    return Json::object { { "stackFrames", out } };
}

Json DapSession::reqScopes(const Json& args)
{
    Json::array out;
    for (const auto& sc : inspector_.scopes(args["frameId"].int_value()))
        out.push_back(Json(Json::object {
            { "name", sc.name },
            { "variablesReference", sc.variablesHandle },
            { "expensive", sc.expensive },
        }));
    return Json::object { { "scopes", out } };
}

Json DapSession::reqVariables(const Json& args)
{
    const int32_t ref = args["variablesReference"].int_value();
    const size_t start = args["start"].is_number()
        ? size_t(args["start"].int_value()) : 0;
    const size_t count = args["count"].is_number()
        ? size_t(args["count"].int_value()) : 0;
    Json::array out;
    for (const auto& v : inspector_.variables(ref, start, count)) {
        Json::object var {
            { "name", v.name },
            { "value", v.value },
            { "type", v.type },
            { "variablesReference", v.childHandle },
        };
        if (v.synthetic)
            var["presentationHint"] = Json::object {
                { "visibility", "internal" } };
        out.push_back(Json(std::move(var)));
    }
    return Json::object { { "variables", out } };
}

Json DapSession::reqPause(bool& ok, std::string& err)
{
    // Non-blocking: the stop is pending until every thread has parked (one
    // inside a native call parks when it returns); `stopped` follows then.
    // Already stopped (the host's hold, say): the stop is now ours too.
    if (coord_.requestDebuggerPause() == StopCoordinator::PauseState::Idle) {
        ok = false;
        err = "nothing is running";
    }
    return Json();
}

Json DapSession::reqContinue()
{
    // Releases the debugger's part of a stop -- or withdraws one still
    // pending (a pause or breakpoint waiting for a thread in a native call).
    coord_.resume(/*releaseHold=*/true);
    return Json::object { { "allThreadsContinued", true } };
}

Json DapSession::reqStep(const Json& args, Thread::DebugStepMode mode,
                         bool& ok, std::string& err)
{
    if (!coord_.isStopped()) {
        ok = false;
        err = "not stopped";
        return Json();
    }
    const uint64_t tid = roxalThreadId(args["threadId"].int_value());
    ptr<Thread> target;
    coord_.forEachEpochThread([&](const ptr<Thread>& t) {
        if (t->id() == tid)
            target = t;
    });
    if (!target) {
        ok = false;
        err = "unknown thread";
        return Json();
    }
    clientStepArmed_.store(true);
    if (!coord_.stepAndResume(*target, mode)) {
        clientStepArmed_.store(false);
        ok = false;
        err = coord_.pauseStatus().host ? "held by the controller: release it to step"
                                        : "cannot step";
    }
    return Json();
}

void DapSession::sendHeldStopped()
{
    // The debugger's part of the stop is gone but the host still holds it:
    // tell the client it is (still) stopped, after the continue response.
    auto info = coord_.stoppedInfo();
    uint64_t tid = info ? info->threadId : 0;
    if (tid == 0)
        tid = mainThreadId_.load();
    sendEvent("stopped", Json::object {
        { "reason", "pause" },
        { "description", "Held by the controller" },
        { "threadId", dapThreadId(tid) },
        { "allThreadsStopped", true },
    });
}

void DapSession::requestTerminate()
{
    if (terminate_)
        terminate_();
    else
        vm_.requestExit(0);
    // Wake the parked threads so they observe the exit interrupt, whoever
    // owns the stop.  The hold generation is NOT released (fail-safe); the
    // session-end notice follows from VM shutdown.
    coord_.releaseForExit();
}

// ---- DebugHostControl -------------------------------------------------------

DebugHostResult DapSession::onDebugHoldRequested(const DebugHoldRequest&) noexcept
{
    return DebugHostResult::Queued;   // no external motion to hold in the CLI
}

void DapSession::onDebugStopped(const DebugStopNotice&) noexcept
{
    // noexcept boundary: serialization/enqueue below allocate; an exception
    // escaping would std::terminate.
    try {
        auto info = coord_.stoppedInfo();
        std::string reason = "pause";
        if (info) {
            switch (info->reason) {
                case DebugStopReason::Breakpoint: reason = "breakpoint"; break;
                case DebugStopReason::Step:       reason = "step"; break;
                case DebugStopReason::Exception:  reason = "exception"; break;
                case DebugStopReason::Pause:
                case DebugStopReason::Host:       reason = "pause"; break;
            }
        }
        // Consumed on the FIRST stop regardless of reason: if a breakpoint
        // preempted the armed entry step, the breakpoint label wins and a
        // LATER step must not resurrect "entry".
        const bool entryWasPending = entryPending_.exchange(false);
        const bool clientStep = clientStepArmed_.exchange(false);
        // The entry step is armed at activation, which for an attached
        // session may come before the host binds the run: a Step stop the
        // client never asked for is that one.
        if (reason == "step" && (entryWasPending || !clientStep))
            reason = "entry";
        uint64_t tid = info ? info->threadId : 0;
        if (tid == 0)
            tid = mainThreadId_.load();
        Json::object body {
            { "reason", reason },
            { "threadId", dapThreadId(tid) },
            { "allThreadsStopped", true },
        };
        if (reason == "exception" && info && !info->description.empty())
            body["text"] = info->description;
        if (info && info->reason == DebugStopReason::Host)
            body["description"] = "Held by the controller";
        sendEvent("stopped", Json(std::move(body)));
    } catch (...) {}
}

void DapSession::onDebugContinueRequested(const DebugContinueNotice&) noexcept
{
    try {
        sendEvent("continued", Json::object { { "allThreadsContinued", true } });
    } catch (...) {}
}

void DapSession::onDebugStopFailed(const DebugStopFailure& f) noexcept
{
    try {
        sendEvent("output", Json::object {
            { "category", "console" },
            { "output", "[roxal] stop failed: " + f.failedThread + "\n" },
        });
    } catch (...) {}
}

void DapSession::onDebugSessionEnded(const DebugSessionEnd&) noexcept
{
    try {
        if (!terminatedSent_.exchange(true))
            sendEvent("terminated", Json());
    } catch (...) {}
}

// ---- OutputSink -------------------------------------------------------------

OutputResult DapSession::emit(const OutputEventView& event)
{
    // Copy into an owned DAP output frame; the transport's output lane is
    // bounded and DROPS on overflow (producers never block) -- the session
    // reports a coalesced truncation count at program end.
    const std::string category =
        event.channel == "stderr" ? "stderr" : "stdout";
    Json::object body {
        { "category", category },
        { "output", std::string(event.text) },
    };
    if (event.kind != OutputKind::Print)
        body["output"] = std::string(event.text) + "\n";
    if (event.source.has_value() && !event.source->sourceName.empty()) {
        // (Program threads: no source references here -- that map is the
        // worker's.  A synthetic source goes by name.)
        const std::string src(event.source->sourceName);
        std::error_code ec;
        std::string abs;
        if (std::filesystem::exists(src, ec) && !ec) {
            auto c = std::filesystem::weakly_canonical(src, ec);
            abs = ec ? src : c.string();
        }
        Json::object srcObj { { "name", std::filesystem::path(src).filename().string() } };
        std::optional<std::string> mapped;
        if (mapper_.toClient)
            mapped = mapper_.toClient(abs.empty() ? src : abs);
        if (mapped)
            srcObj["path"] = *mapped;
        else if (!abs.empty())
            srcObj["path"] = abs;
        body["source"] = Json(std::move(srcObj));
        if (event.source->line > 0)
            body["line"] = toClientLine(int(event.source->line));
    }
    Json::object ev {
        { "type", "event" },
        { "seq", nextSeq() },
        { "event", "output" },
        { "body", Json(std::move(body)) },
    };
    if (!sendOutput_(Json(std::move(ev)).dump())) {
        outputDropCount_.fetch_add(1, std::memory_order_relaxed);
        return OutputResult::Dropped;
    }
    return OutputResult::Accepted;
}

} // namespace roxal
