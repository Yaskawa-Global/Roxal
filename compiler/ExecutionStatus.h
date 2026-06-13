#pragma once

namespace roxal {

enum class ExecutionStatus {
    OK,
    CompileError,
    RuntimeError,
    Yielded  // Budget exhausted or blocked, state preserved for resume
};

}
