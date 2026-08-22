// The send queue holds packets and the policy that decides their fate, but it
// decides nothing itself - Connection does. So what is checked here is that the
// queue answers honestly: it says no when it is full without swallowing what it
// refused, and dropOldest removes exactly one packet and exactly the right one.
//
// dropOldest is the newest member and the one with a real search in it. A queue
// whose front is Never and whose middle is Oldest is the case that separates
// "look at the front" from "find the oldest droppable", and it is the case a
// server under load actually has.

#include <core/kiotty_block_pool.h>
#include <core/kiotty_bytes.h>
#include <core/kiotty_connection_buffer.h>

#include "support/kiotty_test_pools.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using kiotty::BlockPool;
using kiotty::Bytes;
using kiotty::defaultBlockClasses;
using kiotty::defaultBlockClassCount;
using kiotty::DropPolicy;
using kiotty::SendBuffer;
using kiotty::SentPacket;
using kiotty_test::SmallPool;

namespace
{
    // Packets are told apart by the first byte of their block, because a
    // SentPacket carries no identity of its own and the order after a drop is
    // the whole point of half these tests.
    SentPacket makePacket(BlockPool& pool, uint8_t tag, DropPolicy policy)
    {
        SentPacket packet;

        packet.bytes  = Bytes(pool, 64);
        packet.policy = policy;

        if (packet.bytes)
        {
            packet.bytes.writableSpan().data()[0] = tag;
        }
        return packet;
    }

    bool push(SendBuffer& queue, BlockPool& pool, uint8_t tag, DropPolicy policy)
    {
        SentPacket packet = makePacket(pool, tag, policy);
        return queue.tryPush(packet);
    }

    // Drains the queue into the tags it was holding, in order.
    std::vector<uint8_t> drainTags(SendBuffer& queue)
    {
        std::vector<uint8_t> tags;

        while (!queue.empty())
        {
            const SentPacket taken = queue.pop();

            tags.push_back(taken.bytes ? taken.bytes.data()[0] : 0);
        }
        return tags;
    }

    struct CapacityCase
    {
        size_t      capacity;
        const char* name;     // identifier-safe: this is what ctest lists
        const char* label;
    };

    const CapacityCase kCapacityCases[] =
    {
        { 0, "Capacity0", "0 - a queue that can hold nothing" },
        { 1, "Capacity1", "1 - room for the packet being sent and none behind it" },
        { 2, "Capacity2", "2 - the smallest queue with a front and a back" },
        { 8, "Capacity8", "8 - the shape a configured limit has" },
    };

    class QueueCapacity : public ::testing::TestWithParam<CapacityCase>
    {
    };

    struct DropCase
    {
        DropPolicy  droppable;
        const char* layout;      // one character per queued packet: N or O
        bool        expect_dropped;
        const char* expect_left; // the tags still queued afterwards, in order
        const char* name;        // identifier-safe: this is what ctest lists
        const char* label;
    };

    // Every arrangement that changes what dropOldest has to do. Tags are the
    // one-based position, so the expected leftovers read as digits.
    const DropCase kDropCases[] =
    {
        { DropPolicy::Oldest, "",     false, "",    "DropOldestFromEmpty",   "empty queue" },
        { DropPolicy::Oldest, "N",    false, "1",   "DropOldestFromN",       "one Never" },
        { DropPolicy::Oldest, "NN",   false, "12",  "DropOldestFromNN",      "nothing droppable at all" },
        { DropPolicy::Oldest, "O",    true,  "",    "DropOldestFromO",       "one Oldest" },
        { DropPolicy::Oldest, "OO",   true,  "2",   "DropOldestFromOO",      "the older of two Oldest" },
        { DropPolicy::Oldest, "NO",   true,  "1",   "DropOldestFromNO",      "a Never in front of the Oldest" },
        { DropPolicy::Oldest, "NNO",  true,  "12",  "DropOldestFromNNO",     "two Nevers to skip" },
        { DropPolicy::Oldest, "NONO", true,  "134", "DropOldestFromNONO",    "mixed - only the first Oldest goes" },
        { DropPolicy::Oldest, "ONON", true,  "234", "DropOldestFromONON",    "the front is droppable" },
        { DropPolicy::Never,  "NONO", true,  "234", "DropNeverFromNONO",     "asking for Never drops the Never" },
        { DropPolicy::Never,  "OO",   false, "12",  "DropNeverFromOO",       "no Never to drop" },
    };

    class Drop : public ::testing::TestWithParam<DropCase>
    {
    };

    DropPolicy policyOf(char letter)
    {
        return (letter == 'O') ? DropPolicy::Oldest : DropPolicy::Never;
    }

    // Without these gtest names each row by dumping the struct as bytes, and
    // ctest -R becomes unusable on exactly the tests that need it most.
    std::string nameOfCapacity(const ::testing::TestParamInfo<CapacityCase>& info)
    {
        return info.param.name;
    }

    std::string nameOfDrop(const ::testing::TestParamInfo<DropCase>& info)
    {
        return info.param.name;
    }
}

TEST_P(QueueCapacity, FillsToCapacityThenRefusesWithoutTakingThePacket)
{
    const CapacityCase& sample = GetParam();

    SCOPED_TRACE(sample.label);

    BlockPool  pool(defaultBlockClasses(), defaultBlockClassCount());
    SendBuffer queue(sample.capacity);

    EXPECT_EQ(sample.capacity, queue.capacity());
    EXPECT_TRUE(queue.empty());

    for (size_t i = 0; i < sample.capacity; ++i)
    {
        EXPECT_TRUE(push(queue, pool, static_cast<uint8_t>(i + 1), DropPolicy::Never));
        EXPECT_EQ(i + 1, queue.size());
    }

    EXPECT_TRUE(queue.full());

    SentPacket rejected = makePacket(pool, 99, DropPolicy::Never);

    EXPECT_FALSE(queue.tryPush(rejected));
    EXPECT_EQ(sample.capacity, queue.size());

    // The caller has to be able to decide what happens to a refused packet -
    // drop it, or drop something else and try again. A queue that consumed it
    // would have made that decision on the caller's behalf, silently.
    EXPECT_TRUE(static_cast<bool>(rejected.bytes))
        << "the refused packet was taken anyway";
    EXPECT_EQ(99, rejected.bytes.data()[0]);
}

INSTANTIATE_TEST_SUITE_P(EveryCapacityGroup, QueueCapacity,
                         ::testing::ValuesIn(kCapacityCases), nameOfCapacity);

TEST(SendBuffer, ZeroCapacityQueueIsFalseAndAcceptsNothing)
{
    BlockPool  pool(defaultBlockClasses(), defaultBlockClassCount());
    SendBuffer queue(0);

    EXPECT_FALSE(static_cast<bool>(queue));
    EXPECT_TRUE(queue.empty());
    EXPECT_TRUE(queue.full());
    EXPECT_FALSE(push(queue, pool, 1, DropPolicy::Never));
}

TEST(SendBuffer, NonZeroCapacityQueueIsTrue)
{
    SendBuffer queue(4);

    EXPECT_TRUE(static_cast<bool>(queue));
    EXPECT_FALSE(queue.full());
}

TEST(SendBuffer, PopOnAnEmptyQueueReturnsAnEmptyPacket)
{
    SendBuffer queue(4);

    const SentPacket taken = queue.pop();

    EXPECT_FALSE(static_cast<bool>(taken.bytes));
    EXPECT_EQ(DropPolicy::Never, taken.policy)
        << "an empty packet must not read as droppable";
}

TEST(SendBuffer, PacketsComeBackInTheOrderTheyWentIn)
{
    BlockPool  pool(defaultBlockClasses(), defaultBlockClassCount());
    SendBuffer queue(4);

    ASSERT_TRUE(push(queue, pool, 1, DropPolicy::Never));
    ASSERT_TRUE(push(queue, pool, 2, DropPolicy::Oldest));
    ASSERT_TRUE(push(queue, pool, 3, DropPolicy::Never));

    const std::vector<uint8_t> tags = drainTags(queue);

    ASSERT_EQ(static_cast<size_t>(3), tags.size());
    EXPECT_EQ(1, tags[0]);
    EXPECT_EQ(2, tags[1]);
    EXPECT_EQ(3, tags[2]);
}

TEST(SendBuffer, PopCarriesThePolicyAlongWithTheBytes)
{
    BlockPool  pool(defaultBlockClasses(), defaultBlockClassCount());
    SendBuffer queue(4);

    ASSERT_TRUE(push(queue, pool, 1, DropPolicy::Oldest));

    const SentPacket taken = queue.pop();

    EXPECT_TRUE(static_cast<bool>(taken.bytes));
    EXPECT_EQ(DropPolicy::Oldest, taken.policy);
}

TEST_P(Drop, RemovesExactlyTheOldestPacketCarryingTheAskedForPolicy)
{
    const DropCase& sample = GetParam();

    SCOPED_TRACE(sample.label);

    BlockPool  pool(defaultBlockClasses(), defaultBlockClassCount());
    SendBuffer queue(8);

    size_t queued = 0;

    for (const char* letter = sample.layout; *letter != '\0'; ++letter)
    {
        ++queued;
        ASSERT_TRUE(push(queue, pool, static_cast<uint8_t>('0' + queued),
                         policyOf(*letter)));
    }

    EXPECT_EQ(sample.expect_dropped, queue.dropOldest(sample.droppable));

    const std::vector<uint8_t> tags = drainTags(queue);

    std::vector<uint8_t> expected;

    for (const char* digit = sample.expect_left; *digit != '\0'; ++digit)
    {
        expected.push_back(static_cast<uint8_t>(*digit));
    }

    ASSERT_EQ(expected.size(), tags.size());

    for (size_t i = 0; i < expected.size(); ++i)
    {
        EXPECT_EQ(expected[i], tags[i]) << "at position " << i;
    }
}

INSTANTIATE_TEST_SUITE_P(EveryQueueLayout, Drop, ::testing::ValuesIn(kDropCases),
                         nameOfDrop);

TEST(SendBuffer, DropOldestOnAWrappedQueueKeepsTheRemainingOrder)
{
    BlockPool  pool(defaultBlockClasses(), defaultBlockClassCount());
    SendBuffer queue(4);

    ASSERT_TRUE(push(queue, pool, 1, DropPolicy::Never));
    ASSERT_TRUE(push(queue, pool, 2, DropPolicy::Never));
    ASSERT_TRUE(push(queue, pool, 3, DropPolicy::Never));
    ASSERT_TRUE(push(queue, pool, 4, DropPolicy::Oldest));

    // Sending two packets pushes the head off zero, which is the state a busy
    // connection is in most of the time and the state an index bug needs.
    ASSERT_EQ(1, queue.pop().bytes.data()[0]);
    ASSERT_EQ(2, queue.pop().bytes.data()[0]);

    ASSERT_TRUE(push(queue, pool, 5, DropPolicy::Oldest));
    ASSERT_TRUE(push(queue, pool, 6, DropPolicy::Never));

    EXPECT_TRUE(queue.dropOldest(DropPolicy::Oldest));

    const std::vector<uint8_t> tags = drainTags(queue);

    ASSERT_EQ(static_cast<size_t>(3), tags.size());
    EXPECT_EQ(3, tags[0]);
    EXPECT_EQ(5, tags[1]);
    EXPECT_EQ(6, tags[2]);
}

TEST(SendBuffer, DropOldestFreesTheDroppedPacketsBlock)
{
    SmallPool  small(64, 2);
    SendBuffer queue(4);

    ASSERT_TRUE(push(queue, small.pool(), 1, DropPolicy::Oldest));
    ASSERT_TRUE(push(queue, small.pool(), 2, DropPolicy::Never));
    ASSERT_EQ(static_cast<size_t>(0), small.pool().fallbackCount());

    EXPECT_TRUE(queue.dropOldest(DropPolicy::Oldest));

    // A drop that only unlinked the packet would leave its block out for good.
    void* const reused = small.pool().acquire(64);

    ASSERT_NE(nullptr, reused);
    EXPECT_EQ(static_cast<size_t>(0), small.pool().fallbackCount())
        << "the dropped packet did not return its block";

    small.pool().release(reused);
}

TEST(SendBuffer, DestructionReturnsEveryQueuedBlockToThePool)
{
    SmallPool small(64, 4);

    {
        SendBuffer queue(4);

        for (uint8_t tag = 1; tag <= 4; ++tag)
        {
            ASSERT_TRUE(push(queue, small.pool(), tag, DropPolicy::Never));
        }
        ASSERT_EQ(static_cast<size_t>(0), small.pool().fallbackCount());
    }

    // Every block has to be back, so all four can be taken again without the
    // heap being asked for any of them.
    void* taken[4];

    for (size_t i = 0; i < 4; ++i)
    {
        taken[i] = small.pool().acquire(64);
        ASSERT_NE(nullptr, taken[i]) << "at block " << i;
    }

    EXPECT_EQ(static_cast<size_t>(0), small.pool().fallbackCount())
        << "the queue did not return everything it was holding";

    for (size_t i = 0; i < 4; ++i)
    {
        small.pool().release(taken[i]);
    }
}

TEST(SendBuffer, PoppedPacketLetsGoOfItsBlockWhenTheCallerDropsIt)
{
    SmallPool  small(64, 1);
    SendBuffer queue(4);

    ASSERT_TRUE(push(queue, small.pool(), 1, DropPolicy::Never));

    {
        const SentPacket taken = queue.pop();
        ASSERT_TRUE(static_cast<bool>(taken.bytes));

        // Still out: the queue handed it over rather than copying it.
        EXPECT_EQ(static_cast<size_t>(0), small.pool().fallbackCount());
    }

    void* const reused = small.pool().acquire(64);

    ASSERT_NE(nullptr, reused);
    EXPECT_EQ(static_cast<size_t>(0), small.pool().fallbackCount());

    small.pool().release(reused);
}
