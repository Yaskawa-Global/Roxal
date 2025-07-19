
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


bool roxal::startsWith(const std::string& str, const std::string& prefix) {
    return str.size() >= prefix.size() && str.compare(0, prefix.size(), prefix) == 0;
}

// Function for icu::UnicodeString
bool roxal::startsWith(const icu::UnicodeString& str, const icu::UnicodeString& prefix) {
    return str.length() >= prefix.length() && str.startsWith(prefix);
}



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


std::string roxal::join(const std::vector<std::string>& strings, const std::string& separator)
{
    return std::accumulate(strings.begin() + 1, strings.end(), strings[0],
                            [&separator](const std::string& a, const std::string& b) {
                                return a + separator + b;
                            });
};


icu::UnicodeString roxal::join(const std::vector<icu::UnicodeString>& strings, const std::string& separator)
{
    if (strings.empty()) return icu::UnicodeString(); // Return an empty UnicodeString if the vector is empty
    auto usep = toUnicodeString(separator);
    return std::accumulate(strings.begin() + 1, strings.end(), strings[0],
                            [&usep](const icu::UnicodeString& a, const icu::UnicodeString& b) {
                                return a + usep + b;
                            });
}

std::string roxal::trim(const std::string& s)
{
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

icu::UnicodeString roxal::trim(const icu::UnicodeString& s)
{
    std::string tmp; s.toUTF8String(tmp);
    return toUnicodeString(trim(tmp));
}
