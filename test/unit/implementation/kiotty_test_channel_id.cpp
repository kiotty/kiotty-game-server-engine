// ChannelId is two integers and an equality, and the equality is the whole
// reason it is two integers: a recycled slot index with a new generation must
// not compare equal to the id that used to live there. So the table walks all
// four ways two ids can agree or differ on each field.

#include <domain/entity/kiotty_channel_id.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>

using kiotty::ChannelCode;
using kiotty::ChannelId;
using kiotty::makeChannelId;

// Result reads code() == 0 as success, so the enum has to put success at zero
// and nothing else there. Said at compile time: a renumbered enum should not
// wait for a test run to be noticed.
static_assert(static_cast<int32_t>(ChannelCode::CHANNEL_SUCCESS) == 0,
              "ChannelCode::CHANNEL_SUCCESS must be 0 for Result to read it as ok");
static_assert(static_cast<int32_t>(ChannelCode::CHANNEL_POOL_EXHAUSTED) != 0 &&
              static_cast<int32_t>(ChannelCode::CHANNEL_NOT_FOUND) != 0 &&
              static_cast<int32_t>(ChannelCode::CHANNEL_STALE) != 0,
              "every failure code must be non-zero");
static_assert(ChannelCode::CHANNEL_POOL_EXHAUSTED != ChannelCode::CHANNEL_NOT_FOUND &&
              ChannelCode::CHANNEL_NOT_FOUND != ChannelCode::CHANNEL_STALE &&
              ChannelCode::CHANNEL_POOL_EXHAUSTED != ChannelCode::CHANNEL_STALE,
              "failure codes must be distinguishable");

namespace
{
    struct EqualityCase
    {
        ChannelId   lhs;
        ChannelId   rhs;
        bool        expect_equal;
        const char* name;
    };

    const uint32_t kMax = std::numeric_limits<uint32_t>::max();

    // index x generation, each either same or different: 2 x 2. Only the
    // both-same corner is equal. The last rows push both fields to the limits
    // so a comparison that truncated to 16 bits would still be caught.
    const EqualityCase kEqualityCases[] =
    {
        { makeChannelId(0, 0),    makeChannelId(0, 0),    true,  "SameIndexSameGeneration" },
        { makeChannelId(3, 5),    makeChannelId(3, 5),    true,  "SameIndexSameGenerationNonZero" },
        { makeChannelId(3, 5),    makeChannelId(3, 6),    false, "SameIndexDifferentGeneration" },
        { makeChannelId(3, 5),    makeChannelId(4, 5),    false, "DifferentIndexSameGeneration" },
        { makeChannelId(3, 5),    makeChannelId(4, 6),    false, "DifferentIndexDifferentGeneration" },
        { makeChannelId(kMax, kMax), makeChannelId(kMax, kMax), true,  "BothAtMax" },
        { makeChannelId(kMax, 0), makeChannelId(0, kMax), false, "FieldsSwapped" },
        { makeChannelId(0x10000, 0), makeChannelId(0, 0), false, "IndexDiffersOnlyAboveSixteenBits" },
    };

    class Equality : public ::testing::TestWithParam<EqualityCase>
    {
    };

    std::string nameOf(const ::testing::TestParamInfo<EqualityCase>& info)
    {
        return info.param.name;
    }
}

TEST(ChannelId, DefaultConstructedIsIndexZeroGenerationZero)
{
    ChannelId id;

    EXPECT_EQ(0u, id.index);
    EXPECT_EQ(0u, id.generation);
}

TEST(ChannelId, MakeChannelIdStoresBothFieldsInOrder)
{
    const ChannelId id = makeChannelId(7, 11);

    EXPECT_EQ(7u, id.index);
    EXPECT_EQ(11u, id.generation);
}

TEST(ChannelId, MakeChannelIdKeepsEveryBitOfBothFields)
{
    const ChannelId id = makeChannelId(kMax, kMax - 1);

    EXPECT_EQ(kMax, id.index);
    EXPECT_EQ(kMax - 1, id.generation);
}

TEST_P(Equality, EqualsAndNotEqualsAgreeWithEachOther)
{
    const EqualityCase& c = GetParam();

    EXPECT_EQ(c.expect_equal, c.lhs == c.rhs);
    EXPECT_EQ(!c.expect_equal, c.lhs != c.rhs);

    // Equality is symmetric; a comparison that only checked one field in one
    // direction would pass the line above for half the table.
    EXPECT_EQ(c.expect_equal, c.rhs == c.lhs);
    EXPECT_EQ(!c.expect_equal, c.rhs != c.lhs);
}

INSTANTIATE_TEST_SUITE_P(AllFieldCombinations, Equality,
                         ::testing::ValuesIn(kEqualityCases), nameOf);

TEST(ChannelId, EveryIdEqualsItself)
{
    const ChannelId id = makeChannelId(3, 5);

    EXPECT_TRUE(id == id);
    EXPECT_FALSE(id != id);
}
