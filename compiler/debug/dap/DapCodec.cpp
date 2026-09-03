#include "DapCodec.h"

#include <cctype>

namespace roxal {

std::string dapFrame(const std::string& body)
{
    std::string out = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    out += body;
    return out;
}

void DapFrameParser::fail(const std::string& why)
{
    error_ = true;
    errorText_ = why;
    buf_.clear();
}

void DapFrameParser::append(const char* data, size_t n)
{
    if (error_)
        return;
    // The header region is tiny; a buffer growing large with no header
    // terminator is a garbage stream.
    buf_.append(data, n);
}

bool DapFrameParser::next(std::string& body)
{
    if (error_)
        return false;
    const size_t hdrEnd = buf_.find("\r\n\r\n");
    if (hdrEnd == std::string::npos) {
        if (buf_.size() > 8192)
            fail("header terminator not found within 8KB");
        return false;
    }

    // Parse headers: only Content-Length is meaningful; others are skipped.
    size_t contentLength = size_t(-1);
    size_t lineStart = 0;
    while (lineStart < hdrEnd) {
        size_t lineEnd = buf_.find("\r\n", lineStart);
        if (lineEnd == std::string::npos || lineEnd > hdrEnd)
            lineEnd = hdrEnd;
        const std::string line = buf_.substr(lineStart, lineEnd - lineStart);
        const size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string name = line.substr(0, colon);
            for (auto& c : name) c = char(std::tolower((unsigned char)c));
            if (name == "content-length") {
                size_t v = 0;
                size_t i = colon + 1;
                while (i < line.size() && line[i] == ' ') ++i;
                if (i >= line.size() || !std::isdigit((unsigned char)line[i])) {
                    fail("malformed Content-Length");
                    return false;
                }
                for (; i < line.size(); ++i) {
                    if (!std::isdigit((unsigned char)line[i])) {
                        fail("malformed Content-Length");
                        return false;
                    }
                    v = v * 10 + size_t(line[i] - '0');
                    if (v > maxFrameBytes_) {
                        fail("frame exceeds cap");
                        return false;
                    }
                }
                contentLength = v;
            }
        }
        lineStart = lineEnd + 2;
    }
    if (contentLength == size_t(-1)) {
        fail("missing Content-Length header");
        return false;
    }

    const size_t bodyStart = hdrEnd + 4;
    if (buf_.size() - bodyStart < contentLength)
        return false;   // body not fully buffered yet

    body.assign(buf_, bodyStart, contentLength);
    buf_.erase(0, bodyStart + contentLength);
    return true;
}

} // namespace roxal
