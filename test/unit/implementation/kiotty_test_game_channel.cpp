// GameChannel is three streams and two views of them pointing in opposite
// directions. The io side pushes requests and listens for responses and
// events; the business side does the reverse. The tests here ask only one
// thing of each stream: what goes in on one view comes out on the other, and
// both views name the same channel.
//
// Each call to io() and business() builds a fresh view struct, so a second
// question is whether two views are really the same streams underneath and
// not snapshots. That is the "listener added through one view, emit through
// another" case.

#include <domain/channel/kiotty_game_channel.h>

#include "support/kiotty_test_channel_listeners.h"

#include <gtest/gtest.h>

using kiotty::BusinessGameChannel;
using kiotty::ChannelId;
using kiotty::GameChannel;
using kiotty::GameEvent;
using kiotty::GameRequest;
using kiotty::GameResponse;
using kiotty::IoGameChannel;
using kiotty::makeChannelId;
using kiotty_test::EventListener;
using kiotty_test::RequestListener;
using kiotty_test::ResponseListener;

namespace
{
    const ChannelId kId = makeChannelId(3, 7);
}

// -----------------------------------------------------------------------------
// identity
// -----------------------------------------------------------------------------

TEST(GameChannel, IdReturnsTheConstructedChannelId)
{
    GameChannel channel(kId);

    EXPECT_EQ(kId, channel.id());
}

TEST(GameChannel, BothViewsCarryTheSameChannelId)
{
    GameChannel channel(kId);

    IoGameChannel       io       = channel.io();
    BusinessGameChannel business = channel.business();

    EXPECT_EQ(kId, io.channel_id);
    EXPECT_EQ(kId, business.channel_id);
}

// -----------------------------------------------------------------------------
// request: io -> business
// -----------------------------------------------------------------------------

TEST(GameChannel, RequestEmittedOnIoSideReachesBusinessListener)
{
    GameChannel     channel(kId);
    RequestListener listener;

    ASSERT_TRUE(channel.business().request.addListener(listener));

    GameRequest request;
    request.command = 5;

    EXPECT_TRUE(channel.io().request.emit(request));
    EXPECT_EQ(1, listener.calls);
    EXPECT_EQ(&request, listener.last);
}

TEST(GameChannel, RequestEmittedWithNoBusinessListenerReturnsFalse)
{
    GameChannel channel(kId);
    GameRequest request;

    EXPECT_FALSE(channel.io().request.emit(request));
}

// -----------------------------------------------------------------------------
// response: business -> io
// -----------------------------------------------------------------------------

TEST(GameChannel, ResponseEmittedOnBusinessSideReachesIoListener)
{
    GameChannel      channel(kId);
    ResponseListener listener;

    ASSERT_TRUE(channel.io().response.addListener(listener));

    GameResponse response;
    response.correlation_id = 9;

    EXPECT_TRUE(channel.business().response.emit(response));
    EXPECT_EQ(1, listener.calls);
    EXPECT_EQ(&response, listener.last);
}

TEST(GameChannel, ResponseEmittedWithNoIoListenerReturnsFalse)
{
    GameChannel  channel(kId);
    GameResponse response;

    EXPECT_FALSE(channel.business().response.emit(response));
}

// -----------------------------------------------------------------------------
// event: business -> io
// -----------------------------------------------------------------------------

TEST(GameChannel, EventEmittedOnBusinessSideReachesIoListener)
{
    GameChannel   channel(kId);
    EventListener listener;

    ASSERT_TRUE(channel.io().event.addListener(listener));

    GameEvent event;
    event.command = 2;

    EXPECT_TRUE(channel.business().event.emit(event));
    EXPECT_EQ(1, listener.calls);
    EXPECT_EQ(&event, listener.last);
}

TEST(GameChannel, EventEmittedWithNoIoListenerReturnsFalse)
{
    GameChannel channel(kId);
    GameEvent   event;

    EXPECT_FALSE(channel.business().event.emit(event));
}

// -----------------------------------------------------------------------------
// the three streams are separate
// -----------------------------------------------------------------------------

TEST(GameChannel, ResponseAndEventStreamsDoNotCrossTalk)
{
    GameChannel      channel(kId);
    ResponseListener on_response;
    EventListener    on_event;

    ASSERT_TRUE(channel.io().response.addListener(on_response));
    ASSERT_TRUE(channel.io().event.addListener(on_event));

    GameResponse response;
    GameEvent    event;

    EXPECT_TRUE(channel.business().response.emit(response));
    EXPECT_EQ(1, on_response.calls);
    EXPECT_EQ(0, on_event.calls);

    EXPECT_TRUE(channel.business().event.emit(event));
    EXPECT_EQ(1, on_response.calls);
    EXPECT_EQ(1, on_event.calls);
}

// -----------------------------------------------------------------------------
// views alias the streams, they do not copy them
// -----------------------------------------------------------------------------

TEST(GameChannel, ViewsObtainedAtDifferentTimesShareTheSameStreams)
{
    GameChannel      channel(kId);
    ResponseListener listener;

    // Take the business view first, add the listener through a later io view,
    // then emit through the earlier one. If views were snapshots the early
    // one would not know about the listener.
    BusinessGameChannel business = channel.business();
    IoGameChannel       io       = channel.io();

    ASSERT_TRUE(io.response.addListener(listener));

    GameResponse response;
    EXPECT_TRUE(business.response.emit(response));
    EXPECT_EQ(1, listener.calls);
}

TEST(GameChannel, ViewIsCopyConstructible)
{
    GameChannel      channel(kId);
    ResponseListener listener;

    IoGameChannel original = channel.io();
    IoGameChannel copy(original);

    ASSERT_TRUE(copy.response.addListener(listener));

    GameResponse response;
    EXPECT_TRUE(channel.business().response.emit(response));
    EXPECT_EQ(1, listener.calls);
    EXPECT_EQ(original.channel_id, copy.channel_id);
}

TEST(GameChannel, RemovingAListenerThroughOneViewIsSeenByAnother)
{
    GameChannel      channel(kId);
    ResponseListener listener;

    ASSERT_TRUE(channel.io().response.addListener(listener));
    channel.io().response.removeListener(listener);

    GameResponse response;
    EXPECT_FALSE(channel.business().response.emit(response));
    EXPECT_EQ(0, listener.calls);
}
