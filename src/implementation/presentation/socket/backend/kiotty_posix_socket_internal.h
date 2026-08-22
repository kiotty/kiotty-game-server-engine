#if defined(__linux__)

#if !defined(KIOTTY_PRESENTATION_SOCKET_BACKEND_POSIX_SOCKET_INTERNAL_H)
#define KIOTTY_PRESENTATION_SOCKET_BACKEND_POSIX_SOCKET_INTERNAL_H

#include "../kiotty_socket.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace kiotty
{

    inline int toDescriptor(SocketHandle handle)
    {
        return static_cast<int>(handle);
    }

    inline void closeHandle(SocketHandle& handle)
    {
        if (handle != INVALID_SOCKET_HANDLE)
        {
            ::close(toDescriptor(handle));
            handle = INVALID_SOCKET_HANDLE;
        }
    }

    inline SocketCode toSocketCode(int error_number)
    {
        switch (error_number)
        {
        case 0:             return SocketCode::SOCKET_SUCCESS;
        case EAGAIN:
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
        case EWOULDBLOCK:
#endif
        case EINPROGRESS:   return SocketCode::SOCKET_WOULD_BLOCK;
        case ECONNRESET:
        case ECONNABORTED:
        case EPIPE:
        case ENOTCONN:      return SocketCode::SOCKET_CLOSED;
        case EINTR:         return SocketCode::SOCKET_INTERRUPTED;
        case EBADF:
        case ENOTSOCK:      return SocketCode::SOCKET_INVALID_HANDLE;
        case EINVAL:
        case EFAULT:        return SocketCode::SOCKET_INVALID_ARGUMENT;
        case EADDRINUSE:    return SocketCode::SOCKET_ADDRESS_IN_USE;
        case EMFILE:
        case ENFILE:
        case ENOBUFS:
        case ENOMEM:        return SocketCode::SOCKET_OUT_OF_RESOURCE;
        case EOPNOTSUPP:
        case EAFNOSUPPORT:  return SocketCode::SOCKET_NOT_SUPPORTED;
        default:            return SocketCode::SOCKET_FAILED;
        }
    }


}

#endif

#endif
