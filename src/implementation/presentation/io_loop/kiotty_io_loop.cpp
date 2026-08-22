#include "kiotty_io_loop.h"

namespace kiotty
{
    namespace
    {
        const uint64_t WAIT_HEARTBEAT_MS = 1000;
    }

    IoLoop::IoLoop(Endpoint& endpoint) :
        _endpoint(endpoint),
        _listener(callbacks()),
        _stopped(false),
        _code(SocketCode::SOCKET_SUCCESS)
    {
        if (!static_cast<bool>(_listener))
        {
            _code.store(SocketCode::SOCKET_OUT_OF_RESOURCE, std::memory_order_release);
            return;
        }
        if (!static_cast<bool>(_endpoint))
        {
            _code.store(_endpoint.code(), std::memory_order_release);
        }
    }

    IoLoop::operator bool() const
    {
        return code() == SocketCode::SOCKET_SUCCESS;
    }

    IOEventCallback IoLoop::callbacks()
    {
        IOEventCallback table;

        table.onAccepted     = &IoLoop::handleAccepted;
        table.onReceived     = &IoLoop::handleReceived;
        table.onSent         = &IoLoop::handleSent;
        table.onDisconnected = &IoLoop::handleDisconnected;

        return table;
    }

    void IoLoop::run()
    {
        if (!static_cast<bool>(*this) || _stopped.load(std::memory_order_acquire))
        {
            return;
        }

        const SocketCode armed = _endpoint.submitAccept(_listener);

        _code.store(armed, std::memory_order_release);

        if (armed != SocketCode::SOCKET_SUCCESS)
        {
            return;
        }

        while (!_stopped.load(std::memory_order_acquire))
        {
            const IOEventListenType listened = _listener.wait(WAIT_HEARTBEAT_MS);

            _endpoint.reapClosedConnections();

            if (listened == IOEventListenType::CANCELLED)
            {
                return;
            }
        }
    }

    void IoLoop::stop()
    {
        _stopped.store(true, std::memory_order_release);
        _listener.cancel();
    }

    void IoLoop::handleAccepted(IOMultiEventListener& listener, IOContext& context,
                                IOAcceptResponse& response)
    {
        Endpoint& endpoint = static_cast<Endpoint&>(context);

        if (!endpoint.handleAccepted(listener, response))
        {
            listener.cancel();
        }
    }

    void IoLoop::handleReceived(IOMultiEventListener& listener, IOContext& context,
                                IOReceiveResponse& response)
    {
        (void)listener;

        static_cast<Connection&>(context).handleReceived(response);
    }

    void IoLoop::handleSent(IOMultiEventListener& listener, IOContext& context,
                            IOSendResponse& response)
    {
        (void)listener;

        static_cast<Connection&>(context).handleSent(response);
    }

    void IoLoop::handleDisconnected(IOMultiEventListener& listener, IOContext& context)
    {
        (void)listener;

        static_cast<Connection&>(context).handleDisconnected();
    }
}
