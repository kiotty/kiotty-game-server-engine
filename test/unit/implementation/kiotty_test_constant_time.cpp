// constantTimeEquals promises two things: the answer is the plain byte
// comparison, and the time it takes does not depend on where the first
// difference sits. A unit test can only check the first - timing is not
// observable deterministically - so the table concentrates on the shapes a
// timing-safe implementation is most likely to get wrong: a difference only
// in the last byte (an early-exit loop would still catch it, an OR-fold that
// truncated would not), a length mismatch with a common prefix (must be false
// without reading past the shorter one) and two empty views.

#include <core/kiotty_constant_time.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>

using kiotty::ByteView;
using kiotty::constantTimeEquals;

namespace
{
    struct EqualsCase
    {
        const uint8_t* lhs;
        size_t         lhs_size;
        const uint8_t* rhs;
        size_t         rhs_size;
        bool           expect_equal;
        const char*    name;
    };

    const uint8_t kAbc[]        = { 'a', 'b', 'c' };
    const uint8_t kAbcCopy[]    = { 'a', 'b', 'c' };
    const uint8_t kAbd[]        = { 'a', 'b', 'd' };
    const uint8_t kXbc[]        = { 'x', 'b', 'c' };
    const uint8_t kAxc[]        = { 'a', 'x', 'c' };
    const uint8_t kAbcd[]       = { 'a', 'b', 'c', 'd' };
    const uint8_t kZeros[]      = { 0, 0, 0 };
    const uint8_t kHighBit[]    = { 0x80, 0xff, 0x01 };
    const uint8_t kHighBitAlt[] = { 0x80, 0xff, 0x81 };   // differs by the top bit only

    // length relation (same / lhs shorter / rhs shorter / both empty) x where
    // the content differs (nowhere / first / middle / last). Rows that cannot
    // exist (differing content with both empty) are left out.
    const EqualsCase kEqualsCases[] =
    {
        { kAbc,     3, kAbcCopy,    3, true,  "SameBytesDifferentArrays" },
        { kAbc,     3, kAbc,        3, true,  "SameArrayTwice" },
        { kAbc,     3, kXbc,        3, false, "FirstByteDiffers" },
        { kAbc,     3, kAxc,        3, false, "MiddleByteDiffers" },
        { kAbc,     3, kAbd,        3, false, "LastByteDiffers" },
        { kAbc,     3, kAbcd,       4, false, "LhsShorterWithCommonPrefix" },
        { kAbcd,    4, kAbc,        3, false, "RhsShorterWithCommonPrefix" },
        { kAbc,     0, kAbc,        0, true,  "BothEmptyOverSameBuffer" },
        { kAbc,     0, kAbd,        0, true,  "BothEmptyOverDifferentBuffers" },
        { nullptr,  0, nullptr,     0, true,  "BothEmptyWithNullData" },
        { kAbc,     0, kAbc,        1, false, "EmptyAgainstOneByte" },
        { kZeros,   3, kZeros,      3, true,  "AllZeroBytes" },
        { kHighBit, 3, kHighBitAlt, 3, false, "TopBitOnlyDifference" },
        { kAbc,     1, kAbd,        1, true,  "PrefixOfLengthOneEqual" },
        { kAbc,     2, kAbd,        2, true,  "DifferenceBeyondComparedLength" },
    };

    std::string nameOf(const ::testing::TestParamInfo<EqualsCase>& info)
    {
        return info.param.name;
    }

    class ConstantTimeEquals : public ::testing::TestWithParam<EqualsCase>
    {
    };
}

TEST_P(ConstantTimeEquals, ReturnsTrueOnlyWhenLengthAndEveryByteAgree)
{
    const EqualsCase& c = GetParam();

    const ByteView lhs(c.lhs, c.lhs_size);
    const ByteView rhs(c.rhs, c.rhs_size);

    EXPECT_EQ(c.expect_equal, constantTimeEquals(lhs, rhs));
    // Equality has no preferred side.
    EXPECT_EQ(c.expect_equal, constantTimeEquals(rhs, lhs));
}

INSTANTIATE_TEST_SUITE_P(AllShapes, ConstantTimeEquals, ::testing::ValuesIn(kEqualsCases), nameOf);

TEST(ConstantTime, ThirtyTwoByteTokensDifferingInTheLastByteAreUnequal)
{
    // The size a SessionToken has; a fold that lost the last byte of a
    // 32-byte block would pass every shorter row above.
    uint8_t a[32];
    uint8_t b[32];

    for (size_t i = 0; i < 32; ++i)
    {
        a[i] = static_cast<uint8_t>(i);
        b[i] = static_cast<uint8_t>(i);
    }

    EXPECT_TRUE(constantTimeEquals(ByteView(a, 32), ByteView(b, 32)));

    b[31] ^= 0x01;
    EXPECT_FALSE(constantTimeEquals(ByteView(a, 32), ByteView(b, 32)));
}
