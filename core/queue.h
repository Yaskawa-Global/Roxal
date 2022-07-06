#pragma once

#include <string>
#include <memory>
#include <vector>
#include <queue>
#include <iostream>


namespace roxal {



template<typename T, typename SortCompare, typename RemoveEqual>
class priority_queue : public std::priority_queue<T, std::vector<T>, SortCompare>
{
public:

    void remove(const T& value) {
        RemoveEqual removeEqual {};
        for(auto it = this->c.begin(); it != this->c.end();) {
            if (removeEqual(*it,value))
                this->c.erase(it);
            else
                ++it;
        }
    }

    priority_queue::container_type::const_iterator cbegin() const {
        return this->c.cbegin();
    }

    priority_queue::container_type::const_iterator cend() const {
        return this->c.cend();
    }


};


}
