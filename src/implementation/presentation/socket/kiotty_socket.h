#if !defined(KIOTTY_PRESENTATION_SOCKET_SOCKET_H)
#define KIOTTY_PRESENTATION_SOCKET_SOCKET_H

#include <core/kiotty_result.h>
#include <domain/entity/kiotty_socket_code.h>

#include <cstddef>
#include <cstdint>

namespace kiotty
{
    struct IOAcceptEvent;
    struct IOTransferEvent;

    typedef intptr_t SocketHandle;

    static const SocketHandle INVALID_SOCKET_HANDLE = -1;

    static const size_t SOCKET_ADDRESS_SIZE = 16;

    typedef Result<SocketCode, size_t> TransferResult;

    class ActiveSocket
    {
    public:
        ActiveSocket() {}

        explicit ActiveSocket(SocketHandle handle);

        ~ActiveSocket();
        ActiveSocket(const ActiveSocket&) = delete;
        ActiveSocket& operator=(const ActiveSocket&) = delete;
        ActiveSocket(ActiveSocket&& other) noexcept;
        ActiveSocket& operator=(ActiveSocket&& other) noexcept;

        SocketHandle handle() const { return _handle; }
        const char* ip() const { return _address; }
        uint16_t port() const { return _port; }

        void shutdown();

        TransferResult send(
            const void* data,
            size_t size,
            IOTransferEvent& event
        );
        TransferResult receive(
            void* data,
            size_t size,
            IOTransferEvent& event
        );

    private:
        SocketHandle _handle {INVALID_SOCKET_HANDLE};
        uint16_t _port {0};
        char _address[SOCKET_ADDRESS_SIZE] {0};
    };

    class PassiveSocket
    {
    public:
        PassiveSocket(const char* ip, uint16_t port);

        ~PassiveSocket();
        PassiveSocket(const PassiveSocket&) = delete;
        PassiveSocket& operator=(const PassiveSocket&) = delete;
        PassiveSocket(PassiveSocket&& other) noexcept;
        PassiveSocket& operator=(PassiveSocket&& other) noexcept;

        SocketHandle handle() const { return _handle; }
        const char* ip() const { return _address; }
        uint16_t port() const { return _port; }

        SocketCode bind();
        SocketCode listen(int32_t backlog);

    private:
        SocketHandle _handle {INVALID_SOCKET_HANDLE};
        uint16_t _port {0};
        char _address[SOCKET_ADDRESS_SIZE] {0};
    };

    SocketCode acceptConnection(SocketHandle listening, IOAcceptEvent& event);
}

#endif
