#pragma once
#include <cstddef>
#include <type_traits>

namespace compat {
template <class T>
class span {
    T* p_{nullptr};
    std::size_t n_{0};
public:
    using element_type = T;
    using value_type   = std::remove_cv_t<T>;
    using size_type    = std::size_t;
    span() = default;
    span(T* p, std::size_t n) : p_(p), n_(n) {}
    T* data()  const { return p_; }
    size_type size() const { return n_; }
    T* begin() const { return p_; }
    T* end()   const { return p_ + n_; }
    T& operator[](size_type i) const { return p_[i]; }
};
}
