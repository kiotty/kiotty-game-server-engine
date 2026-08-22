// The pool has one job: answer a length with the smallest block that fits, and
// say so out loud when it could not. Picking the wrong class wastes memory
// quietly; borrowing from a bigger class when the right one is empty starves
// the bigger class and is quieter still. Both are pinned here.

#include <core/kiotty_block_pool.h>

#include "support/kiotty_test_pools.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>

using kiotty::BlockClass;
using kiotty::BlockPool;
using kiotty::defaultBlockClasses;
using kiotty::defaultBlockClassCount;
using kiotty_test::oneBlockPerClass;
using kiotty_test::oneBlockPerClassCount;
using kiotty_test::SmallPool;

namespace
{
    struct BlockLengthCase
    {
        size_t      length;
        size_t      expected_class;   // 0 means no class fits and the heap answers
        const char* name;             // identifier-safe: this is what ctest lists
        const char* label;
    };

    // The nine length groups, confirmed before any of this was written. 0 and 1
    // are in because every defect this layer has produced so far was one of
    // them; the boundary triples are in because "smallest class that fits" is
    // exactly a boundary claim.
    const BlockLengthCase kBlockLengthCases[] =
    {
        {     0,     0, "Length0",     "0 - not a request at all" },
        {     1,    64, "Length1",     "1 - the smallest real request" },
        {    63,    64, "Length63",    "boundary - 1" },
        {    64,    64, "Length64",    "boundary, exact" },
        {    65,   128, "Length65",    "boundary + 1 - must step up one class, not stay" },
        {  1024,  1024, "Length1024",  "middle of the table, exact" },
        { 65535, 65536, "Length65535", "last class - 1" },
        { 65536, 65536, "Length65536", "last class, exact" },
        { 65537,     0, "Length65537", "past the last class - the heap answers" },
    };

    class BlockLength : public ::testing::TestWithParam<BlockLengthCase>
    {
    };

    // Without this gtest names each row by dumping the struct as bytes, and
    // ctest -R becomes unusable on exactly the tests that need it most.
    std::string nameOf(const ::testing::TestParamInfo<BlockLengthCase>& info)
    {
        return info.param.name;
    }
}

TEST_P(BlockLength, LandsInTheSmallestClassThatFitsOrOnTheHeap)
{
    const BlockLengthCase& sample = GetParam();

    // Without the label a failing row is indistinguishable from the other eight.
    SCOPED_TRACE(sample.label);

    BlockPool pool(oneBlockPerClass(), oneBlockPerClassCount());

    void* const block = pool.acquire(sample.length);

    if (sample.length == 0)
    {
        EXPECT_EQ(nullptr, block);
        EXPECT_EQ(static_cast<size_t>(0), pool.fallbackCount());
        return;
    }

    ASSERT_NE(nullptr, block);

    if (sample.expected_class == 0)
    {
        EXPECT_EQ(static_cast<size_t>(1), pool.fallbackCount())
            << "no class fits this length, so the heap had to answer";
        pool.release(block);
        return;
    }

    EXPECT_EQ(static_cast<size_t>(0), pool.fallbackCount())
        << "a class fits this length, so the heap should not have been touched";

    // Each class holds exactly one block, so the class this length should have
    // used is now empty - and an exact-size request for it has to fall back. If
    // the length went to some other class, this request finds its block and the
    // count stays at zero, which is the failure being looked for.
    void* const second = pool.acquire(sample.expected_class);

    EXPECT_EQ(static_cast<size_t>(1), pool.fallbackCount())
        << "class " << sample.expected_class << " should have been drained by the "
        << "request for " << sample.length << " bytes";

    pool.release(second);
    pool.release(block);
}

INSTANTIATE_TEST_SUITE_P(EveryLengthGroup, BlockLength,
                         ::testing::ValuesIn(kBlockLengthCases), nameOf);

TEST(BlockPool, EmptyClassFallsToTheHeapInsteadOfBorrowingTheNextClass)
{
    BlockPool pool(oneBlockPerClass(), oneBlockPerClassCount());

    void* const first = pool.acquire(64);
    ASSERT_NE(nullptr, first);
    ASSERT_EQ(static_cast<size_t>(0), pool.fallbackCount());

    // The 128 class is untouched and would fit. Taking it would leave a real
    // 128 request with nothing while a 64 request is served twice its size, so
    // the pool is supposed to go to the heap instead.
    void* const second = pool.acquire(64);

    ASSERT_NE(nullptr, second);
    EXPECT_EQ(static_cast<size_t>(1), pool.fallbackCount());

    void* const from_128 = pool.acquire(128);
    ASSERT_NE(nullptr, from_128);
    EXPECT_EQ(static_cast<size_t>(1), pool.fallbackCount())
        << "the 128 class was borrowed by a 64 byte request";

    pool.release(from_128);
    pool.release(second);
    pool.release(first);
}

TEST(BlockPool, ReleasedBlockIsHandedOutAgain)
{
    SmallPool small(64, 1);

    void* const first = small.pool().acquire(64);
    ASSERT_NE(nullptr, first);

    small.pool().release(first);

    void* const second = small.pool().acquire(64);

    EXPECT_EQ(first, second);
    EXPECT_EQ(static_cast<size_t>(0), small.pool().fallbackCount());

    small.pool().release(second);
}

TEST(BlockPool, ReleaseOfNullDoesNothing)
{
    SmallPool small(64, 1);

    small.pool().release(nullptr);

    // A null threaded onto the free list would be handed out as a block.
    void* const block = small.pool().acquire(64);

    ASSERT_NE(nullptr, block);
    EXPECT_EQ(static_cast<size_t>(0), small.pool().fallbackCount());

    small.pool().release(block);
}

TEST(BlockPool, NullClassTableProducesAPoolWithNoRegionsThatStillAnswers)
{
    BlockPool pool(nullptr, defaultBlockClassCount());

    EXPECT_EQ(static_cast<size_t>(0), pool.regionCount());
    EXPECT_EQ(static_cast<size_t>(0), pool.reservedBytes());

    void* const block = pool.acquire(64);

    ASSERT_NE(nullptr, block);
    EXPECT_EQ(static_cast<size_t>(1), pool.fallbackCount());

    pool.release(block);
}

TEST(BlockPool, ClassCountAboveMaxRegionsIsClampedRatherThanOverrunning)
{
    // The table has to be at least as long as the count being passed, or this
    // would be testing the clamp by reading past the end of the array.
    BlockClass many[BlockPool::MAX_REGIONS + 4];

    for (size_t i = 0; i < BlockPool::MAX_REGIONS + 4; ++i)
    {
        many[i].block_size   = 64;
        many[i].num_of_block = 1;
    }

    BlockPool pool(many, BlockPool::MAX_REGIONS + 4);

    // The cast keeps this a value comparison: MAX_REGIONS is declared in the
    // class and defined nowhere, so binding it to a const reference would not
    // link.
    EXPECT_EQ(static_cast<size_t>(BlockPool::MAX_REGIONS), pool.regionCount());
    EXPECT_EQ(BlockPool::MAX_REGIONS * 64, pool.reservedBytes());
}

TEST(BlockPool, ClassNarrowerThanAPointerReservesNothingAndIsNeverChosen)
{
    // The free list is threaded through the blocks themselves, so a block that
    // cannot hold a pointer cannot be linked. reserve() refuses such a class.
    SmallPool small(4, 16);

    EXPECT_EQ(static_cast<size_t>(1), small.pool().regionCount());
    EXPECT_EQ(static_cast<size_t>(0), small.pool().reservedBytes());

    void* const block = small.pool().acquire(4);

    ASSERT_NE(nullptr, block);
    EXPECT_EQ(static_cast<size_t>(1), small.pool().fallbackCount())
        << "a refused class must not be handed out";

    small.pool().release(block);
}

TEST(BlockPool, ZeroBlockClassReservesNothingAndIsNeverChosen)
{
    SmallPool small(64, 0);

    EXPECT_EQ(static_cast<size_t>(0), small.pool().reservedBytes());

    void* const block = small.pool().acquire(64);

    ASSERT_NE(nullptr, block);
    EXPECT_EQ(static_cast<size_t>(1), small.pool().fallbackCount());

    small.pool().release(block);
}

TEST(BlockPool, DefaultTableReservesTheSumOfEveryClass)
{
    BlockPool pool(defaultBlockClasses(), defaultBlockClassCount());

    size_t expected = 0;

    for (size_t i = 0; i < defaultBlockClassCount(); ++i)
    {
        expected += defaultBlockClasses()[i].block_size *
                    defaultBlockClasses()[i].num_of_block;
    }

    EXPECT_EQ(defaultBlockClassCount(), pool.regionCount());
    EXPECT_EQ(expected, pool.reservedBytes());
    EXPECT_EQ(static_cast<size_t>(0), pool.fallbackCount());
}

TEST(BlockPool, HeapBlockIsRecognisedOnReleaseAndNotThreadedOntoAClass)
{
    SmallPool small(64, 1);

    void* const pooled = small.pool().acquire(64);
    void* const heaped = small.pool().acquire(64);   // the class is empty by now

    ASSERT_NE(nullptr, pooled);
    ASSERT_NE(nullptr, heaped);
    ASSERT_EQ(static_cast<size_t>(1), small.pool().fallbackCount());

    // If the heap block were pushed onto the class free list, the next acquire
    // would hand out memory the class does not own.
    small.pool().release(heaped);

    void* const after = small.pool().acquire(64);

    ASSERT_NE(nullptr, after);
    EXPECT_EQ(static_cast<size_t>(2), small.pool().fallbackCount())
        << "the class is still empty, so the heap had to answer this one too";

    small.pool().release(after);
    small.pool().release(pooled);
}
