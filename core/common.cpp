
#include <cstdarg>
#include <vector>

#include "common.h"

using namespace roxal;


// demangle typeid(T).name() strings to be more human readable
// (https://stackoverflow.com/questions/281818/unmangling-the-result-of-stdtype-infoname)
#ifdef __GNUG__
#include <cstdlib>
#include <memory>
#include <cxxabi.h>

std::string roxal::demangle(const char* name) 
{

    int status = -4; // some arbitrary value to eliminate the compiler warning

    // enable c++11 by passing the flag -std=c++11 to g++
    std::unique_ptr<char, void(*)(void*)> res {
        abi::__cxa_demangle(name, NULL, NULL, &status),
        std::free
    };

    return (status==0) ? res.get() : name ;
}

#else

// does nothing if not g++
std::string roxal::demangle(const char* name) {
    return name;
}

#endif




std::string roxal::stringInterval(const std::string s, size_t startLine, size_t startPos, size_t endLine, size_t endPos)
{
    // TODO: handle non-\n line endings
    long linenum=1;
    long pos=0;
    long i=0;
    long starti;
    for(;i<s.size();i++) {
        if (linenum==startLine && pos==startPos) {
            starti = i;
            break;
        }
        if (s.at(i) == '\n') {
            linenum++;
            pos=0;
        }
        else
            pos++;
    }
    for(;i<s.size();i++) {
        if ((linenum==endLine && pos==endPos) || (linenum>endLine)) {
            return s.substr(starti, i-starti+1);
        }
        if (s.at(i) == '\n') {
            linenum++;
            pos=0;
        }
        else
            pos++;
    }
    return s.substr(starti,s.size()-starti);
}


std::string roxal::replace(const std::string& str, const std::string& from, const std::string& to) 
{
    size_t start_pos = str.find(from);
    if(start_pos == std::string::npos)
        return str;
    auto ret { str };
    ret.replace(start_pos, from.length(), to);
    return ret;
}

