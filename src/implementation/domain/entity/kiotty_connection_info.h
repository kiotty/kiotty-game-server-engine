#if !defined(KIOTTY_DOMAIN_ENTITY_CONNECTION_INFO_H)
#define KIOTTY_DOMAIN_ENTITY_CONNECTION_INFO_H

#include <cstddef>
#include <cstdint>

namespace kiotty
{
    static const size_t CONNECTION_IPV4_SIZE = 16;

    struct ConnectionInfo
    {
        char     ip[CONNECTION_IPV4_SIZE] {0};
        uint16_t port {0};
    };

    inline ConnectionInfo makeConnectionInfo(const char* ip, uint16_t port)
    {
        ConnectionInfo info;
        info.port = port;

        if (ip == nullptr)
        {
            return info;
        }

        for (size_t i = 0; i < CONNECTION_IPV4_SIZE - 1 && ip[i] != 0; ++i)
        {
            info.ip[i] = ip[i];
        }
        return info;
    }
}

#endif
