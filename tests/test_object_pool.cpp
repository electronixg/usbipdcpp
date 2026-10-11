#include <atomic>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

#include "usbipdcpp/utils/ObjectPool.h"

using namespace usbipdcpp;

// ============== ObjectPool Tests (not thread-safe) ==============

struct TestObject {
    int value = 0;
    std::string name;

    TestObject() = default;
    TestObject(int v, std::string n) : value(v), name(std::move(n)) {
    }
};

TEST(ObjectPool, BasicAllocFree) {
    ObjectPool<TestObject, 4> pool;

    // Initial state
    EXPECT_EQ(pool.available(), 4u);
    EXPECT_EQ(pool.capacity(), 4u);

    // Allocate
    auto *obj1 = pool.alloc();
    ASSERT_NE(obj1, nullptr);
    EXPECT_EQ(pool.available(), 3u);

    auto *obj2 = pool.alloc();
    ASSERT_NE(obj2, nullptr);
    EXPECT_EQ(pool.available(), 2u);

    // Return
    EXPECT_TRUE(pool.free(obj1));
    EXPECT_EQ(pool.available(), 3u);

    EXPECT_TRUE(pool.free(obj2));
    EXPECT_EQ(pool.available(), 4u);
}

TEST(ObjectPool, PoolExhausted) {
    ObjectPool<int, 2> pool;

    auto *obj1 = pool.alloc();
    auto *obj2 = pool.alloc();
    auto *obj3 = pool.alloc(); // Pool empty

    EXPECT_NE(obj1, nullptr);
    EXPECT_NE(obj2, nullptr);
    EXPECT_EQ(obj3, nullptr);
    EXPECT_EQ(pool.available(), 0u);
}

TEST(ObjectPool, InvalidFree) {
    ObjectPool<int, 4> pool;

    int external_value = 0;
    EXPECT_FALSE(pool.free(&external_value)); // Object not in pool

    EXPECT_FALSE(pool.free(nullptr)); // Null pointer
}

TEST(ObjectPool, DoubleFree) {
    ObjectPool<int, 4> pool;

    auto *obj = pool.alloc();
    ASSERT_NE(obj, nullptr);

    EXPECT_TRUE(pool.free(obj));
    EXPECT_FALSE(pool.free(obj)); // Repeated free
}

TEST(ObjectPool, ObjectReuse) {
    ObjectPool<TestObject, 2> pool;

    // Allocate and set value
    auto *obj1 = pool.alloc();
    obj1->value = 42;
    obj1->name = "test";

    // Return
    pool.free(obj1);

    // Allocate again; should get same object (value may not have been reset)
    auto *obj2 = pool.alloc();
    EXPECT_EQ(obj1, obj2); // Same address
}

TEST(ObjectPool, Clear) {
    ObjectPool<int, 4> pool;

    pool.alloc();
    pool.alloc();
    EXPECT_EQ(pool.available(), 2u);

    pool.clear();
    EXPECT_EQ(pool.available(), 0u);

    // Cannot allocate after clear (objects deleted)
    EXPECT_EQ(pool.alloc(), nullptr);
}

TEST(ObjectPool, Reset) {
    ObjectPool<int, 4> pool;

    // Allocate some objects
    auto *obj1 = pool.alloc();
    auto *obj2 = pool.alloc();
    EXPECT_EQ(pool.available(), 2u);

    // reset returns all objects to pool
    pool.reset();
    EXPECT_EQ(pool.available(), 4u);

    // Can allocate again after reset
    auto *obj3 = pool.alloc();
    auto *obj4 = pool.alloc();
    auto *obj5 = pool.alloc();
    auto *obj6 = pool.alloc();
    ASSERT_NE(obj3, nullptr);
    ASSERT_NE(obj4, nullptr);
    ASSERT_NE(obj5, nullptr);
    ASSERT_NE(obj6, nullptr);
    EXPECT_EQ(pool.available(), 0u);

    // Cannot allocate when pool empty
    EXPECT_EQ(pool.alloc(), nullptr);

    // Return all
    pool.free(obj3);
    pool.free(obj4);
    pool.free(obj5);
    pool.free(obj6);
    EXPECT_EQ(pool.available(), 4u);
}

TEST(ObjectPool, ResetPreservesObjects) {
    ObjectPool<TestObject, 4> pool;

    // Allocate and set value
    auto *obj1 = pool.alloc();
    obj1->value = 100;
    obj1->name = "original";

    // Objects should remain after reset (addresses unchanged)
    pool.reset();
    EXPECT_EQ(pool.available(), 4u);

    // Allocating again should yield original object (value preserved)
    auto *obj2 = pool.alloc();
    EXPECT_EQ(obj1, obj2); // Same address
    EXPECT_EQ(obj2->value, 100); // Value preserved
    EXPECT_EQ(obj2->name, "original");
}

TEST(ObjectPool, ResetVsClear) {
    ObjectPool<int, 4> pool;

    // Allocate some
    pool.alloc();
    pool.alloc();

    // reset: objects retained, still usable
    pool.reset();
    EXPECT_EQ(pool.available(), 4u);
    EXPECT_NE(pool.alloc(), nullptr);

    // clear: objects deleted, unusable
    pool.clear();
    EXPECT_EQ(pool.available(), 0u);
    EXPECT_EQ(pool.alloc(), nullptr);
}

TEST(ObjectPoolThreadSafe, ResetThreadSafe) {
    ObjectPool<int, 100, true> pool;

    // Concurrent allocation
    std::vector<std::thread> threads;
    std::vector<int *> ptrs;
    std::mutex ptrs_mutex;

    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&]() {
            for (int i = 0; i < 20; ++i) {
                auto *p = pool.alloc();
                if (p) {
                    std::lock_guard<std::mutex> lock(ptrs_mutex);
                    ptrs.push_back(p);
                }
            }
        });
    }

    for (auto &th: threads) {
        th.join();
    }

    EXPECT_LT(pool.available(), pool.capacity());

    // Pool should return to full state after reset
    pool.reset();
    EXPECT_EQ(pool.available(), pool.capacity());

    // Can allocate again
    auto *p = pool.alloc();
    ASSERT_NE(p, nullptr);
    pool.free(p);
}

// ============== ObjectPool Tests (thread-safe) ==============

TEST(ObjectPoolThreadSafe, ConcurrentAllocFree) {
    ObjectPool<int, 1000, true> pool;
    std::atomic<int> alloc_count{0};
    std::atomic<int> free_count{0};
    constexpr int num_threads = 8;
    constexpr int ops_per_thread = 1000;

    std::vector<std::thread> threads;
    std::vector<std::vector<int *>> thread_objects(num_threads);

    // Concurrent allocation
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < ops_per_thread; ++i) {
                auto *obj = pool.alloc();
                if (obj) {
                    alloc_count++;
                    thread_objects[t].push_back(obj);
                    *obj = t * ops_per_thread + i;
                }
            }
        });
    }

    for (auto &th: threads) {
        th.join();
    }

    // Concurrent return
    threads.clear();
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t]() {
            for (auto *obj: thread_objects[t]) {
                if (pool.free(obj)) {
                    free_count++;
                }
            }
        });
    }

    for (auto &th: threads) {
        th.join();
    }

    EXPECT_EQ(alloc_count.load(), free_count.load());
    EXPECT_EQ(pool.available(), pool.capacity());
}

TEST(ObjectPoolThreadSafe, StressTest) {
    ObjectPool<int, 100, true> pool;
    std::atomic<bool> running{true};
    std::atomic<int> total_ops{0};
    constexpr int num_threads = 4;

    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&]() {
            int *objs[10] = {};
            while (running.load()) {
                for (int i = 0; i < 10; ++i) {
                    if (!objs[i]) {
                        objs[i] = pool.alloc();
                    }
                    if (objs[i]) {
                        *objs[i] = i;
                        if (pool.free(objs[i])) {
                            objs[i] = nullptr;
                            total_ops++;
                        }
                    }
                }
            }
            // Clean up
            for (int i = 0; i < 10; ++i) {
                if (objs[i]) {
                    pool.free(objs[i]);
                }
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    running = false;

    for (auto &th: threads) {
        th.join();
    }

    EXPECT_GT(total_ops.load(), 0);
}

// ============== Edge-case tests ==============

TEST(ObjectPool, AllocateAllFreeAll) {
    ObjectPool<int, 4> pool;
    std::vector<int *> ptrs;

    // Allocate all
    for (int i = 0; i < 4; ++i) {
        auto *p = pool.alloc();
        ASSERT_NE(p, nullptr);
        ptrs.push_back(p);
    }
    EXPECT_EQ(pool.available(), 0u);

    // Another allocation should fail
    EXPECT_EQ(pool.alloc(), nullptr);

    // Return all
    for (auto *p: ptrs) {
        EXPECT_TRUE(pool.free(p));
    }
    EXPECT_EQ(pool.available(), 4u);

    // Can allocate again
    EXPECT_NE(pool.alloc(), nullptr);
}

TEST(ObjectPool, FreeWrongPointer) {
    ObjectPool<int, 2> pool;

    auto *p1 = pool.alloc();
    auto *p2 = pool.alloc();

    // Return sequentially, return out of pointer order
    EXPECT_TRUE(pool.free(p1));
    EXPECT_FALSE(pool.free(p1)); // Already returned
    EXPECT_TRUE(pool.free(p2));
    EXPECT_FALSE(pool.free(p2)); // Already returned
}

TEST(ObjectPoolThreadSafe, AvailableThreadSafe) {
    ObjectPool<int, 100, true> pool;
    std::atomic<bool> running{true};
    std::atomic<int> min_available{100};

    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&]() {
            std::vector<int *> local;
            while (running.load()) {
                if (auto *p = pool.alloc()) {
                    local.push_back(p);
                }
                size_t avail = pool.available();
                // Record minimum
                int current_min = min_available.load();
                while (avail < static_cast<size_t>(current_min)) {
                    if (min_available.compare_exchange_weak(current_min, avail)) {
                        break;
                    }
                }
                // Return some randomly
                if (local.size() > 10) {
                    for (int i = 0; i < 5; ++i) {
                        if (!local.empty()) {
                            pool.free(local.back());
                            local.pop_back();
                        }
                    }
                }
            }
            // Clean up
            for (auto *p: local) {
                pool.free(p);
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    running = false;

    for (auto &th: threads) {
        th.join();
    }

    // Pool should eventually return to full state
    EXPECT_EQ(pool.available(), pool.capacity());
    EXPECT_GE(min_available.load(), 0);
}

// ============== Custom LifeManager and Reset Tests ==============

struct CountingLM {
    static inline std::atomic<int> create_count{0};
    static inline std::atomic<int> destroy_count{0};

    static int *create() {
        create_count++;
        return new int(0);
    }
    static void destroy(int *p) {
        destroy_count++;
        delete p;
    }
    static void reset_counts() {
        create_count = 0;
        destroy_count = 0;
    }
};

TEST(ObjectPool, CustomLifeManagerCreateDestroy) {
    CountingLM::reset_counts();
    {
        ObjectPool<int, 4, false, CountingLM> pool;
        EXPECT_EQ(CountingLM::create_count.load(), 4);
        EXPECT_EQ(CountingLM::destroy_count.load(), 0);

        auto *p1 = pool.alloc();
        auto *p2 = pool.alloc();
        pool.free(p1);
        pool.free(p2);
    }
    // Destroy 4 objects in destructor
    EXPECT_EQ(CountingLM::destroy_count.load(), 4);
}

TEST(ObjectPool, CustomLifeManagerClear) {
    CountingLM::reset_counts();
    {
        ObjectPool<int, 4, false, CountingLM> pool;
        CountingLM::reset_counts(); // Exclude create calls in constructor
        pool.clear();
        EXPECT_EQ(CountingLM::destroy_count.load(), 4);
    }
    // No additional destroy in destructor (already cleared)
    EXPECT_EQ(CountingLM::destroy_count.load(), 4);
}

struct SetValueReset {
    static void reset(int &obj, int new_value) {
        obj = new_value;
    }
};

TEST(ObjectPool, CustomResetForwardArgs) {
    ObjectPool<int, 4, false, detail::DefaultLM<int>, SetValueReset> pool;

    auto *p = pool.alloc(42);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(*p, 42);

    auto *p2 = pool.alloc(99);
    ASSERT_NE(p2, nullptr);
    EXPECT_EQ(*p2, 99);
    EXPECT_NE(p, p2);
}

// Simulate libusb_transfer LifeManager + Reset usage
struct MockTransfer {
    int num_iso_packets = -1;
    int actual_length = -1;
    int status = -1;
};

struct MockTransferLM {
    static inline std::atomic<int> alloc_count{0};
    static inline std::atomic<int> free_count{0};

    static MockTransfer *create() {
        alloc_count++;
        auto *t = new MockTransfer{};
        t->num_iso_packets = 0;
        return t;
    }
    static void destroy(MockTransfer *p) {
        free_count++;
        delete p;
    }
    static void reset_counts() {
        alloc_count = 0;
        free_count = 0;
    }
};

struct MockTransferReset {
    static void reset(MockTransfer &t) {
        t.actual_length = 0;
        t.status = 0; // LIBUSB_TRANSFER_COMPLETED
    }
};

TEST(ObjectPool, CustomLifeManagerAndReset) {
    MockTransferLM::reset_counts();

    ObjectPool<MockTransfer, 4, false, MockTransferLM, MockTransferReset> pool;
    EXPECT_EQ(MockTransferLM::alloc_count.load(), 4);

    auto *t = pool.alloc();
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->actual_length, 0); // Reset zeroes it
    EXPECT_EQ(t->status, 0);
    EXPECT_EQ(t->num_iso_packets, 0); // LifeManager sets it

    t->actual_length = 100;
    t->status = -5;
    pool.free(t);

    // Allocate same object again; should be reset by Reset
    auto *t2 = pool.alloc();
    EXPECT_EQ(t, t2);
    EXPECT_EQ(t2->actual_length, 0);
    EXPECT_EQ(t2->status, 0);

    pool.free(t2);
}

TEST(ObjectPool, LifeManagerFallbackPattern) {
    CountingLM::reset_counts();

    ObjectPool<int, 2, false, CountingLM> pool;
    CountingLM::reset_counts(); // Exclude construction

    auto *p1 = pool.alloc();
    auto *p2 = pool.alloc();
    auto *p3 = pool.alloc(); // Pool empty
    EXPECT_NE(p1, nullptr);
    EXPECT_NE(p2, nullptr);
    EXPECT_EQ(p3, nullptr);

    // Fallback: create directly
    p3 = CountingLM::create();
    ASSERT_NE(p3, nullptr);

    pool.free(p1);
    pool.free(p2);
    // p3 not in pool; destroy directly
    CountingLM::destroy(p3);

    // Can still allocate after pool restored
    auto *p4 = pool.alloc();
    EXPECT_NE(p4, nullptr);
    pool.free(p4);
}

// Verify default LifeManager behavior
TEST(ObjectPool, DefaultLifeManagerUsesNewDelete) {
    ObjectPool<int, 4> pool;
    auto *p = pool.alloc();
    ASSERT_NE(p, nullptr);
    *p = 123;
    EXPECT_TRUE(pool.free(p));
    // Default Reset does not modify value
    auto *p2 = pool.alloc();
    EXPECT_EQ(p, p2);
    // Default Reset is a no-op; value preserved
    EXPECT_EQ(*p2, 123);
}

TEST(ObjectPool, SingleElement) {
    ObjectPool<int, 1> pool;

    EXPECT_EQ(pool.capacity(), 1u);
    EXPECT_EQ(pool.available(), 1u);

    auto *p = pool.alloc();
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(pool.available(), 0u);

    EXPECT_EQ(pool.alloc(), nullptr); // Pool empty

    EXPECT_TRUE(pool.free(p));
    EXPECT_EQ(pool.available(), 1u);
}
