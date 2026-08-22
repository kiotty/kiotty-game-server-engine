// A fixed capacity FIFO is two things: a wrap-around and a slot that must be
// emptied when its element leaves. Both are invisible with an int, so the
// element type here is Tracked - a value that reports when it stops owning.
//
// at() and removeAt() are the newest members and the least walked. removeAt on
// a wrapped buffer is where an index calculation goes wrong without saying so,
// which is why the head is deliberately pushed off zero before it is called.

#include <core/kiotty_ring_buffer.h>

#include "support/kiotty_test_tracked.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <utility>

using kiotty::RingBuffer;
using kiotty_test::Ledger;
using kiotty_test::Tracked;

namespace
{
    struct CapacityCase
    {
        size_t      capacity;
        const char* name;     // identifier-safe: this is what ctest lists
        const char* label;
    };

    // The four capacity groups. 1 makes the buffer wrap on every single push,
    // 2 is the smallest capacity where front and back are different slots, and
    // 8 is large enough to go round several times.
    const CapacityCase kCapacityCases[] =
    {
        { 0, "Capacity0", "0 - accepts nothing" },
        { 1, "Capacity1", "1 - wraps on every push" },
        { 2, "Capacity2", "2 - front and back differ" },
        { 8, "Capacity8", "8 - room to lap" },
    };

    class RingCapacity : public ::testing::TestWithParam<CapacityCase>
    {
    };

    // Without this gtest names each row by dumping the struct as bytes, and
    // ctest -R becomes unusable on exactly the tests that need it most.
    std::string nameOf(const ::testing::TestParamInfo<CapacityCase>& info)
    {
        return info.param.name;
    }

    // Pushes ids 1..count and asserts every push was accepted.
    void pushIds(RingBuffer<Tracked>& buffer, Ledger& ledger, int count)
    {
        for (int id = 1; id <= count; ++id)
        {
            ASSERT_TRUE(buffer.tryPush(Tracked(ledger, id))) << "at id " << id;
        }
    }

    // Pops one element and reports the id it carried, or 0 if the buffer was
    // empty. Returning the id rather than the Tracked keeps the ledger honest -
    // a returned Tracked would still be alive at the point of comparison.
    int popId(RingBuffer<Tracked>& buffer)
    {
        Tracked taken;

        if (!buffer.tryPop(taken))
        {
            return 0;
        }
        return taken.id();
    }
}

TEST_P(RingCapacity, FillsToCapacityThenRefusesAndKeepsItsSize)
{
    const CapacityCase& sample = GetParam();

    SCOPED_TRACE(sample.label);

    Ledger              ledger;
    RingBuffer<Tracked> buffer(sample.capacity);

    EXPECT_EQ(sample.capacity, buffer.capacity());
    EXPECT_TRUE(buffer.empty());

    for (size_t i = 0; i < sample.capacity; ++i)
    {
        EXPECT_TRUE(buffer.tryPush(Tracked(ledger, static_cast<int>(i + 1))));
        EXPECT_EQ(i + 1, buffer.size());
    }

    EXPECT_TRUE(buffer.full());

    Tracked overflow(ledger, 999);

    EXPECT_FALSE(buffer.tryPush(std::move(overflow)));
    EXPECT_EQ(sample.capacity, buffer.size());
}

INSTANTIATE_TEST_SUITE_P(EveryCapacityGroup, RingCapacity,
                         ::testing::ValuesIn(kCapacityCases), nameOf);

TEST(RingBuffer, ZeroCapacityBufferIsFalseAndHoldsNoStorage)
{
    RingBuffer<Tracked> buffer(0);

    // operator bool reports the one allocation, and a capacity of zero means
    // there was none to make - so it reads false without that being a failure.
    EXPECT_FALSE(static_cast<bool>(buffer));
    EXPECT_EQ(static_cast<size_t>(0), buffer.capacity());
    EXPECT_TRUE(buffer.empty());
    EXPECT_TRUE(buffer.full());
}

TEST(RingBuffer, NonZeroCapacityBufferIsTrue)
{
    RingBuffer<Tracked> buffer(4);

    EXPECT_TRUE(static_cast<bool>(buffer));
    EXPECT_EQ(static_cast<size_t>(4), buffer.capacity());
    EXPECT_FALSE(buffer.full());
}

TEST(RingBuffer, TryPushOnFullBufferLeavesACopiedValueWithTheCaller)
{
    Ledger              ledger;
    RingBuffer<Tracked> buffer(1);

    ASSERT_TRUE(buffer.tryPush(Tracked(ledger, 1)));

    Tracked rejected(ledger, 2);

    EXPECT_FALSE(buffer.tryPush(rejected));
    EXPECT_TRUE(rejected.owns()) << "a copied value must survive a rejected push";
    EXPECT_EQ(2, rejected.id());
}

TEST(RingBuffer, TryPushOnFullBufferLeavesAMovedValueWithTheCaller)
{
    Ledger              ledger;
    RingBuffer<Tracked> buffer(1);

    ASSERT_TRUE(buffer.tryPush(Tracked(ledger, 1)));

    Tracked rejected(ledger, 2);

    // The class contract says a rejected push takes nothing from the caller, so
    // the caller can still drop or retry with what it passed. A move-only
    // element - which is what the send queue holds - is the case that matters.
    EXPECT_FALSE(buffer.tryPush(std::move(rejected)));
    EXPECT_TRUE(rejected.owns())
        << "the rejected value was consumed, so the caller cannot retry with it";
}

TEST(RingBuffer, TryPopOnEmptyBufferReturnsFalseAndLeavesOutUntouched)
{
    Ledger              ledger;
    RingBuffer<Tracked> buffer(4);

    Tracked out(ledger, 7);

    EXPECT_FALSE(buffer.tryPop(out));
    EXPECT_TRUE(out.owns());
    EXPECT_EQ(7, out.id());
}

TEST(RingBuffer, TryPopEmptiesTheSlotItTookFrom)
{
    Ledger              ledger;
    RingBuffer<Tracked> buffer(4);

    ASSERT_TRUE(buffer.tryPush(Tracked(ledger, 1)));
    ASSERT_EQ(1, ledger.live());

    {
        Tracked taken;
        ASSERT_TRUE(buffer.tryPop(taken));

        // One live element: the one the caller now holds. A slot that kept its
        // moved-from element would show two here, and the element would stay
        // held until something happened to overwrite the slot.
        EXPECT_EQ(1, ledger.live());
    }

    EXPECT_EQ(0, ledger.live());
}

TEST(RingBuffer, OrderSurvivesSeveralLapsAroundTheBuffer)
{
    Ledger              ledger;
    RingBuffer<Tracked> buffer(3);

    int next_id = 1;

    for (int lap = 0; lap < 5; ++lap)
    {
        ASSERT_TRUE(buffer.tryPush(Tracked(ledger, next_id)));
        ASSERT_TRUE(buffer.tryPush(Tracked(ledger, next_id + 1)));

        EXPECT_EQ(next_id, popId(buffer)) << "at lap " << lap;
        EXPECT_EQ(next_id + 1, popId(buffer)) << "at lap " << lap;

        next_id += 2;
    }

    EXPECT_TRUE(buffer.empty());
    EXPECT_EQ(0, ledger.live());
}

TEST(RingBuffer, AtReadsThroughTheHeadOffsetRatherThanTheRawIndex)
{
    Ledger              ledger;
    RingBuffer<Tracked> buffer(4);

    pushIds(buffer, ledger, 4);

    ASSERT_EQ(1, popId(buffer));
    ASSERT_EQ(2, popId(buffer));

    // The head is at raw index 2 now, so these two land at raw 0 and 1.
    ASSERT_TRUE(buffer.tryPush(Tracked(ledger, 5)));
    ASSERT_TRUE(buffer.tryPush(Tracked(ledger, 6)));

    EXPECT_EQ(3, buffer.at(0).id());
    EXPECT_EQ(4, buffer.at(1).id());
    EXPECT_EQ(5, buffer.at(2).id());
    EXPECT_EQ(6, buffer.at(3).id());
}

TEST(RingBuffer, RemoveAtZeroMatchesTryPop)
{
    Ledger              ledger;
    RingBuffer<Tracked> buffer(4);

    pushIds(buffer, ledger, 3);

    EXPECT_TRUE(buffer.removeAt(0));
    EXPECT_EQ(static_cast<size_t>(2), buffer.size());
    EXPECT_EQ(2, popId(buffer));
    EXPECT_EQ(3, popId(buffer));
}

TEST(RingBuffer, RemoveAtLastRemovesWithoutDisturbingTheRest)
{
    Ledger              ledger;
    RingBuffer<Tracked> buffer(4);

    pushIds(buffer, ledger, 3);

    EXPECT_TRUE(buffer.removeAt(buffer.size() - 1));
    EXPECT_EQ(static_cast<size_t>(2), buffer.size());
    EXPECT_EQ(1, popId(buffer));
    EXPECT_EQ(2, popId(buffer));
    EXPECT_TRUE(buffer.empty());
}

TEST(RingBuffer, RemoveAtMiddleWhileWrappedKeepsTheRemainingOrder)
{
    Ledger              ledger;
    RingBuffer<Tracked> buffer(4);

    pushIds(buffer, ledger, 4);

    ASSERT_EQ(1, popId(buffer));
    ASSERT_EQ(2, popId(buffer));

    // Head sits at raw index 2, so the four elements are stored at raw
    // 2, 3, 0, 1. An implementation that shifted by raw index instead of by
    // offset from the head goes wrong here and nowhere else.
    ASSERT_TRUE(buffer.tryPush(Tracked(ledger, 5)));
    ASSERT_TRUE(buffer.tryPush(Tracked(ledger, 6)));
    ASSERT_EQ(static_cast<size_t>(4), buffer.size());

    EXPECT_TRUE(buffer.removeAt(1));

    EXPECT_EQ(static_cast<size_t>(3), buffer.size());
    EXPECT_EQ(3, popId(buffer));
    EXPECT_EQ(5, popId(buffer));
    EXPECT_EQ(6, popId(buffer));
}

TEST(RingBuffer, RemoveAtWrapPointKeepsTheRemainingOrder)
{
    Ledger              ledger;
    RingBuffer<Tracked> buffer(4);

    pushIds(buffer, ledger, 4);

    ASSERT_EQ(1, popId(buffer));
    ASSERT_EQ(2, popId(buffer));

    ASSERT_TRUE(buffer.tryPush(Tracked(ledger, 5)));
    ASSERT_TRUE(buffer.tryPush(Tracked(ledger, 6)));

    // Offset 2 is raw index 0 - the element the shift has to step across the
    // end of the array to reach.
    EXPECT_TRUE(buffer.removeAt(2));

    EXPECT_EQ(3, popId(buffer));
    EXPECT_EQ(4, popId(buffer));
    EXPECT_EQ(6, popId(buffer));
}

TEST(RingBuffer, RemoveAtSizeOrBeyondReturnsFalseAndChangesNothing)
{
    Ledger              ledger;
    RingBuffer<Tracked> buffer(4);

    pushIds(buffer, ledger, 3);

    EXPECT_FALSE(buffer.removeAt(buffer.size()));
    EXPECT_FALSE(buffer.removeAt(buffer.size() + 1));

    EXPECT_EQ(static_cast<size_t>(3), buffer.size());
    EXPECT_EQ(3, ledger.live());
    EXPECT_EQ(1, popId(buffer));
    EXPECT_EQ(2, popId(buffer));
    EXPECT_EQ(3, popId(buffer));
}

TEST(RingBuffer, RemoveAtOnEmptyBufferReturnsFalse)
{
    RingBuffer<Tracked> buffer(4);

    EXPECT_FALSE(buffer.removeAt(0));
    EXPECT_TRUE(buffer.empty());
}

TEST(RingBuffer, RemoveAtEmptiesTheVacatedLastSlot)
{
    Ledger              ledger;
    RingBuffer<Tracked> buffer(4);

    pushIds(buffer, ledger, 3);
    ASSERT_EQ(3, ledger.live());

    EXPECT_TRUE(buffer.removeAt(1));

    // Two elements remain, and the slot the shift vacated must hold nothing.
    // A slot left holding its moved-from element would show three here.
    EXPECT_EQ(2, ledger.live());
}

TEST(RingBuffer, DestructionLetsGoOfEverythingStillQueued)
{
    Ledger ledger;

    {
        RingBuffer<Tracked> buffer(8);
        pushIds(buffer, ledger, 5);
        ASSERT_EQ(5, ledger.live());
    }

    EXPECT_EQ(0, ledger.live());
}
