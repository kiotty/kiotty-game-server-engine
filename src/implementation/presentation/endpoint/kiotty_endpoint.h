#if !defined(KIOTTY_PRESENTATION_ENDPOINT_ENDPOINT_H)
#define KIOTTY_PRESENTATION_ENDPOINT_ENDPOINT_H

#include <core/kiotty_holder.h>
#include <domain/entity/kiotty_socket_code.h>
#include <presentation/connection/kiotty_connection_table.h>
#include <presentation/event/kiotty_io_event.h>
#include <presentation/event_listener/kiotty_io_event_listener.h>
#include <presentation/socket/kiotty_socket.h>

#include <cstdint>

namespace kiotty
{
    class Endpoint : public IOContext
    {
    public:
        Endpoint(const char* ip, uint16_t port, int32_t waiting_limit,
                 ConnectionTable& connections);

        Endpoint(const Endpoint&) = delete;
        Endpoint& operator=(const Endpoint&) = delete;
        Endpoint(Endpoint&&) = delete;
        Endpoint& operator=(Endpoint&&) = delete;

        explicit operator bool() const;

        SocketCode code() const { return _code; }

        const char* ip() const { return _sock.ip(); }

        uint16_t port() const { return _sock.port(); }

        size_t connectionCount() const { return _connections.size(); }
        size_t connectionCapacity() const { return _connections.capacity(); }

        SocketCode submitAccept(IOMultiEventListener& listener);

        bool handleAccepted(IOMultiEventListener& listener, IOAcceptResponse& response);

        void reapClosedConnections() { _connections.reapClosed(); }

    private:
        void openConnection(IOMultiEventListener& listener, IOAcceptResponse& response);

        PassiveSocket         _sock;
        Holder<IOAcceptEvent> _event;
        ConnectionTable&      _connections;
        SocketCode            _code;
    };
}

#endif
