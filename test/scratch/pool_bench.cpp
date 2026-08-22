// Is kiotty::BlockPool actually faster than plain malloc/free, and where?
//
// This measures the header as it now stands - one mutex for the whole pool,
// regions in a fixed member array - not the earlier prototype.
//
// The mean is not the interesting number. A 1/60 s deadline is missed by one
// slow allocation, not by a thousand ordinary ones, so the percentiles are what
// this reports.
//
// Workload shape: blocks are acquired, held while "in flight", and released -
// the same shape as a send queue.

#include <core/kiotty_block_pool.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

using namespace kiotty;

static uint64_t nowNanos()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct Stats
{
    double mean;
    double p50;
    double p99;
    double p999;
    double max;
};

static Stats summarize(std::vector<uint64_t>& samples)
{
    std::sort(samples.begin(), samples.end());

    double total = 0;

    for (size_t i = 0; i < samples.size(); ++i)
    {
        total += static_cast<double>(samples[i]);
    }

    Stats s;
    s.mean = total / static_cast<double>(samples.size());
    s.p50  = static_cast<double>(samples[samples.size() / 2]);
    s.p99  = static_cast<double>(samples[samples.size() * 99 / 100]);
    s.p999 = static_cast<double>(samples[samples.size() * 999 / 1000]);
    s.max  = static_cast<double>(samples[samples.size() - 1]);
    return s;
}

// The shape assumed for a realtime game: mostly small input and delta packets,
// a thin tail of snapshots and fragments. Not measured - there is no game logic
// yet - so it is written here where it can be argued with.
static size_t sizeFor(unsigned step)
{
    const unsigned bucket = step % 100;

    if (bucket < 70) { return 32 + (step % 96); }        // 32..127    70%
    if (bucket < 90) { return 128 + (step % 384); }      // 128..511   20%
    if (bucket < 98) { return 512 + (step % 3584); }     // 512..4095   8%
    return 4096 + (step % 61440);                        // 4K..64K     2%
}

static const size_t IN_FLIGHT = 64;

static Stats runMalloc(int rounds, bool mixed, size_t fixed_size)
{
    std::vector<uint64_t> samples;
    std::vector<void*>    live(IN_FLIGHT, nullptr);

    samples.reserve(static_cast<size_t>(rounds));

    for (int r = 0; r < rounds; ++r)
    {
        const size_t slot = static_cast<size_t>(r) % IN_FLIGHT;
        const size_t size = mixed ? sizeFor(static_cast<unsigned>(r)) : fixed_size;

        const uint64_t start = nowNanos();

        std::free(live[slot]);
        live[slot] = std::malloc(size);

        samples.push_back(nowNanos() - start);

        *static_cast<uint8_t*>(live[slot]) = static_cast<uint8_t>(r);
    }

    for (size_t i = 0; i < live.size(); ++i)
    {
        std::free(live[i]);
    }
    return summarize(samples);
}

static Stats runPool(BlockPool& pool, int rounds, bool mixed, size_t fixed_size)
{
    std::vector<uint64_t> samples;
    std::vector<void*>    live(IN_FLIGHT, nullptr);

    samples.reserve(static_cast<size_t>(rounds));

    for (int r = 0; r < rounds; ++r)
    {
        const size_t slot = static_cast<size_t>(r) % IN_FLIGHT;
        const size_t size = mixed ? sizeFor(static_cast<unsigned>(r)) : fixed_size;

        const uint64_t start = nowNanos();

        pool.release(live[slot]);
        live[slot] = pool.acquire(size);

        samples.push_back(nowNanos() - start);

        *static_cast<uint8_t*>(live[slot]) = static_cast<uint8_t>(r);
    }

    for (size_t i = 0; i < live.size(); ++i)
    {
        pool.release(live[i]);
    }
    return summarize(samples);
}

static void report(const char* label, const Stats& s)
{
    std::printf("  %-8s mean %7.0f   p50 %7.0f   p99 %8.0f   p99.9 %9.0f   max %10.0f\n",
                label, s.mean, s.p50, s.p99, s.p999, s.max);
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    BlockPool pool(defaultBlockClasses(), defaultBlockClassCount());

    std::printf("pool: %u regions, %.3f MiB reserved\n\n",
                static_cast<unsigned>(pool.regionCount()),
                static_cast<double>(pool.reservedBytes()) / (1024.0 * 1024.0));

    const int rounds = 500000;

    runMalloc(20000, true, 0);
    runPool(pool, 20000, true, 0);

    std::printf("nanoseconds per release+acquire, %d rounds, %u in flight\n",
                rounds, static_cast<unsigned>(IN_FLIGHT));

    std::printf("\n256 byte payloads (the common case):\n");
    report("malloc", runMalloc(rounds, false, 256));
    report("pool", runPool(pool, rounds, false, 256));

    std::printf("\n16 KiB payloads:\n");
    report("malloc", runMalloc(rounds, false, 16384));
    report("pool", runPool(pool, rounds, false, 16384));

    std::printf("\nmixed sizes (70%% small, 20%% medium, 8%% large, 2%% huge):\n");
    report("malloc", runMalloc(rounds, true, 0));
    report("pool", runPool(pool, rounds, true, 0));

    std::printf("\nheap fallbacks so far: %u\n",
                static_cast<unsigned>(pool.fallbackCount()));

    // Four producers at once. One mutex guards the whole pool now, so this is
    // where that decision has to answer for itself against malloc's per-thread
    // cache.
    std::printf("\n4 threads, mixed sizes (worst thread shown):\n");

    for (int pass = 0; pass < 2; ++pass)
    {
        std::vector<std::thread> threads;
        std::vector<Stats>       results(4);

        for (int t = 0; t < 4; ++t)
        {
            threads.push_back(std::thread([pass, t, rounds, &results, &pool]()
            {
                results[static_cast<size_t>(t)] = (pass == 0)
                    ? runMalloc(rounds / 4, true, 0)
                    : runPool(pool, rounds / 4, true, 0);
            }));
        }

        for (size_t i = 0; i < threads.size(); ++i)
        {
            threads[i].join();
        }

        Stats worst = results[0];

        for (size_t i = 1; i < results.size(); ++i)
        {
            worst.mean = (worst.mean + results[i].mean) / 2.0;
            worst.p50  = std::max(worst.p50, results[i].p50);
            worst.p99  = std::max(worst.p99, results[i].p99);
            worst.p999 = std::max(worst.p999, results[i].p999);
            worst.max  = std::max(worst.max, results[i].max);
        }
        report((pass == 0) ? "malloc" : "pool", worst);
    }

    // What the engine actually asks for: 8 clients at 60 Hz is on the order of a
    // thousand payloads a second, not millions. Both numbers below are the share
    // of one 16.7 ms frame spent allocating.
    std::printf("\ntotal heap fallbacks: %u\n",
                static_cast<unsigned>(pool.fallbackCount()));
    return 0;
}
