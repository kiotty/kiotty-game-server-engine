#include "kiotty_endpoint.h"

namespace kiotty
{
    Endpoint::Endpoint(const char* ip, uint16_t port, int32_t waiting_limit,
                       ConnectionTable& connections) :
        _sock(ip, port),
        _event(makeAcceptIOEvent(this)),
        _connections(connections),
        _code(SocketCode::SOCKET_SUCCESS)
    {
        if (_sock.handle() == INVALID_SOCKET_HANDLE)
        {
            _code = SocketCode::SOCKET_INVALID_HANDLE;
            return;
        }
        if (!static_cast<bool>(_connections))
        {
            _code = SocketCode::SOCKET_OUT_OF_RESOURCE;
            return;
        }

        _code = _sock.bind();

        if (_code != SocketCode::SOCKET_SUCCESS)
        {
            return;
        }
        _code = _sock.listen(waiting_limit);
    }

    Endpoint::operator bool() const
    {
        return _code == SocketCode::SOCKET_SUCCESS;
    }

    SocketCode Endpoint::submitAccept(IOMultiEventListener& listener)
    {
        if (_code != SocketCode::SOCKET_SUCCESS)
        {
            return _code;
        }

        const IOAcceptRequest request = { _sock.handle() };

        return listener.submit(*_event, request);
    }

    bool Endpoint::handleAccepted(IOMultiEventListener& listener,
                                  IOAcceptResponse& response)
    {
        if (response.code == SocketCode::SOCKET_SUCCESS)
        {
            openConnection(listener, response);
        }

        return submitAccept(listener) == SocketCode::SOCKET_SUCCESS;
    }

    void Endpoint::openConnection(IOMultiEventListener& listener,
                                  IOAcceptResponse& response)
    {
        Connection* const connection = _connections.open(response.accepting, listener);

        if (connection == nullptr)
        {
            return;
        }

        if (!static_cast<bool>(*connection) ||
            connection->submitReceive() != SocketCode::SOCKET_SUCCESS)
        {
            _connections.close(*connection);
        }
    }
}
