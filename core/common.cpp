
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
#include <random>


uint16_t roxal::randomUint16(uint16_t min, uint16_t max)
{
    // Create a random device to seed the generator
    static std::random_device rd;

    // Initialize a Mersenne Twister random number generator
    static std::mt19937 gen(rd());

    // Define the distribution range for uint16_t
    static std::uniform_int_distribution<uint16_t> dist(min, max);

    // Generate and return the random number
    return dist(gen);
}



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

std::string roxal::deleteStringLinesAtInterval(const std::string& s, size_t startLine, size_t startPos, size_t endLine, size_t endPos)
{
    // TODO: handle non-\n line endings
    long linenum=1;
    long startidx = -1;
    long endidx = -1;

    //copy
    std::string cs = s;

    //find the new lines
    int index = 0;
    std::vector<int> newLines;
    newLines.push_back(0);//start of first line is at newLines(0)
    while((index = cs.find('\n', index)) != std::string::npos)
    {
        index++;
        newLines.push_back(index);
        std::string g = cs.substr(index);
    }

    //delete the lines
    if(newLines.size() >= endLine)
        cs.erase(newLines.at(startLine-1), newLines.at(endLine) - newLines.at(startLine-1));
    else
        //handle last line, no linefeed
        cs.erase(newLines.at(startLine-1), (int)endPos+1 - (int)startPos);

    return cs;
}

std::string roxal::insertStringLinesAtInterval(const std::string& s, const std::string& insertS, size_t startLine, size_t startPos)
{
     // TODO: handle non-\n line endings
    long linenum=1;

    //copy
    std::string cs = s;

    //find the new lines
    int index = 0;
    std::vector<int> newLines;
    newLines.push_back(0);//start of first line is at newLines(0)
    while((index = cs.find('\n', index)) != std::string::npos)
    {
        index++;
        newLines.push_back(index);
        //std::string g = cs.substr(index);
    }

    //insert
    if(startLine < newLines.size())
        cs.insert(newLines.at(startLine - 1), spaces(startPos) + insertS + "\n");
    //append
    else if(startLine == newLines.size())
        cs += (spaces(startPos) + insertS + "\n");

    return cs;
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
