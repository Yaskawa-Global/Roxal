/****************************************************************************
**
** Copyright (C) 2017 Yaskawa Innovation Inc.
** http://yaskawainnovation.com/
**
** Authors: David Jung
**
** Wrappers for key data types to add atomic access
**
** $Id$
****************************************************************************/

#pragma once

#include <functional>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <mutex>
#include <stack>
#include <queue>
#include <optional>
//Jay
#include <unordered_map>
#include <functional>
#include <mutex>
//Jay

namespace roxal {

class atomic_string
{
public:
    atomic_string();
    atomic_string(const atomic_string& str);
    atomic_string(const std::string& str);
    virtual ~atomic_string();

    std::string load() const;
    void store(const std::string& str);

    bool empty() const { return load().empty(); }

    atomic_string& operator=(const std::string& str);
    atomic_string& operator=(const atomic_string& str);

    bool operator==(const atomic_string& rhs) const
    { return load() == rhs.load(); }

    bool operator!=(const atomic_string& rhs) const
    { return load() != rhs.load(); }

    bool operator==(const std::string& rhs) const
    { return load() == rhs; }

    bool operator!=(const std::string& rhs) const
    { return load() != rhs; }

    atomic_string& operator+=(const atomic_string& rhs);
    atomic_string& operator+=(const std::string& rhs);
    atomic_string& operator+=(const char* rhs);

    atomic_string operator+(const atomic_string& rhs) const;
    atomic_string operator+(const std::string& rhs) const;
    atomic_string operator+(const char* rhs) const;

    operator std::string() const { return load(); }

private:
    mutable std::mutex m_lock;

    std::string s;
};


atomic_string operator+(const char* lhs, const atomic_string& rhs);
atomic_string operator+(const std::string& lhs, const atomic_string& rhs);



template<typename T>
class atomic_set
{
public:
    atomic_set() {}
    ~atomic_set()
    {
        std::lock_guard<std::mutex> lock(m_lock);
    }

    void insert(const T& e)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        s.insert(e);
    }

    void erase(const T& e)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        s.erase(e);
    }

    bool contains(const T& e) const
    {
        std::lock_guard<std::mutex> lock(m_lock);
        return s.count(e) > 0;
    }

    std::set<T> get() const
    {
        std::lock_guard<std::mutex> lock(m_lock);
        return s;
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(m_lock);
        s.clear();
    }

private:
    mutable std::mutex m_lock;

    std::set<T> s;

};


template<typename T>
class atomic_vector
{
public:
    typedef T value_type;
    typedef size_t size_type;

    atomic_vector() {}
    ~atomic_vector()
    {
        std::lock_guard<std::mutex> lock(m_lock);
    }

    const T at(size_t i) const
    {
        std::lock_guard<std::mutex> lock(m_lock);
        return v.at(i);
    }

    void store(size_t i, const T& value)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        v.at(i) = value;
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(m_lock);
        v.clear();
    }

    size_t size() const
    {
        std::lock_guard<std::mutex> lock(m_lock);
        return v.size();
    }

    bool empty() const
    {
        return size()==0;
    }

    void resize(size_t s)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        v.resize(s);
    }

    void reserve(size_t s)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        v.reserve(s);
    }

    void push_back(const T& value)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        v.push_back(value);
    }

    // Append multiple elements efficiently in one lock
    void append(const std::vector<T>& values)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        v.reserve(v.size() + values.size());
        v.insert(v.end(), values.begin(), values.end());
    }

    // Append elements from another atomic_vector efficiently
    void append(const atomic_vector<T>& other)
    {
        std::vector<T> otherValues = other.get();
        std::lock_guard<std::mutex> lock(m_lock);
        v.reserve(v.size() + otherValues.size());
        v.insert(v.end(), otherValues.begin(), otherValues.end());
    }

    T back() const 
    {
        std::lock_guard<std::mutex> lock(m_lock);
        return v.back();
    }

    T pop_back()
    {
        std::lock_guard<std::mutex> lock(m_lock);
        T back { v.back() };
        v.pop_back();
        return back;
    }

    // if !empty() pop_back(), release lock, apply f to value and return true, otherwise return false
    bool pop_back_and_apply(std::function<void(const typename std::vector<T>::value_type&)> f) 
    { 
        T back {};
        {
            std::lock_guard<std::mutex> lock(m_lock);
            if (!v.empty()) {
                back = v.back();
                v.pop_back();
                // fall through
            }
            else
                return false;
        }
        f(back);
        return true;
    }


    void erase(const T& value)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        for(auto it = v.begin(); it < v.end(); ++it) {
            if (*it == value) {
                v.erase(it);
                return;
            }
        }
    }

    std::vector<T> get() const
    {
        std::lock_guard<std::mutex> lock(m_lock);
        return v;
    }

    bool contains(const T& value) const
    {
        std::lock_guard<std::mutex> lock(m_lock);
        for(auto it = v.cbegin(); it < v.cend(); ++it)
            if (*it == value)
                return true;
        return false;
    }

    size_t indexOf(const T& value) const
    {
        std::lock_guard<std::mutex> lock(m_lock);
        for(auto it = v.cbegin(); it < v.cend(); ++it)
            if (*it == value)
                return size_t(std::distance(v.cbegin(), it));
        return size()+1;
    }

    atomic_vector& operator=(const atomic_vector& rhs)
    {
        if (&rhs != this) {
            std::lock_guard<std::mutex> lock(m_lock);
            v = rhs.get();
        }
        return *this;
    }

    atomic_vector& operator=(const std::vector<T>& rhs)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        v = rhs;
        return *this;
    }

private:
    mutable std::mutex m_lock;

    std::vector<T> v;
};


template<typename Key, typename T>
class atomic_map
{
public:

    typedef Key key_type;
    typedef T mapped_type;
    typedef std::pair<const Key, T>	value_type;
    typedef typename std::map<Key, T>::key_compare key_compare;
    typedef typename std::map<Key, T>::allocator_type allocator_type;

    atomic_map() {}
    ~atomic_map()
    {
        std::lock_guard<std::mutex> lock(m_lock);
    }

    const T load(const Key& key) const
    {
        std::lock_guard<std::mutex> lock(m_lock);
        return m.at(key);
    }

    void store(const Key& key, const T& value)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        m[key] = value;
    }

    const T at(const Key& key) const { return load(key); }

    bool containsKey(const Key& key) const
    {
        std::lock_guard<std::mutex> lock(m_lock);
        return m.find(key) != m.cend();
    }

    // returns value if key present, otherwise no value
    std::optional<T> lookup(const Key& key) const 
    {
        std::lock_guard<std::mutex> lock(m_lock);
        auto it = m.find(key);
        if (it != m.cend())
            return std::optional<T>(it->second);
        return std::optional<T>();
    }

    std::map<Key, T> get() const
    {
        std::lock_guard<std::mutex> lock(m_lock);
        return m;
    }


    // apply f to each map entry *while map is locked*
    //  (exceptions thrown by f are ignored)
    void apply(std::function<void(const typename std::map<Key, T>::value_type&)> f)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        for(const auto& entry : m) {
            try {
                f(entry);
            }
            catch (...) {}
        }
    }

    size_t erase(const Key& key)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        return m.erase(key);
    }

    // if key present, erase it, unlock and apply f to value
    size_t erase_and_apply(const Key& key, std::function<void(const typename std::map<Key, T>::mapped_type&)> f)
    {
        T value {};
        {
            std::lock_guard<std::mutex> lock(m_lock);
            auto it = m.find(key);
            if (it != m.cend()) {
                value = it->second;
                m.erase(it);
                // fall through (unlock)
            }
            else
                return 0;
        }
        f(value);
        return 1;
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(m_lock);
        m.clear();
    }

    size_t size() const
    {
        std::lock_guard<std::mutex> lock(m_lock);
        return m.size();
    }

    atomic_map& operator=(const atomic_map& rhs)
    {
        if (&rhs != this) {
            std::lock_guard<std::mutex> lock(m_lock);
            m = rhs.get();
        }
        return *this;
    }

    atomic_map& operator=(const std::map<Key,T>& rhs)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        m = rhs;
        return *this;
    }

    std::vector<Key> keys() const {
        std::lock_guard<std::mutex> lock(m_lock);
        std::vector<Key> ks;
        ks.reserve(m.size());
        for(const auto& kv : m)
            ks.push_back(kv.first);
        return ks;
    }

private:
    mutable std::mutex m_lock;

    std::map<Key, T> m;
};





template<typename Key, typename T>
class atomic_unordered_map
{
public:

    typedef Key key_type;
    typedef T mapped_type;
    typedef std::pair<const Key, T>	value_type;
    typedef typename std::unordered_map<Key, T>::allocator_type allocator_type;

    atomic_unordered_map() {}
    ~atomic_unordered_map()
    {
        std::lock_guard<std::mutex> lock(m_lock);
    }

    const T load(const Key& key) const
    {
        std::lock_guard<std::mutex> lock(m_lock);
        return m.at(key);
    }

    void store(const Key& key, const T& value)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        m[key] = value;
    }

    const T at(const Key& key) const { return load(key); }

    bool containsKey(const Key& key) const
    {
        std::lock_guard<std::mutex> lock(m_lock);
        return m.find(key) != m.cend();
    }

    // returns value if key present, otherwise no value
    std::optional<T> lookup(const Key& key) const 
    {
        std::lock_guard<std::mutex> lock(m_lock);
        auto it = m.find(key);
        if (it != m.cend())
            return std::optional<T>(it->second);
        return std::optional<T>();
    }

    std::unordered_map<Key, T> get() const
    {
        std::lock_guard<std::mutex> lock(m_lock);
        return m;
    }


    // apply f to each map entry *while map is locked*
    //  (exceptions thrown by f are ignored)
    void apply(std::function<void(const typename std::unordered_map<Key, T>::value_type&)> f)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        for(const auto& entry : m) {
            try {
                f(entry);
            }
            catch (...) {}
        }
    }

    size_t erase(const Key& key)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        return m.erase(key);
    }

    // if key present, erase it, unlock and apply f to value
    size_t erase_and_apply(const Key& key, std::function<void(const typename std::unordered_map<Key, T>::mapped_type&)> f)
    {
        T value {};
        {
            std::lock_guard<std::mutex> lock(m_lock);
            auto it = m.find(key);
            if (it != m.cend()) {
                value = it->second;
                m.erase(it);
                // fall through (unlock)
            }
            else
                return 0;
        }
        f(value);
        return 1;
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(m_lock);
        m.clear();
    }

    size_t size() const
    {
        std::lock_guard<std::mutex> lock(m_lock);
        return m.size();
    }

    atomic_unordered_map& operator=(const atomic_unordered_map& rhs)
    {
        if (&rhs != this) {
            std::lock_guard<std::mutex> lock(m_lock);
            m = rhs.get();
        }
        return *this;
    }

    atomic_unordered_map& operator=(const std::unordered_map<Key,T>& rhs)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        m = rhs;
        return *this;
    }

    std::vector<Key> keys() const {
        std::lock_guard<std::mutex> lock(m_lock);
        std::vector<Key> ks;
        ks.reserve(m.size());
        for(const auto& kv : m)
            ks.push_back(kv.first);
        return ks;
    }

private:
    mutable std::mutex m_lock;

    std::unordered_map<Key, T> m;
};






template<typename T>
class atomic_stack
{
public:

    typedef T value_type;

    atomic_stack() {}
    ~atomic_stack()
    {
        std::lock_guard<std::mutex> lock(m_lock);
    }

    bool contains(const T& value) const
    {
        std::lock_guard<std::mutex> lock(m_lock);
        return s.find(value) != s.cend();
    }

    std::stack<T> get() const
    {
        std::lock_guard<std::mutex> lock(m_lock);
        return s;
    }

    void push(const T& value)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        s.push(value);
    }

    void pop()
    {
        std::lock_guard<std::mutex> lock(m_lock);
        s.pop();
    }

    T top() const
    {
        std::lock_guard<std::mutex> lock(m_lock);
        return s.top();
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(m_lock);
        s.clear();
    }

    size_t size() const
    {
        std::lock_guard<std::mutex> lock(m_lock);
        return s.size();
    }

    atomic_stack& operator=(const atomic_stack& rhs)
    {
        if (&rhs != this) {
            std::lock_guard<std::mutex> lock(m_lock);
            s = rhs.get();
        }
        return *this;
    }

    atomic_stack& operator=(const std::stack<T>& rhs)
    {
        //Jay std::lock_guard<std::timed_mutex> lock(m_lock);
        std::lock_guard<std::mutex> lock(m_lock);
        s = rhs;
        return *this;
    }


private:
    mutable std::mutex m_lock;

    std::stack<T> s;
};










template<typename T>
class atomic_queue
{
public:

    typedef T value_type;

    atomic_queue() {}
    ~atomic_queue()
    {
        std::lock_guard<std::mutex> lock(m_lock);
    }

    std::stack<T> get() const
    {
        std::lock_guard<std::mutex> lock(m_lock);
        return q;
    }

    void push(const T& value)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        q.push(value);
    }

    T pop()
    {
        std::lock_guard<std::mutex> lock(m_lock);
        T f { q.front() };
        q.pop();
        return f;
    }

    T front() const
    {
        std::lock_guard<std::mutex> lock(m_lock);
        return q.front();
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(m_lock);
        q.clear();
    }

    size_t size() const
    {
        std::lock_guard<std::mutex> lock(m_lock);
        return q.size();
    }

    bool empty() const
    {
        std::lock_guard<std::mutex> lock(m_lock);
        return q.empty();
    }

private:
    mutable std::mutex m_lock;

    std::queue<T> q;
};


template<typename T, typename Compare>
class atomic_priority_queue
{
public:

    typedef T value_type;

    atomic_priority_queue() {}
    ~atomic_priority_queue()
    {
        std::lock_guard<std::mutex> lock(m_lock);
    }

    void push(const T& value)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        q.push(value);
    }

    T pop()
    {
        std::lock_guard<std::mutex> lock(m_lock);
        T v { q.top() };
        q.pop();
        return v;
    }

    template<typename Pred>
    bool pop_if(Pred pred, T& value)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        if (!q.empty() && pred(q.top())) {
            value = q.top();
            q.pop();
            return true;
        }
        return false;
    }

    T top() const
    {
        std::lock_guard<std::mutex> lock(m_lock);
        return q.top();
    }

    bool empty() const
    {
        std::lock_guard<std::mutex> lock(m_lock);
        return q.empty();
    }

    size_t size() const
    {
        std::lock_guard<std::mutex> lock(m_lock);
        return q.size();
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(m_lock);
        while(!q.empty()) q.pop();
    }

private:
    mutable std::mutex m_lock;

    std::priority_queue<T,std::vector<T>,Compare> q;
};


} // ns
