#pragma once

#include <memory>
#include "core/common.h"


//
// Memory management

// There are two independent memory management schemes:
//  - one for internal C++ objects in the implementation (e.g. Thread, Signal)
//  - one for Roxal objects (subclasses of class Obj) managed by class Value
//
// For C++ code not using Roxal Obj types, use roxal::ptr<T> similar to how you'd use std::shared_ptr<T>
//  Specifically:
//   ptr<T>                  <- std::shared_ptr<T>
//   make_ptr<T>             <- std::make_shared<T>
//   unique_ptr<T>           <- std::unique_ptr<T>
//   dynamic_ptr_cast<T>(p)  <- std::dynamic_pointer_cast<T>(p)
//   enable_ptr_from_this<T> <- std::enable_shared_from_this<T>
//   ptr_from_this<T>        <- std::shared_from_this<T>
//
// One significant difference is that make_ptr<T> creates a unique_ptr<T>, which can be
//  immediately deleted upon unique_ptr destruction if not copied.  However, ptr<T> can be
//  constructed from a unique_ptr, essentially 'promoting' it to a shared instance.
//
// Instances of Roxal Obj types (e.g. ObjList, ...) are created with newObj<T> that also
//  yields a unique_ptr, with the intention that it is immediately handed over to Value(unique_ptr<Obj>)
//  which will take over the management (resetting the unique_ptr to nullptr).  Currently, Value
//  uses its own reference counting implementation for management.
//
namespace roxal {


template<class T, class D = std::default_delete<T>>
using unique_ptr = std::unique_ptr<T, D>;

template<class T> class weak_ptr;   // fwd

// thin wrapper around std::shared_ptr to provide
//  automatic construction from unique_ptr
template<class T>
class ptr {
  std::shared_ptr<T> sp_;
  template<class U> friend class weak_ptr;   // allow weak_ptr to read sp_
  template<class U> friend class ptr; // allow cross-type alias_from
  template<class A, class B> friend ptr<A> dynamic_ptr_cast(const ptr<B>&);
  template<class U>
  static ptr alias_from(const ptr<U>& owner, T* sub) {
        // share control block with owner; points at subobject 'sub'
        return ptr(std::shared_ptr<T>(owner.sp_, sub));
    }
public:
  using element_type = T;

  ptr() noexcept = default;
  ptr(std::nullptr_t) noexcept : sp_(nullptr) {}
  // allow seamless use where a shared_ptr<T> already exists:
  ptr(std::shared_ptr<T> sp) noexcept : sp_(std::move(sp)) {}

  // implicit ctor from unique_ptr<U,D>&&
  template<class U, class D,
           class = std::enable_if_t<std::is_convertible_v<U*,T*>>>
  ptr(std::unique_ptr<U,D>&& up) {
    U* raw = up.release();  // take over the object
    sp_.reset(static_cast<T*>(raw));
  }

  template<class U,
           class = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  ptr(const ptr<U>& other) noexcept
    : sp_(other.sp_) {}

  template<class U,
           class = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  ptr(ptr<U>&& other) noexcept
    : sp_(std::move(other.sp_)) {}

  static ptr from_raw(T* raw) {
    return ptr(std::shared_ptr<T>(raw));
  }

  template<class U, class D,
         class = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  ptr& operator=(std::unique_ptr<U, D>&& up) {
        U* raw = up.release();
        sp_.reset(static_cast<T*>(raw));
        return *this;
    }

  // minimal shared_ptr surface
  T*  get()  const noexcept { return sp_.get(); }
  T&  operator*()   const noexcept { return *sp_; }
  T*  operator->()  const noexcept { return sp_.get(); }
  explicit operator bool() const noexcept { return static_cast<bool>(sp_); }
  void reset() noexcept { sp_.reset(); }
  void reset(std::nullptr_t) noexcept { sp_.reset(); }
  bool operator==(std::nullptr_t) const noexcept { return sp_ == nullptr; }
  bool operator!=(std::nullptr_t) const noexcept { return sp_ != nullptr; }
  bool operator==(const ptr& rhs) const noexcept { return sp_ == rhs.sp_; }
  bool operator!=(const ptr& rhs) const noexcept { return sp_ != rhs.sp_; }

  friend bool operator<(const ptr& a, const ptr& b) noexcept {
        // use std::less on pointers (well-defined total order)
        return std::less<const void*>{}(a.get(), b.get());
    }

  //void swap(ptr& other) noexcept { sp_.swap(other.sp_); }
  //long use_count() const noexcept { return sp_.use_count(); }
  //std::shared_ptr<T> as_shared() const noexcept { return sp_; }

  // ensure weak_ptr::lock() can build a ptr from a shared_ptr
  template<class U, class = std::enable_if_t<std::is_convertible_v<U*,T*>>>
  ptr(std::shared_ptr<U> sp) noexcept : sp_(std::move(sp)) {}
};


template<class T>
class weak_ptr {
    std::weak_ptr<T> wp_;
public:
    weak_ptr() = default;
    weak_ptr(std::nullptr_t) noexcept : wp_{} {}

    // // from std::weak_ptr<U>
    // template<class U, class = std::enable_if_t<std::is_convertible_v<U*,T*>>>
    // weak_ptr(const std::weak_ptr<U>& other) : wp_(other) {}

    // from roxal::weak_ptr<U>
    template<class U, class = std::enable_if_t<std::is_convertible_v<U*,T*>>>
    weak_ptr(const weak_ptr<U>& other) : wp_(other.wp_) {}

    template<class U, class = std::enable_if_t<std::is_convertible_v<U*,T*>>>
    weak_ptr(const ptr<U>& p) : wp_(p.sp_) {}

    template<class U, class = std::enable_if_t<std::is_convertible_v<U*,T*>>>
    weak_ptr& operator=(const ptr<U>& p) { wp_ = p.sp_; return *this; }

    void reset() noexcept { wp_.reset(); }
    bool expired() const noexcept { return wp_.expired(); }
    ptr<T> lock() const noexcept { return ptr<T>(wp_.lock()); }

    bool operator==(std::nullptr_t) const noexcept { return expired(); }
    bool operator!=(std::nullptr_t) const noexcept { return !expired(); }
};

template<class T, class... Args>
inline unique_ptr<T> make_ptr(Args&&... args) {
    return std::make_unique<T>(std::forward<Args>(args)...);
}

// template<class T>
// ptr<T> ptr_from_this(T* t) { return t->shared_from_this(); }

template<class T, class U>
inline ptr<T> dynamic_ptr_cast(const ptr<U>& p) {
    if (auto* q = dynamic_cast<T*>(p.get()))
        return ptr<T>::template alias_from<U>(p, q);
    return {}; // empty
}


// inherit from this class instead of std::enable_shared_from_this<T> directly
template<class T>
struct enable_ptr_from_this
    : std::enable_shared_from_this<T>
{
    ptr<T> ptr_from_this() {
        return this->shared_from_this();           // std::shared_ptr path
    }
};


} // namespace roxal
