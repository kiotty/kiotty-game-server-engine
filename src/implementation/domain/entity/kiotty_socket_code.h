#if !defined(KIOTTY_DOMAIN_ENTITY_SOCKET_CODE_H)
#define KIOTTY_DOMAIN_ENTITY_SOCKET_CODE_H

#include <cstdint>

namespace kiotty
{
    enum class SocketCode : int32_t
    {
        SOCKET_SUCCESS = 0,

        SOCKET_PENDING,

        SOCKET_CLOSED,
        SOCKET_WOULD_BLOCK,
        SOCKET_INTERRUPTED,

        SOCKET_INVALID_HANDLE,
        SOCKET_INVALID_ARGUMENT,
        SOCKET_ADDRESS_IN_USE,
        SOCKET_OUT_OF_RESOURCE,
        SOCKET_NOT_SUPPORTED,

        SOCKET_FAILED,
    };
}

#endif
