#pragma once

#ifdef __EMSCRIPTEN__

namespace roxal {
namespace web {

// Give ustring's case mapping a Unicode implementation by borrowing the
// browser's (or node's) own -- toUpperCase/toLowerCase and Intl.Segmenter,
// which are ICU underneath in every engine that ships them.
//
// This is the same trade as the NN provider: the host already contains a
// large, correct, well-maintained implementation, so shipping a second copy
// inside the wasm binary would cost megabytes to be less right. Without it
// the builtin backend raises "builtin upper case is unsupported", and a
// script that upper-cases a string works natively and fails in a browser.
//
// Idempotent; safe to call before any script runs.
void installUnicodeHost();

} // namespace web
} // namespace roxal

#endif // __EMSCRIPTEN__
