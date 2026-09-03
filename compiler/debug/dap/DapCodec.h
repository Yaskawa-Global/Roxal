#pragma once

// DAP wire framing: Content-Length headers with byte counts, bodies
// handed to core/json5.h at the session layer.  The parser is incremental
// (frames may split or coalesce across reads), caps every frame BEFORE
// allocating for it, and latches a hard error on a malformed or oversized
// header -- a transport that cannot trust its framing must drop the
// connection, not resynchronize.

#include <cstddef>
#include <string>

namespace roxal {

// "Content-Length: N\r\n\r\n" + body.  N is a BYTE count.
std::string dapFrame(const std::string& body);

class DapFrameParser {
public:
    explicit DapFrameParser(size_t maxFrameBytes = 16u * 1024u * 1024u)
        : maxFrameBytes_(maxFrameBytes) {}

    // Feed raw transport bytes.  No-op after a framing error.
    void append(const char* data, size_t n);

    // Extract the next complete frame body; false when none is buffered.
    bool next(std::string& body);

    // A malformed header, non-numeric/oversized length, or missing
    // header terminator: the stream is unrecoverable.
    bool error() const { return error_; }
    const std::string& errorText() const { return errorText_; }

private:
    void fail(const std::string& why);

    std::string buf_;
    size_t maxFrameBytes_;
    bool error_ { false };
    std::string errorText_;
};

} // namespace roxal
