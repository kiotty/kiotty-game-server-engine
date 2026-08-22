#if defined(KIOTTY_HAS_IO_URING)

#include "kiotty_posix_socket_internal.h"
#include <presentation/event/backend/kiotty_io_uring_io_event.h>

namespace kiotty
{

    TransferResult ActiveSocket::send(const void* data, size_t size, IOTransferEvent& event)
    {
        if (_handle == INVALID_SOCKET_HANDLE)
        {
            return error(SocketCode::SOCKET_INVALID_HANDLE);
        }

        IoUringSendSlot& slot = static_cast<IoUringTransferEvent&>(event).send;

        if (slot.header.handle == nullptr)
        {
            return error(SocketCode::SOCKET_OUT_OF_RESOURCE);
        }

        ::io_uring_prep_send(slot.header.handle, toDescriptor(_handle),
                             data, size, MSG_NOSIGNAL);
        ::io_uring_sqe_set_data(slot.header.handle, &slot);

        return error(SocketCode::SOCKET_PENDING);
    }

    TransferResult ActiveSocket::receive(void* data, size_t size, IOTransferEvent& event)
    {
        if (_handle == INVALID_SOCKET_HANDLE)
        {
            return error(SocketCode::SOCKET_INVALID_HANDLE);
        }

        IoUringReceiveSlot& slot = static_cast<IoUringTransferEvent&>(event).receive;

        if (slot.header.handle == nullptr)
        {
            return error(SocketCode::SOCKET_OUT_OF_RESOURCE);
        }

        ::io_uring_prep_recv(slot.header.handle, toDescriptor(_handle), data, size, 0);
        ::io_uring_sqe_set_data(slot.header.handle, &slot);

        return error(SocketCode::SOCKET_PENDING);
    }

    SocketCode acceptConnection(SocketHandle listening, IOAcceptEvent& event)
    {
        if (listening == INVALID_SOCKET_HANDLE)
        {
            return SocketCode::SOCKET_INVALID_HANDLE;
        }

        IoUringAcceptEvent& target = static_cast<IoUringAcceptEvent&>(event);

        if (target.header.handle == nullptr)
        {
            return SocketCode::SOCKET_OUT_OF_RESOURCE;
        }

        ::io_uring_prep_accept(target.header.handle, toDescriptor(listening),
                               nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        ::io_uring_sqe_set_data(target.header.handle, &target);

        return SocketCode::SOCKET_PENDING;
    }
}

#endif
