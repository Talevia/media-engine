/*
 * test_stateful_pool — pins StatefulResourcePool's borrow/return/evict
 * contract.
 *
 * Covers:
 *   1. borrow() creates a fresh instance when the key has none.
 *   2. release on Handle destruction puts the instance back; next
 *      borrow of the same key returns the SAME object.
 *   3. evict drops a key's cached instance (next borrow → fresh).
 *   4. Different keys keep separate instances.
 *   5. Factory returning nullptr → Handle is empty, conversion to bool false.
 *   6. size() reflects only released (returned) instances.
 */
#include <doctest/doctest.h>

#include "resource/stateful_pool.hpp"

#include <atomic>
#include <memory>

using me::resource::StatefulResourcePool;

namespace {

struct Counted {
    int  value = 0;
    static std::atomic<int> live;
    Counted()  { ++live; }
    ~Counted() { --live; }
};
std::atomic<int> Counted::live{0};

}  // namespace

TEST_CASE("StatefulResourcePool: borrow constructs via factory; same key → same instance") {
    Counted::live.store(0);
    int factory_calls = 0;
    StatefulResourcePool<Counted> pool([&]() {
        ++factory_calls;
        auto p = std::make_unique<Counted>();
        p->value = 42;
        return p;
    });

    Counted* first_addr = nullptr;
    {
        auto h = pool.borrow(/*key=*/123);
        REQUIRE(h);
        CHECK(h->value == 42);
        h->value = 99;          /* mutate state */
        first_addr = h.get();
    }
    CHECK(factory_calls == 1);
    CHECK(pool.size() == 1);    /* released back into pool */

    {
        auto h = pool.borrow(/*key=*/123);
        REQUIRE(h);
        CHECK(h->value == 99);   /* state preserved */
        CHECK(h.get() == first_addr);
    }
    CHECK(factory_calls == 1);   /* no new factory call */
}

TEST_CASE("StatefulResourcePool: distinct keys get distinct instances") {
    StatefulResourcePool<Counted> pool([]() {
        return std::make_unique<Counted>();
    });

    Counted* addr_1 = nullptr;
    Counted* addr_2 = nullptr;
    {
        auto h1 = pool.borrow(1);
        auto h2 = pool.borrow(2);
        REQUIRE(h1);
        REQUIRE(h2);
        addr_1 = h1.get();
        addr_2 = h2.get();
        CHECK(addr_1 != addr_2);
    }
    CHECK(pool.size() == 2);
    {
        auto h1 = pool.borrow(1);
        auto h2 = pool.borrow(2);
        CHECK(h1.get() == addr_1);
        CHECK(h2.get() == addr_2);
    }
}

TEST_CASE("StatefulResourcePool: evict drops the instance; next borrow → fresh") {
    int factory_calls = 0;
    StatefulResourcePool<Counted> pool([&]() {
        ++factory_calls;
        return std::make_unique<Counted>();
    });

    {
        auto h = pool.borrow(7);
        h->value = 1;
    }
    CHECK(factory_calls == 1);
    pool.evict(7);
    CHECK(pool.size() == 0);
    {
        auto h = pool.borrow(7);
        CHECK(h->value == 0);     /* fresh */
    }
    CHECK(factory_calls == 2);
}

TEST_CASE("StatefulResourcePool: clear() empties the cache") {
    StatefulResourcePool<Counted> pool([]() {
        return std::make_unique<Counted>();
    });
    {
        auto h1 = pool.borrow(10);
        auto h2 = pool.borrow(20);
        auto h3 = pool.borrow(30);
    }
    CHECK(pool.size() == 3);
    pool.clear();
    CHECK(pool.size() == 0);
}

TEST_CASE("StatefulResourcePool: factory returning nullptr → empty Handle") {
    StatefulResourcePool<Counted> pool([]() {
        return std::unique_ptr<Counted>();
    });
    auto h = pool.borrow(1);
    CHECK_FALSE(h);
    CHECK(h.get() == nullptr);
}

TEST_CASE("StatefulResourcePool: size() doesn't count outstanding handles") {
    StatefulResourcePool<Counted> pool([]() {
        return std::make_unique<Counted>();
    });
    auto h = pool.borrow(1);
    CHECK(pool.size() == 0);     /* h holds it, pool has none stored */
    {
        auto h2 = pool.borrow(2);
        CHECK(pool.size() == 0); /* both out */
    }                             /* h2 returns to pool */
    CHECK(pool.size() == 1);
}

TEST_CASE("StatefulResourcePool: lifecycle — instances destruct on clear/evict") {
    Counted::live.store(0);
    {
        StatefulResourcePool<Counted> pool([]() {
            return std::make_unique<Counted>();
        });
        {
            auto h = pool.borrow(1);
            CHECK(Counted::live.load() == 1);
        }
        CHECK(Counted::live.load() == 1);   /* still in pool */
        pool.evict(1);
        CHECK(Counted::live.load() == 0);
    }
    CHECK(Counted::live.load() == 0);
}
