// Scratch harness for the domain boundary: UsecaseRegistry, UsecaseDispatcher,
// and the Sink wiring between a connection and its usecases.
//
//   offline - the dispatcher alone, against capturing sinks. No socket at all,
//             which is the point of the boundary being plain data.
//   live    - the same usecases behind a real connection: unauthenticated and
//             unknown commands rejected, a reply and an event round tripped on
//             the wire, the session gate opening.

#include <core/kiotty_block_pool.h>
#include <datalayer/repository/cryptor/kiotty_secure_random.h>
#include <datalayer/repository/session/kiotty_session_repository.h>
#include <domain/channel/kiotty_channel_binder.h>
#include <domain/channel/kiotty_game_channel_pool.h>
#include <domain/codec/kiotty_packet_codec.h>
#include <domain/usecase/kiotty_usecase_dispatcher.h>
#include <presentation/connection/kiotty_connection_table.h>
#include <presentation/io_loop/kiotty_io_loop.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

using namespace kiotty;

namespace
{
    int g_failures = 0;

    void check(bool condition, const char* what)
    {
        if (condition)
        {
            std::printf("[ok]   %s\n", what);
            return;
        }
        std::printf("[FAIL] %s\n", what);
        ++g_failures;
    }

    const uint16_t COMMAND_PING    = 10;   // public
    const uint16_t COMMAND_MOVE    = 11;   // needs a session
    const uint16_t COMMAND_UNKNOWN = 99;

    const char PING_REPLY[] = "pong";
    const char MOVE_REPLY[] = "moved";
    const char EVENT_BODY[] = "tick";

    const size_t PING_REPLY_SIZE = sizeof(PING_REPLY) - 1;
    const size_t MOVE_REPLY_SIZE = sizeof(MOVE_REPLY) - 1;
    const size_t EVENT_BODY_SIZE = sizeof(EVENT_BODY) - 1;

    // Written by usecases on the loop thread, read by the client thread, so
    // the harness itself has to be race-free under TSan.
    std::atomic<int>      g_ping_runs(0);
    std::atomic<int>      g_move_runs(0);
    std::atomic<uint64_t> g_last_sequence(0);

    Bytes bytesFrom(BlockPool& pool, const char* text, size_t size)
    {
        Bytes bytes(pool, size);

        if (bytes)
        {
            std::memcpy(bytes.writableSpan().data(), text, size);
        }
        return bytes;
    }

    class PingUsecase : public IPublicUsecase
    {
    public:
        PingUsecase(BlockPool& pool, SessionRepository& sessions) :
            _pool(pool),
            _sessions(sessions)
        {
        }

        uint16_t command() const override { return COMMAND_PING; }

        void execute(const GameRequest& request, BusinessGameChannel& channel) override
        {
            ++g_ping_runs;
            g_last_sequence = request.state_sequence;
            AccountId account;
            tryMakeAccountId("scratch", account);
            _sessions.open(request.channel_id, account);

            GameResponse response;
            response.correlation_id = request.correlation_id;
            response.command        = COMMAND_PING;
            response.payload        = bytesFrom(_pool, PING_REPLY, PING_REPLY_SIZE);
            channel.response.emit(response);

            GameEvent event;
            event.command = COMMAND_PING;
            event.payload = bytesFrom(_pool, EVENT_BODY, EVENT_BODY_SIZE);
            channel.event.emit(event);
        }

    private:
        BlockPool&         _pool;
        SessionRepository& _sessions;
    };

    class MoveUsecase : public IUsecase
    {
    public:
        explicit MoveUsecase(BlockPool& pool) :
            _pool(pool)
        {
        }

        uint16_t command() const override { return COMMAND_MOVE; }

        void execute(const GameRequest& request, BusinessGameChannel& channel) override
        {
            ++g_move_runs;
            g_last_sequence = request.state_sequence;

            GameResponse response;
            response.correlation_id = request.correlation_id;
            response.command        = COMMAND_MOVE;
            response.payload        = bytesFrom(_pool, MOVE_REPLY, MOVE_REPLY_SIZE);
            channel.response.emit(response);
        }

    private:
        BlockPool& _pool;
    };

    std::vector<Holder<IUsecase> > makeUsecases(BlockPool& pool, SessionRepository& sessions)
    {
        std::vector<Holder<IUsecase> > usecases;

        usecases.push_back(Holder<IUsecase>::make<PingUsecase>(pool, sessions));
        usecases.push_back(Holder<IUsecase>::make<MoveUsecase>(pool));
        return usecases;
    }

    // -------------------------------------------------------------------------
    // offline - the dispatcher against a real channel with capturing listeners
    // -------------------------------------------------------------------------

    struct Captured
    {
        uint32_t             correlation_id;
        uint16_t             command;
        std::vector<uint8_t> payload;
    };

    template <typename T>
    class CapturingListener : public StreamListener<T>
    {
    public:
        std::vector<Captured> items;

        void onStream(const T& item) override
        {
            Captured captured;
            captured.correlation_id = correlationOf(item);
            captured.command        = item.command;
            captured.payload.assign(item.payload.data(),
                                    item.payload.data() + item.payload.size());
            items.push_back(captured);
        }

    private:
        static uint32_t correlationOf(const GameResponse& response)
        {
            return response.correlation_id;
        }

        static uint32_t correlationOf(const GameEvent&)
        {
            return 0;
        }
    };

    GameRequest makeRequest(ChannelId channel_id, uint16_t command, uint32_t correlation_id,
                            uint64_t sequence)
    {
        GameRequest request;
        request.channel_id     = channel_id;
        request.command        = command;
        request.correlation_id = correlation_id;
        request.state_sequence = sequence;
        return request;
    }

    void checkDispatcherOffline(const UsecaseRegistry& registry, GameChannelPool& channels,
                                SessionRepository& sessions)
    {
        UsecaseDispatcher dispatcher(registry, channels, sessions);

        ChannelResult created = channels.create();
        check(created.isOk(), "a channel came out of the pool");

        if (!created.isOk())
        {
            return;
        }

        GameChannel& channel = created.value();
        const ChannelId id   = channel.id();

        CapturingListener<GameResponse> replies;
        CapturingListener<GameEvent>    events;

        channel.io().response.addListener(replies);
        channel.io().event.addListener(events);

        GameRequest unauth = makeRequest(id, COMMAND_MOVE, 7, 1);
        check(!dispatcher.dispatch(unauth), "a session usecase is refused without a session");
        check(g_move_runs == 0, "and it did not run");

        GameRequest unknown = makeRequest(id, COMMAND_UNKNOWN, 8, 2);
        check(!dispatcher.dispatch(unknown), "an unclaimed command is refused");

        GameRequest ping = makeRequest(id, COMMAND_PING, 9, 3);
        check(dispatcher.dispatch(ping), "a public usecase runs without a session");
        check(g_ping_runs == 1 && g_last_sequence == 3, "and saw its state_sequence");
        check(replies.items.size() == 1 &&
              replies.items[0].correlation_id == 9 &&
              replies.items[0].payload.size() == PING_REPLY_SIZE &&
              std::memcmp(replies.items[0].payload.data(), PING_REPLY,
                          PING_REPLY_SIZE) == 0,
              "its reply landed on the response stream");
        check(events.items.size() == 1 &&
              events.items[0].command == COMMAND_PING,
              "its event landed on the event stream");

        GameRequest authed = makeRequest(id, COMMAND_MOVE, 10, 4);
        check(sessions.find(id).isOk(), "the ping opened a session on the channel");
        check(dispatcher.dispatch(authed), "the session gate opens with a session");
        check(g_move_runs == 1, "and the session usecase ran");

        ChannelId stale = id;
        channels.remove(id);
        check(!channels.access(stale), "a removed channel cannot be reached");

        GameRequest orphan = makeRequest(stale, COMMAND_PING, 11, 5);
        check(!dispatcher.dispatch(orphan), "a request for a gone channel is dropped");
        check(g_ping_runs == 1, "and its usecase did not run");
    }

    // -------------------------------------------------------------------------
    // live - the same usecases behind a real connection
    // -------------------------------------------------------------------------

    UsecaseDispatcher* g_dispatcher = nullptr;

    std::atomic<int> g_rejects(0);

    struct DropOrphans : ISessionPolicy
    {
        uint32_t orphanLifetimeMs(const Session&) const override { return 0; }
        bool     replacesPreviousLogin(const AccountId&) const override { return true; }
    };

    // A ping stands in for a login here: PingUsecase opens a session through
    // the repository, so the gate is the real one. The codec only numbers the
    // requests per channel so state_sequence can be checked on the wire.
    class SequenceCodec : public DefaultPacketCodec
    {
    public:
        static const size_t MAX_CHANNELS = 4;

        SequenceCodec() :
            _sequence()
        {
        }

        bool decode(ReceivedPacket& packet, ChannelId channel_id, GameRequest& out) override
        {
            if (channel_id.index >= MAX_CHANNELS)
            {
                return false;
            }

            if (!DefaultPacketCodec::decode(packet, channel_id, out))
            {
                return false;
            }

            out.state_sequence = ++_sequence[channel_id.index];
            return true;
        }

    private:
        uint64_t _sequence[MAX_CHANNELS];
    };

    // A real server would answer with an error packet or close; this harness
    // has to keep reading to see what comes next, so it counts.
    class RejectCountingListener : public StreamListener<GameRequest>
    {
    public:
        void onStream(const GameRequest& request) override
        {
            if (!g_dispatcher->dispatch(request))
            {
                ++g_rejects;
            }
        }
    };

#if defined(_WIN32)
    typedef SOCKET ClientSocket;
    const ClientSocket NO_CLIENT = INVALID_SOCKET;
#else
    typedef int ClientSocket;
    const ClientSocket NO_CLIENT = -1;
#endif

    ClientSocket connectToServer(uint16_t port)
    {
#if defined(_WIN32)
        const ClientSocket fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#else
        const ClientSocket fd = ::socket(AF_INET, SOCK_STREAM, 0);
#endif

        sockaddr_in address;
        std::memset(&address, 0, sizeof(address));

        address.sin_family = AF_INET;
        address.sin_port   = ::htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);

        if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
        {
            return NO_CLIENT;
        }

        // Nothing here may block forever - a bug has to show up as a failed
        // check, not as a hang.
#if defined(_WIN32)
        const DWORD one_second = 1000;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&one_second), sizeof(one_second));
#else
        timeval one_second;
        one_second.tv_sec  = 1;
        one_second.tv_usec = 0;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &one_second, sizeof(one_second));
#endif
        return fd;
    }

    void closeClient(ClientSocket fd)
    {
#if defined(_WIN32)
        ::closesocket(fd);
#else
        ::close(fd);
#endif
    }

    void sendCommand(ClientSocket fd, uint16_t command, uint32_t correlation_id)
    {
        PacketHeader header;
        std::memset(&header, 0, sizeof(header));

        header.magic.set(PACKET_MAGIC);
        header.correlation_id.set(correlation_id);
        header.command.set(command);
        header.version.set(PACKET_VERSION);
        header.payload_length.set(0);

        uint8_t out[PACKET_HEADER_SIZE];
        std::memcpy(out, &header, PACKET_HEADER_SIZE);

        ::send(fd, reinterpret_cast<const char*>(out), PACKET_HEADER_SIZE, 0);
    }

    bool readExactly(ClientSocket fd, uint8_t* out, size_t length)
    {
        size_t filled = 0;

        while (filled < length)
        {
            const int back = static_cast<int>(
                ::recv(fd, reinterpret_cast<char*>(out) + filled,
                       static_cast<int>(length - filled), 0));

            if (back <= 0)
            {
                return false;
            }
            filled += static_cast<size_t>(back);
        }
        return true;
    }

    bool readPacket(ClientSocket fd, PacketHeader& header, uint8_t* payload,
                    size_t payload_capacity)
    {
        uint8_t raw[PACKET_HEADER_SIZE];

        if (!readExactly(fd, raw, PACKET_HEADER_SIZE))
        {
            return false;
        }
        std::memcpy(&header, raw, PACKET_HEADER_SIZE);

        const size_t length = header.payload_length.get();

        if (length > payload_capacity)
        {
            return false;
        }
        return length == 0 || readExactly(fd, payload, length);
    }

    IoLoop*   g_loop     = nullptr;
    Endpoint* g_endpoint = nullptr;

    void runClient()
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        const ClientSocket fd = connectToServer(g_endpoint->port());

        if (fd == NO_CLIENT)
        {
            check(false, "client connected");
            g_loop->stop();
            return;
        }

        PacketHeader header;
        uint8_t      payload[64];

        // 1. a session usecase before anything authenticated us.
        sendCommand(fd, COMMAND_MOVE, 100);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        check(g_move_runs == 0, "a session usecase did not run unauthenticated");
        check(g_rejects == 1, "and it was rejected");

        // 2. an unknown command.
        sendCommand(fd, COMMAND_UNKNOWN, 101);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        check(g_rejects == 2, "an unknown command was rejected");

        // 3. the public usecase - replies, then pushes an event.
        sendCommand(fd, COMMAND_PING, 102);

        check(readPacket(fd, header, payload, sizeof(payload)), "the reply arrived");
        check(header.command.get() == COMMAND_PING, "reply carried the command");
        check(header.correlation_id.get() == 102, "reply reflected the correlation id");
        check((header.flags.get() & PACKET_FLAG_EVENT) == 0, "a reply is not an event");
        check(header.payload_length.get() == PING_REPLY_SIZE &&
              std::memcmp(payload, PING_REPLY, PING_REPLY_SIZE) == 0,
              "reply body round tripped");
        check(header.timestamp.get() != 0, "the wire stamped a timestamp");

        check(readPacket(fd, header, payload, sizeof(payload)), "the event arrived");
        check((header.flags.get() & PACKET_FLAG_EVENT) != 0, "an event says so in flags");
        check(header.correlation_id.get() == 1, "the event took the first sequence");
        check(header.payload_length.get() == EVENT_BODY_SIZE &&
              std::memcmp(payload, EVENT_BODY, EVENT_BODY_SIZE) == 0,
              "event body round tripped");

        check(g_ping_runs == 1, "the public usecase ran");
        check(g_last_sequence == 3, "state_sequence counted all three packets");

        // 4. the session usecase again, now that the ping authenticated us.
        sendCommand(fd, COMMAND_MOVE, 103);

        check(readPacket(fd, header, payload, sizeof(payload)),
              "the session usecase answered once authenticated");
        check(header.correlation_id.get() == 103, "and reflected its correlation id");
        check(header.payload_length.get() == MOVE_REPLY_SIZE &&
              std::memcmp(payload, MOVE_REPLY, MOVE_REPLY_SIZE) == 0,
              "session reply body round tripped");

        check(g_move_runs == 1, "the session usecase ran exactly once");
        check(g_rejects == 2, "and nothing else was rejected");

        closeClient(fd);

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        g_loop->stop();
    }
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);

#if defined(_WIN32)
    WSADATA wsa;
    ::WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    BlockPool pool(defaultBlockClasses(), defaultBlockClassCount());

    GameChannelPool   channels(4);
    DropOrphans       policy;
    SecureRandom      random;
    SessionRepository sessions(channels, policy, random, 4);

    {
        std::vector<Holder<IUsecase> > clashing;
        clashing.push_back(Holder<IUsecase>::make<PingUsecase>(pool, sessions));
        clashing.push_back(Holder<IUsecase>::make<PingUsecase>(pool, sessions));

        UsecaseRegistry duplicated(std::move(clashing));
        check(!static_cast<bool>(duplicated), "a duplicate command is refused");
    }
    UsecaseRegistry   registry(makeUsecases(pool, sessions));

    check(static_cast<bool>(registry), "registry came up");
    check(registry.find(COMMAND_PING) != nullptr, "ping is registered");
    check(registry.find(COMMAND_MOVE) != nullptr, "move is registered");
    check(registry.find(COMMAND_UNKNOWN) == nullptr, "an unclaimed command finds nothing");

    std::printf("--- dispatcher, offline ---\n");
    checkDispatcherOffline(registry, channels, sessions);

    g_ping_runs     = 0;
    g_move_runs     = 0;
    g_last_sequence = 0;

    std::printf("--- live ---\n");

    UsecaseDispatcher      dispatcher(registry, channels, sessions);
    RejectCountingListener requests;
    ChannelPoolBinder      binder(channels, sessions, requests);
    SequenceCodec          codec;

    ConnectionTable connections(4, pool, 8, binder, codec);
    Endpoint        endpoint("127.0.0.1", 0, 16, connections);
    IoLoop          loop(endpoint);

    check(static_cast<bool>(loop), "loop came up");

    if (!static_cast<bool>(loop))
    {
        std::printf("code=%d\n", static_cast<int>(loop.code()));
        return 1;
    }

    g_dispatcher = &dispatcher;
    g_loop       = &loop;
    g_endpoint   = &endpoint;

    std::thread client(&runClient);

    loop.run();
    client.join();

    check(endpoint.connectionCount() == 0, "nothing left in the table");
    check(pool.fallbackCount() == 0, "nothing fell through to the heap");

    std::printf("ping=%d move=%d rejects=%d failures=%d\n",
                g_ping_runs.load(), g_move_runs.load(), g_rejects.load(), g_failures);
    std::printf("%s\n", (g_failures == 0) ? "PASS" : "FAIL");

#if defined(_WIN32)
    ::WSACleanup();
#endif

    return (g_failures == 0) ? 0 : 1;
}
