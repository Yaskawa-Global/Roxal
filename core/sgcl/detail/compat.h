#pragma once

#if __cplusplus < 202002L
#include <atomic>
#include <type_traits>
#include <climits>
#include <condition_variable>
#include <mutex>
#include <unordered_map>
#include <memory>
#include <cstdint>

namespace sgcl::detail {
    // minimal std::strong_ordering replacement
    enum class strong_ordering { less = -1, equal = 0, greater = 1 };

    struct compare_three_way {
        template<class L, class R>
        constexpr strong_ordering operator()(const L& l, const R& r) const {
            if (l < r) return strong_ordering::less;
            if (r < l) return strong_ordering::greater;
            return strong_ordering::equal;
        }
    };

    template<class T, class U = T>
    using compare_three_way_result_t = strong_ordering;

    template<class It1, class It2, class Comp>
    constexpr strong_ordering lexicographical_compare_three_way(
        It1 f1, It1 l1, It2 f2, It2 l2, Comp comp) {
        for (; f1 != l1 && f2 != l2; ++f1, ++f2) {
            auto c = comp(*f1, *f2);
            if (c != strong_ordering::equal) return c;
        }
        if (f1 == l1 && f2 == l2) return strong_ordering::equal;
        return (f1 == l1) ? strong_ordering::less : strong_ordering::greater;
    }

    struct wait_state {
        std::mutex mtx;
        std::condition_variable cv;
        std::size_t waiters = 0;
    };

    struct wait_bucket {
        std::mutex mtx;
        std::unordered_map<void*, std::shared_ptr<wait_state>> map;
    };

    constexpr std::size_t wait_bucket_count = 256;

    inline wait_bucket& wait_bucket_for(void* key) {
        static wait_bucket buckets[wait_bucket_count];
        auto idx = (reinterpret_cast<std::uintptr_t>(key) >> 3) % wait_bucket_count;
        return buckets[idx];
    }

    // condition_variable based implementation for atomic wait/notify
    template<class Atomic, class T>
    void atomic_wait(Atomic& a, T old, std::memory_order order = std::memory_order_seq_cst) noexcept {
        if (a.load(order) != old) return;

        void* key = const_cast<void*>(static_cast<const void*>(&a));
        auto& bucket = wait_bucket_for(key);
        std::shared_ptr<wait_state> ws;
        {
            std::lock_guard<std::mutex> lg(bucket.mtx);
            auto& ptr = bucket.map[key];
            if (!ptr) ptr = std::make_shared<wait_state>();
            ws = ptr;
            ++ws->waiters;
        }

        std::unique_lock<std::mutex> lk(ws->mtx);
        while (a.load(order) == old) {
            ws->cv.wait(lk);
        }
        lk.unlock();

        {
            std::lock_guard<std::mutex> lg(bucket.mtx);
            if (--ws->waiters == 0) {
                bucket.map.erase(key);
            }
        }
    }

    template<class T>
    constexpr int countr_zero(T v) noexcept {
        static_assert(std::is_unsigned_v<T>, "countr_zero requires unsigned type");
        int c = 0;
        while (v && !(v & 1)) {
            v >>= 1;
            ++c;
        }
        return c;
    }

    template<class Atomic>
    void atomic_notify_one(Atomic& a) noexcept {
        void* key = const_cast<void*>(static_cast<const void*>(&a));
        auto& bucket = wait_bucket_for(key);
        std::shared_ptr<wait_state> ws;
        {
            std::lock_guard<std::mutex> lg(bucket.mtx);
            auto it = bucket.map.find(key);
            if (it == bucket.map.end()) return;
            ws = it->second;
        }
        ws->cv.notify_one();
    }

    template<class Atomic>
    void atomic_notify_all(Atomic& a) noexcept {
        void* key = const_cast<void*>(static_cast<const void*>(&a));
        auto& bucket = wait_bucket_for(key);
        std::shared_ptr<wait_state> ws;
        {
            std::lock_guard<std::mutex> lg(bucket.mtx);
            auto it = bucket.map.find(key);
            if (it == bucket.map.end()) return;
            ws = it->second;
        }
        ws->cv.notify_all();
    }
}

#else
#include <compare>
#include <atomic>
namespace sgcl::detail {
    using std::strong_ordering;
    using std::compare_three_way;
    using std::lexicographical_compare_three_way;
    template<class T, class U = T>
    using compare_three_way_result_t = std::compare_three_way_result_t<T, U>;
    using std::countr_zero;
    template<class Atomic, class T>
    inline void atomic_wait(Atomic& a, T old, std::memory_order order = std::memory_order_seq_cst) noexcept { a.wait(old, order); }
    template<class Atomic>
    inline void atomic_notify_one(Atomic& a) noexcept { a.notify_one(); }
    template<class Atomic>
    inline void atomic_notify_all(Atomic& a) noexcept { a.notify_all(); }
}
#endif
