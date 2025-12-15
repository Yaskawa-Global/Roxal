/****************************************************************************
**
** Copyright (C) 2018 Yaskawa Innovation Inc.
** http://yaskawainnovation.com/
**
** Authors: David Jung
**
****************************************************************************/
#ifdef VXWORKS_BUILD
#include <unordered_map>
#include <functional>
#include <mutex>
#endif
#include "atomic.h"

using namespace roxal;


// atomic_string

atomic_string::atomic_string()
{
}

atomic_string::atomic_string(const atomic_string& str)
    : s(str.load())
{
}


atomic_string::atomic_string(const std::string& str)
    : s(str)
{
}


atomic_string::~atomic_string()
{
    std::lock_guard<std::mutex> lock(m_lock);
}


std::string atomic_string::load() const
{
    std::lock_guard<std::mutex> lock(m_lock);
    return s;
}

void atomic_string::store(const std::string& str)
{
    std::lock_guard<std::mutex> lock(m_lock);
    s = str;
}

atomic_string& atomic_string::operator=(const std::string& str)
{
    std::lock_guard<std::mutex> lock(m_lock);
    s = str;
    return *this;
}

atomic_string& atomic_string::operator=(const atomic_string& str)
{
    if (&str != this)
        store(str.load());
    return *this;
}


atomic_string& atomic_string::operator+=(const atomic_string& rhs)
{
    std::lock_guard<std::mutex> lock(m_lock);
    s += rhs.load();
    return *this;
}

atomic_string& atomic_string::operator+=(const std::string& rhs)
{
    std::lock_guard<std::mutex> lock(m_lock);
    s += rhs;
    return *this;
}

atomic_string& atomic_string::operator+=(const char* rhs)
{
    std::lock_guard<std::mutex> lock(m_lock);
    s += std::string(rhs);
    return *this;
}


atomic_string atomic_string::operator+(const atomic_string& rhs) const
{
    atomic_string result { *this };
    result += rhs;
    return result;
}

atomic_string atomic_string::operator+(const std::string& rhs) const
{
    atomic_string result { *this };
    result += rhs;
    return result;
}

atomic_string atomic_string::operator+(const char* rhs) const
{
    atomic_string result { *this };
    result += rhs;
    return result;
}


atomic_string roxal::operator+(const char* lhs, const atomic_string& rhs)
{ return atomic_string(std::string(lhs)) + rhs; }

atomic_string roxal::operator+(const std::string& lhs, const atomic_string& rhs)
{ return atomic_string(lhs) + rhs; }




