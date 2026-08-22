// PasswordHasher wraps argon2id behind two calls and a usability flag. What
// the engine relies on is not argon2 itself but the wrapper's edges: which
// parameter sets it refuses, which inputs it refuses, where the salt comes
// from, and that a stored hash survives the trip back through matches.
//
// The parameters are the smallest argon2 accepts (t=1, m=8 KiB, p=1) so the
// suite stays fast; the cost knobs change the digest, not the contract.

#include <datalayer/repository/cryptor/kiotty_password_hasher.h>

#include "support/kiotty_test_session.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

using kiotty::ByteView;
using kiotty::CryptoCode;
using kiotty::HashParameters;
using kiotty::PASSWORD_HASH_ENCODED_SIZE;
using kiotty::PasswordHash;
using kiotty::PasswordHasher;
using kiotty::PasswordHashResult;
using kiotty_test::CounterRandom;

static_assert(static_cast<int32_t>(CryptoCode::CRYPTO_SUCCESS) == 0,
              "CryptoCode::CRYPTO_SUCCESS must be 0 for Result to read it as ok");

namespace
{
    HashParameters fastParameters()
    {
        HashParameters p;
        p.time_cost   = 1;
        p.memory_kib  = 8;
        p.parallelism = 1;
        return p;
    }

    HashParameters parameters(uint32_t t, uint32_t m, uint32_t p)
    {
        HashParameters out;
        out.time_cost   = t;
        out.memory_kib  = m;
        out.parallelism = p;
        return out;
    }

    ByteView text(const char* s)
    {
        return ByteView(reinterpret_cast<const uint8_t*>(s), std::strlen(s));
    }

    ByteView text(const std::string& s)
    {
        return ByteView(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }

    std::string encodedOf(const PasswordHash& hash)
    {
        return std::string(hash.encoded);
    }

    // -------------------------------------------------------------------------
    // usability table
    // -------------------------------------------------------------------------

    struct ParameterCase
    {
        uint32_t    time_cost;
        uint32_t    memory_kib;
        uint32_t    parallelism;
        bool        expect_usable;
        const char* name;
    };

    // time_cost (0 / 1) x parallelism (0 / 1 / 2) x memory relative to
    // 8*parallelism (below / exact / above). Rows with parallelism 0 make
    // the memory bound 0, so "below" does not exist there.
    const ParameterCase kParameterCases[] =
    {
        { 0, 8,  1, false, "TimeZeroIsRejected" },
        { 0, 16, 1, false, "TimeZeroIsRejectedEvenWithMemoryToSpare" },
        { 1, 8,  0, false, "ParallelismZeroIsRejected" },
        { 1, 0,  0, false, "ParallelismZeroAndMemoryZero" },
        { 1, 7,  1, false, "MemoryBelowEightPerLane" },
        { 1, 8,  1, true,  "MemoryExactlyEightPerLane" },
        { 1, 9,  1, true,  "MemoryAboveEightPerLane" },
        { 1, 15, 2, false, "TwoLanesMemoryBelowSixteen" },
        { 1, 16, 2, true,  "TwoLanesMemoryExactlySixteen" },
        { 1, 64, 2, true,  "TwoLanesMemoryAboveSixteen" },
        { 1, 0,  1, false, "MemoryZero" },
        { 2, 8,  1, true,  "TimeAboveOne" },
    };

    std::string parameterNameOf(const ::testing::TestParamInfo<ParameterCase>& info)
    {
        return info.param.name;
    }

    class HashParametersBounds : public ::testing::TestWithParam<ParameterCase>
    {
    };
}

TEST_P(HashParametersBounds, IsUsableOnlyInsideTheArgon2Bounds)
{
    const ParameterCase& c = GetParam();

    CounterRandom  random;
    PasswordHasher hasher(parameters(c.time_cost, c.memory_kib, c.parallelism), random);

    EXPECT_EQ(c.expect_usable, static_cast<bool>(hasher));
    EXPECT_EQ(c.time_cost, hasher.parameters().time_cost);
    EXPECT_EQ(c.memory_kib, hasher.parameters().memory_kib);
    EXPECT_EQ(c.parallelism, hasher.parameters().parallelism);
}

INSTANTIATE_TEST_SUITE_P(AllBounds, HashParametersBounds, ::testing::ValuesIn(kParameterCases), parameterNameOf);

TEST(PasswordHasher, DefaultParametersAreUsable)
{
    CounterRandom  random;
    PasswordHasher hasher(HashParameters(), random);

    EXPECT_TRUE(static_cast<bool>(hasher));
}

TEST(PasswordHasher, UnusableHasherRefusesToHash)
{
    CounterRandom  random;
    PasswordHasher hasher(parameters(0, 8, 1), random);

    ASSERT_FALSE(static_cast<bool>(hasher));

    PasswordHashResult result = hasher.hashBlocking(text("secret"));

    EXPECT_FALSE(result.isOk());
    EXPECT_EQ(CryptoCode::CRYPTO_INVALID_ARGUMENT, result.code());
    // No salt was asked for: the random source was never touched.
    EXPECT_EQ(1u, random.next);
}

// -----------------------------------------------------------------------------
// hashBlocking
// -----------------------------------------------------------------------------

TEST(PasswordHasher, HashOfEmptyPasswordIsInvalidArgument)
{
    CounterRandom  random;
    PasswordHasher hasher(fastParameters(), random);

    PasswordHashResult result = hasher.hashBlocking(ByteView());

    EXPECT_FALSE(result.isOk());
    EXPECT_EQ(CryptoCode::CRYPTO_INVALID_ARGUMENT, result.code());
    EXPECT_EQ(1u, random.next);
}

TEST(PasswordHasher, HashWhenRandomFailsIsRandomUnavailable)
{
    CounterRandom  random;
    PasswordHasher hasher(fastParameters(), random);

    random.fails = true;

    PasswordHashResult result = hasher.hashBlocking(text("secret"));

    EXPECT_FALSE(result.isOk());
    EXPECT_EQ(CryptoCode::CRYPTO_RANDOM_UNAVAILABLE, result.code());
}

TEST(PasswordHasher, HashProducesATerminatedArgon2idString)
{
    CounterRandom  random;
    PasswordHasher hasher(fastParameters(), random);

    PasswordHashResult result = hasher.hashBlocking(text("secret"));

    ASSERT_TRUE(result.isOk());

    const PasswordHash& hash = result.value();

    ASSERT_NE(nullptr, std::memchr(hash.encoded, 0, PASSWORD_HASH_ENCODED_SIZE));
    EXPECT_EQ(0u, encodedOf(hash).find("$argon2id$"));
    // The parameters travel inside the string so matches can verify without
    // being told them.
    EXPECT_NE(std::string::npos, encodedOf(hash).find("m=8,t=1,p=1"));
}

TEST(PasswordHasher, HashDrawsExactlySixteenSaltBytesFromTheRandomSource)
{
    CounterRandom  random;
    PasswordHasher hasher(fastParameters(), random);

    ASSERT_TRUE(hasher.hashBlocking(text("secret")).isOk());

    // CounterRandom advances once per byte, so the distance is the salt size.
    EXPECT_EQ(1u + 16u, random.next);
}

TEST(PasswordHasher, SamePasswordHashedTwiceGivesDifferentEncodings)
{
    CounterRandom  random;
    PasswordHasher hasher(fastParameters(), random);

    PasswordHashResult first  = hasher.hashBlocking(text("secret"));
    PasswordHashResult second = hasher.hashBlocking(text("secret"));

    ASSERT_TRUE(first.isOk());
    ASSERT_TRUE(second.isOk());
    EXPECT_NE(encodedOf(first.value()), encodedOf(second.value()));
}

TEST(PasswordHasher, SameSaltAndPasswordGiveTheSameEncoding)
{
    // Two sources that start at the same counter hand out the same salt, so
    // the output is a function of (password, salt, parameters) and nothing
    // hidden. This is what makes the "different encodings" test above mean
    // "different salt" rather than "non-deterministic".
    CounterRandom  random_a;
    CounterRandom  random_b;
    PasswordHasher hasher_a(fastParameters(), random_a);
    PasswordHasher hasher_b(fastParameters(), random_b);

    PasswordHashResult a = hasher_a.hashBlocking(text("secret"));
    PasswordHashResult b = hasher_b.hashBlocking(text("secret"));

    ASSERT_TRUE(a.isOk());
    ASSERT_TRUE(b.isOk());
    EXPECT_EQ(encodedOf(a.value()), encodedOf(b.value()));
}

// -----------------------------------------------------------------------------
// matchesBlocking
// -----------------------------------------------------------------------------

namespace
{
    struct MatchCase
    {
        const char* hashed;        // what was hashed
        const char* presented;     // what is presented to matches; nullptr = empty
        bool        expect_match;
        const char* name;
    };

    // presented password: same / different / prefix / longer / empty /
    // case-changed / long password (over a typical block).
    const MatchCase kMatchCases[] =
    {
        { "secret",   "secret",   true,  "SamePasswordMatches" },
        { "secret",   "Secret",   false, "CaseDiffersDoesNotMatch" },
        { "secret",   "secre",    false, "PrefixDoesNotMatch" },
        { "secret",   "secret1",  false, "LongerDoesNotMatch" },
        { "secret",   nullptr,    false, "EmptyDoesNotMatch" },
        { "secret",   "other",    false, "DifferentDoesNotMatch" },
        { "a",        "a",        true,  "SingleCharMatches" },
        { "\xEC\x98\xA4\xEB\xB2\x84", "\xEC\x98\xA4\xEB\xB2\x84", true, "Utf8BytesMatch" },
    };

    std::string matchNameOf(const ::testing::TestParamInfo<MatchCase>& info)
    {
        return info.param.name;
    }

    class PasswordMatches : public ::testing::TestWithParam<MatchCase>
    {
    };
}

TEST_P(PasswordMatches, OnlyTheExactPasswordMatchesItsHash)
{
    const MatchCase& c = GetParam();

    CounterRandom  random;
    PasswordHasher hasher(fastParameters(), random);

    PasswordHashResult hashed = hasher.hashBlocking(text(c.hashed));
    ASSERT_TRUE(hashed.isOk());

    const ByteView presented = c.presented == nullptr ? ByteView() : text(c.presented);

    EXPECT_EQ(c.expect_match, hasher.matchesBlocking(presented, hashed.value()));
}

INSTANTIATE_TEST_SUITE_P(AllPresented, PasswordMatches, ::testing::ValuesIn(kMatchCases), matchNameOf);

TEST(PasswordHasher, BothHashesOfTheSamePasswordMatchIt)
{
    CounterRandom  random;
    PasswordHasher hasher(fastParameters(), random);

    PasswordHashResult first  = hasher.hashBlocking(text("secret"));
    PasswordHashResult second = hasher.hashBlocking(text("secret"));

    ASSERT_TRUE(first.isOk());
    ASSERT_TRUE(second.isOk());
    EXPECT_TRUE(hasher.matchesBlocking(text("secret"), first.value()));
    EXPECT_TRUE(hasher.matchesBlocking(text("secret"), second.value()));
}

TEST(PasswordHasher, LongPasswordRoundTrips)
{
    CounterRandom  random;
    PasswordHasher hasher(fastParameters(), random);

    const std::string long_password(1000, 'p');

    PasswordHashResult hashed = hasher.hashBlocking(text(long_password));
    ASSERT_TRUE(hashed.isOk());

    EXPECT_TRUE(hasher.matchesBlocking(text(long_password), hashed.value()));
    EXPECT_FALSE(hasher.matchesBlocking(text(std::string(999, 'p')), hashed.value()));
}

TEST(PasswordHasher, MatchesAgainstAnEmptyEncodingIsFalse)
{
    CounterRandom  random;
    PasswordHasher hasher(fastParameters(), random);

    const PasswordHash blank;

    EXPECT_FALSE(hasher.matchesBlocking(text("secret"), blank));
}

TEST(PasswordHasher, MatchesAgainstAnUnterminatedEncodingIsFalse)
{
    CounterRandom  random;
    PasswordHasher hasher(fastParameters(), random);

    PasswordHash broken;
    std::memset(broken.encoded, 'x', PASSWORD_HASH_ENCODED_SIZE);

    EXPECT_FALSE(hasher.matchesBlocking(text("secret"), broken));
}

TEST(PasswordHasher, MatchesAgainstGarbageTextIsFalse)
{
    CounterRandom  random;
    PasswordHasher hasher(fastParameters(), random);

    PasswordHash garbage;
    const char* const junk = "$argon2id$not-a-real-hash";
    std::memcpy(garbage.encoded, junk, std::strlen(junk) + 1);

    EXPECT_FALSE(hasher.matchesBlocking(text("secret"), garbage));
}

TEST(PasswordHasher, MatchesAgainstATamperedDigestIsFalse)
{
    CounterRandom  random;
    PasswordHasher hasher(fastParameters(), random);

    PasswordHashResult hashed = hasher.hashBlocking(text("secret"));
    ASSERT_TRUE(hashed.isOk());

    PasswordHash tampered = hashed.value();
    const size_t last     = std::strlen(tampered.encoded) - 1;

    // Flip the last digest character to another base64 character.
    tampered.encoded[last] = tampered.encoded[last] == 'A' ? 'B' : 'A';

    EXPECT_FALSE(hasher.matchesBlocking(text("secret"), tampered));
}

TEST(PasswordHasher, MatchesDoesNotNeedTheHasherThatMadeTheHash)
{
    // The parameters live in the encoded string, so a hasher configured
    // differently (or unusable) still verifies. This is what lets the
    // server raise its cost without invalidating stored hashes.
    CounterRandom  random;
    PasswordHasher maker(fastParameters(), random);
    PasswordHasher other(parameters(2, 16, 1), random);
    PasswordHasher unusable(parameters(0, 8, 1), random);

    PasswordHashResult hashed = maker.hashBlocking(text("secret"));
    ASSERT_TRUE(hashed.isOk());

    EXPECT_TRUE(other.matchesBlocking(text("secret"), hashed.value()));
    EXPECT_TRUE(unusable.matchesBlocking(text("secret"), hashed.value()));
}

TEST(PasswordHasher, MatchesDoesNotTouchTheRandomSource)
{
    CounterRandom  random;
    PasswordHasher hasher(fastParameters(), random);

    PasswordHashResult hashed = hasher.hashBlocking(text("secret"));
    ASSERT_TRUE(hashed.isOk());

    const uint32_t before = random.next;
    random.fails = true;

    EXPECT_TRUE(hasher.matchesBlocking(text("secret"), hashed.value()));
    EXPECT_EQ(before, random.next);
}
