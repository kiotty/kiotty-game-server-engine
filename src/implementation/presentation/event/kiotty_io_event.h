#if !defined(KIOTTY_PRESENTATION_EVENT_IO_EVENT_H)
#define KIOTTY_PRESENTATION_EVENT_IO_EVENT_H

#include <core/kiotty_holder.h>
#include <domain/entity/kiotty_socket_code.h>
#include <presentation/socket/kiotty_socket.h>

#include <cstddef>
#include <cstdint>

namespace kiotty
{
    struct IOContext {};

    typedef uint32_t IOEventKind;

    static const IOEventKind IO_EVENT_NONE     = 0;
    static const IOEventKind IO_EVENT_ACCEPT   = 1u << 1;
    static const IOEventKind IO_EVENT_RECEIVE  = 1u << 2;
    static const IOEventKind IO_EVENT_SEND     = 1u << 3;
    static const IOEventKind IO_EVENT_TRANSFER = IO_EVENT_RECEIVE | IO_EVENT_SEND;

    struct IOEventHeader;

    struct IOAcceptRequest
    {
        SocketHandle listening;
    };

    struct IOReceiveRequest
    {
        ActiveSocket* sock;
        char*         buffer;
        size_t        length;
    };

    struct IOSendRequest
    {
        ActiveSocket* sock;
        const char*   buffer;
        size_t        length;
    };

    struct IOAcceptResponse
    {
        SocketCode   code {SocketCode::SOCKET_SUCCESS};

        SocketHandle accepting {INVALID_SOCKET_HANDLE};
    };

    struct IOReceiveResponse
    {
        SocketCode code {SocketCode::SOCKET_SUCCESS};
        size_t     length {0};
    };

    struct IOSendResponse
    {
        SocketCode code {SocketCode::SOCKET_SUCCESS};
        size_t     length {0};
    };

    inline bool endsConnection(SocketCode code)
    {
        return code == SocketCode::SOCKET_CLOSED ||
               code == SocketCode::SOCKET_FAILED ||
               code == SocketCode::SOCKET_INVALID_HANDLE;
    }

    struct IOAcceptEvent
    {
        static const size_t HOLDER_SIZE  = 96;
        static const size_t HOLDER_ALIGN = 8;
    };

    struct IOTransferEvent
    {
        static const size_t HOLDER_SIZE  = 192;
        static const size_t HOLDER_ALIGN = 8;
    };

    Holder<IOAcceptEvent>   makeAcceptIOEvent(IOContext* context);
    Holder<IOTransferEvent> makeTransferIOEvent(IOContext* context);
}

#endif
