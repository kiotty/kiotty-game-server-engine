// End-to-end smoke test for the presentation layer. Not a unit test - it drives
// one real client through the whole path and checks the bytes come back:
//
//   accept -> receive -> send (from the loop thread, inside a callback)
//          -> send again (from a worker thread, which is the case every backend
//             handles differently)
//          -> arm a receive and a send at once, let the client close, and check
//             onDisconnected arrives exactly once
//
// The last leg is the one worth explaining. onDisconnected has to wait until
// nothing is left in flight, and the way to get that wrong is to fire it per
// failed operation - so the test deliberately has both directions outstanding
// when the peer goes away, which is the only shape where a double call shows up.
//
// Built against whichever backend the platform selects.

#include <core/kiotty_block_pool.h>
#include <datalayer/repository/cryptor/kiotty_secure_random.h>
#include <datalayer/repository/session/kiotty_session_repository.h>
#include <domain/channel/kiotty_channel_binder.h>
#include <domain/channel/kiotty_game_channel_pool.h>
#include <domain/codec/kiotty_packet_codec.h>
#include <presentation/connection/kiotty_connection_table.h>
#include <presentation/endpoint/kiotty_endpoint.h>
#include <presentation/event_listener/kiotty_io_event_listener.h>
#include <presentation/event/kiotty_io_event.h>
#include <presentation/socket/kiotty_socket.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
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

// 0 means "any free port". A fixed one makes the test fail after a previous run,
// because Windows exclusive-use binding counts sockets still in TIME_WAIT.
static const uint16_t TEST_PORT = 0;

static const char ECHO_PAYLOAD[]  = "kiotty-ping";
static const char EVENT_PAYLOAD[] = "kiotty-event";

static const size_t ECHO_SIZE  = sizeof(ECHO_PAYLOAD) - 1;
static const size_t EVENT_SIZE = sizeof(EVENT_PAYLOAD) - 1;

namespace
{
    // A connection runs its receive and its send through one transfer event.
    // This is deliberately not the engine's Connection: the point here is the
    // listener's own guarantees, so the transfer event is driven by hand rather
    // than through a state machine that could hide them.
    struct TransferPeer : IOContext
    {
        ActiveSocket            sock;
        Holder<IOTransferEvent> event;
        char                    buffer[64];

        TransferPeer() :
            event(makeTransferIOEvent(this))
        {
            std::memset(buffer, 0, sizeof(buffer));
        }
    };

    // For the same reason this test takes the accepted handle itself and never
    // lets the endpoint open a connection, so the table it is built with is
    // never asked for anything.
    struct UnusedRequests : StreamListener<GameRequest>
    {
        void onStream(const GameRequest&) override {}
    };

    struct DropOrphans : ISessionPolicy
    {
        uint32_t orphanLifetimeMs(const Session&) const override { return 0; }
        bool     replacesPreviousLogin(const AccountId&) const override { return true; }
    };

    IOMultiEventListener* g_listener   = nullptr;
    Endpoint*             g_endpoint   = nullptr;
    TransferPeer*         g_connection = nullptr;

    uint16_t g_bound_port = 0;

    int g_accepted     = 0;
    int g_received     = 0;
    int g_sent         = 0;
    int g_disconnected = 0;
    int g_failures     = 0;

    // Which leg of the test is running. After the two sends are through the
    // connection is deliberately torn down, so completions stop being successes
    // and a failing code is the expected result rather than a bug.
    enum Phase
    {
        PHASE_ECHO,
        PHASE_DISCONNECT,
    };

    Phase g_phase = PHASE_ECHO;

    // Gate the worker thread opens through once the loop-thread send is done.
    std::mutex              g_gate_lock;
    std::condition_variable g_gate_signal;
    bool                    g_gate_open = false;
    bool                    g_gate_quit = false;

    void onAccepted(IOMultiEventListener& listener, IOContext& context,
                    IOAcceptResponse& response)
    {
        if (&context != g_endpoint)
        {
            std::printf("[FAIL] accept context did not round trip\n");
            ++g_failures;
            listener.cancel();
            return;
        }

        if (response.code != SocketCode::SOCKET_SUCCESS)
        {
            std::printf("[FAIL] accept code=%d\n", static_cast<int>(response.code));
            ++g_failures;
            listener.cancel();
            return;
        }

        // Take the handle, or the listener closes it when the callback returns.
        g_connection->sock = ActiveSocket(response.accepting);
        response.accepting = INVALID_SOCKET_HANDLE;

        ++g_accepted;
        std::printf("[ok] accepted from %s:%u\n",
                    g_connection->sock.ip(), g_connection->sock.port());

        const IOReceiveRequest request = {
            &g_connection->sock, g_connection->buffer, sizeof(g_connection->buffer)
        };

        if (listener.submit(*g_connection->event, request)
            != SocketCode::SOCKET_SUCCESS)
        {
            std::printf("[FAIL] submit receive\n");
            ++g_failures;
            listener.cancel();
        }
    }

    void onReceived(IOMultiEventListener& listener, IOContext& context,
                    IOReceiveResponse& response)
    {
        TransferPeer* const connection = static_cast<TransferPeer*>(&context);

        if (connection != g_connection)
        {
            std::printf("[FAIL] receive context did not round trip\n");
            ++g_failures;
            listener.cancel();
            return;
        }

        if (g_phase == PHASE_DISCONNECT)
        {
            // The peer is gone, so this is the completion that ends the
            // connection. onDisconnected is what has to follow, and it must not
            // follow from here - a send may still be inside the kernel.
            std::printf("[ok] disconnect-phase receive code=%d\n",
                        static_cast<int>(response.code));

            if (response.code == SocketCode::SOCKET_SUCCESS)
            {
                std::printf("[FAIL] the peer closed but the receive succeeded\n");
                ++g_failures;
                listener.cancel();
            }
            return;
        }

        if (response.code != SocketCode::SOCKET_SUCCESS)
        {
            std::printf("[FAIL] receive code=%d\n", static_cast<int>(response.code));
            ++g_failures;
            listener.cancel();
            return;
        }

        ++g_received;
        std::printf("[ok] received %u bytes: %.*s\n",
                    static_cast<unsigned>(response.length),
                    static_cast<int>(response.length), connection->buffer);

        if (response.length != ECHO_SIZE ||
            std::memcmp(connection->buffer, ECHO_PAYLOAD, response.length) != 0)
        {
            std::printf("[FAIL] payload mismatch\n");
            ++g_failures;
            listener.cancel();
            return;
        }

        // Echo it straight back out of the same buffer, from the loop thread.
        const IOSendRequest request = {
            &connection->sock, connection->buffer, response.length
        };

        if (listener.submit(*connection->event, request)
            != SocketCode::SOCKET_SUCCESS)
        {
            std::printf("[FAIL] submit send\n");
            ++g_failures;
            listener.cancel();
        }
    }

    void onSent(IOMultiEventListener& listener, IOContext& context,
                IOSendResponse& response)
    {
        TransferPeer* const connection = static_cast<TransferPeer*>(&context);

        if (g_phase == PHASE_DISCONNECT)
        {
            // Either outcome is fine here. A peer that closed after reading only
            // part of what was sent resets the connection, so this send fails;
            // one that closed cleanly leaves it buffered, so it succeeds. What
            // matters is that it completes before onDisconnected does.
            std::printf("[ok] disconnect-phase send code=%d\n",
                        static_cast<int>(response.code));
            return;
        }

        if (response.code != SocketCode::SOCKET_SUCCESS)
        {
            std::printf("[FAIL] send code=%d\n", static_cast<int>(response.code));
            ++g_failures;
            listener.cancel();
            return;
        }

        ++g_sent;
        std::printf("[ok] sent %u bytes (send #%d)\n",
                    static_cast<unsigned>(response.length), g_sent);

        if (g_sent == 1)
        {
            // Hand the next send to a thread that is not the loop, which is the
            // case each backend solves differently: IOCP issues it there and
            // then, epoll arms EPOLLOUT, io_uring takes the submission lock.
            std::lock_guard<std::mutex> guard(g_gate_lock);
            g_gate_open = true;
            g_gate_signal.notify_one();
            return;
        }

        // Both sends are through. Arm both directions and leave them there: the
        // client is about to close, and the point of the leg is that whichever
        // of the two completes last is the one that reports the disconnect.
        g_phase = PHASE_DISCONNECT;

        const IOReceiveRequest receive_request = {
            &connection->sock, connection->buffer, sizeof(connection->buffer)
        };
        const IOSendRequest send_request = {
            &connection->sock, EVENT_PAYLOAD, EVENT_SIZE
        };

        if (listener.submit(*connection->event, receive_request)
            != SocketCode::SOCKET_SUCCESS)
        {
            std::printf("[FAIL] submit receive for the disconnect leg\n");
            ++g_failures;
            listener.cancel();
            return;
        }

        if (listener.submit(*connection->event, send_request)
            != SocketCode::SOCKET_SUCCESS)
        {
            std::printf("[FAIL] submit send for the disconnect leg\n");
            ++g_failures;
            listener.cancel();
        }
    }

    void onDisconnected(IOMultiEventListener& listener, IOContext& context)
    {
        if (&context != g_connection)
        {
            std::printf("[FAIL] disconnect context did not round trip\n");
            ++g_failures;
            listener.cancel();
            return;
        }

        ++g_disconnected;
        std::printf("[ok] disconnected (call #%d)\n", g_disconnected);

        // Nothing is in flight and nothing more can be submitted, so this is the
        // point where a real server would destroy the connection or hand it back
        // to its pool. A second re-arm must be refused rather than accepted.
        const IOReceiveRequest request = {
            &g_connection->sock, g_connection->buffer, sizeof(g_connection->buffer)
        };

        const SocketCode code = listener.submit(*g_connection->event, request);

        if (code != SocketCode::SOCKET_CLOSED)
        {
            std::printf("[FAIL] a closed connection took a submit, code=%d\n",
                        static_cast<int>(code));
            ++g_failures;
        }

        listener.cancel();
    }

    void onInterrupted(IOMultiEventListener& listener)
    {
        (void)listener;
        std::printf("[ok] interrupted\n");
    }

    // Stands in for a worker that produced something to push to a client.
    void runWorker()
    {
        {
            std::unique_lock<std::mutex> guard(g_gate_lock);
            g_gate_signal.wait(guard, [] { return g_gate_open || g_gate_quit; });

            if (g_gate_quit)
            {
                return;
            }
        }

        const IOSendRequest request = {
            &g_connection->sock, EVENT_PAYLOAD, EVENT_SIZE
        };

        const SocketCode code =
            g_listener->submit(*g_connection->event, request);

        if (code != SocketCode::SOCKET_SUCCESS)
        {
            std::printf("[FAIL] worker submit send code=%d\n", static_cast<int>(code));
            ++g_failures;
            g_listener->cancel();
            return;
        }
        std::printf("[ok] worker thread submitted a send\n");
    }

    // A plain blocking client, deliberately not using the engine's own sockets.
    void runClient()
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

#if defined(_WIN32)
        const SOCKET fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#else
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
#endif

        sockaddr_in address;
        std::memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_port   = ::htons(g_bound_port);
        ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);

        if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
        {
            std::printf("[FAIL] client connect\n");
            ++g_failures;
            return;
        }

        ::send(fd, ECHO_PAYLOAD, static_cast<int>(ECHO_SIZE), 0);

        // TCP is a stream: the two server sends may arrive as one read or three.
        // Only ever ask for what is being checked, because the disconnect leg
        // pushes one more payload behind them and it can land in the same read.
        // Leaving it unread is deliberate - closing on top of it is what resets
        // the connection, which is the harsher of the two paths into
        // onDisconnected.
        const size_t expected = ECHO_SIZE + EVENT_SIZE;

        char   received[64];
        size_t filled = 0;

        std::memset(received, 0, sizeof(received));

        while (filled < expected)
        {
            const int back = static_cast<int>(
                ::recv(fd, received + filled,
                       static_cast<int>(expected - filled), 0));

            if (back <= 0)
            {
                break;
            }
            filled += static_cast<size_t>(back);
        }

        if (filled != expected ||
            std::memcmp(received, ECHO_PAYLOAD, ECHO_SIZE) != 0 ||
            std::memcmp(received + ECHO_SIZE, EVENT_PAYLOAD, EVENT_SIZE) != 0)
        {
            std::printf("[FAIL] client got %u bytes: %.*s\n",
                        static_cast<unsigned>(filled),
                        static_cast<int>(filled), received);
            ++g_failures;
        }
        else
        {
            std::printf("[ok] client got the echo and the worker event\n");
        }

#if defined(_WIN32)
        ::closesocket(fd);
#else
        ::close(fd);
#endif
    }
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    IOEventCallback callbacks;
    callbacks.onAccepted     = &onAccepted;
    callbacks.onReceived     = &onReceived;
    callbacks.onSent         = &onSent;
    callbacks.onDisconnected = &onDisconnected;
    callbacks.onInterrupted  = &onInterrupted;

    IOMultiEventListener listener(callbacks);

    if (!static_cast<bool>(listener))
    {
        std::printf("[FAIL] listener did not open\n");
        return 1;
    }

    // socket, bind and listen all happen here, so there is nothing to check
    // between them - either the endpoint came up or it says why it did not.
    BlockPool          pool(defaultBlockClasses(), defaultBlockClassCount());
    GameChannelPool    channels(1);
    UnusedRequests     requests;
    DropOrphans        policy;
    SecureRandom       random;
    SessionRepository  sessions(channels, policy, random, 1);
    ChannelPoolBinder  binder(channels, sessions, requests);
    DefaultPacketCodec codec;
    ConnectionTable    connections(1, pool, 8, binder, codec);
    Endpoint           endpoint("127.0.0.1", TEST_PORT, 16, connections);
    TransferPeer connection;

    g_listener   = &listener;
    g_endpoint   = &endpoint;
    g_connection = &connection;

    if (!static_cast<bool>(endpoint))
    {
        std::printf("[FAIL] endpoint code=%d\n", static_cast<int>(endpoint.code()));
        return 1;
    }

    g_bound_port = endpoint.port();

    std::printf("[ok] listening on %s:%u\n", endpoint.ip(), g_bound_port);

    if (endpoint.submitAccept(listener) != SocketCode::SOCKET_SUCCESS)
    {
        std::printf("[FAIL] submit accept\n");
        return 1;
    }

    std::thread worker(&runWorker);
    std::thread client(&runClient);

    for (int round = 0; round < 50; ++round)
    {
        if (listener.wait(200) == IOEventListenType::CANCELLED)
        {
            break;
        }
    }

    {
        std::lock_guard<std::mutex> guard(g_gate_lock);
        g_gate_quit = true;
        g_gate_signal.notify_one();
    }

    worker.join();
    client.join();

    std::printf("accepted=%d received=%d sent=%d disconnected=%d failures=%d\n",
                g_accepted, g_received, g_sent, g_disconnected, g_failures);

    // disconnected == 1 exactly. Zero means the callback never came - the bug
    // the submit refusal exists to prevent - and two means it fired per failed
    // operation instead of once per connection.
    const bool passed = (g_accepted == 1) && (g_received == 1) &&
                        (g_sent == 2) && (g_disconnected == 1) &&
                        (g_failures == 0);

    std::printf("%s\n", passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
