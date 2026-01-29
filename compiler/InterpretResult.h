#pragma once

namespace roxal {

enum class InterpretResult {
    OK,
    CompileError,
    RuntimeError,
    Yielded  // Budget exhausted or blocked, state preserved for resume
};

}
