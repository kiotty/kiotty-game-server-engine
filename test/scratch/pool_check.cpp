// Scratch verification for BlockPool. Proper unit tests are cpp-tester's job;
// this exists so nothing gets reported as working before it was run.

#include <core/kiotty_block_pool.h>

#include <cstdio>
#include <cstring>
#include <set>
#include <thread>
#include <vector>

using namespace kiotty;

static int g_failures = 0;

static void check(bool ok, const char* what)
{
    if (!ok)
    {
        std::printf("[FAIL] %s\n", what);
        ++g_failures;
    }
}

static void checkDefaults()
{
    BlockPool pool(defaultBlockClasses(), defaultBlockClassCount());

    check(pool.regionCount() == 8, "eight regions");
    check(pool.reservedBytes() == 4849664, "4.625 MiB reserved");
    check(pool.fallbackCount() == 0, "nothing fell back yet");

    void* small = pool.acquire(1);
    void* exact = pool.acquire(64);
    void* over = pool.acquire(65);
    void* biggest = pool.acquire(65536);

    check(small != nullptr && exact != nullptr, "small sizes are served");
    check(over != nullptr && biggest != nullptr, "large sizes are served");
    check(pool.fallbackCount() == 0, "none of that touched the heap");

    pool.release(small);
    pool.release(exact);
    pool.release(over);
    pool.release(biggest);

    check(pool.acquire(0) == nullptr, "zero length is not a request");
    pool.release(nullptr);
}

// Proving the choice without looking inside: with one block per region, taking
// 65 bytes must consume the 128 block, so the next 128 byte request has to fall
// back while a 64 byte request still succeeds.
static void checkSmallestFittingRegionIsUsed()
{
    const BlockClass classes[] = { { 64, 1 }, { 128, 1 } };
    BlockPool        pool(classes, 2);

    void* over_64 = pool.acquire(65);

    check(over_64 != nullptr, "65 bytes is served");
    check(pool.fallbackCount() == 0, "from a region, not the heap");

    void* needs_128 = pool.acquire(128);

    check(needs_128 != nullptr, "128 bytes is still answered");
    check(pool.fallbackCount() == 1, "but from the heap - 65 took the 128 block");

    void* fits_64 = pool.acquire(64);

    check(fits_64 != nullptr, "64 bytes is served");
    check(pool.fallbackCount() == 1, "from its own region, which was untouched");

    pool.release(over_64);
    pool.release(needs_128);
    pool.release(fits_64);
}

// A full region goes to the heap instead of raiding the region above it - the
// big blocks have to stay available for what only they can hold.
static void checkFullRegionDoesNotRaidTheNextOne()
{
    const BlockClass classes[] = { { 64, 2 }, { 256, 4 } };
    BlockPool        pool(classes, 2);

    void* a = pool.acquire(64);
    void* b = pool.acquire(64);

    check(pool.fallbackCount() == 0, "the region covered the first two");

    void* c = pool.acquire(64);

    check(c != nullptr, "a full region still answers, from the heap");
    check(pool.fallbackCount() == 1, "and it reports that it did");

    std::memset(c, 0xAB, 64);

    // All four 256 blocks must still be there; raiding would have taken one.
    void* big[4];

    for (int i = 0; i < 4; ++i)
    {
        big[i] = pool.acquire(256);
    }
    check(pool.fallbackCount() == 1, "the 256 region was left alone");

    for (int i = 0; i < 4; ++i)
    {
        pool.release(big[i]);
    }

    pool.release(c);
    pool.release(a);
    pool.release(b);

    check(pool.acquire(64) != nullptr && pool.fallbackCount() == 1,
          "released blocks are reusable without a new fallback");
}

static void checkBlocksDoNotOverlap()
{
    const BlockClass classes[] = { { 64, 8 } };
    BlockPool        pool(classes, 1);

    std::vector<void*> held;
    std::set<void*>    seen;

    for (int i = 0; i < 8; ++i)
    {
        void* const block = pool.acquire(64);

        check(block != nullptr, "the region hands out a block");
        check(seen.insert(block).second, "no block is handed out twice");

        std::memset(block, 0xC0 + i, 64);
        held.push_back(block);
    }

    for (int i = 0; i < 8; ++i)
    {
        const uint8_t  expected = static_cast<uint8_t>(0xC0 + i);
        const uint8_t* bytes    = static_cast<uint8_t*>(held[static_cast<size_t>(i)]);

        for (size_t b = 0; b < 64; ++b)
        {
            if (bytes[b] != expected)
            {
                check(false, "a block was overwritten by its neighbour");
                b = 64;
                i = 8;
            }
        }
    }

    for (size_t i = 0; i < held.size(); ++i)
    {
        pool.release(held[i]);
    }
    check(pool.fallbackCount() == 0, "all eight came from the region");
}

// A region that cannot hold a free pointer, one with no blocks, and no table at
// all. None of these may crash; all of them answer through the heap.
static void checkDegenerateTables()
{
    const BlockClass too_small[] = { { 4, 4 } };
    const BlockClass no_blocks[] = { { 64, 0 } };

    BlockPool tiny(too_small, 1);
    BlockPool empty(no_blocks, 1);
    BlockPool none(nullptr, 4);

    check(tiny.reservedBytes() == 0, "a block smaller than a pointer reserves nothing");
    check(empty.reservedBytes() == 0, "a region with no blocks reserves nothing");
    check(none.regionCount() == 0, "a null table builds no regions");

    void* from_tiny = tiny.acquire(4);
    void* from_empty = empty.acquire(64);
    void* from_none = none.acquire(64);

    check(from_tiny != nullptr && tiny.fallbackCount() == 1, "tiny falls back");
    check(from_empty != nullptr && empty.fallbackCount() == 1, "empty falls back");
    check(from_none != nullptr && none.fallbackCount() == 1, "no regions falls back");

    tiny.release(from_tiny);
    empty.release(from_empty);
    none.release(from_none);
}

// Acquire on one thread, release on another - the case this engine has, because
// a payload is built on a worker and released on the loop thread when its last
// send completes.
//
// Deliberately over-subscribed, so pool blocks and heap blocks both go through
// release() from threads that did not acquire them. A wrong region lookup frees
// the wrong thing here.
static void checkCrossThread()
{
    BlockPool pool(defaultBlockClasses(), defaultBlockClassCount());

    const int rounds = 20000;

    for (int pass = 0; pass < 4; ++pass)
    {
        std::vector<void*>       handed(static_cast<size_t>(rounds), nullptr);
        std::vector<std::thread> threads;

        for (int t = 0; t < 4; ++t)
        {
            threads.push_back(std::thread([t, rounds, &handed, &pool]()
            {
                for (int r = t; r < rounds; r += 4)
                {
                    handed[static_cast<size_t>(r)] =
                        pool.acquire(64 + static_cast<size_t>(r % 400));
                }
            }));
        }

        for (size_t i = 0; i < threads.size(); ++i)
        {
            threads[i].join();
        }

        std::set<void*> seen;

        for (size_t i = 0; i < handed.size(); ++i)
        {
            check(handed[i] != nullptr, "every acquire answered");

            if (!seen.insert(handed[i]).second)
            {
                check(false, "two threads were handed the same block");
                break;
            }
        }

        threads.clear();

        for (int t = 0; t < 4; ++t)
        {
            threads.push_back(std::thread([t, rounds, &handed, &pool]()
            {
                for (int r = rounds - 1 - t; r >= 0; r -= 4)
                {
                    pool.release(handed[static_cast<size_t>(r)]);
                }
            }));
        }

        for (size_t i = 0; i < threads.size(); ++i)
        {
            threads[i].join();
        }
    }

    void* after = pool.acquire(64);

    check(after != nullptr, "the pool still works after the storm");
    pool.release(after);

    std::printf("  cross-thread heap fallbacks (expected, over-subscribed): %u\n",
                static_cast<unsigned>(pool.fallbackCount()));
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    checkDefaults();
    checkSmallestFittingRegionIsUsed();
    checkFullRegionDoesNotRaidTheNextOne();
    checkBlocksDoNotOverlap();
    checkDegenerateTables();
    checkCrossThread();

    std::printf("block pool check failures=%d\n%s\n",
                g_failures, g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}
