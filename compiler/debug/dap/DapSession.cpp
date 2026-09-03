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
        // Tier 1 only: safe path resolution, identical under every context
        // (hover/watch/repl).
        auto r = inspector_.evaluate(args["frameId"].int_value(),
                                     args["expression"].string_value());
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
    // Honest about the rest (no restart / attach / evaluate / disassembly).
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
    std::vector<int> lines;
    if (args["breakpoints"].is_array()) {
        for (const auto& b : args["breakpoints"].array_items())
            lines.push_back(fromClientLine(b["line"].int_value()));
    } else if (args["lines"].is_array()) {
        for (const auto& l : args["lines"].array_items())
            lines.push_back(fromClientLine(l.int_value()));
    }
    auto bound = BreakpointManager::instance().setBreakpoints(path, lines);
    Json::array out;
    for (const auto& b : bound) {
        Json::object bp { { "verified", b.verified } };
        bp["line"] = toClientLine(b.verified ? b.boundLine : b.requestedLine);
        out.push_back(Json(std::move(bp)));
    }
    return Json::object { { "breakpoints", out } };
}

Json DapSession::reqBreakpointLocations(const Json& args)
{
    Json::array out;
    std::string path = args["source"]["path"].string_value();
    if (path.empty())
        path = args["source"]["name"].string_value();
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
        if (!f.source.empty()) {
            // Only REAL files get a path: canonicalizing a synthetic source
            // name ("cli") would fabricate a filesystem path the client
            // then tries to open.
            std::error_code ec;
            if (std::filesystem::exists(f.source, ec) && !ec) {
                auto abs = std::filesystem::weakly_canonical(f.source, ec);
                fr["source"] = Json::object {
                    { "name", std::filesystem::path(f.source).filename().string() },
                    { "path", ec ? f.source : abs.string() },
                };
            } else {
                fr["source"] = Json::object { { "name", f.source } };
            }
        }
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
            { "expensive", false },
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
    if (coord_.isStopped())
        return Json();   // already stopped: benign
    auto out = coord_.requestStop(DebugStopReason::Pause,
                                  TimeDuration::milliSecs(3000));
    if (!out.stopped && !coord_.isStopped()) {
        // A racing breakpoint/step stop counts as success (a stopped event
        // is on its way); a genuine failure is reported.
        ok = false;
        err = out.failure;
    }
    return Json();
}

Json DapSession::reqContinue()
{
    if (coord_.isStopped())
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
    coord_.stepAndResume(*target, mode);
    return Json();
}

void DapSession::requestTerminate()
{
    if (terminate_)
        terminate_();
    else
        vm_.requestExit(0);
    if (coord_.isStopped()) {
        // Wake the parked threads so they observe the exit interrupt.  The
        // hold generation is NOT released (fail-safe); the session-end
        // notice follows from VM shutdown.
        coord_.resume(/*releaseHold=*/false);
    }
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
        if (entryWasPending && reason == "step")
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
        const std::string src(event.source->sourceName);
        std::error_code ec;
        Json::object srcObj { { "name",
            std::filesystem::path(src).filename().string() } };
        if (std::filesystem::exists(src, ec) && !ec) {
            auto abs = std::filesystem::weakly_canonical(src, ec);
            srcObj["path"] = ec ? src : abs.string();
        }
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
