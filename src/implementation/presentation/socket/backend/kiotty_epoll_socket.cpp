#if defined(__linux__) && !defined(KIOTTY_HAS_IO_URING)

#include "kiotty_posix_socket_internal.h"
#include <presentation/event/backend/kiotty_epoll_io_event.h>

namespace kiotty
{

    TransferResult ActiveSocket::send(const void* data, size_t size, IOTransferEvent& event)
    {
        (void)event;

        if (_handle == INVALID_SOCKET_HANDLE)
        {
            return error(SocketCode::SOCKET_INVALID_HANDLE);
        }

        const ssize_t sent = ::send(toDescriptor(_handle), data, size, MSG_NOSIGNAL);

        if (sent < 0)
        {
            return error(toSocketCode(errno));
        }
        return ok(static_cast<size_t>(sent));
    }

    TransferResult ActiveSocket::receive(void* data, size_t size, IOTransferEvent& event)
    {
        (void)event;

        if (_handle == INVALID_SOCKET_HANDLE)
        {
            return error(SocketCode::SOCKET_INVALID_HANDLE);
        }

        const ssize_t received = ::recv(toDescriptor(_handle), data, size, 0);

        if (received < 0)
        {
            return error(toSocketCode(errno));
        }

        if (received == 0)
        {
            return error(SocketCode::SOCKET_CLOSED);
        }
        return ok(static_cast<size_t>(received));
    }

    SocketCode acceptConnection(SocketHandle listening, IOAcceptEvent& event)
    {
        if (listening == INVALID_SOCKET_HANDLE)
        {
            return SocketCode::SOCKET_INVALID_HANDLE;
        }

        const int accepted = ::accept4(toDescriptor(listening), nullptr, nullptr,
                                       SOCK_NONBLOCK | SOCK_CLOEXEC);

        if (accepted < 0)
        {
            return toSocketCode(errno);
        }

        EpollAcceptEvent& target = static_cast<EpollAcceptEvent&>(event);

        closeHandle(target.response.accepting);
        target.response.accepting = static_cast<SocketHandle>(accepted);

        return SocketCode::SOCKET_SUCCESS;
    }
}

#endif
