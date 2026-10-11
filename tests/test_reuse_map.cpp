#include <gtest/gtest.h>
#include <string>

#include "usbipdcpp/utils/ReuseMap.h"

using namespace usbipdcpp;

// ============== ReuseMap Tests ==============

TEST(ReuseMap, InsertAndFind) {
    ReuseMap<int, std::string> map;

    auto *v1 = map.insert(1, "one");
    ASSERT_NE(v1, nullptr);
    EXPECT_EQ(*v1, "one");

    auto *v2 = map.insert(2, "two");
    EXPECT_EQ(*v2, "two");
    EXPECT_EQ(map.size(), 2u);
}

TEST(ReuseMap, InsertDuplicateReturnsExisting) {
    ReuseMap<int, std::string> map;
    map.insert(1, "one");
    auto *v = map.insert(1, "ONE");
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(*v, "one");  // Old value unchanged
    EXPECT_EQ(map.size(), 1u);
}

TEST(ReuseMap, EraseAndReuseSlot) {
    ReuseMap<int, std::string> map;
    map.reserve(4);
    auto *p1 = map.insert(1, "one");
    map.insert(2, "two");

    EXPECT_TRUE(map.erase(1));
    EXPECT_EQ(map.find(1), nullptr);
    EXPECT_EQ(map.size(), 1u);

    // Reuse empty slot; first two push_back calls did not reallocate, so same slot should be reused
    auto *p3 = map.insert(3, "three");
    ASSERT_NE(p3, nullptr);
    EXPECT_EQ(p3, p1);
}

TEST(ReuseMap, PushBackWhenNoFreeSlot) {
    ReuseMap<int, int> map;
    // push_back individually when filling initially empty vector
    for (int i = 0; i < 3; i++) map.insert(i, i * 10);
    EXPECT_EQ(map.size(), 3u);

    // Reuse after deletion; should not push_back
    map.erase(1);
    auto *reused = map.insert(10, 100);
    ASSERT_NE(reused, nullptr);
    EXPECT_EQ(*reused, 100);
    EXPECT_EQ(map.size(), 3u);
}

TEST(ReuseMap, EraseNotFound) {
    ReuseMap<int, int> map;
    map.insert(1, 10);
    EXPECT_FALSE(map.erase(42));
    EXPECT_EQ(map.size(), 1u);
}

TEST(ReuseMap, FindNotFound) {
    ReuseMap<int, int> map;
    EXPECT_EQ(map.find(42), nullptr);
    map.insert(1, 10);
    EXPECT_NE(map.find(1), nullptr);
    EXPECT_EQ(map.find(2), nullptr);
    EXPECT_EQ(map.size(), 1u);
}

TEST(ReuseMap, ForEach) {
    ReuseMap<int, std::string> map;
    map.insert(1, "one");
    map.insert(2, "two");
    map.erase(1);
    map.insert(3, "three");

    std::string result;
    map.for_each([&](int k, const std::string &v) {
        result += std::to_string(k) + ":" + v + ";";
    });
    EXPECT_NE(result.find("2:two;"), std::string::npos);
    EXPECT_NE(result.find("3:three;"), std::string::npos);
    EXPECT_EQ(result.find("1:one;"), std::string::npos);  // Deleted
}

TEST(ReuseMap, Clear) {
    ReuseMap<int, int> map;
    for (int i = 0; i < 5; i++) map.insert(i, i * 10);
    EXPECT_EQ(map.size(), 5u);
    map.clear();
    EXPECT_EQ(map.size(), 0u);
    EXPECT_TRUE(map.empty());
    for (int i = 0; i < 5; i++) EXPECT_EQ(map.find(i), nullptr);
    // Can insert again after clearing
    map.insert(1, 100);
    EXPECT_EQ(*map.find(1), 100);
}

TEST(ReuseMap, PointerStabilityWithoutReallocation) {
    ReuseMap<int, int> map;
    map.reserve(4);
    auto *p1 = map.insert(1, 10);
    map.insert(2, 20);
    // Reuse insertion after deletion; reserved memory avoids reallocation, keeping pointer unchanged
    map.erase(1);
    auto *p3 = map.insert(3, 30);
    EXPECT_EQ(p3, p1);  // Reuse same slot
}

TEST(ReuseMap, Reserve) {
    ReuseMap<int, int> map;
    map.reserve(64);
    // Insertion after reserve should not reallocate
    for (int i = 0; i < 32; i++) map.insert(i, i);
    EXPECT_EQ(map.size(), 32u);
    for (int i = 0; i < 32; i++) EXPECT_NE(map.find(i), nullptr);
}

TEST(ReuseMap, EmptyFind) {
    ReuseMap<int, int> map;
    EXPECT_EQ(map.find(1), nullptr);
    EXPECT_EQ(map.size(), 0u);
    EXPECT_TRUE(map.empty());
}

TEST(ReuseMap, EmptyErase) {
    ReuseMap<int, int> map;
    EXPECT_FALSE(map.erase(1));
}

TEST(ReuseMap, EmptyForEach) {
    ReuseMap<int, int> map;
    int count = 0;
    map.for_each([&](int, int) { count++; });
    EXPECT_EQ(count, 0);
}

TEST(ReuseMap, ConstFind) {
    ReuseMap<int, std::string> map;
    map.insert(1, "one");
    const auto &cmap = map;
    auto *v = cmap.find(1);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(*v, "one");
    EXPECT_EQ(cmap.find(2), nullptr);
}

TEST(ReuseMap, MultiEraseReuse) {
    ReuseMap<int, int> map;
    // Fill a batch
    for (int i = 0; i < 10; i++) map.insert(i, i * 10);
    // Delete half
    for (int i = 0; i < 5; i++) map.erase(i);
    EXPECT_EQ(map.size(), 5u);
    // Reinsert; should reuse all empty slots
    for (int i = 0; i < 5; i++) map.insert(i + 100, (i + 100) * 10);
    EXPECT_EQ(map.size(), 10u);
    // All keys should be found
    for (int i = 0; i < 5; i++) {
        EXPECT_EQ(map.find(i), nullptr);
        EXPECT_NE(map.find(i + 100), nullptr);
    }
    for (int i = 5; i < 10; i++) EXPECT_NE(map.find(i), nullptr);
}

TEST(ReuseMap, StringKey) {
    ReuseMap<std::string, int> map;
    map.insert(std::string("hello"), 42);
    map.insert(std::string("world"), 99);
    EXPECT_EQ(*map.find("hello"), 42);
    EXPECT_EQ(*map.find("world"), 99);
    map.erase("hello");
    EXPECT_EQ(map.find("hello"), nullptr);
    EXPECT_EQ(map.size(), 1u);
}

TEST(ReuseMap, UpdateValueViaPointer) {
    ReuseMap<int, int> map;
    auto *p = map.insert(1, 10);
    *p = 20;
    EXPECT_EQ(*map.find(1), 20);
}

TEST(ReuseMap, ManyInserts) {
    ReuseMap<int, int> map;
    for (int i = 0; i < 100; i++) map.insert(i, i * 100);
    EXPECT_EQ(map.size(), 100u);
    for (int i = 0; i < 100; i++) {
        auto *v = map.find(i);
        ASSERT_NE(v, nullptr) << "missing key " << i;
        EXPECT_EQ(*v, i * 100);
    }
}