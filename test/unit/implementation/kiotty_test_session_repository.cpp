// SessionRepository is a fixed table of slots and six operations on it. Every
// operation has a precedence of checks (is the channel taken, does the
// account already have a session, is there room, did the random source
// deliver) and the tables below walk the product of those conditions so the
// precedence is pinned, not just the happy path.
//
// Nothing here needs a clock: sweep takes the time as an argument, so the
// orphan lifetime is walked by passing 0, lifetime-1 and lifetime.
//
// Session, the value handed out, is tested at the end: it holds a channel id
// and resolves it through the pool on every reply/notify, which is what makes
// a recycled channel safe to hold a stale Session for.

#include <datalayer/repository/session/kiotty_session.h>
#include <datalayer/repository/session/kiotty_session_repository.h>
#include <domain/channel/kiotty_game_channel_pool.h>
#include <domain/entity/kiotty_account_id.h>
#include <domain/entity/kiotty_session_code.h>
#include <domain/entity/kiotty_session_token.h>

#include "support/kiotty_test_pools.h"
#include "support/kiotty_test_session.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>

using kiotty::AccountId;
using kiotty::Bytes;
using kiotty::ChannelAccess;
using kiotty::ChannelId;
using kiotty::ChannelResult;
using kiotty::GameChannelPool;
using kiotty::GameEvent;
using kiotty::GameResponse;
using kiotty_test::accountOf;
using kiotty::makeChannelId;
using kiotty::SESSION_TOKEN_SIZE;
using kiotty::Session;
using kiotty::SessionCode;
using kiotty::SessionRepository;
using kiotty::SessionResult;
using kiotty::SessionToken;
using kiotty::StreamListener;
using kiotty_test::CounterRandom;
using kiotty_test::FixedPolicy;
using kiotty_test::SmallPool;

static_assert(static_cast<int32_t>(SessionCode::SESSION_SUCCESS) == 0,
              "SessionCode::SESSION_SUCCESS must be 0 for Result to read it as ok");

namespace
{
    // A pool with a few live channels, a repository over it, and the doubles
    // it talks to. capacity is the session limit, not the channel count.
    class RepositoryFixture
    {
    public:
        explicit RepositoryFixture(size_t capacity, size_t channels = 5) :
            pool(channels),
            random(),
            policy(),
            sessions(pool, policy, random, capacity)
        {
            for (size_t i = 0; i < channels && i < 5; ++i)
            {
                ChannelResult created = pool.create();
                ids[i] = created.isOk() ? created.value().id() : ChannelId();
            }
        }

        ChannelId a() const { return ids[0]; }
        ChannelId b() const { return ids[1]; }
        ChannelId c() const { return ids[2]; }
        ChannelId d() const { return ids[3]; }
        ChannelId e() const { return ids[4]; }

        GameChannelPool   pool;
        CounterRandom     random;
        FixedPolicy       policy;
        SessionRepository sessions;
        ChannelId         ids[5];
    };

    const AccountId kAlice = accountOf("alice");
    const AccountId kBob   = accountOf("bob");

    SessionToken predictedToken(uint32_t first)
    {
        SessionToken token;

        for (size_t i = 0; i < SESSION_TOKEN_SIZE; ++i)
        {
            token.bytes[i] = static_cast<uint8_t>(first + i);
        }
        return token;
    }
}

// -----------------------------------------------------------------------------
// construction
// -----------------------------------------------------------------------------

TEST(SessionRepository, ZeroCapacityIsNotUsableAndOpensNothing)
{
    RepositoryFixture f(0);

    EXPECT_FALSE(static_cast<bool>(f.sessions));
    EXPECT_EQ(0u, f.sessions.capacity());
    EXPECT_EQ(0u, f.sessions.size());

    SessionResult opened = f.sessions.open(f.a(), kAlice);

    EXPECT_FALSE(opened.isOk());
    EXPECT_EQ(SessionCode::SESSION_TOO_MANY, opened.code());
    EXPECT_EQ(SessionCode::SESSION_NOT_FOUND, f.sessions.find(f.a()).code());
    EXPECT_EQ(SessionCode::SESSION_UNAVAILABLE, f.sessions.rebind(f.a(), SessionToken()).code());

    // These must not touch slots that were never allocated.
    f.sessions.detach(f.a());
    f.sessions.sweep(1000);
    f.sessions.close(Session());
}

TEST(SessionRepository, CapacityOneIsUsableAndEmpty)
{
    RepositoryFixture f(1);

    EXPECT_TRUE(static_cast<bool>(f.sessions));
    EXPECT_EQ(1u, f.sessions.capacity());
    EXPECT_EQ(0u, f.sessions.size());
}

// -----------------------------------------------------------------------------
// open: the precedence table
// -----------------------------------------------------------------------------

namespace
{
    struct OpenCase
    {
        bool        channel_taken;    // the target channel already has a session
        bool        account_exists;   // the account is logged in on another channel
        bool        replaces;         // policy.replacesPreviousLogin
        bool        full;             // no free slot before the call
        bool        random_fails;
        SessionCode expect;
        const char* name;
    };

    // channel_taken (2) x account_exists (2) x replaces (2) x full (2) x
    // random_fails (2) = 32. The checks run in that order, so the first
    // condition that is true names the code - with one twist: replacing a
    // previous login frees its slot, so "full" no longer blocks.
    const OpenCase kOpenCases[] =
    {
        { true,  false, false, false, false, SessionCode::SESSION_ALREADY_AUTHENTICATED, "Taken" },
        { true,  false, false, false, true,  SessionCode::SESSION_ALREADY_AUTHENTICATED, "TakenRandomFails" },
        { true,  false, false, true,  false, SessionCode::SESSION_ALREADY_AUTHENTICATED, "TakenFull" },
        { true,  false, false, true,  true,  SessionCode::SESSION_ALREADY_AUTHENTICATED, "TakenFullRandomFails" },
        { true,  false, true,  false, false, SessionCode::SESSION_ALREADY_AUTHENTICATED, "TakenReplaces" },
        { true,  false, true,  false, true,  SessionCode::SESSION_ALREADY_AUTHENTICATED, "TakenReplacesRandomFails" },
        { true,  false, true,  true,  false, SessionCode::SESSION_ALREADY_AUTHENTICATED, "TakenReplacesFull" },
        { true,  false, true,  true,  true,  SessionCode::SESSION_ALREADY_AUTHENTICATED, "TakenReplacesFullRandomFails" },
        { true,  true,  false, false, false, SessionCode::SESSION_ALREADY_AUTHENTICATED, "TakenExists" },
        { true,  true,  false, false, true,  SessionCode::SESSION_ALREADY_AUTHENTICATED, "TakenExistsRandomFails" },
        { true,  true,  false, true,  false, SessionCode::SESSION_ALREADY_AUTHENTICATED, "TakenExistsFull" },
        { true,  true,  false, true,  true,  SessionCode::SESSION_ALREADY_AUTHENTICATED, "TakenExistsFullRandomFails" },
        { true,  true,  true,  false, false, SessionCode::SESSION_ALREADY_AUTHENTICATED, "TakenExistsReplaces" },
        { true,  true,  true,  false, true,  SessionCode::SESSION_ALREADY_AUTHENTICATED, "TakenExistsReplacesRandomFails" },
        { true,  true,  true,  true,  false, SessionCode::SESSION_ALREADY_AUTHENTICATED, "TakenExistsReplacesFull" },
        { true,  true,  true,  true,  true,  SessionCode::SESSION_ALREADY_AUTHENTICATED, "TakenExistsReplacesFullRandomFails" },

        { false, false, false, false, false, SessionCode::SESSION_SUCCESS,           "Fresh" },
        { false, false, false, false, true,  SessionCode::SESSION_RANDOM_UNAVAILABLE, "FreshRandomFails" },
        { false, false, false, true,  false, SessionCode::SESSION_TOO_MANY,          "FreshFull" },
        { false, false, false, true,  true,  SessionCode::SESSION_TOO_MANY,          "FreshFullRandomFails" },
        { false, false, true,  false, false, SessionCode::SESSION_SUCCESS,           "FreshReplaces" },
        { false, false, true,  false, true,  SessionCode::SESSION_RANDOM_UNAVAILABLE, "FreshReplacesRandomFails" },
        { false, false, true,  true,  false, SessionCode::SESSION_TOO_MANY,          "FreshReplacesFull" },
        { false, false, true,  true,  true,  SessionCode::SESSION_TOO_MANY,          "FreshReplacesFullRandomFails" },

        { false, true,  false, false, false, SessionCode::SESSION_LOGIN_REJECTED,    "ExistsRejected" },
        { false, true,  false, false, true,  SessionCode::SESSION_LOGIN_REJECTED,    "ExistsRejectedRandomFails" },
        { false, true,  false, true,  false, SessionCode::SESSION_LOGIN_REJECTED,    "ExistsRejectedFull" },
        { false, true,  false, true,  true,  SessionCode::SESSION_LOGIN_REJECTED,    "ExistsRejectedFullRandomFails" },
        { false, true,  true,  false, false, SessionCode::SESSION_SUCCESS,           "ExistsReplaced" },
        { false, true,  true,  false, true,  SessionCode::SESSION_RANDOM_UNAVAILABLE, "ExistsReplacedRandomFails" },
        { false, true,  true,  true,  false, SessionCode::SESSION_SUCCESS,           "ExistsReplacedFull" },
        { false, true,  true,  true,  true,  SessionCode::SESSION_RANDOM_UNAVAILABLE, "ExistsReplacedFullRandomFails" },
    };

    std::string openNameOf(const ::testing::TestParamInfo<OpenCase>& info)
    {
        return info.param.name;
    }

    class SessionOpen : public ::testing::TestWithParam<OpenCase>
    {
    };
}

TEST_P(SessionOpen, ReturnsTheFirstFailingCheckInPrecedenceOrder)
{
    const OpenCase& c = GetParam();

    // Three slots: one for the channel's own session, one for the account's
    // earlier login, the rest are fillers when "full" is asked for.
    RepositoryFixture f(3);
    f.policy.replaces = c.replaces;

    if (c.channel_taken)
    {
        ASSERT_TRUE(f.sessions.open(f.a(), accountOf("occupant")).isOk());
    }
    if (c.account_exists)
    {
        ASSERT_TRUE(f.sessions.open(f.b(), kAlice).isOk());
    }
    if (c.full)
    {
        const ChannelId fillers[] = { f.c(), f.d(), f.e() };
        const char*     names[]   = { "filler-c", "filler-d", "filler-e" };

        for (size_t i = 0; f.sessions.size() < f.sessions.capacity(); ++i)
        {
            ASSERT_LT(i, 3u);
            ASSERT_TRUE(f.sessions.open(fillers[i], accountOf(names[i])).isOk());
        }
    }

    const size_t size_before = f.sessions.size();
    f.random.fails = c.random_fails;

    SessionResult opened = f.sessions.open(f.a(), kAlice);

    EXPECT_EQ(c.expect, opened.code());
    EXPECT_EQ(c.expect == SessionCode::SESSION_SUCCESS, opened.isOk());

    if (opened.isOk())
    {
        EXPECT_EQ(f.a(), opened.value().channel());
        EXPECT_EQ(kAlice, opened.value().account());
        EXPECT_TRUE(f.sessions.find(f.a()).isOk());

        // Replacing moves the account, so size does not grow; fresh grows it.
        EXPECT_EQ(c.account_exists ? size_before : size_before + 1, f.sessions.size());
    }
    else if (c.expect == SessionCode::SESSION_ALREADY_AUTHENTICATED)
    {
        // The occupant keeps the channel.
        SessionResult kept = f.sessions.find(f.a());
        ASSERT_TRUE(kept.isOk());
        EXPECT_EQ(accountOf("occupant"), kept.value().account());
        EXPECT_EQ(size_before, f.sessions.size());
    }
    else
    {
        EXPECT_FALSE(f.sessions.find(f.a()).isOk());
    }

    if (c.account_exists && !c.channel_taken)
    {
        // The earlier login survives a rejection and a random-source failure
        // (nothing is released until the new token is in hand); it dies only
        // when the replacement actually goes through.
        const bool replaced = c.replaces && !c.random_fails;
        EXPECT_EQ(!replaced, f.sessions.find(f.b()).isOk());

        if (c.random_fails)
        {
            EXPECT_EQ(size_before, f.sessions.size());
        }
    }
}

INSTANTIATE_TEST_SUITE_P(AllConditions, SessionOpen, ::testing::ValuesIn(kOpenCases), openNameOf);

// -----------------------------------------------------------------------------
// open: what a fresh session carries
// -----------------------------------------------------------------------------

TEST(SessionRepository, OpenFillsTheTokenFromTheRandomSource)
{
    RepositoryFixture f(2);

    SessionResult opened = f.sessions.open(f.a(), kAlice);

    ASSERT_TRUE(opened.isOk());
    EXPECT_EQ(predictedToken(1), opened.value().token());
    EXPECT_EQ(1u + SESSION_TOKEN_SIZE, f.random.next);
    EXPECT_TRUE(static_cast<bool>(opened.value()));
    EXPECT_EQ(1u, f.sessions.size());
}

TEST(SessionRepository, TwoOpensGiveTwoDifferentTokens)
{
    RepositoryFixture f(2);

    SessionResult first  = f.sessions.open(f.a(), kAlice);
    SessionResult second = f.sessions.open(f.b(), kBob);

    ASSERT_TRUE(first.isOk());
    ASSERT_TRUE(second.isOk());
    EXPECT_NE(first.value().token(), second.value().token());
    EXPECT_EQ(2u, f.sessions.size());
}

TEST(SessionRepository, FailedOpenDoesNotConsumeASlot)
{
    RepositoryFixture f(1);

    f.random.fails = true;
    EXPECT_EQ(SessionCode::SESSION_RANDOM_UNAVAILABLE, f.sessions.open(f.a(), kAlice).code());
    EXPECT_EQ(0u, f.sessions.size());

    f.random.fails = false;
    EXPECT_TRUE(f.sessions.open(f.a(), kAlice).isOk());
    EXPECT_EQ(1u, f.sessions.size());
}

TEST(SessionRepository, ReplacedLoginInvalidatesTheOldToken)
{
    RepositoryFixture f(2);
    f.policy.replaces = true;

    SessionResult old = f.sessions.open(f.a(), kAlice);
    ASSERT_TRUE(old.isOk());

    SessionResult replacement = f.sessions.open(f.b(), kAlice);
    ASSERT_TRUE(replacement.isOk());

    EXPECT_NE(old.value().token(), replacement.value().token());
    EXPECT_EQ(SessionCode::SESSION_UNAVAILABLE, f.sessions.rebind(f.c(), old.value().token()).code());
    EXPECT_EQ(SessionCode::SESSION_NOT_FOUND, f.sessions.find(f.a()).code());
    EXPECT_EQ(1u, f.sessions.size());
}

TEST(SessionRepository, ReplacePolicyAlsoCoversAnOrphanedSession)
{
    // An orphan still holds the account; a new login for it follows the same
    // policy as a live one.
    RepositoryFixture f(2);
    f.policy.orphan_lifetime_ms = 1000;

    ASSERT_TRUE(f.sessions.open(f.a(), kAlice).isOk());
    f.sessions.detach(f.a());
    ASSERT_EQ(1u, f.sessions.size());

    f.policy.replaces = false;
    EXPECT_EQ(SessionCode::SESSION_LOGIN_REJECTED, f.sessions.open(f.b(), kAlice).code());

    f.policy.replaces = true;
    EXPECT_TRUE(f.sessions.open(f.b(), kAlice).isOk());
    EXPECT_EQ(1u, f.sessions.size());
}

// -----------------------------------------------------------------------------
// find
// -----------------------------------------------------------------------------

TEST(SessionRepository, FindReturnsTheSessionOpenedOnThatChannel)
{
    RepositoryFixture f(2);

    SessionResult opened = f.sessions.open(f.a(), kAlice);
    ASSERT_TRUE(opened.isOk());

    SessionResult found = f.sessions.find(f.a());

    ASSERT_TRUE(found.isOk());
    EXPECT_EQ(f.a(), found.value().channel());
    EXPECT_EQ(kAlice, found.value().account());
    EXPECT_EQ(opened.value().token(), found.value().token());
}

TEST(SessionRepository, FindOnAChannelWithoutASessionIsNotFound)
{
    RepositoryFixture f(2);

    ASSERT_TRUE(f.sessions.open(f.a(), kAlice).isOk());

    EXPECT_EQ(SessionCode::SESSION_NOT_FOUND, f.sessions.find(f.b()).code());
    // Same slot index, other generation: a recycled channel is not the old one.
    EXPECT_EQ(SessionCode::SESSION_NOT_FOUND,
              f.sessions.find(makeChannelId(f.a().index, f.a().generation + 1)).code());
    // Out of the pool altogether.
    EXPECT_EQ(SessionCode::SESSION_NOT_FOUND, f.sessions.find(makeChannelId(99, 0)).code());
}

// -----------------------------------------------------------------------------
// rebind: the precedence table
// -----------------------------------------------------------------------------

namespace
{
    enum class Token
    {
        Attached,   // a live session on another channel
        Orphan,     // a detached session
        Unknown,    // never issued
        Zero,       // the default token
        Closed,     // issued, then closed
    };

    struct RebindCase
    {
        bool        channel_taken;
        Token       token;
        SessionCode expect;
        const char* name;
    };

    // channel_taken (2) x token (5) = 10 rows.
    const RebindCase kRebindCases[] =
    {
        { true,  Token::Attached, SessionCode::SESSION_ALREADY_AUTHENTICATED, "TakenAttached" },
        { true,  Token::Orphan,   SessionCode::SESSION_ALREADY_AUTHENTICATED, "TakenOrphan" },
        { true,  Token::Unknown,  SessionCode::SESSION_ALREADY_AUTHENTICATED, "TakenUnknown" },
        { true,  Token::Zero,     SessionCode::SESSION_ALREADY_AUTHENTICATED, "TakenZero" },
        { true,  Token::Closed,   SessionCode::SESSION_ALREADY_AUTHENTICATED, "TakenClosed" },
        { false, Token::Attached, SessionCode::SESSION_SUCCESS,               "FreeAttached" },
        { false, Token::Orphan,   SessionCode::SESSION_SUCCESS,               "FreeOrphan" },
        { false, Token::Unknown,  SessionCode::SESSION_UNAVAILABLE,           "FreeUnknown" },
        { false, Token::Zero,     SessionCode::SESSION_UNAVAILABLE,           "FreeZero" },
        { false, Token::Closed,   SessionCode::SESSION_UNAVAILABLE,           "FreeClosed" },
    };

    std::string rebindNameOf(const ::testing::TestParamInfo<RebindCase>& info)
    {
        return info.param.name;
    }

    class SessionRebind : public ::testing::TestWithParam<RebindCase>
    {
    };
}

TEST_P(SessionRebind, MovesTheSessionOnlyForAKnownTokenOntoAFreeChannel)
{
    const RebindCase& c = GetParam();

    RepositoryFixture f(3);
    f.policy.orphan_lifetime_ms = 1000;   // so detach leaves an orphan

    if (c.channel_taken)
    {
        ASSERT_TRUE(f.sessions.open(f.a(), accountOf("occupant")).isOk());
    }

    // Alice logs in on channel b; the row decides what happens to her next.
    SessionResult alice = f.sessions.open(f.b(), kAlice);
    ASSERT_TRUE(alice.isOk());

    SessionToken presented = alice.value().token();

    switch (c.token)
    {
    case Token::Attached:
        break;
    case Token::Orphan:
        f.sessions.detach(f.b());
        break;
    case Token::Unknown:
        presented = predictedToken(200);
        break;
    case Token::Zero:
        presented = SessionToken();
        break;
    case Token::Closed:
        f.sessions.close(alice.value());
        break;
    }

    const size_t size_before = f.sessions.size();

    SessionResult rebound = f.sessions.rebind(f.a(), presented);

    EXPECT_EQ(c.expect, rebound.code());
    EXPECT_EQ(size_before, f.sessions.size());

    if (rebound.isOk())
    {
        EXPECT_EQ(f.a(), rebound.value().channel());
        EXPECT_EQ(kAlice, rebound.value().account());
        EXPECT_EQ(alice.value().token(), rebound.value().token());

        SessionResult on_a = f.sessions.find(f.a());
        ASSERT_TRUE(on_a.isOk());
        EXPECT_EQ(kAlice, on_a.value().account());
        // The old channel no longer resolves.
        EXPECT_EQ(SessionCode::SESSION_NOT_FOUND, f.sessions.find(f.b()).code());
    }
    else if (c.channel_taken)
    {
        SessionResult kept = f.sessions.find(f.a());
        ASSERT_TRUE(kept.isOk());
        EXPECT_EQ(accountOf("occupant"), kept.value().account());
        // Alice's session is where the row left it: still on b unless the
        // row detached or closed it.
        const bool still_on_b = c.token != Token::Orphan && c.token != Token::Closed;
        EXPECT_EQ(still_on_b, f.sessions.find(f.b()).isOk());
    }
    else
    {
        EXPECT_EQ(SessionCode::SESSION_NOT_FOUND, f.sessions.find(f.a()).code());
    }
}

INSTANTIATE_TEST_SUITE_P(AllConditions, SessionRebind, ::testing::ValuesIn(kRebindCases), rebindNameOf);

TEST(SessionRepository, RebindOntoTheSameChannelIsAlreadyAuthenticated)
{
    RepositoryFixture f(2);

    SessionResult opened = f.sessions.open(f.a(), kAlice);
    ASSERT_TRUE(opened.isOk());

    EXPECT_EQ(SessionCode::SESSION_ALREADY_AUTHENTICATED,
              f.sessions.rebind(f.a(), opened.value().token()).code());
}

TEST(SessionRepository, RebindTwiceFollowsTheSessionAcrossChannels)
{
    RepositoryFixture f(2);

    SessionResult opened = f.sessions.open(f.a(), kAlice);
    ASSERT_TRUE(opened.isOk());
    const SessionToken token = opened.value().token();

    ASSERT_TRUE(f.sessions.rebind(f.b(), token).isOk());
    ASSERT_TRUE(f.sessions.rebind(f.c(), token).isOk());

    EXPECT_EQ(SessionCode::SESSION_NOT_FOUND, f.sessions.find(f.a()).code());
    EXPECT_EQ(SessionCode::SESSION_NOT_FOUND, f.sessions.find(f.b()).code());
    EXPECT_TRUE(f.sessions.find(f.c()).isOk());
    EXPECT_EQ(1u, f.sessions.size());
}

TEST(SessionRepository, RebindDoesNotTouchTheRandomSource)
{
    RepositoryFixture f(2);

    SessionResult opened = f.sessions.open(f.a(), kAlice);
    ASSERT_TRUE(opened.isOk());

    f.random.fails = true;

    EXPECT_TRUE(f.sessions.rebind(f.b(), opened.value().token()).isOk());
}

// -----------------------------------------------------------------------------
// close
// -----------------------------------------------------------------------------

TEST(SessionRepository, CloseRemovesTheSessionImmediately)
{
    RepositoryFixture f(2);

    SessionResult opened = f.sessions.open(f.a(), kAlice);
    ASSERT_TRUE(opened.isOk());
    ASSERT_EQ(1u, f.sessions.size());

    f.sessions.close(opened.value());

    EXPECT_EQ(0u, f.sessions.size());
    EXPECT_EQ(SessionCode::SESSION_NOT_FOUND, f.sessions.find(f.a()).code());
    EXPECT_EQ(SessionCode::SESSION_UNAVAILABLE, f.sessions.rebind(f.b(), opened.value().token()).code());
}

TEST(SessionRepository, CloseTwiceIsHarmless)
{
    RepositoryFixture f(2);

    SessionResult opened = f.sessions.open(f.a(), kAlice);
    ASSERT_TRUE(opened.isOk());

    f.sessions.close(opened.value());
    f.sessions.close(opened.value());

    EXPECT_EQ(0u, f.sessions.size());
}

TEST(SessionRepository, CloseOfAnUnknownOrDefaultSessionChangesNothing)
{
    RepositoryFixture f(2);

    ASSERT_TRUE(f.sessions.open(f.a(), kAlice).isOk());

    f.sessions.close(Session());
    f.sessions.close(Session(f.sessions, kBob, predictedToken(200)));

    EXPECT_EQ(1u, f.sessions.size());
    EXPECT_TRUE(f.sessions.find(f.a()).isOk());
}

TEST(SessionRepository, CloseFindsTheSessionByTokenNotByChannel)
{
    // A Session copy taken before a rebind still names the old channel; close
    // must still remove the session because the token is what identifies it.
    RepositoryFixture f(2);

    SessionResult opened = f.sessions.open(f.a(), kAlice);
    ASSERT_TRUE(opened.isOk());
    ASSERT_TRUE(f.sessions.rebind(f.b(), opened.value().token()).isOk());

    f.sessions.close(opened.value());

    EXPECT_EQ(0u, f.sessions.size());
    EXPECT_EQ(SessionCode::SESSION_NOT_FOUND, f.sessions.find(f.b()).code());
}

TEST(SessionRepository, CloseRemovesAnOrphanToo)
{
    RepositoryFixture f(2);
    f.policy.orphan_lifetime_ms = 1000;

    SessionResult opened = f.sessions.open(f.a(), kAlice);
    ASSERT_TRUE(opened.isOk());

    f.sessions.detach(f.a());
    ASSERT_EQ(1u, f.sessions.size());

    f.sessions.close(opened.value());

    EXPECT_EQ(0u, f.sessions.size());
    EXPECT_EQ(SessionCode::SESSION_UNAVAILABLE, f.sessions.rebind(f.b(), opened.value().token()).code());
}

TEST(SessionRepository, ClosedSlotCanBeOpenedAgain)
{
    RepositoryFixture f(1);

    SessionResult first = f.sessions.open(f.a(), kAlice);
    ASSERT_TRUE(first.isOk());
    EXPECT_EQ(SessionCode::SESSION_TOO_MANY, f.sessions.open(f.b(), kBob).code());

    f.sessions.close(first.value());

    SessionResult second = f.sessions.open(f.b(), kBob);
    ASSERT_TRUE(second.isOk());
    EXPECT_NE(first.value().token(), second.value().token());
    EXPECT_EQ(1u, f.sessions.size());
}

// -----------------------------------------------------------------------------
// detach
// -----------------------------------------------------------------------------

namespace
{
    struct DetachCase
    {
        uint32_t    lifetime_ms;
        bool        expect_removed;
        const char* name;
    };

    // orphan lifetime: 0 (drop at once) / 1 (smallest orphan) / large.
    const DetachCase kDetachCases[] =
    {
        { 0,          true,  "ZeroLifetimeRemovesAtOnce" },
        { 1,          false, "OneMillisecondLeavesAnOrphan" },
        { 0xFFFFFFFFu, false, "MaxLifetimeLeavesAnOrphan" },
    };

    std::string detachNameOf(const ::testing::TestParamInfo<DetachCase>& info)
    {
        return info.param.name;
    }

    class SessionDetach : public ::testing::TestWithParam<DetachCase>
    {
    };
}

TEST_P(SessionDetach, RemovesOrOrphansAccordingToThePolicy)
{
    const DetachCase& c = GetParam();

    RepositoryFixture f(2);
    f.policy.orphan_lifetime_ms = c.lifetime_ms;

    SessionResult opened = f.sessions.open(f.a(), kAlice);
    ASSERT_TRUE(opened.isOk());

    f.sessions.detach(f.a());

    // Either way the channel no longer resolves.
    EXPECT_EQ(SessionCode::SESSION_NOT_FOUND, f.sessions.find(f.a()).code());
    EXPECT_EQ(c.expect_removed ? 0u : 1u, f.sessions.size());

    SessionResult rebound = f.sessions.rebind(f.b(), opened.value().token());

    EXPECT_EQ(c.expect_removed ? SessionCode::SESSION_UNAVAILABLE : SessionCode::SESSION_SUCCESS,
              rebound.code());
}

INSTANTIATE_TEST_SUITE_P(AllLifetimes, SessionDetach, ::testing::ValuesIn(kDetachCases), detachNameOf);

TEST(SessionRepository, DetachOfAChannelWithoutASessionChangesNothing)
{
    RepositoryFixture f(2);

    ASSERT_TRUE(f.sessions.open(f.a(), kAlice).isOk());

    f.sessions.detach(f.b());
    f.sessions.detach(makeChannelId(99, 0));

    EXPECT_EQ(1u, f.sessions.size());
    EXPECT_TRUE(f.sessions.find(f.a()).isOk());
}

TEST(SessionRepository, DetachTwiceIsHarmless)
{
    RepositoryFixture f(2);
    f.policy.orphan_lifetime_ms = 1000;

    ASSERT_TRUE(f.sessions.open(f.a(), kAlice).isOk());

    f.sessions.detach(f.a());
    f.sessions.detach(f.a());

    EXPECT_EQ(1u, f.sessions.size());
}

TEST(SessionRepository, DetachedChannelCanBeOpenedByAnotherAccount)
{
    RepositoryFixture f(2);
    f.policy.orphan_lifetime_ms = 1000;

    ASSERT_TRUE(f.sessions.open(f.a(), kAlice).isOk());
    f.sessions.detach(f.a());

    EXPECT_TRUE(f.sessions.open(f.a(), kBob).isOk());
    EXPECT_EQ(2u, f.sessions.size());
}

TEST(SessionRepository, DetachAsksThePolicyWithTheDetachedSession)
{
    // A policy that keys the lifetime on the account: records what it saw.
    class RecordingPolicy : public kiotty::ISessionPolicy
    {
    public:
        RecordingPolicy() : seen(), calls(0) {}

        uint32_t orphanLifetimeMs(const Session& session) const override
        {
            seen = session.account();
            ++calls;
            return 0;
        }

        bool replacesPreviousLogin(const AccountId&) const override { return true; }

        mutable AccountId seen;
        mutable int       calls;
    };

    GameChannelPool   pool(2);
    CounterRandom     random;
    RecordingPolicy   policy;
    SessionRepository sessions(pool, policy, random, 2);

    ChannelResult created = pool.create();
    ASSERT_TRUE(created.isOk());
    const ChannelId id = created.value().id();

    ASSERT_TRUE(sessions.open(id, kAlice).isOk());
    sessions.detach(id);

    EXPECT_EQ(1, policy.calls);
    EXPECT_EQ(kAlice, policy.seen);
}

// -----------------------------------------------------------------------------
// sweep
// -----------------------------------------------------------------------------

namespace
{
    const uint32_t kLifetime = 100;

    struct SweepCase
    {
        uint64_t    first_sweep_ms;   // stamps the detach time
        uint64_t    second_sweep_ms;  // the one that may expire it
        bool        expect_removed;
        const char* name;
    };

    // The detach time is whatever the first sweep after detach says; expiry is
    // measured from there. Rows: second sweep below / at / above the boundary,
    // with a zero and a non-zero first stamp, plus no time passing at all.
    const SweepCase kSweepCases[] =
    {
        { 0,   0,                 false, "NoTimePasses" },
        { 0,   kLifetime - 1,     false, "OneBelowFromZero" },
        { 0,   kLifetime,         true,  "ExactlyLifetimeFromZero" },
        { 0,   kLifetime + 1,     true,  "OneAboveFromZero" },
        { 50,  50 + kLifetime - 1, false, "OneBelowFromFifty" },
        { 50,  50 + kLifetime,    true,  "ExactlyLifetimeFromFifty" },
        { 50,  1000000,           true,  "FarBeyond" },
        { 50,  kLifetime,         false, "LifetimeMeasuredFromStampNotFromZero" },
    };

    std::string sweepNameOf(const ::testing::TestParamInfo<SweepCase>& info)
    {
        return info.param.name;
    }

    class SessionSweep : public ::testing::TestWithParam<SweepCase>
    {
    };
}

TEST_P(SessionSweep, RemovesAnOrphanOnceLifetimeHasPassedSinceTheFirstSweepAfterDetach)
{
    const SweepCase& c = GetParam();

    RepositoryFixture f(2);
    f.policy.orphan_lifetime_ms = kLifetime;

    SessionResult opened = f.sessions.open(f.a(), kAlice);
    ASSERT_TRUE(opened.isOk());

    f.sessions.detach(f.a());
    f.sessions.sweep(c.first_sweep_ms);
    ASSERT_EQ(1u, f.sessions.size()) << "the stamping sweep must not remove";

    f.sessions.sweep(c.second_sweep_ms);

    EXPECT_EQ(c.expect_removed ? 0u : 1u, f.sessions.size());
    EXPECT_EQ(c.expect_removed ? SessionCode::SESSION_UNAVAILABLE : SessionCode::SESSION_SUCCESS,
              f.sessions.rebind(f.b(), opened.value().token()).code());
}

INSTANTIATE_TEST_SUITE_P(AllTimings, SessionSweep, ::testing::ValuesIn(kSweepCases), sweepNameOf);

TEST(SessionRepository, SweepLeavesAttachedSessionsAlone)
{
    RepositoryFixture f(2);
    f.policy.orphan_lifetime_ms = kLifetime;

    ASSERT_TRUE(f.sessions.open(f.a(), kAlice).isOk());

    f.sessions.sweep(0);
    f.sessions.sweep(1000000);

    EXPECT_EQ(1u, f.sessions.size());
    EXPECT_TRUE(f.sessions.find(f.a()).isOk());
}

TEST(SessionRepository, SweepOnAnEmptyRepositoryIsHarmless)
{
    RepositoryFixture f(2);

    f.sessions.sweep(0);
    f.sessions.sweep(1000000);

    EXPECT_EQ(0u, f.sessions.size());
}

TEST(SessionRepository, SweepBeforeDetachDoesNotStartTheClock)
{
    RepositoryFixture f(2);
    f.policy.orphan_lifetime_ms = kLifetime;

    ASSERT_TRUE(f.sessions.open(f.a(), kAlice).isOk());

    f.sessions.sweep(0);
    f.sessions.detach(f.a());
    // Had the clock started at 0 this would expire it; it only stamps.
    f.sessions.sweep(kLifetime * 10);
    EXPECT_EQ(1u, f.sessions.size());

    f.sessions.sweep(kLifetime * 10 + kLifetime);
    EXPECT_EQ(0u, f.sessions.size());
}

TEST(SessionRepository, RebindBeforeExpiryStopsTheClock)
{
    RepositoryFixture f(2);
    f.policy.orphan_lifetime_ms = kLifetime;

    SessionResult opened = f.sessions.open(f.a(), kAlice);
    ASSERT_TRUE(opened.isOk());

    f.sessions.detach(f.a());
    f.sessions.sweep(0);
    ASSERT_TRUE(f.sessions.rebind(f.b(), opened.value().token()).isOk());

    f.sessions.sweep(1000000);

    EXPECT_EQ(1u, f.sessions.size());
    EXPECT_TRUE(f.sessions.find(f.b()).isOk());
}

TEST(SessionRepository, DetachAfterRebindRestartsTheClock)
{
    RepositoryFixture f(2);
    f.policy.orphan_lifetime_ms = kLifetime;

    SessionResult opened = f.sessions.open(f.a(), kAlice);
    ASSERT_TRUE(opened.isOk());

    f.sessions.detach(f.a());
    f.sessions.sweep(0);
    ASSERT_TRUE(f.sessions.rebind(f.b(), opened.value().token()).isOk());

    f.sessions.detach(f.b());
    f.sessions.sweep(500);              // new stamp
    f.sessions.sweep(500 + kLifetime - 1);
    EXPECT_EQ(1u, f.sessions.size());

    f.sessions.sweep(500 + kLifetime);
    EXPECT_EQ(0u, f.sessions.size());
}

TEST(SessionRepository, EachOrphanExpiresOnItsOwnClock)
{
    RepositoryFixture f(3);
    f.policy.orphan_lifetime_ms = kLifetime;

    SessionResult alice = f.sessions.open(f.a(), kAlice);
    SessionResult bob   = f.sessions.open(f.b(), kBob);
    ASSERT_TRUE(alice.isOk());
    ASSERT_TRUE(bob.isOk());

    f.sessions.detach(f.a());
    f.sessions.sweep(0);                 // alice stamped at 0
    f.sessions.detach(f.b());
    f.sessions.sweep(50);                // bob stamped at 50

    f.sessions.sweep(kLifetime);         // alice expires, bob has 50 left
    EXPECT_EQ(1u, f.sessions.size());
    EXPECT_EQ(SessionCode::SESSION_UNAVAILABLE, f.sessions.rebind(f.c(), alice.value().token()).code());

    f.sessions.sweep(50 + kLifetime);
    EXPECT_EQ(0u, f.sessions.size());
}

TEST(SessionRepository, OrphanLifetimeIsFrozenAtDetachTime)
{
    // The policy is consulted once, on detach. A later change to the policy
    // does not reach an orphan that already exists.
    RepositoryFixture f(2);
    f.policy.orphan_lifetime_ms = kLifetime;

    ASSERT_TRUE(f.sessions.open(f.a(), kAlice).isOk());
    f.sessions.detach(f.a());
    f.sessions.sweep(0);

    f.policy.orphan_lifetime_ms = 1;
    f.sessions.sweep(kLifetime - 1);
    EXPECT_EQ(1u, f.sessions.size());

    f.sessions.sweep(kLifetime);
    EXPECT_EQ(0u, f.sessions.size());
}

// -----------------------------------------------------------------------------
// Session: the value
// -----------------------------------------------------------------------------

namespace
{
    // Copies the fields out: the payload is moved into the emitted struct and
    // gone when emit returns.
    class ResponseRecorder : public StreamListener<GameResponse>
    {
    public:
        ResponseRecorder() : calls(0), correlation_id(0), command(0), payload_size(0) {}

        void onStream(const GameResponse& response) override
        {
            ++calls;
            correlation_id = response.correlation_id;
            command        = response.command;
            payload_size   = response.payload.size();
        }

        int      calls;
        uint32_t correlation_id;
        uint16_t command;
        size_t   payload_size;
    };

    class EventRecorder : public StreamListener<GameEvent>
    {
    public:
        EventRecorder() : calls(0), command(0), payload_size(0) {}

        void onStream(const GameEvent& event) override
        {
            ++calls;
            command      = event.command;
            payload_size = event.payload.size();
        }

        int      calls;
        uint16_t command;
        size_t   payload_size;
    };

    bool listenOn(GameChannelPool& pool, ChannelId id, ResponseRecorder& responses, EventRecorder& events)
    {
        ChannelAccess access = pool.access(id);

        if (!access)
        {
            return false;
        }
        return access.channel().io().response.addListener(responses) &&
               access.channel().io().event.addListener(events);
    }
}

TEST(Session, DefaultSessionIsFalseAndEmitsNothing)
{
    Session session;

    EXPECT_FALSE(static_cast<bool>(session));
    EXPECT_FALSE(session.reply(1, 2, Bytes()));
    EXPECT_FALSE(session.notify(2, Bytes()));
    EXPECT_TRUE(kiotty::isNull(session.channel()));
    EXPECT_EQ(SessionToken(), session.token());
    EXPECT_EQ(AccountId(), session.account());
}

TEST(Session, ReplyReachesTheIoSideOfItsChannel)
{
    RepositoryFixture f(2);
    SmallPool         bytes(64, 2);
    ResponseRecorder  responses;
    EventRecorder     events;

    ASSERT_TRUE(listenOn(f.pool, f.a(), responses, events));

    SessionResult opened = f.sessions.open(f.a(), kAlice);
    ASSERT_TRUE(opened.isOk());

    EXPECT_TRUE(opened.value().reply(42, 7, Bytes(bytes.pool(), 5)));

    ASSERT_EQ(1, responses.calls);
    EXPECT_EQ(42u, responses.correlation_id);
    EXPECT_EQ(7, responses.command);
    EXPECT_EQ(5u, responses.payload_size);
    EXPECT_EQ(0, events.calls);
}

TEST(Session, NotifyReachesTheIoSideOfItsChannel)
{
    RepositoryFixture f(2);
    SmallPool         bytes(64, 2);
    ResponseRecorder  responses;
    EventRecorder     events;

    ASSERT_TRUE(listenOn(f.pool, f.a(), responses, events));

    SessionResult opened = f.sessions.open(f.a(), kAlice);
    ASSERT_TRUE(opened.isOk());

    EXPECT_TRUE(opened.value().notify(9, Bytes(bytes.pool(), 3)));

    ASSERT_EQ(1, events.calls);
    EXPECT_EQ(9, events.command);
    EXPECT_EQ(3u, events.payload_size);
    EXPECT_EQ(0, responses.calls);
}

TEST(Session, ReplyAndNotifyWithAnEmptyPayloadStillEmit)
{
    RepositoryFixture f(2);
    ResponseRecorder  responses;
    EventRecorder     events;

    ASSERT_TRUE(listenOn(f.pool, f.a(), responses, events));

    SessionResult opened = f.sessions.open(f.a(), kAlice);
    ASSERT_TRUE(opened.isOk());

    EXPECT_TRUE(opened.value().reply(1, 1, Bytes()));
    EXPECT_TRUE(opened.value().notify(1, Bytes()));
    EXPECT_EQ(0u, responses.payload_size);
    EXPECT_EQ(0u, events.payload_size);
}

TEST(Session, ReplyWithNobodyListeningReturnsFalse)
{
    RepositoryFixture f(2);

    SessionResult opened = f.sessions.open(f.a(), kAlice);
    ASSERT_TRUE(opened.isOk());

    EXPECT_FALSE(opened.value().reply(1, 1, Bytes()));
    EXPECT_FALSE(opened.value().notify(1, Bytes()));
}

TEST(Session, ReplyAfterTheChannelIsRemovedReturnsFalse)
{
    RepositoryFixture f(2);
    ResponseRecorder  responses;
    EventRecorder     events;

    ASSERT_TRUE(listenOn(f.pool, f.a(), responses, events));

    SessionResult opened = f.sessions.open(f.a(), kAlice);
    ASSERT_TRUE(opened.isOk());

    f.pool.remove(f.a());

    EXPECT_FALSE(opened.value().reply(1, 1, Bytes()));
    EXPECT_FALSE(opened.value().notify(1, Bytes()));
    EXPECT_EQ(0, responses.calls);
    EXPECT_EQ(0, events.calls);
}

TEST(Session, ReplyAfterTheChannelSlotIsRecycledDoesNotReachTheNewOccupant)
{
    // Pool of one: remove then create lands in the same slot with a new
    // generation. The stale Session must not talk to whoever is there now.
    GameChannelPool   pool(1);
    CounterRandom     random;
    FixedPolicy       policy;
    SessionRepository sessions(pool, policy, random, 2);

    ChannelResult first = pool.create();
    ASSERT_TRUE(first.isOk());
    const ChannelId old_id = first.value().id();

    SessionResult opened = sessions.open(old_id, kAlice);
    ASSERT_TRUE(opened.isOk());

    pool.remove(old_id);

    ChannelResult second = pool.create();
    ASSERT_TRUE(second.isOk());
    ASSERT_EQ(old_id.index, second.value().id().index);
    ASSERT_NE(old_id.generation, second.value().id().generation);

    ResponseRecorder responses;
    EventRecorder    events;
    ASSERT_TRUE(listenOn(pool, second.value().id(), responses, events));

    EXPECT_FALSE(opened.value().reply(1, 1, Bytes()));
    EXPECT_FALSE(opened.value().notify(1, Bytes()));
    EXPECT_EQ(0, responses.calls);
    EXPECT_EQ(0, events.calls);
}

TEST(Session, SessionFromRebindTalksToTheNewChannel)
{
    RepositoryFixture f(2);
    ResponseRecorder  on_a;
    EventRecorder     events_a;
    ResponseRecorder  on_b;
    EventRecorder     events_b;

    ASSERT_TRUE(listenOn(f.pool, f.a(), on_a, events_a));
    ASSERT_TRUE(listenOn(f.pool, f.b(), on_b, events_b));

    SessionResult opened = f.sessions.open(f.a(), kAlice);
    ASSERT_TRUE(opened.isOk());

    SessionResult rebound = f.sessions.rebind(f.b(), opened.value().token());
    ASSERT_TRUE(rebound.isOk());

    EXPECT_TRUE(rebound.value().reply(1, 1, Bytes()));
    EXPECT_EQ(0, on_a.calls);
    EXPECT_EQ(1, on_b.calls);

    // The copy from before the rebind is not a snapshot: it looks the channel
    // up by token on every call, so it follows the session to channel b even
    // though channel a is still alive.
    EXPECT_EQ(f.b(), opened.value().channel());
    EXPECT_TRUE(opened.value().reply(2, 2, Bytes()));
    EXPECT_EQ(0, on_a.calls);
    EXPECT_EQ(2, on_b.calls);
}

TEST(Session, ChannelFollowsTheRepository)
{
    RepositoryFixture f(2);

    SessionResult opened = f.sessions.open(f.a(), kAlice);
    ASSERT_TRUE(opened.isOk());
    EXPECT_EQ(f.a(), opened.value().channel());

    ASSERT_TRUE(f.sessions.rebind(f.b(), opened.value().token()).isOk());
    EXPECT_EQ(f.b(), opened.value().channel());
}

TEST(Session, ReplyAfterDetachIsFalseEvenThoughTheOldChannelLives)
{
    RepositoryFixture f(2);
    ResponseRecorder  responses;
    EventRecorder     events;
    f.policy.orphan_lifetime_ms = 1000;

    ASSERT_TRUE(listenOn(f.pool, f.a(), responses, events));

    SessionResult opened = f.sessions.open(f.a(), kAlice);
    ASSERT_TRUE(opened.isOk());

    f.sessions.detach(f.a());
    ASSERT_TRUE(static_cast<bool>(f.pool.access(f.a())));   // the channel itself is untouched

    EXPECT_TRUE(kiotty::isNull(opened.value().channel()));
    EXPECT_FALSE(opened.value().reply(1, 1, Bytes()));
    EXPECT_FALSE(opened.value().notify(1, Bytes()));
    EXPECT_EQ(0, responses.calls);
    EXPECT_EQ(0, events.calls);

    // Rebinding brings it back to life on the new channel.
    ResponseRecorder on_b;
    EventRecorder    events_b;
    ASSERT_TRUE(listenOn(f.pool, f.b(), on_b, events_b));
    ASSERT_TRUE(f.sessions.rebind(f.b(), opened.value().token()).isOk());

    EXPECT_EQ(f.b(), opened.value().channel());
    EXPECT_TRUE(opened.value().reply(1, 1, Bytes()));
    EXPECT_EQ(1, on_b.calls);
    EXPECT_EQ(0, responses.calls);
}

TEST(Session, ReplyAfterCloseIsFalseEvenThoughTheChannelLives)
{
    RepositoryFixture f(2);
    ResponseRecorder  responses;
    EventRecorder     events;

    ASSERT_TRUE(listenOn(f.pool, f.a(), responses, events));

    SessionResult opened = f.sessions.open(f.a(), kAlice);
    ASSERT_TRUE(opened.isOk());

    f.sessions.close(opened.value());

    EXPECT_TRUE(kiotty::isNull(opened.value().channel()));
    EXPECT_FALSE(opened.value().reply(1, 1, Bytes()));
    EXPECT_FALSE(opened.value().notify(1, Bytes()));
    EXPECT_EQ(0, responses.calls);
    EXPECT_EQ(0, events.calls);
}

// -----------------------------------------------------------------------------
// channelOf
// -----------------------------------------------------------------------------

TEST(SessionRepository, ChannelOfAnAttachedTokenIsItsChannel)
{
    RepositoryFixture f(2);

    SessionResult opened = f.sessions.open(f.a(), kAlice);
    ASSERT_TRUE(opened.isOk());

    EXPECT_EQ(f.a(), f.sessions.channelOf(opened.value().token()));

    ASSERT_TRUE(f.sessions.rebind(f.b(), opened.value().token()).isOk());
    EXPECT_EQ(f.b(), f.sessions.channelOf(opened.value().token()));
}

TEST(SessionRepository, ChannelOfAnOrphanIsNull)
{
    RepositoryFixture f(2);
    f.policy.orphan_lifetime_ms = 1000;

    SessionResult opened = f.sessions.open(f.a(), kAlice);
    ASSERT_TRUE(opened.isOk());

    f.sessions.detach(f.a());

    EXPECT_TRUE(kiotty::isNull(f.sessions.channelOf(opened.value().token())));
}

TEST(SessionRepository, ChannelOfAnUnknownOrClosedTokenIsNull)
{
    RepositoryFixture f(2);

    EXPECT_TRUE(kiotty::isNull(f.sessions.channelOf(predictedToken(200))));
    EXPECT_TRUE(kiotty::isNull(f.sessions.channelOf(SessionToken())));

    SessionResult opened = f.sessions.open(f.a(), kAlice);
    ASSERT_TRUE(opened.isOk());
    f.sessions.close(opened.value());

    EXPECT_TRUE(kiotty::isNull(f.sessions.channelOf(opened.value().token())));
}
