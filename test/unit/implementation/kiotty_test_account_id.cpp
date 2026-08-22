// AccountId is a fixed 64-byte name behind a maker that refuses what does not
// fit instead of cutting it: a null pointer and a name longer than 63 chars
// are rejected, everything else (including the empty name) is copied and
// terminated. Each row below is one input, whether it is accepted, and what
// the output holds afterwards - the output is reset on every call, so a
// rejected call must not leave the previous name in it either.
//
// SessionToken rides along because it is the other entity introduced with
// sessions and its only logic is "== goes through constantTimeEquals".

#include <domain/entity/kiotty_account_id.h>
#include <domain/entity/kiotty_session_token.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>
#include <string>

using kiotty::ACCOUNT_NAME_MAX_CHARS;
using kiotty::ACCOUNT_NAME_SIZE;
using kiotty::AccountId;
using kiotty::SESSION_TOKEN_SIZE;
using kiotty::SessionToken;
using kiotty::tryMakeAccountId;

static_assert(ACCOUNT_NAME_SIZE == 64, "the tests below assume a 64-byte name buffer");
static_assert(ACCOUNT_NAME_MAX_CHARS == 63, "the tests below assume 63 usable chars");
static_assert(SESSION_TOKEN_SIZE == 32, "the tests below assume a 32-byte token");

namespace
{
    std::string repeated(char c, size_t count)
    {
        return std::string(count, c);
    }

    AccountId accountOf(const char* name)
    {
        AccountId id;
        EXPECT_TRUE(tryMakeAccountId(name, id));
        return id;
    }

    struct MakeCase
    {
        std::string input;        // what is passed in
        bool        pass_null;    // pass nullptr instead of input
        bool        expect_ok;
        const char* name;
    };

    // name: null / empty / ordinary / exactly 63 (fits) / 64 (one too many)
    // / much longer / UTF-8 / whitespace and newline kept as-is / one char.
    const MakeCase kMakeCases[] =
    {
        { "",                  true,  false, "NullIsRejected" },
        { "",                  false, true,  "EmptyIsAccepted" },
        { "player",            false, true,  "OrdinaryNameIsCopied" },
        { repeated('a', 63),   false, true,  "SixtyThreeCharsFitExactly" },
        { repeated('b', 64),   false, false, "SixtyFourCharsAreRejected" },
        { repeated('c', 500),  false, false, "LongNameIsRejected" },
        { "\xEC\x98\xA4\xEB\xB2\x84", false, true, "Utf8BytesAreKeptVerbatim" },
        { " padded \n",        false, true,  "WhitespaceIsNotTrimmed" },
        { "a",                 false, true,  "SingleChar" },
    };

    std::string nameOf(const ::testing::TestParamInfo<MakeCase>& info)
    {
        return info.param.name;
    }

    class MakeAccountId : public ::testing::TestWithParam<MakeCase>
    {
    };
}

TEST_P(MakeAccountId, AcceptsUpToSixtyThreeCharsAndAlwaysTerminates)
{
    const MakeCase& c = GetParam();
    const char* const input = c.pass_null ? nullptr : c.input.c_str();

    // Start from a dirty output so "out is reset" is observable: a rejected
    // call must not leave "stale" behind, and an accepted one must not keep
    // its tail.
    AccountId id;
    ASSERT_TRUE(tryMakeAccountId("stale-name-that-must-not-survive", id));

    EXPECT_EQ(c.expect_ok, tryMakeAccountId(input, id));

    // The terminator is the contract that lets every other reader treat name
    // as a C string; check it before strlen is allowed to run.
    ASSERT_NE(nullptr, std::memchr(id.name, 0, ACCOUNT_NAME_SIZE));
    EXPECT_LE(std::strlen(id.name), ACCOUNT_NAME_MAX_CHARS);

    if (c.expect_ok)
    {
        EXPECT_EQ(c.input, std::string(id.name));

        // Everything after the text is zero, so equality over the whole
        // buffer does not depend on what was there before.
        for (size_t i = c.input.size(); i < ACCOUNT_NAME_SIZE; ++i)
        {
            EXPECT_EQ(0, id.name[i]) << "at index " << i;
        }

        // Making it twice gives the same value.
        AccountId again;
        ASSERT_TRUE(tryMakeAccountId(input, again));
        EXPECT_EQ(id, again);
    }
    else
    {
        // Rejected: whatever is left is not the previous name. The header
        // says nothing about the exact content, so only that is pinned.
        EXPECT_STRNE("stale-name-that-must-not-survive", id.name);
    }
}

INSTANTIATE_TEST_SUITE_P(AllInputs, MakeAccountId, ::testing::ValuesIn(kMakeCases), nameOf);

// -----------------------------------------------------------------------------
// equality
// -----------------------------------------------------------------------------

TEST(AccountId, DefaultConstructedEqualsEmptyName)
{
    const AccountId blank;

    EXPECT_EQ(blank, accountOf(""));
}

TEST(AccountId, DifferentNamesAreNotEqual)
{
    EXPECT_NE(accountOf("alice"), accountOf("bob"));
    EXPECT_NE(accountOf("alice"), accountOf("alic"));
    EXPECT_NE(accountOf("alice"), accountOf(""));
}

TEST(AccountId, CaseIsSignificant)
{
    EXPECT_NE(accountOf("Alice"), accountOf("alice"));
}

TEST(AccountId, NamesThatDifferOnlyInTheLastAllowedCharAreNotEqual)
{
    const std::string base = repeated('x', 62);

    EXPECT_NE(accountOf((base + "1").c_str()), accountOf((base + "2").c_str()));
}

// -----------------------------------------------------------------------------
// SessionToken
// -----------------------------------------------------------------------------

TEST(SessionToken, DefaultIsAllZeroAndEqualsAnotherDefault)
{
    const SessionToken a;
    const SessionToken b;

    for (size_t i = 0; i < SESSION_TOKEN_SIZE; ++i)
    {
        EXPECT_EQ(0u, a.bytes[i]) << "at index " << i;
    }
    EXPECT_EQ(a, b);
}

TEST(SessionToken, ViewAndWritableSpanCoverTheWholeBuffer)
{
    SessionToken token;

    EXPECT_EQ(token.bytes, token.view().data());
    EXPECT_EQ(SESSION_TOKEN_SIZE, token.view().size());
    EXPECT_EQ(token.bytes, token.writableSpan().data());
    EXPECT_EQ(SESSION_TOKEN_SIZE, token.writableSpan().size());
}

TEST(SessionToken, WritingThroughTheSpanChangesTheTokenAndItsEquality)
{
    SessionToken a;
    SessionToken b;

    a.writableSpan().data()[SESSION_TOKEN_SIZE - 1] = 1;

    EXPECT_NE(a, b);

    b.writableSpan().data()[SESSION_TOKEN_SIZE - 1] = 1;

    EXPECT_EQ(a, b);
}

TEST(SessionToken, FirstByteAloneDecidesInequality)
{
    SessionToken a;
    SessionToken b;

    a.bytes[0] = 0x80;

    EXPECT_NE(a, b);
    EXPECT_FALSE(a == b);
    EXPECT_TRUE(a != b);
}
