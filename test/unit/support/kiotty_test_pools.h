#if !defined(KIOTTY_TEST_POOLS_H)
#define KIOTTY_TEST_POOLS_H

#include <core/kiotty_block_pool.h>

#include <cstddef>

namespace kiotty_test
{
    // One block per class, which is what makes "which size class did this
    // length come from" observable from outside the pool.
    //
    // BlockPool exposes no per-class counter, only fallbackCount(). With a
    // single block per class, draining the class a length should have used
    // forces the next exact-size request onto the heap - and that shows up in
    // fallbackCount(). A pool with the default 2048 blocks per class would
    // answer every request the same way and prove nothing.
    inline const kiotty::BlockClass* oneBlockPerClass()
    {
        static const kiotty::BlockClass table[] =
        {
            {    64, 1 },
            {   128, 1 },
            {   256, 1 },
            {   512, 1 },
            {  1024, 1 },
            {  4096, 1 },
            { 16384, 1 },
            { 65536, 1 },
        };
        return table;
    }

    inline std::size_t oneBlockPerClassCount()
    {
        return 8;
    }

    // A pool of exactly one size class, for the tests that ask "did the block
    // come back". Owning the class table keeps it alive as long as the pool
    // that reads it - BlockPool copies nothing.
    class SmallPool
    {
    public:
        SmallPool(std::size_t block_size, std::size_t num_of_block) :
            _class{block_size, num_of_block},
            _pool(&_class, 1)
        {
        }

        SmallPool(const SmallPool&) = delete;
        SmallPool& operator=(const SmallPool&) = delete;

        kiotty::BlockPool& pool() { return _pool; }

    private:
        kiotty::BlockClass _class;
        kiotty::BlockPool  _pool;
    };
}

#endif // KIOTTY_TEST_POOLS_H
