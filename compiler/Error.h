#pragma once

#include <iostream>
#include <string>

namespace roxal {

inline void compileError(const std::string& message)
{
    std::cerr << "Compile error: " << message << std::endl;
}

}
