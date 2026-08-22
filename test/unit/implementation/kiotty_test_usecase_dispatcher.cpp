// UsecaseDispatcher is three gates in a row - is the command registered, is
// the session requirement met, is the channel alive - and one action. The
// gates are independent booleans, so the table is their full product: every
// row says whether dispatch returns true and, just as important, whether the
// usecase ran. A gate that returned false after executing would pass a test
// that only looked at the return value.
//
// The usecases here record what they were given. The channel they are handed
// is proven to be the real one by emitting a response through it and hearing
// it on the io side.

#include <core/kiotty_holder.h>
#include <domain/channel/kiotty_channel_binder.h>
#include <domain/channel/kiotty_game_channel_pool.h>
#include <domain/usecase/kiotty_usecase.h>
#include <domain/usecase/kiotty_usecase_dispatcher.h>
#include <domain/usecase/kiotty_usecase_registry.h>

#include "support/kiotty_test_channel_listeners.h"
#include "support/kiotty_test_session.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using kiotty::BusinessGameChannel;
using kiotty::ChannelAccess;
using kiotty::ChannelId;
using kiotty::ChannelPoolBinder;
using kiotty::GameChannelPool;
using kiotty::GameRequest;
using kiotty::GameResponse;
using kiotty::Holder;
using kiotty::IoChannelResult;
using kiotty::IPublicUsecase;
using kiotty::IUsecase;
using kiotty_test::accountOf;
using kiotty::makeChannelId;
using kiotty::makeConnectionInfo;
using kiotty::UsecaseDispatcher;
using kiotty::SessionRepository;
using kiotty::UsecaseRegistry;
using kiotty_test::CounterRandom;
using kiotty_test::FixedPolicy;
using kiotty::StreamListener;

namespace
{
    // The response a usecase emits lives on its stack and is gone when emit
    // returns, so the io-side listener has to copy the fields there and then.
    // Keeping a pointer, as CountingListener does, would read freed memory.
    class CapturingResponseListener : public StreamListener<GameResponse>
    {
    public:
        CapturingResponseListener() :
            calls(0),
            correlation_id(0),
            command(0)
        {
        }

        void onStream(const GameResponse& response) override
        {
            ++calls;
            correlation_id = response.correlation_id;
            command        = response.command;
        }

        int      calls;
        uint32_t correlation_id;
        uint16_t command;
    };

    const uint16_t COMMAND_SESSION = 10;
    const uint16_t COMMAND_PUBLIC  = 20;
    const uint16_t COMMAND_UNKNOWN = 30;

    // What a usecase saw, kept outside the usecase because the registry owns
    // the usecase objects and the test needs to read after they are moved.
    struct Recorder
    {
        int       runs      = 0;
        ChannelId channel_id;
        uint32_t  correlation_id = 0;
        bool      respond   = false;     // emit a response through the channel
        GameChannelPool* reenter = nullptr;   // access this pool from execute
        bool      reentry_ok = false;
    };

    // Holder limits a usecase to IUsecase::HOLDER_SIZE bytes; a vtable and a
    // pointer is well inside that. Both usecases share one body through
    // this base, the only difference being requiresSession.
    template <typename Base, uint16_t Command>
    class RecordingUsecase : public Base
    {
    public:
        explicit RecordingUsecase(Recorder& recorder) : _recorder(&recorder) {}

        uint16_t command() const override { return Command; }

        void execute(const GameRequest& request, BusinessGameChannel& channel) override
        {
            ++_recorder->runs;
            _recorder->channel_id     = channel.channel_id;
            _recorder->correlation_id = request.correlation_id;

            if (_recorder->respond)
            {
                GameResponse response;
                response.correlation_id = request.correlation_id;
                response.command        = Command;
                channel.response.emit(response);
            }

            if (_recorder->reenter != nullptr)
            {
                ChannelAccess again = _recorder->reenter->access(request.channel_id);
                _recorder->reentry_ok = static_cast<bool>(again);
            }
        }

    private:
        Recorder* _recorder;
    };

    typedef RecordingUsecase<IUsecase, COMMAND_SESSION>      SessionUsecase;
    typedef RecordingUsecase<IPublicUsecase, COMMAND_PUBLIC> PublicUsecase;

    static_assert(sizeof(SessionUsecase) <= IUsecase::HOLDER_SIZE,
                  "the test usecase must fit the registry's Holder");

    std::vector<Holder<IUsecase> > makeUsecases(Recorder& session, Recorder& pub)
    {
        std::vector<Holder<IUsecase> > usecases;

        usecases.push_back(Holder<IUsecase>::make<SessionUsecase>(session));
        usecases.push_back(Holder<IUsecase>::make<PublicUsecase>(pub));
        return usecases;
    }

    enum class Channel
    {
        Live,
        Stale,
        NotFound,
    };

    struct DispatchCase
    {
        uint16_t    command;
        bool        authenticated;
        Channel     channel;
        bool        expect_dispatched;
        const char* name;
    };

    // command (session / public / unknown) x authenticated (2) x channel
    // (live / stale / not found) = 18 rows. Dispatched only when the command
    // is known, the session gate is satisfied and the channel is live.
    const DispatchCase kDispatchCases[] =
    {
        { COMMAND_SESSION, true,  Channel::Live,     true,  "SessionAuthLive" },
        { COMMAND_SESSION, true,  Channel::Stale,    false, "SessionAuthStale" },
        { COMMAND_SESSION, true,  Channel::NotFound, false, "SessionAuthNotFound" },
        { COMMAND_SESSION, false, Channel::Live,     false, "SessionUnauthLive" },
        { COMMAND_SESSION, false, Channel::Stale,    false, "SessionUnauthStale" },
        { COMMAND_SESSION, false, Channel::NotFound, false, "SessionUnauthNotFound" },
        { COMMAND_PUBLIC,  true,  Channel::Live,     true,  "PublicAuthLive" },
        { COMMAND_PUBLIC,  true,  Channel::Stale,    false, "PublicAuthStale" },
        { COMMAND_PUBLIC,  true,  Channel::NotFound, false, "PublicAuthNotFound" },
        { COMMAND_PUBLIC,  false, Channel::Live,     true,  "PublicUnauthLive" },
        { COMMAND_PUBLIC,  false, Channel::Stale,    false, "PublicUnauthStale" },
        { COMMAND_PUBLIC,  false, Channel::NotFound, false, "PublicUnauthNotFound" },
        { COMMAND_UNKNOWN, true,  Channel::Live,     false, "UnknownAuthLive" },
        { COMMAND_UNKNOWN, true,  Channel::Stale,    false, "UnknownAuthStale" },
        { COMMAND_UNKNOWN, true,  Channel::NotFound, false, "UnknownAuthNotFound" },
        { COMMAND_UNKNOWN, false, Channel::Live,     false, "UnknownUnauthLive" },
        { COMMAND_UNKNOWN, false, Channel::Stale,    false, "UnknownUnauthStale" },
        { COMMAND_UNKNOWN, false, Channel::NotFound, false, "UnknownUnauthNotFound" },
    };

    std::string nameOf(const ::testing::TestParamInfo<DispatchCase>& info)
    {
        return info.param.name;
    }

    // One pool with one live channel; the fixture turns the Channel enum into
    // an id that is live, stale (same slot, old generation) or absent.
    class DispatcherFixture
    {
    public:
        DispatcherFixture() :
            pool(2),
            registry(makeUsecases(session, pub)),
            random(),
            policy(),
            sessions(pool, policy, random, 2),
            dispatcher(registry, pool, sessions),
            live_id()
        {
            kiotty::ChannelResult created = pool.create();

            if (created.isOk())
            {
                live_id = created.value().id();
            }
        }

        ChannelId idFor(Channel channel) const
        {
            switch (channel)
            {
            case Channel::Live:     return live_id;
            case Channel::Stale:    return makeChannelId(live_id.index, live_id.generation + 1);
            case Channel::NotFound: return makeChannelId(1, 1);   // in range, never created
            }
            return ChannelId();
        }

        Recorder          session;
        Recorder          pub;
        GameChannelPool   pool;
        UsecaseRegistry   registry;
        CounterRandom     random;
        FixedPolicy       policy;
        SessionRepository sessions;
        UsecaseDispatcher dispatcher;
        ChannelId         live_id;
    };

    class Dispatch : public ::testing::TestWithParam<DispatchCase>
    {
    };
}

// -----------------------------------------------------------------------------
// the gate table
// -----------------------------------------------------------------------------

TEST_P(Dispatch, ReturnsTrueAndRunsTheUsecaseOnlyWhenEveryGateIsOpen)
{
    const DispatchCase& c = GetParam();
    DispatcherFixture   f;

    ASSERT_TRUE(static_cast<bool>(f.registry));

    GameRequest request;
    request.command        = c.command;
    request.channel_id     = f.idFor(c.channel);
    request.correlation_id = 77;

    if (c.authenticated)
    {
        ASSERT_TRUE(f.sessions.open(request.channel_id, accountOf("tester")).isOk());
    }

    EXPECT_EQ(c.expect_dispatched, f.dispatcher.dispatch(request));

    const int expected_runs = c.expect_dispatched ? 1 : 0;
    const int session_runs  = c.command == COMMAND_SESSION ? expected_runs : 0;
    const int public_runs   = c.command == COMMAND_PUBLIC ? expected_runs : 0;

    EXPECT_EQ(session_runs, f.session.runs);
    EXPECT_EQ(public_runs, f.pub.runs);
}

INSTANTIATE_TEST_SUITE_P(AllGates, Dispatch, ::testing::ValuesIn(kDispatchCases), nameOf);

// -----------------------------------------------------------------------------
// what the usecase receives
// -----------------------------------------------------------------------------

TEST(UsecaseDispatcher, ExecuteReceivesTheRequestAndTheChannelItNamed)
{
    DispatcherFixture f;

    GameRequest request;
    request.command        = COMMAND_PUBLIC;
    request.channel_id     = f.live_id;
    request.correlation_id = 1234;

    ASSERT_TRUE(f.dispatcher.dispatch(request));
    EXPECT_EQ(1234u, f.pub.correlation_id);
    EXPECT_EQ(f.live_id, f.pub.channel_id);
}

TEST(UsecaseDispatcher, ChannelGivenToTheUsecaseIsTheRealOne)
{
    DispatcherFixture f;
    CapturingResponseListener io_side;

    {
        ChannelAccess access = f.pool.access(f.live_id);
        ASSERT_TRUE(static_cast<bool>(access));
        ASSERT_TRUE(access.channel().io().response.addListener(io_side));
    }

    f.pub.respond = true;

    GameRequest request;
    request.command        = COMMAND_PUBLIC;
    request.channel_id     = f.live_id;
    request.correlation_id = 5;

    ASSERT_TRUE(f.dispatcher.dispatch(request));

    // The usecase answered through the business view it was handed; the
    // answer arrived on the io side of the same channel.
    ASSERT_EQ(1, io_side.calls);
    EXPECT_EQ(5u, io_side.correlation_id);
    EXPECT_EQ(COMMAND_PUBLIC, io_side.command);
}

TEST(UsecaseDispatcher, UsecaseMayAccessThePoolAgainWhileItRuns)
{
    DispatcherFixture f;

    // dispatch holds the pool lock around execute; a usecase that broadcasts
    // re-enters the pool from there. With a non-recursive lock this would
    // hang rather than fail.
    f.pub.reenter = &f.pool;

    GameRequest request;
    request.command    = COMMAND_PUBLIC;
    request.channel_id = f.live_id;

    ASSERT_TRUE(f.dispatcher.dispatch(request));
    EXPECT_TRUE(f.pub.reentry_ok);
}

TEST(UsecaseDispatcher, DispatchTwiceRunsTheUsecaseTwice)
{
    DispatcherFixture f;

    GameRequest request;
    request.command    = COMMAND_PUBLIC;
    request.channel_id = f.live_id;

    EXPECT_TRUE(f.dispatcher.dispatch(request));
    EXPECT_TRUE(f.dispatcher.dispatch(request));
    EXPECT_EQ(2, f.pub.runs);
}

TEST(UsecaseDispatcher, ChannelRemovedBetweenDispatchesStopsTheSecond)
{
    DispatcherFixture f;

    GameRequest request;
    request.command    = COMMAND_PUBLIC;
    request.channel_id = f.live_id;

    EXPECT_TRUE(f.dispatcher.dispatch(request));

    f.pool.remove(f.live_id);

    EXPECT_FALSE(f.dispatcher.dispatch(request));
    EXPECT_EQ(1, f.pub.runs);
}

// -----------------------------------------------------------------------------
// as a stream listener
// -----------------------------------------------------------------------------

TEST(UsecaseDispatcher, OnStreamDispatchesTheRequest)
{
    DispatcherFixture f;

    GameRequest request;
    request.command        = COMMAND_PUBLIC;
    request.channel_id     = f.live_id;
    request.correlation_id = 8;

    f.dispatcher.onStream(request);

    EXPECT_EQ(1, f.pub.runs);
    EXPECT_EQ(8u, f.pub.correlation_id);
}

TEST(UsecaseDispatcher, OnStreamWithAnUnknownCommandRunsNothing)
{
    DispatcherFixture f;

    GameRequest request;
    request.command    = COMMAND_UNKNOWN;
    request.channel_id = f.live_id;

    f.dispatcher.onStream(request);

    EXPECT_EQ(0, f.session.runs);
    EXPECT_EQ(0, f.pub.runs);
}

// The path a real packet takes: binder attaches the dispatcher, presentation
// emits on the io view, the usecase runs against the business view of the
// same channel.
TEST(UsecaseDispatcher, RequestEmittedThroughABoundChannelReachesTheUsecase)
{
    Recorder          session;
    Recorder          pub;
    GameChannelPool   pool(2);
    UsecaseRegistry   registry(makeUsecases(session, pub));
    CounterRandom     random;
    FixedPolicy       policy;
    SessionRepository sessions(pool, policy, random, 2);
    UsecaseDispatcher dispatcher(registry, pool, sessions);
    ChannelPoolBinder binder(pool, sessions, dispatcher);

    ASSERT_TRUE(static_cast<bool>(registry));

    IoChannelResult bound = binder.onConnected(makeConnectionInfo("127.0.0.1", 1));
    ASSERT_TRUE(bound.isOk());

    CapturingResponseListener io_side;
    ASSERT_TRUE(bound.value().response.addListener(io_side));
    pub.respond = true;

    GameRequest request;
    request.command        = COMMAND_PUBLIC;
    request.channel_id     = bound.value().channel_id;
    request.correlation_id = 21;

    EXPECT_TRUE(bound.value().request.emit(request));
    EXPECT_EQ(1, pub.runs);
    EXPECT_EQ(bound.value().channel_id, pub.channel_id);
    ASSERT_EQ(1, io_side.calls);
    EXPECT_EQ(21u, io_side.correlation_id);
}

// -----------------------------------------------------------------------------
// the session gate over time
// -----------------------------------------------------------------------------
//
// The table above fixes the gate at one moment. These follow a session through
// the repository's transitions and check the gate moves with it.

TEST(UsecaseDispatcher, SessionOnAnotherChannelDoesNotOpenTheGate)
{
    DispatcherFixture f;

    kiotty::ChannelResult other = f.pool.create();
    ASSERT_TRUE(other.isOk());
    ASSERT_TRUE(f.sessions.open(other.value().id(), accountOf("tester")).isOk());

    GameRequest request;
    request.command    = COMMAND_SESSION;
    request.channel_id = f.live_id;

    EXPECT_FALSE(f.dispatcher.dispatch(request));
    EXPECT_EQ(0, f.session.runs);
}

TEST(UsecaseDispatcher, DetachWithZeroLifetimeClosesTheGate)
{
    DispatcherFixture f;
    f.policy.orphan_lifetime_ms = 0;

    ASSERT_TRUE(f.sessions.open(f.live_id, accountOf("tester")).isOk());

    GameRequest request;
    request.command    = COMMAND_SESSION;
    request.channel_id = f.live_id;

    EXPECT_TRUE(f.dispatcher.dispatch(request));

    f.sessions.detach(f.live_id);

    EXPECT_FALSE(f.dispatcher.dispatch(request));
    EXPECT_EQ(1, f.session.runs);
}

TEST(UsecaseDispatcher, OrphanedSessionDoesNotOpenTheGateUntilRebound)
{
    DispatcherFixture f;
    f.policy.orphan_lifetime_ms = 1000;

    kiotty::SessionResult opened = f.sessions.open(f.live_id, accountOf("tester"));
    ASSERT_TRUE(opened.isOk());

    GameRequest request;
    request.command    = COMMAND_SESSION;
    request.channel_id = f.live_id;

    f.sessions.detach(f.live_id);
    EXPECT_FALSE(f.dispatcher.dispatch(request));

    // Reconnect on a new channel: the gate opens there and stays shut here.
    kiotty::ChannelResult next = f.pool.create();
    ASSERT_TRUE(next.isOk());
    ASSERT_TRUE(f.sessions.rebind(next.value().id(), opened.value().token()).isOk());

    EXPECT_FALSE(f.dispatcher.dispatch(request));

    request.channel_id = next.value().id();
    EXPECT_TRUE(f.dispatcher.dispatch(request));
    EXPECT_EQ(1, f.session.runs);
    EXPECT_EQ(next.value().id(), f.session.channel_id);
}

TEST(UsecaseDispatcher, CloseShutsTheGate)
{
    DispatcherFixture f;

    kiotty::SessionResult opened = f.sessions.open(f.live_id, accountOf("tester"));
    ASSERT_TRUE(opened.isOk());

    GameRequest request;
    request.command    = COMMAND_SESSION;
    request.channel_id = f.live_id;

    EXPECT_TRUE(f.dispatcher.dispatch(request));

    f.sessions.close(opened.value());

    EXPECT_FALSE(f.dispatcher.dispatch(request));
    EXPECT_EQ(1, f.session.runs);
}

TEST(UsecaseDispatcher, PublicUsecaseIgnoresEverySessionTransition)
{
    DispatcherFixture f;
    f.policy.orphan_lifetime_ms = 1000;

    GameRequest request;
    request.command    = COMMAND_PUBLIC;
    request.channel_id = f.live_id;

    EXPECT_TRUE(f.dispatcher.dispatch(request));

    kiotty::SessionResult opened = f.sessions.open(f.live_id, accountOf("tester"));
    ASSERT_TRUE(opened.isOk());
    EXPECT_TRUE(f.dispatcher.dispatch(request));

    f.sessions.detach(f.live_id);
    EXPECT_TRUE(f.dispatcher.dispatch(request));

    f.sessions.close(opened.value());
    EXPECT_TRUE(f.dispatcher.dispatch(request));

    EXPECT_EQ(4, f.pub.runs);
}

TEST(UsecaseDispatcher, DisconnectThroughTheBinderClosesTheGateForTheRecycledChannel)
{
    // The real sequence: bound channel, login, disconnect (binder detaches
    // and removes), reconnect into the same slot. A request on the new
    // channel must not inherit the old login.
    Recorder          session;
    Recorder          pub;
    GameChannelPool   pool(1);
    UsecaseRegistry   registry(makeUsecases(session, pub));
    CounterRandom     random;
    FixedPolicy       policy;
    SessionRepository sessions(pool, policy, random, 2);
    UsecaseDispatcher dispatcher(registry, pool, sessions);
    ChannelPoolBinder binder(pool, sessions, dispatcher);

    policy.orphan_lifetime_ms = 1000;

    IoChannelResult first = binder.onConnected(makeConnectionInfo("127.0.0.1", 1));
    ASSERT_TRUE(first.isOk());
    ASSERT_TRUE(sessions.open(first.value().channel_id, accountOf("tester")).isOk());

    binder.onDisconnected(makeConnectionInfo("127.0.0.1", 1), first.value());

    IoChannelResult second = binder.onConnected(makeConnectionInfo("127.0.0.1", 2));
    ASSERT_TRUE(second.isOk());

    GameRequest request;
    request.command    = COMMAND_SESSION;
    request.channel_id = second.value().channel_id;

    EXPECT_TRUE(second.value().request.emit(request));
    EXPECT_EQ(0, session.runs);
}
