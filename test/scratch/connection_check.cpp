// Scratch harness for kiotty::Connection. Not a unit test - it drives a real
// Connection through a real socket so the receive state machine, the send chain
// and the drop policy are exercised the way the engine will use them.

#include <core/kiotty_block_pool.h>
#include <core/kiotty_connection_buffer.h>
#include <datalayer/repository/cryptor/kiotty_secure_random.h>
#include <datalayer/repository/session/kiotty_session_repository.h>
#include <domain/channel/kiotty_channel_binder.h>
#include <domain/channel/kiotty_game_channel_pool.h>
#include <domain/codec/kiotty_packet_codec.h>
#include <presentation/connection/kiotty_connection.h>
#include <presentation/connection/kiotty_connection_table.h>
#include <presentation/endpoint/kiotty_endpoint.h>
#include <presentation/event_listener/kiotty_io_event_listener.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

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
    struct DropOrphans : kiotty::ISessionPolicy
    {
        uint32_t orphanLifetimeMs(const kiotty::Session&) const override { return 0; }
        bool     replacesPreviousLogin(const kiotty::AccountId&) const override { return true; }
    };

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

    // -------------------------------------------------------------------------
    // offline - the send queue and its drop policy
    // -------------------------------------------------------------------------

    Bytes makeBlock(BlockPool& pool, size_t length, uint8_t fill)
    {
        Bytes bytes(pool, length);

        if (bytes)
        {
            std::memset(bytes.writableSpan().data(), fill, length);
        }
        return bytes;
    }

    bool pushInto(SendBuffer& send, BlockPool& pool, uint8_t fill, DropPolicy policy)
    {
        SentPacket packet;
        packet.bytes  = makeBlock(pool, 64, fill);
        packet.policy = policy;

        return send.tryPush(packet);
    }

    void checkSendQueue(BlockPool& pool)
    {
        SendBuffer send(4);

        check(static_cast<bool>(send), "send queue allocated");

        check(pushInto(send, pool, 1, DropPolicy::Never), "push response 1");
        check(pushInto(send, pool, 2, DropPolicy::Oldest), "push event 2");
        check(pushInto(send, pool, 3, DropPolicy::Never), "push response 3");
        check(pushInto(send, pool, 4, DropPolicy::Oldest), "push event 4");
        check(!pushInto(send, pool, 5, DropPolicy::Never), "a full queue refuses");
        check(send.size() == 4, "a refused push changed nothing");

        check(send.dropOldest(DropPolicy::Oldest), "the oldest event is droppable");
        check(send.size() == 3, "dropping removed exactly one");

        SentPacket first = send.pop();
        check(first.bytes && first.bytes.data()[0] == 1, "response 1 survived the drop");

        SentPacket second = send.pop();
        check(second.bytes && second.bytes.data()[0] == 3, "event 2 is the one that went");

        SentPacket third = send.pop();
        check(third.bytes && third.bytes.data()[0] == 4, "event 4 kept its place");

        check(send.empty(), "queue drained");
        check(!send.dropOldest(DropPolicy::Oldest), "an empty queue drops nothing");

        SendBuffer responses(2);
        check(pushInto(responses, pool, 1, DropPolicy::Never), "push response");
        check(pushInto(responses, pool, 2, DropPolicy::Never), "push response");
        check(!responses.dropOldest(DropPolicy::Oldest),
              "a queue of responses has nothing to give up");
    }

    // -------------------------------------------------------------------------
    // live - one Connection behind a real endpoint
    // -------------------------------------------------------------------------

    const char PAYLOAD[] = "kiotty-connection-payload";

    const size_t PAYLOAD_SIZE = sizeof(PAYLOAD) - 1;

    void writeHeader(uint8_t* out, uint32_t magic, uint16_t version, uint16_t payload_length)
    {
        PacketHeader header;
        std::memset(&header, 0, sizeof(header));

        header.magic.set(magic);
        header.correlation_id.set(7);
        header.timestamp.set(0);
        header.command.set(11);
        header.flags.set(0);
        header.version.set(version);
        header.payload_length.set(payload_length);

        std::memcpy(out, &header, PACKET_HEADER_SIZE);
    }

    BlockPool*       g_pool       = nullptr;
    Connection*      g_connection = nullptr;
    GameChannelPool* g_channels   = nullptr;

    int  g_packets = 0;
    bool g_echo    = false;

    void inspectRequest(const GameRequest& request);
    void echoBack(const GameRequest& request);

    // The smallest server there is: every request the channel carries is
    // inspected and, when asked, echoed back through the same channel.
    class CheckListener : public StreamListener<GameRequest>
    {
    public:
        void onStream(const GameRequest& request) override
        {
            ++g_packets;
            inspectRequest(request);

            if (g_echo)
            {
                echoBack(request);
            }
        }
    };

    ConnectionTable* g_connections = nullptr;

    int g_disconnected = 0;

    void onAccepted(IOMultiEventListener& listener, IOContext& context,
                    IOAcceptResponse& response)
    {
        if (response.code != SocketCode::SOCKET_SUCCESS)
        {
            check(false, "accept succeeded");
            listener.cancel();
            return;
        }

        check(static_cast<Endpoint&>(context).handleAccepted(listener, response),
              "the endpoint stayed on the air");

        g_connection = g_connections->at(0);

        check(g_connection != nullptr, "connection came up");
        check(g_connection != nullptr &&
              g_connection->lifeState() == LifeState::Active,
              "connection starts Active");
    }

    void echoBack(const GameRequest& request)
    {
        GameResponse response;
        response.correlation_id = request.correlation_id;
        response.command        = request.command;
        response.payload        = makeBlock(*g_pool, request.payload.size(), 0);

        if (request.payload.size() > 0)
        {
            std::memcpy(response.payload.writableSpan().data(),
                        request.payload.data(), request.payload.size());
        }

        ChannelAccess access = g_channels->access(request.channel_id);

        check(static_cast<bool>(access), "the request names a live channel");

        if (access)
        {
            check(access.channel().business().response.emit(response), "reply reached the connection");
        }
    }

    void inspectRequest(const GameRequest& request)
    {
        check(request.command == 11, "command survived");
        check(request.correlation_id == 7, "correlation id survived");
        check(request.payload.size() == PAYLOAD_SIZE, "payload block sized to the header");
        check(std::memcmp(request.payload.data(), PAYLOAD, PAYLOAD_SIZE) == 0,
              "payload bytes round tripped");
    }

    void onReceived(IOMultiEventListener& listener, IOContext& context,
                    IOReceiveResponse& response)
    {
        (void)listener;

        static_cast<Connection&>(context).handleReceived(response);
    }

    void onSent(IOMultiEventListener& listener, IOContext& context,
                IOSendResponse& response)
    {
        (void)listener;

        static_cast<Connection&>(context).handleSent(response);
    }

    void onDisconnected(IOMultiEventListener& listener, IOContext& context)
    {
        Connection& connection = static_cast<Connection&>(context);

        ++g_disconnected;

        connection.handleDisconnected();

        check(connection.lifeState() == LifeState::Closed, "connection ends Closed");
        listener.cancel();
    }

    void onInterrupted(IOMultiEventListener& listener)
    {
        (void)listener;
    }

#if defined(_WIN32)
    typedef SOCKET ClientSocket;
#else
    typedef int ClientSocket;
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
            check(false, "client connected");
        }
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

    void sendAll(ClientSocket fd, const uint8_t* data, size_t length)
    {
        ::send(fd, reinterpret_cast<const char*>(data), static_cast<int>(length), 0);
    }

    // Header first, payload a beat later. That is the split the two-step receive
    // exists for, and sending both at once never takes the ReadingPayload path.
    void runSplitWriteClient(uint16_t port)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        const ClientSocket fd = connectToServer(port);

        uint8_t header[PACKET_HEADER_SIZE];
        writeHeader(header, PACKET_MAGIC, PACKET_VERSION,
                    static_cast<uint16_t>(PAYLOAD_SIZE));

        sendAll(fd, header, PACKET_HEADER_SIZE);
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        sendAll(fd, reinterpret_cast<const uint8_t*>(PAYLOAD), PAYLOAD_SIZE);

        const size_t expected = PACKET_HEADER_SIZE + PAYLOAD_SIZE;

        uint8_t received[128];
        size_t  filled = 0;

        std::memset(received, 0, sizeof(received));

        while (filled < expected)
        {
            const int back = static_cast<int>(
                ::recv(fd, reinterpret_cast<char*>(received) + filled,
                       static_cast<int>(expected - filled), 0));

            if (back <= 0)
            {
                break;
            }
            filled += static_cast<size_t>(back);
        }

        check(filled == expected, "client read the whole reply");
        check(std::memcmp(received + PACKET_HEADER_SIZE, PAYLOAD, PAYLOAD_SIZE) == 0,
              "reply payload matched what was sent");

        closeClient(fd);
    }

    void runBadMagicClient(uint16_t port)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        const ClientSocket fd = connectToServer(port);

        uint8_t header[PACKET_HEADER_SIZE];
        writeHeader(header, 0xDEADBEEFu, PACKET_VERSION, 0);

        sendAll(fd, header, PACKET_HEADER_SIZE);

        char      sink[8];
        const int back = static_cast<int>(::recv(fd, sink, sizeof(sink), 0));

        check(back <= 0, "a bad magic got the connection dropped");

        closeClient(fd);
    }

    void runLoop(IOMultiEventListener& listener)
    {
        for (int i = 0; i < 20; ++i)
        {
            const IOEventListenType type = listener.wait(1000);

            if (type == IOEventListenType::CANCELLED)
            {
                return;
            }
            if (type == IOEventListenType::TIMEOUT)
            {
                continue;
            }
        }
        check(false, "the loop finished before its iteration budget");
        listener.cancel();
    }

    void runLivePhase(BlockPool& pool, bool echo, void (*client)(uint16_t))
    {
        IOEventCallback callbacks;
        callbacks.onAccepted     = &onAccepted;
        callbacks.onReceived     = &onReceived;
        callbacks.onSent         = &onSent;
        callbacks.onDisconnected = &onDisconnected;
        callbacks.onInterrupted  = &onInterrupted;

        IOMultiEventListener listener(callbacks);

        if (!static_cast<bool>(listener))
        {
            check(false, "listener opened");
            return;
        }

        GameChannelPool    channels(1);
        CheckListener      check_listener;
        DropOrphans        policy;
        SecureRandom       random;
        SessionRepository  sessions(channels, policy, random, 1);
        ChannelPoolBinder  binder(channels, sessions, check_listener);
        DefaultPacketCodec codec;
        ConnectionTable    connections(1, pool, 8, binder, codec);
        Endpoint           endpoint("127.0.0.1", 0, 16, connections);

        if (!static_cast<bool>(endpoint))
        {
            check(false, "endpoint came up");
            return;
        }

        g_pool        = &pool;
        g_channels    = &channels;
        g_connections = &connections;
        g_connection  = nullptr;
        g_packets     = 0;
        g_echo        = echo;

        const int disconnected_before = g_disconnected;

        if (endpoint.submitAccept(listener) != SocketCode::SOCKET_SUCCESS)
        {
            check(false, "accept armed");
            return;
        }

        std::thread client_thread(client, endpoint.port());

        runLoop(listener);
        client_thread.join();

        check(g_disconnected == disconnected_before + 1, "onDisconnected arrived once");

        connections.reapClosed();
        check(connections.size() == 0, "the closed connection was reaped");

        g_connections = nullptr;
        g_connection  = nullptr;
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

    std::printf("--- send queue ---\n");
    checkSendQueue(pool);

    std::printf("--- split write, echo back ---\n");
    runLivePhase(pool, true, &runSplitWriteClient);
    check(g_packets == 1, "exactly one packet came out of the split write");

    std::printf("--- bad magic ---\n");
    runLivePhase(pool, false, &runBadMagicClient);
    check(g_packets == 0, "a bad header produced no packet");

    check(pool.fallbackCount() == 0, "nothing fell through to the heap");

    std::printf("failures=%d\n", g_failures);
    std::printf("%s\n", (g_failures == 0) ? "PASS" : "FAIL");

#if defined(_WIN32)
    ::WSACleanup();
#endif

    return (g_failures == 0) ? 0 : 1;
}
