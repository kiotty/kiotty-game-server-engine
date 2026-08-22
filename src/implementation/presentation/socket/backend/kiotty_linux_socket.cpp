#if defined(__linux__)

#include "kiotty_posix_socket_internal.h"

namespace kiotty
{

    namespace
    {
        int createStreamSocket()
        {
            return ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        }

        void copyAddress(const char* source, char (&address)[SOCKET_ADDRESS_SIZE])
        {
            const size_t length = std::strlen(source);
            const size_t copied = (length < SOCKET_ADDRESS_SIZE - 1)
                                      ? length
                                      : SOCKET_ADDRESS_SIZE - 1;

            std::memcpy(address, source, copied);
            address[copied] = '\0';
        }
    }

    ActiveSocket::ActiveSocket(SocketHandle handle) :
        _handle(handle)
    {
        if (_handle == INVALID_SOCKET_HANDLE)
        {
            return;
        }

        sockaddr_in peer;
        socklen_t   length = sizeof(peer);
        std::memset(&peer, 0, sizeof(peer));

        if (::getpeername(toDescriptor(_handle),
                          reinterpret_cast<sockaddr*>(&peer), &length) == 0)
        {
            ::inet_ntop(AF_INET, &peer.sin_addr, _address, SOCKET_ADDRESS_SIZE);
            _port = ::ntohs(peer.sin_port);
        }
    }

    ActiveSocket::~ActiveSocket()
    {
        closeHandle(_handle);
    }

    ActiveSocket::ActiveSocket(ActiveSocket&& other) noexcept :
        _handle(other._handle),
        _port(other._port)
    {
        std::memcpy(_address, other._address, sizeof(_address));
        other._handle = INVALID_SOCKET_HANDLE;
    }

    ActiveSocket& ActiveSocket::operator=(ActiveSocket&& other) noexcept
    {
        if (this != &other)
        {
            closeHandle(_handle);
            _handle = other._handle;
            _port   = other._port;
            std::memcpy(_address, other._address, sizeof(_address));
            other._handle = INVALID_SOCKET_HANDLE;
        }
        return *this;
    }

    void ActiveSocket::shutdown()
    {
        if (_handle == INVALID_SOCKET_HANDLE)
        {
            return;
        }
        ::shutdown(toDescriptor(_handle), SHUT_RDWR);
    }

    PassiveSocket::PassiveSocket(const char* ip, uint16_t port) :
        _handle(static_cast<SocketHandle>(createStreamSocket())),
        _port(port)
    {
        copyAddress((ip != nullptr) ? ip : "0.0.0.0", _address);
    }

    PassiveSocket::~PassiveSocket()
    {
        closeHandle(_handle);
    }

    PassiveSocket::PassiveSocket(PassiveSocket&& other) noexcept :
        _handle(other._handle),
        _port(other._port)
    {
        std::memcpy(_address, other._address, sizeof(_address));
        other._handle = INVALID_SOCKET_HANDLE;
    }

    PassiveSocket& PassiveSocket::operator=(PassiveSocket&& other) noexcept
    {
        if (this != &other)
        {
            closeHandle(_handle);
            _handle = other._handle;
            _port   = other._port;
            std::memcpy(_address, other._address, sizeof(_address));
            other._handle = INVALID_SOCKET_HANDLE;
        }
        return *this;
    }

    SocketCode PassiveSocket::bind()
    {
        if (_handle == INVALID_SOCKET_HANDLE)
        {
            return SocketCode::SOCKET_INVALID_HANDLE;
        }

        sockaddr_in address;
        std::memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_port   = ::htons(_port);

        if (::inet_pton(AF_INET, _address, &address.sin_addr) != 1)
        {
            return SocketCode::SOCKET_INVALID_ARGUMENT;
        }

        int reuse = 1;
        ::setsockopt(toDescriptor(_handle), SOL_SOCKET, SO_REUSEADDR,
                     &reuse, sizeof(reuse));

        if (::bind(toDescriptor(_handle),
                   reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
        {
            return toSocketCode(errno);
        }

        sockaddr_in bound;
        socklen_t   bound_length = sizeof(bound);
        std::memset(&bound, 0, sizeof(bound));

        if (::getsockname(toDescriptor(_handle),
                          reinterpret_cast<sockaddr*>(&bound), &bound_length) == 0)
        {
            _port = ::ntohs(bound.sin_port);
        }
        return SocketCode::SOCKET_SUCCESS;
    }

    SocketCode PassiveSocket::listen(int32_t backlog)
    {
        if (_handle == INVALID_SOCKET_HANDLE)
        {
            return SocketCode::SOCKET_INVALID_HANDLE;
        }

        if (::listen(toDescriptor(_handle), backlog) != 0)
        {
            return toSocketCode(errno);
        }
        return SocketCode::SOCKET_SUCCESS;
    }
}

#endif
