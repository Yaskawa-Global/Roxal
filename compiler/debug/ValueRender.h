#pragma once

#include <string>

#include "../Value.h"

namespace roxal {

// Bounded, side-effect-free Value preview.
//
// Guarantees:
//  - never invokes user code: no toString/objToString dispatch (which can run
//    a user `@implicit operator string()`), no getters -- object previews read
//    declared backing fields only;
//  - never resolves or waits on a future (polled, shown pending/ready);
//  - never calls into an actor, never subscribes to or samples a signal;
//  - bounded output AND bounded work: container previews cap at maxChildren
//    elements and maxDepth nesting -- and enumeration itself is bounded
//    (dict entries via itemsPrefix, declared properties via a capped walk),
//    so a huge value costs O(maxChildren), never O(n); strings clip at
//    maxStringChars code units; the whole result, truncation marker
//    included, never exceeds maxTotalChars;
//  - strings are escaped to stay one-line ('\n', '\t', quotes, backslash,
//    control bytes as \xNN);
//  - cycles are detected by object identity and rendered as "...".
//
// Thread-safety: the caller must legally hold `v` (its own frame/args, or a
// debugger holding a quiesced stop epoch).  Reads of shared containers use
// their snapshot-style accessors; deeper synchronized-inspection contracts
// (signals, native-backed objects) are not covered here.
struct RenderOptions {
    size_t maxStringChars = 512;   // max string code units before clipping
    size_t maxChildren    = 100;   // max container elements in a preview
    size_t maxDepth       = 1;     // container nesting depth rendered inline
    size_t maxTotalChars  = 4096;  // hard cap on the rendered result bytes
};

std::string renderValue(const Value& v, const RenderOptions& opts = {});

} // namespace roxal
