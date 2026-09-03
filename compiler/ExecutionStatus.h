#pragma once

namespace roxal {

enum class ExecutionStatus {
    OK,
    CompileError,
    RuntimeError,
    Yielded,  // Budget exhausted or blocked, state preserved for resume
    Paused,   // Debugger stop acknowledged, state preserved for resume.
              // Distinct from Yielded: a host must NOT treat this as a time
              // budget expiry (it will not clear by calling again) nor as
              // completion (frames are live and inspection may be in
              // progress).  Resume comes from the debugger releasing the
              // stop epoch, after which the normal call again succeeds.
    Busy      // The VM is not this caller's to run: an embedded driver owns
              // execution, or the VM has shut down.  NO work was done --
              // neither completion nor a suspension of any work of yours.
};

// True for the two suspended-but-resumable statuses.  Use this in resume
// loops that previously tested `== Yielded` so a debugger pause cannot be
// misread as completion or error.
inline constexpr bool isSuspended(ExecutionStatus s) {
    return s == ExecutionStatus::Yielded || s == ExecutionStatus::Paused;
}

}
