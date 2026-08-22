// ChannelPoolBinder is the default answer to "a connection arrived, which
// channel does it get". It does three things on connect - create, attach the
// one request listener, hand back the io view - and one on disconnect. Each
// of the three is observable from outside: the pool's size, an emit through
// the returned view, and the channel_id the view carries.
//
// ConnectionInfo is accepted and ignored by this implementation; the tests
// pass it anyway so a future binder that reads it is exercised the same way.

#include <domain/channel/kiotty_channel_binder.h>
#include <domain/channel/kiotty_game_channel_pool.h>

#include "support/kiotty_test_channel_listeners.h"
#include "support/kiotty_test_session.h"

#include <gtest/gtest.h>

#include <cstddef>

using kiotty::ChannelAccess;
using kiotty::ChannelCode;
using kiotty::ChannelId;
using kiotty::ChannelPoolBinder;
using kiotty::ConnectionInfo;
using kiotty::GameChannelPool;
using kiotty::GameRequest;
using kiotty::IoChannelResult;
using kiotty::IoGameChannel;
using kiotty::makeConnectionInfo;
using kiotty::SessionRepository;
using kiotty_test::CounterRandom;
using kiotty_test::FixedPolicy;
using kiotty_test::RequestListener;

namespace
{
    const ConnectionInfo kInfo = makeConnectionInfo("127.0.0.1", 40000);
}

// -----------------------------------------------------------------------------
// onConnected
// -----------------------------------------------------------------------------

TEST(ChannelPoolBinder, OnConnectedCreatesAChannelInThePool)
{
    GameChannelPool   pool(4);
    RequestListener   listener;
    CounterRandom     random;
    FixedPolicy       policy;
    SessionRepository sessions(pool, policy, random, 4);
    ChannelPoolBinder binder(pool, sessions, listener);

    IoChannelResult bound = binder.onConnected(kInfo);

    ASSERT_TRUE(bound.isOk());
    EXPECT_EQ(ChannelCode::CHANNEL_SUCCESS, bound.code());
    EXPECT_EQ(1u, pool.size());

    ChannelAccess access = pool.access(bound.value().channel_id);
    EXPECT_TRUE(static_cast<bool>(access));
}

TEST(ChannelPoolBinder, OnConnectedAttachesTheRequestListenerToTheNewChannel)
{
    GameChannelPool   pool(4);
    RequestListener   listener;
    CounterRandom     random;
    FixedPolicy       policy;
    SessionRepository sessions(pool, policy, random, 4);
    ChannelPoolBinder binder(pool, sessions, listener);

    IoChannelResult bound = binder.onConnected(kInfo);
    ASSERT_TRUE(bound.isOk());

    GameRequest request;
    request.command = 3;

    // The io view's request sink is the presentation side; if the listener
    // was attached, the emit is heard and returns true.
    EXPECT_TRUE(bound.value().request.emit(request));
    EXPECT_EQ(1, listener.calls);
    EXPECT_EQ(&request, listener.last);
}

TEST(ChannelPoolBinder, OnConnectedReturnsTheViewOfTheChannelItCreated)
{
    GameChannelPool   pool(4);
    RequestListener   listener;
    CounterRandom     random;
    FixedPolicy       policy;
    SessionRepository sessions(pool, policy, random, 4);
    ChannelPoolBinder binder(pool, sessions, listener);

    IoChannelResult bound = binder.onConnected(kInfo);
    ASSERT_TRUE(bound.isOk());

    const ChannelId id     = bound.value().channel_id;
    ChannelAccess   access = pool.access(id);

    ASSERT_TRUE(static_cast<bool>(access));
    EXPECT_EQ(id, access.channel().id());

    // And the view's streams are that channel's streams: a listener attached
    // through the pool's object hears an emit through the returned view.
    RequestListener second;
    ASSERT_TRUE(access.channel().business().request.addListener(second));

    GameRequest request;
    EXPECT_TRUE(bound.value().request.emit(request));
    EXPECT_EQ(1, second.calls);
}

TEST(ChannelPoolBinder, EachOnConnectedGetsItsOwnChannelWithTheSameListener)
{
    GameChannelPool   pool(4);
    RequestListener   listener;
    CounterRandom     random;
    FixedPolicy       policy;
    SessionRepository sessions(pool, policy, random, 4);
    ChannelPoolBinder binder(pool, sessions, listener);

    IoChannelResult first  = binder.onConnected(kInfo);
    IoChannelResult second = binder.onConnected(kInfo);

    ASSERT_TRUE(first.isOk());
    ASSERT_TRUE(second.isOk());
    EXPECT_NE(first.value().channel_id, second.value().channel_id);
    EXPECT_EQ(2u, pool.size());

    // One dispatcher, many channels: the same listener must hear both.
    GameRequest request;
    EXPECT_TRUE(first.value().request.emit(request));
    EXPECT_TRUE(second.value().request.emit(request));
    EXPECT_EQ(2, listener.calls);
}

TEST(ChannelPoolBinder, OnConnectedOnAFullPoolReturnsPoolExhausted)
{
    GameChannelPool   pool(1);
    RequestListener   listener;
    CounterRandom     random;
    FixedPolicy       policy;
    SessionRepository sessions(pool, policy, random, 4);
    ChannelPoolBinder binder(pool, sessions, listener);

    IoChannelResult first  = binder.onConnected(kInfo);
    IoChannelResult second = binder.onConnected(kInfo);

    ASSERT_TRUE(first.isOk());
    EXPECT_FALSE(second.isOk());
    EXPECT_EQ(ChannelCode::CHANNEL_POOL_EXHAUSTED, second.code());
    EXPECT_EQ(1u, pool.size());
}

TEST(ChannelPoolBinder, OnConnectedOnACapacityZeroPoolReturnsPoolExhausted)
{
    GameChannelPool   pool(0);
    RequestListener   listener;
    CounterRandom     random;
    FixedPolicy       policy;
    SessionRepository sessions(pool, policy, random, 4);
    ChannelPoolBinder binder(pool, sessions, listener);

    IoChannelResult bound = binder.onConnected(kInfo);

    EXPECT_FALSE(bound.isOk());
    EXPECT_EQ(ChannelCode::CHANNEL_POOL_EXHAUSTED, bound.code());
    EXPECT_EQ(0, listener.calls);
}

// -----------------------------------------------------------------------------
// onDisconnected
// -----------------------------------------------------------------------------

TEST(ChannelPoolBinder, OnDisconnectedRemovesTheChannelFromThePool)
{
    GameChannelPool   pool(4);
    RequestListener   listener;
    CounterRandom     random;
    FixedPolicy       policy;
    SessionRepository sessions(pool, policy, random, 4);
    ChannelPoolBinder binder(pool, sessions, listener);

    IoChannelResult bound = binder.onConnected(kInfo);
    ASSERT_TRUE(bound.isOk());

    // Copy the id out first: the view refers into the channel, which is gone
    // after the call.
    const ChannelId id = bound.value().channel_id;

    binder.onDisconnected(kInfo, bound.value());

    EXPECT_EQ(0u, pool.size());

    ChannelAccess access = pool.access(id);
    EXPECT_FALSE(static_cast<bool>(access));
    EXPECT_EQ(ChannelCode::CHANNEL_NOT_FOUND, access.code());
}

TEST(ChannelPoolBinder, OnDisconnectedTwiceWithTheSameViewIsHarmless)
{
    GameChannelPool   pool(4);
    RequestListener   listener;
    CounterRandom     random;
    FixedPolicy       policy;
    SessionRepository sessions(pool, policy, random, 4);
    ChannelPoolBinder binder(pool, sessions, listener);

    IoChannelResult bound = binder.onConnected(kInfo);
    ASSERT_TRUE(bound.isOk());

    binder.onDisconnected(kInfo, bound.value());
    binder.onDisconnected(kInfo, bound.value());

    EXPECT_EQ(0u, pool.size());
}

TEST(ChannelPoolBinder, OnDisconnectedLeavesOtherChannelsAlone)
{
    GameChannelPool   pool(4);
    RequestListener   listener;
    CounterRandom     random;
    FixedPolicy       policy;
    SessionRepository sessions(pool, policy, random, 4);
    ChannelPoolBinder binder(pool, sessions, listener);

    IoChannelResult first  = binder.onConnected(kInfo);
    IoChannelResult second = binder.onConnected(kInfo);
    ASSERT_TRUE(first.isOk());
    ASSERT_TRUE(second.isOk());

    const ChannelId kept = second.value().channel_id;

    binder.onDisconnected(kInfo, first.value());

    EXPECT_EQ(1u, pool.size());
    EXPECT_TRUE(static_cast<bool>(pool.access(kept)));

    GameRequest request;
    EXPECT_TRUE(second.value().request.emit(request));
    EXPECT_EQ(1, listener.calls);
}

TEST(ChannelPoolBinder, ReconnectAfterDisconnectReusesTheSlotWithANewGeneration)
{
    GameChannelPool   pool(1);
    RequestListener   listener;
    CounterRandom     random;
    FixedPolicy       policy;
    SessionRepository sessions(pool, policy, random, 4);
    ChannelPoolBinder binder(pool, sessions, listener);

    IoChannelResult first = binder.onConnected(kInfo);
    ASSERT_TRUE(first.isOk());
    const ChannelId old_id = first.value().channel_id;

    binder.onDisconnected(kInfo, first.value());

    IoChannelResult second = binder.onConnected(kInfo);
    ASSERT_TRUE(second.isOk());
    const ChannelId new_id = second.value().channel_id;

    EXPECT_EQ(old_id.index, new_id.index);
    EXPECT_EQ(old_id.generation + 1, new_id.generation);

    ChannelAccess stale = pool.access(old_id);
    EXPECT_FALSE(static_cast<bool>(stale));
    EXPECT_EQ(ChannelCode::CHANNEL_STALE, stale.code());

    // The listener was attached to the new channel, not carried over.
    GameRequest request;
    EXPECT_TRUE(second.value().request.emit(request));
    EXPECT_EQ(1, listener.calls);
}

// -----------------------------------------------------------------------------
// onDisconnected and the session repository
// -----------------------------------------------------------------------------
//
// The binder is the only place that tells the repository a channel went away.
// What happens to the session is the policy's decision, so both answers are
// checked: drop at once, or keep an orphan that a reconnect can pick up.

using kiotty_test::accountOf;
using kiotty::SessionCode;
using kiotty::SessionResult;

TEST(ChannelPoolBinder, OnDisconnectedWithZeroOrphanLifetimeDropsTheSession)
{
    GameChannelPool   pool(4);
    RequestListener   listener;
    CounterRandom     random;
    FixedPolicy       policy;
    SessionRepository sessions(pool, policy, random, 4);
    ChannelPoolBinder binder(pool, sessions, listener);

    policy.orphan_lifetime_ms = 0;

    IoChannelResult bound = binder.onConnected(kInfo);
    ASSERT_TRUE(bound.isOk());
    const ChannelId id = bound.value().channel_id;

    SessionResult opened = sessions.open(id, accountOf("alice"));
    ASSERT_TRUE(opened.isOk());
    ASSERT_EQ(1u, sessions.size());

    binder.onDisconnected(kInfo, bound.value());

    EXPECT_EQ(0u, sessions.size());
    EXPECT_EQ(SessionCode::SESSION_NOT_FOUND, sessions.find(id).code());

    IoChannelResult next = binder.onConnected(kInfo);
    ASSERT_TRUE(next.isOk());
    EXPECT_EQ(SessionCode::SESSION_UNAVAILABLE,
              sessions.rebind(next.value().channel_id, opened.value().token()).code());
}

TEST(ChannelPoolBinder, OnDisconnectedWithAnOrphanLifetimeKeepsTheSessionForRebind)
{
    GameChannelPool   pool(4);
    RequestListener   listener;
    CounterRandom     random;
    FixedPolicy       policy;
    SessionRepository sessions(pool, policy, random, 4);
    ChannelPoolBinder binder(pool, sessions, listener);

    policy.orphan_lifetime_ms = 1000;

    IoChannelResult bound = binder.onConnected(kInfo);
    ASSERT_TRUE(bound.isOk());
    const ChannelId old_id = bound.value().channel_id;

    SessionResult opened = sessions.open(old_id, accountOf("alice"));
    ASSERT_TRUE(opened.isOk());

    binder.onDisconnected(kInfo, bound.value());

    // Orphaned: still counted, not reachable through the dead channel.
    EXPECT_EQ(1u, sessions.size());
    EXPECT_EQ(SessionCode::SESSION_NOT_FOUND, sessions.find(old_id).code());

    IoChannelResult next = binder.onConnected(kInfo);
    ASSERT_TRUE(next.isOk());
    const ChannelId new_id = next.value().channel_id;

    SessionResult rebound = sessions.rebind(new_id, opened.value().token());
    ASSERT_TRUE(rebound.isOk());
    EXPECT_EQ(new_id, rebound.value().channel());
    EXPECT_EQ(accountOf("alice"), rebound.value().account());
    EXPECT_TRUE(sessions.find(new_id).isOk());
    EXPECT_EQ(1u, sessions.size());
}

namespace
{
    // A policy that, from inside the callback, looks at the session it is
    // being asked about: what channel does it claim, is that channel still in
    // the pool, and can it still send. The repository must not be holding its
    // lock while this runs, or the first question deadlocks.
    class ProbingPolicy : public kiotty::ISessionPolicy
    {
    public:
        explicit ProbingPolicy(GameChannelPool& pool) :
            _pool(pool),
            calls(0),
            channel_seen(),
            channel_was_live(false),
            reply_result(false),
            rebind_to(),
            rebind_target(nullptr),
            rebind_ok(false)
        {
        }

        uint32_t orphanLifetimeMs(const kiotty::Session& session) const override
        {
            ++calls;
            channel_seen     = session.channel();
            channel_was_live = static_cast<bool>(_pool.access(channel_seen));

            kiotty::Session copy = session;
            reply_result = copy.reply(1, 1, kiotty::Bytes());

            if (rebind_target != nullptr)
            {
                rebind_ok = rebind_target->rebind(rebind_to, session.token()).isOk();
            }
            return 1000;
        }

        bool replacesPreviousLogin(const kiotty::AccountId&) const override { return true; }

        GameChannelPool&   _pool;
        mutable int        calls;
        mutable ChannelId  channel_seen;
        mutable bool       channel_was_live;
        mutable bool       reply_result;
        ChannelId          rebind_to;
        SessionRepository* rebind_target;
        mutable bool       rebind_ok;
    };
}

TEST(ChannelPoolBinder, OnDisconnectedDetachesBeforeRemovingSoThePolicySeesALiveChannel)
{
    GameChannelPool   pool(4);
    RequestListener   listener;
    CounterRandom     random;
    ProbingPolicy     policy(pool);
    SessionRepository sessions(pool, policy, random, 4);
    ChannelPoolBinder binder(pool, sessions, listener);

    IoChannelResult bound = binder.onConnected(kInfo);
    ASSERT_TRUE(bound.isOk());
    const ChannelId id = bound.value().channel_id;
    ASSERT_TRUE(sessions.open(id, accountOf("alice")).isOk());

    binder.onDisconnected(kInfo, bound.value());

    ASSERT_EQ(1, policy.calls);
    EXPECT_EQ(id, policy.channel_seen);
    EXPECT_TRUE(policy.channel_was_live);
    // The binder clears the io listeners before detaching, so the emit has
    // nobody to reach and reply() reports false; the channel being live is
    // the observation that matters here.
    EXPECT_FALSE(policy.reply_result);

    EXPECT_EQ(0u, pool.size());
    EXPECT_EQ(1u, sessions.size());   // lifetime 1000: left as an orphan
    EXPECT_EQ(SessionCode::SESSION_NOT_FOUND, sessions.find(id).code());
}

TEST(ChannelPoolBinder, RebindDuringThePolicyCallbackWinsOverTheDetach)
{
    // The session moves to another channel while the policy is deciding; the
    // detach that started this must not orphan it from its new channel.
    GameChannelPool   pool(4);
    RequestListener   listener;
    CounterRandom     random;
    ProbingPolicy     policy(pool);
    SessionRepository sessions(pool, policy, random, 4);
    ChannelPoolBinder binder(pool, sessions, listener);

    IoChannelResult first  = binder.onConnected(kInfo);
    IoChannelResult second = binder.onConnected(kInfo);
    ASSERT_TRUE(first.isOk());
    ASSERT_TRUE(second.isOk());
    const ChannelId old_id = first.value().channel_id;
    const ChannelId new_id = second.value().channel_id;

    kiotty::SessionResult opened = sessions.open(old_id, accountOf("alice"));
    ASSERT_TRUE(opened.isOk());

    policy.rebind_target = &sessions;
    policy.rebind_to     = new_id;

    binder.onDisconnected(kInfo, first.value());

    ASSERT_EQ(1, policy.calls);
    EXPECT_TRUE(policy.rebind_ok);

    // Still attached, on the new channel, and not counted as an orphan.
    EXPECT_EQ(1u, sessions.size());
    kiotty::SessionResult found = sessions.find(new_id);
    ASSERT_TRUE(found.isOk());
    EXPECT_EQ(opened.value().token(), found.value().token());
    EXPECT_EQ(new_id, opened.value().channel());
    EXPECT_EQ(SessionCode::SESSION_NOT_FOUND, sessions.find(old_id).code());

    // No orphan clock was started: a far-future sweep leaves it alone.
    sessions.sweep(1000000);
    EXPECT_EQ(1u, sessions.size());
    EXPECT_TRUE(sessions.find(new_id).isOk());
}

TEST(ChannelPoolBinder, OnDisconnectedOfAChannelWithoutASessionLeavesOtherSessionsAlone)
{
    GameChannelPool   pool(4);
    RequestListener   listener;
    CounterRandom     random;
    FixedPolicy       policy;
    SessionRepository sessions(pool, policy, random, 4);
    ChannelPoolBinder binder(pool, sessions, listener);

    IoChannelResult first  = binder.onConnected(kInfo);
    IoChannelResult second = binder.onConnected(kInfo);
    ASSERT_TRUE(first.isOk());
    ASSERT_TRUE(second.isOk());

    const ChannelId kept = second.value().channel_id;
    ASSERT_TRUE(sessions.open(kept, accountOf("alice")).isOk());

    binder.onDisconnected(kInfo, first.value());

    EXPECT_EQ(1u, sessions.size());
    EXPECT_TRUE(sessions.find(kept).isOk());
}
