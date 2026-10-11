#pragma once

#include <algorithm>
#include <mutex>
#include <type_traits>
#include <utility>

namespace usbipdcpp {

namespace detail {
    template<typename T>
    struct DefaultLM {
        static T *create() {
            return new T{};
        }
        static void destroy(T *p) {
            delete p;
        }
    };

    template<typename T>
    struct DefaultReset {
        static void reset(T &) {
        }
    };
} // namespace detail

/**
 * @brief Fixed size object pool
 *
 * Features:
 * - Preallocate all objects to avoid runtime memory allocation
 * - Verifiable pointer ownership to prevent duplication free
 * - alloc O(1),free O(log n)
 * - Optional thread safety
 * - Supports custom LifeManager and Reset policies
 *
 * @warning **Reset is called when alloc()**: After the object is returned, its status will not be cleared immediately until the next alloc()
 * invokes Reset::reset(). If an object holds external resources (handles, locks, reference counts, etc.),
 * The caller must release these resources by itself before free(), otherwise the resource life cycle will be delayed until the next alloc.
 * Reset policy is only responsible for resetting object value fields and should not be used for resource release.
 *
 * @tparam T object type
 * @tparam PoolSize pool size
 * @tparam ThreadSafe Whether thread is safe (default false)
 * @tparam LifeManager life cycle manager, providing create() / destroy(T*)
 * @tparam Reset Reset policy, calls reset(T&, args...) on allocation; defaults to a no-op
 */
template<typename T, size_t PoolSize, bool ThreadSafe = false, typename LifeManager = detail::DefaultLM<T>,
         typename Reset = detail::DefaultReset<T>>
class ObjectPool {
    static_assert(PoolSize > 0, "PoolSize must be greater than 0");

public:
    ObjectPool() {
        // Create object
        for (size_t i = 0; i < PoolSize; ++i) {
            pool_[i] = {LifeManager::create(), false};
        }
        // Sort by pointer, used for binary search
        std::sort(pool_, pool_ + PoolSize, [](const auto &a, const auto &b) { return a.first < b.first; });
        // Initialize the free index stack
        for (size_t i = 0; i < PoolSize; ++i) {
            free_stack_[i] = i;
        }
        free_top_ = PoolSize;
    }

    ~ObjectPool() {
        clear();
    }

    // Copying and moving are prohibited
    ObjectPool(const ObjectPool &) = delete;
    ObjectPool &operator=(const ObjectPool &) = delete;
    ObjectPool(ObjectPool &&) = delete;
    ObjectPool &operator=(ObjectPool &&) = delete;

    /**
     * @brief allocation object
     * @param args Parameters passed to Reset::reset(T&, args...)
     * @return object pointer, nullptr is returned when the pool is empty, the caller should fall back to LifeManager::create()
     *
     * Typical usage:
     * @code
     * auto* p = pool.alloc();
     * if (!p) p = LifeManager::create();
     * @endcode
     */
    template<typename... Args>
    T *alloc(Args &&...args) {
        if constexpr (ThreadSafe) {
            std::lock_guard<std::mutex> lock(mutex_);
            return alloc_impl(std::forward<Args>(args)...);
        }
        else {
            return alloc_impl(std::forward<Args>(args)...);
        }
    }

    /**
     * @brief Return object
     * @param obj object pointer
     * @return true if returned successfully; false if the pointer does not belong to this pool (such as create used as an alloc fallback); the caller should fall back to LifeManager::destroy()
     *
     * Typical usage:
     * @code
     * if (!pool.free(p)) LifeManager::destroy(p);
     * @endcode
     */
    bool free(T *obj) {
        if (!obj) {
            return false;
        }

        if constexpr (ThreadSafe) {
            std::lock_guard<std::mutex> lock(mutex_);
            return free_impl(obj);
        }
        else {
            return free_impl(obj);
        }
    }

    /**
     * @brief Get the number available in the pool
     */
    size_t available() const {
        if constexpr (ThreadSafe) {
            std::lock_guard<std::mutex> lock(mutex_);
            return free_top_;
        }
        else {
            return free_top_;
        }
    }

    /**
     * @brief Get the total capacity of the pool
     */
    constexpr size_t capacity() const {
        return PoolSize;
    }

    /**
     * @brief Clear the pool (destroy all objects)
     */
    void clear() {
        if constexpr (ThreadSafe) {
            std::lock_guard<std::mutex> lock(mutex_);
            clear_impl();
        }
        else {
            clear_impl();
        }
    }

    /**
     * @brief Reset the pool (return all objects to the pool without deleting)
     */
    void reset() {
        if constexpr (ThreadSafe) {
            std::lock_guard<std::mutex> lock(mutex_);
            reset_impl();
        }
        else {
            reset_impl();
        }
    }

private:
    std::pair<T *, bool> pool_[PoolSize] = {};
    size_t free_stack_[PoolSize] = {};
    size_t free_top_ = 0;
    mutable std::conditional_t<ThreadSafe, std::mutex, char> mutex_{};

    template<typename... Args>
    T *alloc_impl(Args &&...args) {
        if (free_top_ == 0) {
            return nullptr;
        }
        size_t index = free_stack_[--free_top_];
        pool_[index].second = true;
        T *obj = pool_[index].first;
        Reset::reset(*obj, std::forward<Args>(args)...);
        return obj;
    }

    bool free_impl(T *obj) {
        // binary search pointer
        auto it = std::lower_bound(pool_, pool_ + PoolSize, obj,
                                   [](const auto &elem, T *val) { return elem.first < val; });
        if (it == pool_ + PoolSize || it->first != obj) {
            return false; // is not an object of this pool
        }
        if (!it->second) {
            return false; // Duplicate free
        }
        it->second = false;
        // Calculate the index and push it back to the stack
        size_t index = it - pool_;
        free_stack_[free_top_++] = index;
        return true;
    }

    void clear_impl() {
        for (size_t i = 0; i < PoolSize; ++i) {
            if (pool_[i].first) {
                LifeManager::destroy(pool_[i].first);
                pool_[i] = {nullptr, false};
            }
        }
        free_top_ = 0;
    }

    void reset_impl() {
        // Return all objects to the pool without deleting them
        for (size_t i = 0; i < PoolSize; ++i) {
            pool_[i].second = false;
        }
        // Rebuild the free index stack
        for (size_t i = 0; i < PoolSize; ++i) {
            free_stack_[i] = i;
        }
        free_top_ = PoolSize;
    }
};

} // namespace usbipdcpp
