#if defined(_WIN32)

#include "../kiotty_socket.h"
#include <presentation/event/backend/kiotty_iocp_io_event.h>

#include <mswsock.h>

#include <cstring>

#pragma comment(lib, "ws2_32.lib")

#pragma comment(lib, "mswsock.lib")

namespace kiotty
{
    namespace
    {
        SOCKET toWinsock(SocketHandle handle)
        {
            return static_cast<SOCKET>(handle);
        }

        void closeHandle(SocketHandle& handle)
        {
            if (handle != INVALID_SOCKET_HANDLE)
            {
                ::closesocket(toWinsock(handle));
                handle = INVALID_SOCKET_HANDLE;
            }
        }

        SocketCode toSocketCode(int error)
        {
            switch (error)
            {
            case 0:                  return SocketCode::SOCKET_SUCCESS;
            case WSA_IO_PENDING:     return SocketCode::SOCKET_PENDING;
            case WSAEWOULDBLOCK:     return SocketCode::SOCKET_WOULD_BLOCK;
            case WSAECONNRESET:
            case WSAECONNABORTED:
            case WSAENETRESET:
            case WSAESHUTDOWN:       return SocketCode::SOCKET_CLOSED;
            case WSAEINTR:           return SocketCode::SOCKET_INTERRUPTED;
            case WSAENOTSOCK:
            case WSAEBADF:           return SocketCode::SOCKET_INVALID_HANDLE;
            case WSAEINVAL:
            case WSAEFAULT:          return SocketCode::SOCKET_INVALID_ARGUMENT;
            case WSAEADDRINUSE:      return SocketCode::SOCKET_ADDRESS_IN_USE;
            case WSAEMFILE:
            case WSAENOBUFS:         return SocketCode::SOCKET_OUT_OF_RESOURCE;
            case WSAEOPNOTSUPP:
            case WSAEAFNOSUPPORT:    return SocketCode::SOCKET_NOT_SUPPORTED;
            default:                 return SocketCode::SOCKET_FAILED;
            }
        }

        bool ensureWinsockStarted()
        {
            static const bool started = []() -> bool
            {
                WSADATA data;
                return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
            }();
            return started;
        }

        SOCKET createOverlappedSocket()
        {
            if (!ensureWinsockStarted())
            {
                return INVALID_SOCKET;
            }
            return ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP,
                                nullptr, 0, WSA_FLAG_OVERLAPPED);
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
        int         length = sizeof(peer);
        std::memset(&peer, 0, sizeof(peer));

        if (::getpeername(toWinsock(_handle),
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
        ::shutdown(toWinsock(_handle), SD_BOTH);
    }

    TransferResult ActiveSocket::send(const void* data, size_t size, IOTransferEvent& event)
    {
        if (_handle == INVALID_SOCKET_HANDLE)
        {
            return error(SocketCode::SOCKET_INVALID_HANDLE);
        }

        IocpSendSlot& slot = static_cast<IocpTransferEvent&>(event).send;

        WSABUF buffer;
        buffer.buf = static_cast<char*>(const_cast<void*>(data));
        buffer.len = static_cast<ULONG>(size);

        const int result = ::WSASend(toWinsock(_handle), &buffer, 1,
                                     nullptr, 0,
                                     &slot.header.handle, nullptr);

        if (result == 0)
        {
            return error(SocketCode::SOCKET_PENDING);
        }

        const int last_error = ::WSAGetLastError();

        if (last_error == WSA_IO_PENDING)
        {
            return error(SocketCode::SOCKET_PENDING);
        }
        return error(toSocketCode(last_error));
    }

    TransferResult ActiveSocket::receive(void* data, size_t size, IOTransferEvent& event)
    {
        if (_handle == INVALID_SOCKET_HANDLE)
        {
            return error(SocketCode::SOCKET_INVALID_HANDLE);
        }

        IocpReceiveSlot& slot = static_cast<IocpTransferEvent&>(event).receive;

        WSABUF buffer;
        buffer.buf = static_cast<char*>(data);
        buffer.len = static_cast<ULONG>(size);

        DWORD flags = 0;

        const int result = ::WSARecv(toWinsock(_handle), &buffer, 1,
                                     nullptr, &flags,
                                     &slot.header.handle, nullptr);

        if (result == 0)
        {
            return error(SocketCode::SOCKET_PENDING);
        }

        const int last_error = ::WSAGetLastError();

        if (last_error == WSA_IO_PENDING)
        {
            return error(SocketCode::SOCKET_PENDING);
        }
        return error(toSocketCode(last_error));
    }

    PassiveSocket::PassiveSocket(const char* ip, uint16_t port) :
        _handle(static_cast<SocketHandle>(createOverlappedSocket())),
        _port(port)
    {
        copyAddress((ip != nullptr) ? ip : "0.0.0.0", _address);

        if (toWinsock(_handle) == INVALID_SOCKET)
        {
            _handle = INVALID_SOCKET_HANDLE;
        }
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

        BOOL exclusive = TRUE;
        ::setsockopt(toWinsock(_handle), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                     reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));

        if (::bind(toWinsock(_handle),
                   reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
        {
            return toSocketCode(::WSAGetLastError());
        }

        sockaddr_in bound;
        int         bound_length = sizeof(bound);
        std::memset(&bound, 0, sizeof(bound));

        if (::getsockname(toWinsock(_handle),
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

        if (::listen(toWinsock(_handle), backlog) != 0)
        {
            return toSocketCode(::WSAGetLastError());
        }
        return SocketCode::SOCKET_SUCCESS;
    }

    SocketCode acceptConnection(SocketHandle listening, IOAcceptEvent& event)
    {
        if (listening == INVALID_SOCKET_HANDLE)
        {
            return SocketCode::SOCKET_INVALID_HANDLE;
        }

        IocpAcceptEvent& target = static_cast<IocpAcceptEvent&>(event);

        closeHandle(target.response.accepting);

        const SOCKET pending = createOverlappedSocket();

        if (pending == INVALID_SOCKET)
        {
            return SocketCode::SOCKET_OUT_OF_RESOURCE;
        }

        target.response.accepting = static_cast<SocketHandle>(pending);

        static char address_buffer[(sizeof(sockaddr_in) + 16) * 2];

        DWORD received = 0;

        const BOOL ok = ::AcceptEx(static_cast<SOCKET>(listening),
                                   pending,
                                   address_buffer,
                                   0,
                                   sizeof(sockaddr_in) + 16,
                                   sizeof(sockaddr_in) + 16,
                                   &received,
                                   &target.header.handle);

        if (ok)
        {
            return SocketCode::SOCKET_PENDING;
        }

        const int last_error = ::WSAGetLastError();

        if (last_error == WSA_IO_PENDING || last_error == ERROR_IO_PENDING)
        {
            return SocketCode::SOCKET_PENDING;
        }

        closeHandle(target.response.accepting);
        return toSocketCode(last_error);
    }
}

#endif
