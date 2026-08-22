// Scratch harness for kiotty::IoLoop. Drives the whole transport - endpoint,
// listener, connection table, connections - from a handler that only sees
// packets, which is the seam everything above the loop will sit on.
//
//   1. three clients connect at once, each sends one packet, each gets an echo
//   2. the connection limit refuses a fourth
//   3. every client goes away and the table drains back to zero
//   4. stop() from another thread ends run()

#include <core/kiotty_block_pool.h>
#include <domain/channel/kiotty_channel_binder.h>
#include <domain/channel/kiotty_game_channel_pool.h>
#include <domain/codec/kiotty_packet_codec.h>
#include <presentation/connection/kiotty_connection_table.h>
#include <presentation/io_loop/kiotty_io_loop.h>

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

    const char PAYLOAD[] = "kiotty-io-loop";

    const size_t PAYLOAD_SIZE = sizeof(PAYLOAD) - 1;

    const size_t MAX_CONNECTIONS = 3;

    void writeHeader(uint8_t* out, uint32_t magic, uint16_t version,
                     uint16_t payload_length, uint32_t correlation_id)
    {
        PacketHeader header;
        std::memset(&header, 0, sizeof(header));

        header.magic.set(magic);
        header.correlation_id.set(correlation_id);
        header.timestamp.set(0);
        header.command.set(11);
        header.flags.set(0);
        header.version.set(version);
        header.payload_length.set(payload_length);

        std::memcpy(out, &header, PACKET_HEADER_SIZE);
    }

    // Counted across every connection, so they outlive any one of them.
    int g_opened  = 0;
    int g_packets = 0;
    int g_closed  = 0;

    // Echoes every request straight back through its own channel. That is the
    // whole server - a request listener plus a binder is how an application
    // plugs in, and anything a session needs to remember would live in what
    // the binder attaches.
    class EchoListener : public StreamListener<GameRequest>
    {
    public:
        EchoListener(BlockPool& pool, GameChannelPool& channels) :
            _pool(pool),
            _channels(channels)
        {
        }

        void onStream(const GameRequest& request) override
        {
            ++g_packets;

            GameResponse response;
            response.correlation_id = request.correlation_id;
            response.command        = request.command;
            response.payload        = Bytes(_pool, request.payload.size());

            if (!response.payload)
            {
                check(false, "reply block acquired");
                return;
            }

            if (request.payload.size() > 0)
            {
                std::memcpy(response.payload.writableSpan().data(),
                            request.payload.data(), request.payload.size());
            }

            ChannelAccess access = _channels.access(request.channel_id);

            if (access)
            {
                access.channel().business().response.emit(response);
            }
        }

    private:
        BlockPool&       _pool;
        GameChannelPool& _channels;
    };

    class CountingBinder : public IChannelBinder
    {
    public:
        explicit CountingBinder(IChannelBinder& inner) :
            _inner(inner)
        {
        }

        IoChannelResult onConnected(const ConnectionInfo& info) override
        {
            ++g_opened;
            return _inner.onConnected(info);
        }

        void onDisconnected(const ConnectionInfo& info, const IoGameChannel& channel) override
        {
            ++g_closed;
            _inner.onDisconnected(info, channel);
        }

    private:
        IChannelBinder& _inner;
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

        // Nothing in this harness may block forever - a bug in the loop has to
        // show up as a failed check, not as a hang.
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

    // One client sends one packet stamped with its own id and expects that same
    // id back - which is what proves each connection got its own buffers rather
    // than sharing one.
    bool runOneClient(uint16_t port, uint32_t id)
    {
        const ClientSocket fd = connectToServer(port);

        if (fd == NO_CLIENT)
        {
            return false;
        }

        uint8_t out[PACKET_HEADER_SIZE + PAYLOAD_SIZE];
        writeHeader(out, PACKET_MAGIC, PACKET_VERSION,
                    static_cast<uint16_t>(PAYLOAD_SIZE), id);
        std::memcpy(out + PACKET_HEADER_SIZE, PAYLOAD, PAYLOAD_SIZE);

        ::send(fd, reinterpret_cast<const char*>(out), static_cast<int>(sizeof(out)), 0);

        uint8_t back[PACKET_HEADER_SIZE + PAYLOAD_SIZE];
        std::memset(back, 0, sizeof(back));

        const bool read = readExactly(fd, back, sizeof(back));

        PacketHeader echoed;
        std::memcpy(&echoed, back, PACKET_HEADER_SIZE);

        const bool matched =
            read &&
            hasPacketMagic(echoed) &&
            echoed.correlation_id.get() == id &&
            std::memcmp(back + PACKET_HEADER_SIZE, PAYLOAD, PAYLOAD_SIZE) == 0;

        closeClient(fd);
        return matched;
    }

    IoLoop*   g_loop     = nullptr;
    Endpoint* g_endpoint = nullptr;

    void runClients()
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        const uint16_t port = g_endpoint->port();

        // Held open together so the table really is full when the fourth knocks.
        std::vector<ClientSocket> held;

        for (size_t i = 0; i < MAX_CONNECTIONS; ++i)
        {
            const ClientSocket fd = connectToServer(port);

            if (fd == NO_CLIENT)
            {
                check(false, "client connected");
                continue;
            }
            held.push_back(fd);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        check(held.size() == MAX_CONNECTIONS, "every client connected");
        check(g_loop->code() == SocketCode::SOCKET_SUCCESS, "the loop is still healthy");
        check(g_endpoint->connectionCount() == MAX_CONNECTIONS,
              "the table holds one connection per client");

        // The fourth is accepted by the kernel and then dropped by the loop, so
        // the connect succeeds and the read comes back empty.
        const ClientSocket extra = connectToServer(port);

        if (extra != NO_CLIENT)
        {
            uint8_t one = 0;
            check(!readExactly(extra, &one, 1), "the connection limit refused the fourth");
            closeClient(extra);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        check(g_endpoint->connectionCount() == MAX_CONNECTIONS,
              "a refused arrival left the table alone");

        for (size_t i = 0; i < held.size(); ++i)
        {
            closeClient(held[i]);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        check(g_endpoint->connectionCount() == 0, "the table drained");

        // Now the round trip, one client at a time, on slots that have already
        // been used once - which is the reuse path.
        for (uint32_t id = 1; id <= 3; ++id)
        {
            check(runOneClient(port, id), "packet echoed with its own correlation id");
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(300));

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

    GameChannelPool    channels(MAX_CONNECTIONS);
    EchoListener       echo(pool, channels);
    ChannelPoolBinder  pool_binder(channels, echo);
    CountingBinder     binder(pool_binder);
    DefaultPacketCodec codec;
    ConnectionTable    connections(MAX_CONNECTIONS, pool, 8, binder, codec);
    Endpoint           endpoint("127.0.0.1", 0, 16, connections);
    IoLoop             loop(endpoint);

    check(static_cast<bool>(loop), "loop came up");

    if (!static_cast<bool>(loop))
    {
        std::printf("code=%d\n", static_cast<int>(loop.code()));
        return 1;
    }

    check(endpoint.port() != 0, "the kernel picked a port and the endpoint reports it");
    check(endpoint.connectionCount() == 0, "no connections before anything arrives");

    g_loop     = &loop;
    g_endpoint = &endpoint;

    std::thread clients(&runClients);

    loop.run();
    clients.join();

    check(g_opened == 6, "onConnected once per accepted connection");
    check(g_packets == 3, "one request per packet");
    check(g_closed == 6, "onDisconnected once per connection");
    check(endpoint.connectionCount() == 0, "nothing left in the table");
    check(pool.fallbackCount() == 0, "nothing fell through to the heap");

    std::printf("opened=%d packets=%d closed=%d failures=%d\n",
                g_opened, g_packets, g_closed, g_failures);
    std::printf("%s\n", (g_failures == 0) ? "PASS" : "FAIL");

#if defined(_WIN32)
    ::WSACleanup();
#endif

    return (g_failures == 0) ? 0 : 1;
}
