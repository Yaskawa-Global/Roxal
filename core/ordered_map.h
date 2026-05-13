/****************************************************************************
**
** Copyright (C) 2026
**
** Authors: David Jung, OpenAI Codex
**
** Map that preserves insertion order for key iteration
**
****************************************************************************/

#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>


namespace roxal {

template<typename Key, typename T, typename Hash = std::hash<Key>, typename KeyEqual = std::equal_to<Key>>
class ordered_map
{
public:

    typedef Key key_type;
    typedef T mapped_type;
    typedef std::pair<const Key, T> value_type;
    typedef std::unordered_map<Key, T, Hash, KeyEqual> map_type;
    typedef typename map_type::allocator_type allocator_type;
    typedef typename map_type::hasher hasher;
    typedef typename map_type::key_equal key_equal;

    template<bool IsConst>
    class ordered_iterator
    {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = typename ordered_map::value_type;
        using difference_type = std::ptrdiff_t;
        using reference = std::conditional_t<IsConst, const value_type&, value_type&>;
        using pointer = std::conditional_t<IsConst, const value_type*, value_type*>;

        ordered_iterator() = default;

        template<bool B = IsConst, typename = std::enable_if_t<B>>
        ordered_iterator(const ordered_iterator<false>& rhs)
            : m(rhs.m), order(rhs.order), index(rhs.index) {}

        reference operator*() const
        {
            return *current();
        }

        pointer operator->() const
        {
            return &(*current());
        }

        ordered_iterator& operator++()
        {
            ++index;
            return *this;
        }

        ordered_iterator operator++(int)
        {
            ordered_iterator tmp { *this };
            ++(*this);
            return tmp;
        }

        bool operator==(const ordered_iterator& rhs) const
        {
            return m == rhs.m && order == rhs.order && index == rhs.index;
        }

        bool operator!=(const ordered_iterator& rhs) const
        {
            return !(*this == rhs);
        }

    private:
        using map_ptr = std::conditional_t<IsConst, const map_type*, map_type*>;
        using order_ptr = std::conditional_t<IsConst, const std::vector<Key>*, std::vector<Key>*>;

        ordered_iterator(map_ptr m_, order_ptr order_, size_t index_)
            : m(m_), order(order_), index(index_) {}

        auto current() const
        {
            auto it = m->find((*order)[index]);
            if (it == m->end())
                throw std::out_of_range("ordered_map iterator key not found");
            return it;
        }

        map_ptr m { nullptr };
        order_ptr order { nullptr };
        size_t index { 0 };

        friend class ordered_iterator<true>;
        friend class ordered_map;
    };

    using iterator = ordered_iterator<false>;
    using const_iterator = ordered_iterator<true>;

    ordered_map() {}
    ~ordered_map() {}

    T& operator[](const Key& key)
    {
        auto result = m.try_emplace(key);
        if (result.second)
            order.push_back(result.first->first);
        return result.first->second;
    }

    T& operator[](Key&& key)
    {
        auto result = m.try_emplace(std::move(key));
        if (result.second)
            order.push_back(result.first->first);
        return result.first->second;
    }

    T& at(const Key& key)
    {
        return m.at(key);
    }

    const T& at(const Key& key) const
    {
        return m.at(key);
    }

    std::pair<iterator, bool> insert(const value_type& value)
    {
        auto result = m.insert(value);
        if (result.second) {
            order.push_back(result.first->first);
            return std::pair<iterator, bool>(iterator(&m, &order, order.size() - 1), true);
        }
        return std::pair<iterator, bool>(iteratorAtKey(result.first->first), false);
    }

    std::pair<iterator, bool> insert(value_type&& value)
    {
        auto result = m.insert(std::move(value));
        if (result.second) {
            order.push_back(result.first->first);
            return std::pair<iterator, bool>(iterator(&m, &order, order.size() - 1), true);
        }
        return std::pair<iterator, bool>(iteratorAtKey(result.first->first), false);
    }

    template<typename... Args>
    std::pair<iterator, bool> emplace(Args&&... args)
    {
        auto result = m.emplace(std::forward<Args>(args)...);
        if (result.second) {
            order.push_back(result.first->first);
            return std::pair<iterator, bool>(iterator(&m, &order, order.size() - 1), true);
        }
        return std::pair<iterator, bool>(iteratorAtKey(result.first->first), false);
    }

    bool containsKey(const Key& key) const
    {
        return m.find(key) != m.cend();
    }

    // returns value if key present, otherwise no value
    std::optional<T> lookup(const Key& key) const
    {
        auto it = m.find(key);
        if (it != m.cend())
            return std::optional<T>(it->second);
        return std::optional<T>();
    }

    map_type get() const
    {
        return m;
    }

    // apply f to each map entry in insertion order
    //  (exceptions thrown by f are ignored)
    void apply(std::function<void(const typename map_type::value_type&)> f)
    {
        for (const auto& key : order) {
            auto it = m.find(key);
            if (it == m.cend())
                continue;
            try {
                f(*it);
            }
            catch (...) {}
        }
    }

    size_t erase(const Key& key)
    {
        const size_t erased = m.erase(key);
        if (erased == 0)
            return 0;
        auto oit = std::find(order.begin(), order.end(), key);
        if (oit != order.end())
            order.erase(oit);
        return erased;
    }

    // if key present, erase it and apply f to value
    size_t erase_and_apply(const Key& key, std::function<void(const typename map_type::mapped_type&)> f)
    {
        T value {};
        auto it = m.find(key);
        if (it != m.cend()) {
            value = it->second;
            m.erase(it);
            auto oit = std::find(order.begin(), order.end(), key);
            if (oit != order.end())
                order.erase(oit);
        }
        else
            return 0;

        f(value);
        return 1;
    }

    void clear()
    {
        m.clear();
        order.clear();
    }

    size_t size() const
    {
        return m.size();
    }

    bool empty() const
    {
        return m.empty();
    }

    iterator begin()
    {
        return iterator(&m, &order, 0);
    }

    iterator end()
    {
        return iterator(&m, &order, order.size());
    }

    const_iterator begin() const
    {
        return cbegin();
    }

    const_iterator end() const
    {
        return cend();
    }

    const_iterator cbegin() const
    {
        return const_iterator(&m, &order, 0);
    }

    const_iterator cend() const
    {
        return const_iterator(&m, &order, order.size());
    }

    iterator find(const Key& key)
    {
        if (m.find(key) == m.end())
            return end();
        const size_t index = orderIndexOf(key);
        if (index >= order.size())
            return end();
        return iterator(&m, &order, index);
    }

    const_iterator find(const Key& key) const
    {
        if (m.find(key) == m.end())
            return cend();
        const size_t index = orderIndexOf(key);
        if (index >= order.size())
            return cend();
        return const_iterator(&m, &order, index);
    }

    ordered_map& operator=(const ordered_map& rhs)
    {
        if (&rhs != this) {
            m = rhs.m;
            order = rhs.order;
        }
        return *this;
    }

    ordered_map& operator=(const map_type& rhs)
    {
        m = rhs;
        order.clear();
        order.reserve(m.size());
        for (const auto& kv : m)
            order.push_back(kv.first);
        return *this;
    }

    std::vector<Key> keys() const
    {
        return order;
    }

    std::vector<std::pair<Key, T>> entries() const
    {
        std::vector<std::pair<Key, T>> es;
        es.reserve(order.size());
        for (const auto& key : order) {
            auto it = m.find(key);
            if (it != m.cend())
                es.emplace_back(it->first, it->second);
        }
        return es;
    }

private:
    iterator iteratorAtKey(const Key& key)
    {
        const size_t index = orderIndexOf(key);
        if (index >= order.size())
            return end();
        return iterator(&m, &order, index);
    }

    size_t orderIndexOf(const Key& key) const
    {
        auto oit = std::find(order.cbegin(), order.cend(), key);
        if (oit == order.cend())
            return order.size();
        return static_cast<size_t>(std::distance(order.cbegin(), oit));
    }

    map_type m;
    std::vector<Key> order;
};


} // namespace roxal
