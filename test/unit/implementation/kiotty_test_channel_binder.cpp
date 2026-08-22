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
    ChannelPoolBinder binder(pool, listener);

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
    ChannelPoolBinder binder(pool, listener);

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
    ChannelPoolBinder binder(pool, listener);

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
    ChannelPoolBinder binder(pool, listener);

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
    ChannelPoolBinder binder(pool, listener);

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
    ChannelPoolBinder binder(pool, listener);

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
    ChannelPoolBinder binder(pool, listener);

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
    ChannelPoolBinder binder(pool, listener);

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
    ChannelPoolBinder binder(pool, listener);

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
    ChannelPoolBinder binder(pool, listener);

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
