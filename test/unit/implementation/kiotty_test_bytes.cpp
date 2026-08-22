// Bytes owns at most one pool block, and the whole type is the invariant that
// it either has a pool and a block or has neither. Every test here walks a path
// that could break that pairing - a double release and a leak are the two ways
// it shows up in production, and both are silent.

#include <core/kiotty_block_pool.h>
#include <core/kiotty_bytes.h>

#include "support/kiotty_test_pools.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

using kiotty::BlockPool;
using kiotty::ByteSpan;
using kiotty::ByteView;
using kiotty::Bytes;
using kiotty_test::SmallPool;

namespace
{
    // Self-move written directly is diagnosed by -Wself-move, and the warning
    // is right about ordinary code. Going through a function makes the compiler
    // stop guessing and lets the test ask what the operator actually does.
    void moveOnto(Bytes& target, Bytes& source)
    {
        target = std::move(source);
    }
}

TEST(ByteView, DefaultViewReadsAsNothing)
{
    ByteView view;

    EXPECT_FALSE(static_cast<bool>(view));
    EXPECT_EQ(nullptr, view.data());
    EXPECT_EQ(static_cast<size_t>(0), view.size());
}

TEST(ByteView, SliceReturnsTheRequestedWindow)
{
    const uint8_t raw[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };

    const ByteView middle = ByteView(raw, sizeof(raw)).slice(2, 3);

    EXPECT_EQ(static_cast<size_t>(3), middle.size());
    EXPECT_EQ(2, middle.data()[0]);
    EXPECT_EQ(4, middle.data()[2]);
}

TEST(ByteView, SliceAtTheVeryEndIsLegalAndEmpty)
{
    const uint8_t raw[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };

    // offset == size with count == 0 is the one-past-the-end case the receive
    // path hits when a buffer is exactly full. It must not trip the assert.
    const ByteView tail = ByteView(raw, sizeof(raw)).slice(8, 0);

    EXPECT_EQ(static_cast<size_t>(0), tail.size());
}

TEST(ByteSpan, WritesThroughTheSpanAreVisibleThroughTheView)
{
    uint8_t  scratch[4] = { 0, 0, 0, 0 };
    ByteSpan span(scratch, sizeof(scratch));

    span.data()[1] = 9;

    EXPECT_EQ(9, span.view().data()[1]);
    EXPECT_EQ(static_cast<size_t>(2), span.slice(1, 2).size());
}

TEST(Bytes, DefaultBytesOwnsNothing)
{
    Bytes bytes;

    EXPECT_FALSE(static_cast<bool>(bytes));
    EXPECT_EQ(nullptr, bytes.data());
    EXPECT_EQ(static_cast<size_t>(0), bytes.size());
}

TEST(Bytes, ZeroLengthRequestOwnsNothingAndTakesNoBlock)
{
    SmallPool small(64, 1);

    Bytes bytes(small.pool(), 0);

    EXPECT_FALSE(static_cast<bool>(bytes));
    EXPECT_EQ(static_cast<size_t>(0), bytes.size());
    EXPECT_EQ(static_cast<size_t>(0), small.pool().fallbackCount());

    // The one block is still there, which is the point of the check above.
    Bytes taken(small.pool(), 64);
    EXPECT_TRUE(static_cast<bool>(taken));
    EXPECT_EQ(static_cast<size_t>(0), small.pool().fallbackCount());
}

TEST(Bytes, NonZeroLengthRequestOwnsABlockOfExactlyThatLength)
{
    const uint8_t raw[5] = { 10, 20, 30, 40, 50 };

    SmallPool small(64, 1);
    Bytes     bytes(small.pool(), sizeof(raw));

    ASSERT_TRUE(static_cast<bool>(bytes));

    // The block behind it is 64 bytes wide, but size() reports the length that
    // was asked for - the receive path sizes its reads from this number.
    EXPECT_EQ(static_cast<size_t>(5), bytes.size());

    std::memcpy(bytes.writableSpan().data(), raw, sizeof(raw));

    EXPECT_EQ(0, std::memcmp(bytes.data(), raw, sizeof(raw)));
    EXPECT_NE(static_cast<const void*>(raw), static_cast<const void*>(bytes.data()));
}

TEST(Bytes, MoveConstructionLeavesTheSourceEmpty)
{
    SmallPool small(64, 1);

    Bytes source(small.pool(), 64);
    const uint8_t* const block = source.data();

    Bytes moved(std::move(source));

    EXPECT_TRUE(static_cast<bool>(moved));
    EXPECT_EQ(block, moved.data());

    // If the source kept the pointer, its destructor would return the same
    // block a second time and the free list would loop onto itself.
    EXPECT_FALSE(static_cast<bool>(source));
    EXPECT_EQ(nullptr, source.data());
}

TEST(Bytes, MoveAssignmentLeavesTheSourceEmpty)
{
    SmallPool small(64, 2);

    Bytes source(small.pool(), 64);
    Bytes target;

    const uint8_t* const block = source.data();

    target = std::move(source);

    EXPECT_EQ(block, target.data());
    EXPECT_FALSE(static_cast<bool>(source));
}

TEST(Bytes, MoveAssignmentReturnsTheTargetsOldBlockToThePool)
{
    SmallPool small(64, 2);

    Bytes target(small.pool(), 64);
    Bytes source(small.pool(), 64);

    ASSERT_EQ(static_cast<size_t>(0), small.pool().fallbackCount());

    target = std::move(source);

    // Both blocks were out; one came back with the assignment, so a fresh
    // request must still find a block rather than fall to the heap.
    Bytes third(small.pool(), 64);

    EXPECT_TRUE(static_cast<bool>(third));
    EXPECT_EQ(static_cast<size_t>(0), small.pool().fallbackCount());
}

TEST(Bytes, SelfMoveAssignmentKeepsTheBlock)
{
    SmallPool small(64, 1);

    Bytes bytes(small.pool(), 64);
    const uint8_t* const block = bytes.data();

    moveOnto(bytes, bytes);

    EXPECT_TRUE(static_cast<bool>(bytes));
    EXPECT_EQ(block, bytes.data());
}

TEST(Bytes, DestructionReturnsTheBlockToThePool)
{
    SmallPool small(64, 1);

    {
        Bytes bytes(small.pool(), 64);
        ASSERT_TRUE(static_cast<bool>(bytes));
    }

    Bytes again(small.pool(), 64);

    EXPECT_TRUE(static_cast<bool>(again));
    EXPECT_EQ(static_cast<size_t>(0), small.pool().fallbackCount())
        << "the only block was not returned, so this request had to be malloc'd";
}
