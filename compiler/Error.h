#pragma once

#include <iostream>
#include <string>
#include <memory>
#include <sstream>

#include <core/common.h>

namespace roxal {

inline ptr<std::string> compileSource;
inline std::string compileSourceName;

inline void setCompileContext(ptr<std::string> source,
                              const std::string& name)
{
    compileSource = std::move(source);
    compileSourceName = name;
}

inline void clearCompileContext()
{
    compileSource.reset();
    compileSourceName.clear();
}

inline void compileError(const std::string& message)
{
    int line = -1;
    int col  = -1;
    std::string msg { message };

    auto dash = message.find(" - ");
    if (dash != std::string::npos) {
        std::string pos = message.substr(0, dash);
        msg = message.substr(dash + 3);
        sscanf(pos.c_str(), "%d:%d", &line, &col);
    }

    if (line >= 0) {
        if (!compileSourceName.empty())
            fprintf(stderr, "%s:%d:%d: error: %s\n",
                    compileSourceName.c_str(), line, col, msg.c_str());
        else
            fprintf(stderr, "[line %d:%d]: error: %s\n", line, col, msg.c_str());

        if (compileSource && !compileSourceName.empty()) {
            std::istringstream src(*compileSource);
            std::string srcLine;
            for (int i = 1; i <= line && std::getline(src, srcLine); ++i) {
                if (i == line) {
                    fprintf(stderr, "    %d | %s\n", line, srcLine.c_str());
                    std::string lstr = std::to_string(line);
                    size_t indent = 4 + lstr.length() + 1; // spaces before '|'
                    fprintf(stderr, "%s| %s^\n", spaces(indent).c_str(), spaces(col).c_str());
                }
            }
        }
    } else {
        std::cerr << "Compile error: " << msg << std::endl;
    }
}

} // namespace roxal
