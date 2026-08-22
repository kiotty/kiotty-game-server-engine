// GameChannelPool hands out slots and takes them back, and the generation
// counter is what makes "taken back" safe: an id that names a recycled slot
// must fail as STALE instead of quietly reaching whoever lives there now. The
// tests therefore do more remove/create cycles than creates, because that is
// where the counter does its work.
//
// Access is RAII around a recursive mutex. Re-entering the pool from inside an
// access scope is the documented way a usecase broadcasts while it runs, so
// that reentrancy is exercised on the same thread; a deadlock there would hang
// the test rather than fail it, which is loud enough.

#include <domain/channel/kiotty_game_channel_pool.h>

#include "support/kiotty_test_channel_listeners.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using kiotty::ChannelAccess;
using kiotty::ChannelCode;
using kiotty::ChannelId;
using kiotty::ChannelResult;
using kiotty::GameChannel;
using kiotty::GameChannelPool;
using kiotty::GameRequest;
using kiotty::makeChannelId;
using kiotty_test::RequestListener;

namespace
{
    // 1 is the smallest working pool and the one where "full" and "reused" are
    // the same slot; 4 is large enough that the wrong slot can be picked.
    const size_t kCapacities[] = { 1, 4 };

    class Capacity : public ::testing::TestWithParam<size_t>
    {
    };

    std::string nameOf(const ::testing::TestParamInfo<size_t>& info)
    {
        return "Capacity" + std::to_string(info.param);
    }

    ChannelId createOrFail(GameChannelPool& pool)
    {
        ChannelResult created = pool.create();

        EXPECT_TRUE(created.isOk()) << "code=" << static_cast<int>(created.code());
        return created.isOk() ? created.value().id() : ChannelId();
    }
}

// -----------------------------------------------------------------------------
// capacity 0
// -----------------------------------------------------------------------------

TEST(GameChannelPool, CapacityZeroIsNotUsable)
{
    GameChannelPool pool(0);

    EXPECT_FALSE(static_cast<bool>(pool));
    EXPECT_EQ(0u, pool.capacity());
    EXPECT_EQ(0u, pool.size());
}

TEST(GameChannelPool, CapacityZeroCreateIsPoolExhausted)
{
    GameChannelPool pool(0);
    ChannelResult   created = pool.create();

    EXPECT_FALSE(created.isOk());
    EXPECT_EQ(ChannelCode::CHANNEL_POOL_EXHAUSTED, created.code());
    EXPECT_EQ(0u, pool.size());
}

TEST(GameChannelPool, CapacityZeroAccessIsNotFound)
{
    GameChannelPool pool(0);
    ChannelAccess   access = pool.access(makeChannelId(0, 1));

    EXPECT_FALSE(static_cast<bool>(access));
    EXPECT_EQ(ChannelCode::CHANNEL_NOT_FOUND, access.code());
}

TEST(GameChannelPool, CapacityZeroRemoveIsHarmless)
{
    GameChannelPool pool(0);

    pool.remove(makeChannelId(0, 1));
    EXPECT_EQ(0u, pool.size());
}

// -----------------------------------------------------------------------------
// create
// -----------------------------------------------------------------------------

TEST_P(Capacity, FreshPoolIsUsableAndEmpty)
{
    GameChannelPool pool(GetParam());

    EXPECT_TRUE(static_cast<bool>(pool));
    EXPECT_EQ(GetParam(), pool.capacity());
    EXPECT_EQ(0u, pool.size());
}

TEST_P(Capacity, CreateSucceedsExactlyCapacityTimesThenIsExhausted)
{
    const size_t    capacity = GetParam();
    GameChannelPool pool(capacity);

    for (size_t i = 0; i < capacity; ++i)
    {
        SCOPED_TRACE(i);
        ChannelResult created = pool.create();
        EXPECT_TRUE(created.isOk());
        EXPECT_EQ(i + 1, pool.size());
    }

    ChannelResult overflow = pool.create();
    EXPECT_FALSE(overflow.isOk());
    EXPECT_EQ(ChannelCode::CHANNEL_POOL_EXHAUSTED, overflow.code());
    EXPECT_EQ(capacity, pool.size());
}

TEST_P(Capacity, FreshPoolHandsOutDistinctIndexesWithGenerationZero)
{
    const size_t    capacity = GetParam();
    GameChannelPool pool(capacity);
    std::vector<bool> seen(capacity, false);

    for (size_t i = 0; i < capacity; ++i)
    {
        SCOPED_TRACE(i);
        const ChannelId id = createOrFail(pool);

        ASSERT_LT(id.index, capacity);
        EXPECT_FALSE(seen[id.index]) << "index handed out twice";
        seen[id.index] = true;
        EXPECT_EQ(1u, id.generation);   // a fresh slot starts at 1; 0 is the null id
    }
}

TEST(GameChannelPool, CreatedChannelReportsTheIdThePoolGaveIt)
{
    GameChannelPool pool(4);
    ChannelResult   created = pool.create();

    ASSERT_TRUE(created.isOk());

    // The channel's own id and the pool's view of it must agree, or a usecase
    // reading request.channel_id would look up the wrong slot.
    const ChannelId id = created.value().id();
    ChannelAccess   access = pool.access(id);

    ASSERT_TRUE(static_cast<bool>(access));
    EXPECT_EQ(id, access.channel().id());
}

// -----------------------------------------------------------------------------
// access
// -----------------------------------------------------------------------------

TEST_P(Capacity, AccessReturnsTheSameObjectCreateReturned)
{
    GameChannelPool pool(GetParam());
    ChannelResult   created = pool.create();

    ASSERT_TRUE(created.isOk());

    GameChannel&  channel = created.value();
    ChannelAccess access  = pool.access(channel.id());

    ASSERT_TRUE(static_cast<bool>(access));
    EXPECT_EQ(ChannelCode::CHANNEL_SUCCESS, access.code());
    EXPECT_EQ(&channel, &access.channel());
}

TEST_P(Capacity, AccessWithIndexAtCapacityIsNotFound)
{
    const size_t    capacity = GetParam();
    GameChannelPool pool(capacity);

    createOrFail(pool);

    ChannelAccess access = pool.access(makeChannelId(static_cast<uint32_t>(capacity), 1));

    EXPECT_FALSE(static_cast<bool>(access));
    EXPECT_EQ(ChannelCode::CHANNEL_NOT_FOUND, access.code());
}

TEST_P(Capacity, AccessWithMaximumIndexIsNotFound)
{
    GameChannelPool pool(GetParam());

    createOrFail(pool);

    // The largest index that can be spelled: a bound check that compared as a
    // signed value, or not at all, would read far outside the slot array.
    ChannelAccess access = pool.access(makeChannelId(std::numeric_limits<uint32_t>::max(), 1));

    EXPECT_FALSE(static_cast<bool>(access));
    EXPECT_EQ(ChannelCode::CHANNEL_NOT_FOUND, access.code());
}

TEST(GameChannelPool, AccessOfASlotThatWasNeverCreatedIsNotFound)
{
    GameChannelPool pool(4);

    createOrFail(pool);     // occupies index 0

    ChannelAccess access = pool.access(makeChannelId(3, 1));

    EXPECT_FALSE(static_cast<bool>(access));
    EXPECT_EQ(ChannelCode::CHANNEL_NOT_FOUND, access.code());
}

TEST(GameChannelPool, AccessOfALiveSlotWithWrongGenerationIsStale)
{
    GameChannelPool pool(4);
    const ChannelId id = createOrFail(pool);

    ChannelAccess access = pool.access(makeChannelId(id.index, id.generation + 1));

    EXPECT_FALSE(static_cast<bool>(access));
    EXPECT_EQ(ChannelCode::CHANNEL_STALE, access.code());
}

TEST(GameChannelPool, MovedFromAccessNoLongerHoldsTheChannel)
{
    GameChannelPool pool(4);
    const ChannelId id = createOrFail(pool);

    ChannelAccess first = pool.access(id);
    ASSERT_TRUE(static_cast<bool>(first));

    ChannelAccess second(std::move(first));

    EXPECT_FALSE(static_cast<bool>(first));
    EXPECT_TRUE(static_cast<bool>(second));
    EXPECT_EQ(id, second.channel().id());
}

// -----------------------------------------------------------------------------
// remove
// -----------------------------------------------------------------------------

TEST_P(Capacity, RemoveThenAccessWithTheSameIdIsNotFound)
{
    GameChannelPool pool(GetParam());
    const ChannelId id = createOrFail(pool);

    pool.remove(id);

    // Nothing has reclaimed the slot yet, so it is empty rather than stale.
    ChannelAccess access = pool.access(id);

    EXPECT_FALSE(static_cast<bool>(access));
    EXPECT_EQ(ChannelCode::CHANNEL_NOT_FOUND, access.code());
    EXPECT_EQ(0u, pool.size());
}

TEST_P(Capacity, RemoveThenCreateReusesTheSlotWithTheNextGeneration)
{
    const size_t    capacity = GetParam();
    GameChannelPool pool(capacity);

    // Fill the pool so the only free slot after the remove is the one freed.
    std::vector<ChannelId> ids;
    for (size_t i = 0; i < capacity; ++i)
    {
        ids.push_back(createOrFail(pool));
    }

    const ChannelId old_id = ids[0];
    pool.remove(old_id);

    const ChannelId new_id = createOrFail(pool);

    EXPECT_EQ(old_id.index, new_id.index);
    EXPECT_EQ(old_id.generation + 1, new_id.generation);
    EXPECT_NE(old_id, new_id);
    EXPECT_EQ(capacity, pool.size());
}

TEST_P(Capacity, OldIdAfterSlotReuseIsStaleAndDoesNotReachTheNewChannel)
{
    GameChannelPool pool(GetParam());

    const ChannelId old_id = createOrFail(pool);
    pool.remove(old_id);
    const ChannelId new_id = createOrFail(pool);

    ChannelAccess stale = pool.access(old_id);
    EXPECT_FALSE(static_cast<bool>(stale));
    EXPECT_EQ(ChannelCode::CHANNEL_STALE, stale.code());

    ChannelAccess fresh = pool.access(new_id);
    ASSERT_TRUE(static_cast<bool>(fresh));
    EXPECT_EQ(new_id, fresh.channel().id());
}

TEST(GameChannelPool, GenerationKeepsGrowingAcrossRepeatedReuse)
{
    GameChannelPool pool(1);
    ChannelId       id = createOrFail(pool);

    for (uint32_t cycle = 1; cycle <= 5; ++cycle)
    {
        SCOPED_TRACE(cycle);
        pool.remove(id);
        id = createOrFail(pool);
        EXPECT_EQ(0u, id.index);
        EXPECT_EQ(cycle + 1, id.generation);
    }
}

TEST(GameChannelPool, RemoveWithStaleIdLeavesTheCurrentChannelAlone)
{
    GameChannelPool pool(4);

    const ChannelId old_id = createOrFail(pool);
    pool.remove(old_id);
    const ChannelId new_id = createOrFail(pool);

    pool.remove(old_id);

    EXPECT_EQ(1u, pool.size());
    ChannelAccess access = pool.access(new_id);
    EXPECT_TRUE(static_cast<bool>(access));
}

TEST(GameChannelPool, RemoveWithUnknownIndexDoesNothing)
{
    GameChannelPool pool(4);
    const ChannelId id = createOrFail(pool);

    pool.remove(makeChannelId(4, 1));
    pool.remove(makeChannelId(std::numeric_limits<uint32_t>::max(), 1));
    pool.remove(makeChannelId(2, 1));     // in range but never created

    EXPECT_EQ(1u, pool.size());
    EXPECT_TRUE(static_cast<bool>(pool.access(id)));
}

TEST(GameChannelPool, RemoveTwiceDoesNotBumpTheGenerationTwice)
{
    GameChannelPool pool(1);
    const ChannelId id = createOrFail(pool);

    pool.remove(id);
    pool.remove(id);

    EXPECT_EQ(0u, pool.size());

    // A second remove that went through would leave the slot at generation 3.
    const ChannelId next = createOrFail(pool);
    EXPECT_EQ(2u, next.generation);
}

TEST(GameChannelPool, RemovingOneOfSeveralLeavesTheOthersAccessible)
{
    GameChannelPool pool(4);

    const ChannelId a = createOrFail(pool);
    const ChannelId b = createOrFail(pool);
    const ChannelId c = createOrFail(pool);

    pool.remove(b);

    EXPECT_EQ(2u, pool.size());
    EXPECT_TRUE(static_cast<bool>(pool.access(a)));
    EXPECT_FALSE(static_cast<bool>(pool.access(b)));
    EXPECT_TRUE(static_cast<bool>(pool.access(c)));
}

TEST(GameChannelPool, ReusedSlotHoldsAFreshChannelWithoutOldListeners)
{
    GameChannelPool pool(1);
    RequestListener listener;

    const ChannelId old_id = createOrFail(pool);
    {
        ChannelAccess access = pool.access(old_id);
        ASSERT_TRUE(static_cast<bool>(access));
        ASSERT_TRUE(access.channel().business().request.addListener(listener));
    }

    pool.remove(old_id);
    const ChannelId new_id = createOrFail(pool);

    // The slot's memory is the same, the object is not: a remove that skipped
    // the destructor, or a create that skipped the constructor, would leave
    // the old listener attached and this emit would return true.
    ChannelAccess access = pool.access(new_id);
    ASSERT_TRUE(static_cast<bool>(access));

    GameRequest request;
    EXPECT_FALSE(access.channel().io().request.emit(request));
    EXPECT_EQ(0, listener.calls);
}

// -----------------------------------------------------------------------------
// reentrancy - the recursive mutex
// -----------------------------------------------------------------------------

TEST(GameChannelPool, AccessInsideAnAccessScopeSucceeds)
{
    GameChannelPool pool(4);
    const ChannelId a = createOrFail(pool);
    const ChannelId b = createOrFail(pool);

    ChannelAccess outer = pool.access(a);
    ASSERT_TRUE(static_cast<bool>(outer));

    ChannelAccess inner = pool.access(b);
    EXPECT_TRUE(static_cast<bool>(inner));
    EXPECT_EQ(b, inner.channel().id());
}

TEST(GameChannelPool, CreateInsideAnAccessScopeSucceeds)
{
    GameChannelPool pool(4);
    const ChannelId a = createOrFail(pool);

    ChannelAccess outer = pool.access(a);
    ASSERT_TRUE(static_cast<bool>(outer));

    ChannelResult created = pool.create();
    EXPECT_TRUE(created.isOk());
    EXPECT_EQ(2u, pool.size());
}

TEST(GameChannelPool, RemoveOfAnotherChannelInsideAnAccessScopeSucceeds)
{
    GameChannelPool pool(4);
    const ChannelId a = createOrFail(pool);
    const ChannelId b = createOrFail(pool);

    ChannelAccess outer = pool.access(a);
    ASSERT_TRUE(static_cast<bool>(outer));

    pool.remove(b);
    EXPECT_EQ(1u, pool.size());
    EXPECT_FALSE(static_cast<bool>(pool.access(b)));

    // The outer access is still valid after the pool was mutated under it.
    EXPECT_EQ(a, outer.channel().id());
}

TEST(GameChannelPool, FailedAccessDoesNotKeepThePoolLocked)
{
    GameChannelPool pool(4);

    ChannelAccess failed = pool.access(makeChannelId(9, 1));
    ASSERT_FALSE(static_cast<bool>(failed));

    // On a single thread the recursive mutex cannot tell a held lock from a
    // released one, so this only pins that a failed access is inert: the pool
    // stays fully usable while `failed` is alive. The cross-thread half of the
    // promise belongs to the TSan run, not here.
    const ChannelId id = createOrFail(pool);
    EXPECT_TRUE(static_cast<bool>(pool.access(id)));
}

// -----------------------------------------------------------------------------
// size
// -----------------------------------------------------------------------------

TEST(GameChannelPool, SizeFollowsCreatesAndRemoves)
{
    GameChannelPool pool(4);

    EXPECT_EQ(0u, pool.size());
    const ChannelId a = createOrFail(pool);
    EXPECT_EQ(1u, pool.size());
    const ChannelId b = createOrFail(pool);
    EXPECT_EQ(2u, pool.size());
    pool.remove(a);
    EXPECT_EQ(1u, pool.size());
    pool.remove(b);
    EXPECT_EQ(0u, pool.size());
}

TEST(GameChannelPool, DestroyingAPoolWithLiveChannelsIsSafe)
{
    // Observable only under a sanitizer (a leaked or double-destroyed
    // GameChannel); here it simply must not crash.
    GameChannelPool pool(4);

    createOrFail(pool);
    createOrFail(pool);
}

INSTANTIATE_TEST_SUITE_P(Capacities, Capacity, ::testing::ValuesIn(kCapacities), nameOf);

// -----------------------------------------------------------------------------
// generation numbering
// -----------------------------------------------------------------------------
//
// Generation 0 belongs to the null id, so a pool starts every slot at 1 and
// nothing it hands out is ever null.

TEST(GameChannelPool, FirstChannelOfANewPoolHasGenerationOne)
{
    GameChannelPool pool(4);
    const ChannelId id = createOrFail(pool);

    EXPECT_EQ(0u, id.index);
    EXPECT_EQ(1u, id.generation);
    EXPECT_FALSE(kiotty::isNull(id));
}

TEST(GameChannelPool, NoIssuedIdIsEverNull)
{
    GameChannelPool pool(4);

    for (int cycle = 0; cycle < 3; ++cycle)
    {
        ChannelId ids[4];

        for (size_t i = 0; i < 4; ++i)
        {
            SCOPED_TRACE(i);
            ids[i] = createOrFail(pool);
            EXPECT_FALSE(kiotty::isNull(ids[i]));
        }
        for (size_t i = 0; i < 4; ++i)
        {
            pool.remove(ids[i]);
        }
    }
}

TEST(GameChannelPool, RecreateAfterRemoveHasGenerationTwo)
{
    GameChannelPool pool(1);
    const ChannelId first = createOrFail(pool);
    ASSERT_EQ(1u, first.generation);

    pool.remove(first);
    const ChannelId second = createOrFail(pool);

    EXPECT_EQ(first.index, second.index);
    EXPECT_EQ(2u, second.generation);
}

TEST(GameChannelPool, AccessWithTheNullIdIsNotFound)
{
    GameChannelPool pool(4);
    createOrFail(pool);   // slot 0 is live at generation 1

    ChannelAccess access = pool.access(ChannelId());

    EXPECT_FALSE(static_cast<bool>(access));
}

TEST(GameChannelPool, RemoveWithTheNullIdLeavesTheLiveChannelAlone)
{
    GameChannelPool pool(4);
    const ChannelId id = createOrFail(pool);

    pool.remove(ChannelId());

    EXPECT_EQ(1u, pool.size());
    EXPECT_TRUE(static_cast<bool>(pool.access(id)));
}
