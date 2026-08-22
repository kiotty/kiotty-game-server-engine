#if !defined(KIOTTY_PRESENTATION_IO_LOOP_IO_LOOP_H)
#define KIOTTY_PRESENTATION_IO_LOOP_IO_LOOP_H

#include <domain/entity/kiotty_socket_code.h>
#include <presentation/connection/kiotty_connection.h>
#include <presentation/endpoint/kiotty_endpoint.h>
#include <presentation/event_listener/kiotty_io_event_listener.h>

#include <atomic>
#include <cstdint>

namespace kiotty
{
    class IoLoop
    {
    public:
        explicit IoLoop(Endpoint& endpoint);

        IoLoop(const IoLoop&) = delete;
        IoLoop& operator=(const IoLoop&) = delete;
        IoLoop(IoLoop&&) = delete;
        IoLoop& operator=(IoLoop&&) = delete;

        explicit operator bool() const;

        SocketCode code() const { return _code.load(std::memory_order_acquire); }

        IOMultiEventListener& listener() { return _listener; }

        void run();

        void stop();

    private:
        static void handleAccepted(IOMultiEventListener& listener, IOContext& context,
                                   IOAcceptResponse& response);
        static void handleReceived(IOMultiEventListener& listener, IOContext& context,
                                   IOReceiveResponse& response);
        static void handleSent(IOMultiEventListener& listener, IOContext& context,
                               IOSendResponse& response);
        static void handleDisconnected(IOMultiEventListener& listener, IOContext& context);

        static IOEventCallback callbacks();

        Endpoint&               _endpoint;
        IOMultiEventListener    _listener;
        std::atomic<bool>       _stopped;
        std::atomic<SocketCode> _code;
    };
}

#endif
