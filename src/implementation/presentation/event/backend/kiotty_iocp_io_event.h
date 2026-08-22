#if defined(_WIN32)

#if !defined(KIOTTY_PRESENTATION_EVENT_BACKEND_IOCP_IO_EVENT_H)
#define KIOTTY_PRESENTATION_EVENT_BACKEND_IOCP_IO_EVENT_H

#include "../kiotty_io_event.h"

#ifndef _WIN32_WINNT
#  define _WIN32_WINNT 0x0600
#endif

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstddef>
#include <cstring>
#include <type_traits>

namespace kiotty
{
    struct IOEventHeader
    {
        OVERLAPPED  handle;
        IOEventKind kind;
        IOContext*  context;
    };

    struct IocpAcceptEvent : IOAcceptEvent
    {
        IOEventHeader    header;

        IOAcceptRequest  request;
        IOAcceptResponse response;

        IocpAcceptEvent()
        {
            std::memset(&header.handle, 0, sizeof(header.handle));

            header.kind       = IO_EVENT_NONE;
            header.context    = nullptr;
            request.listening = INVALID_SOCKET_HANDLE;
        }
    };

    struct IocpReceiveSlot
    {
        IOEventHeader     header;

        IOReceiveRequest  request;
        IOReceiveResponse response;
    };

    struct IocpSendSlot
    {
        IOEventHeader  header;

        IOSendRequest  request;
        IOSendResponse response;
    };

    struct IocpTransferEvent : IOTransferEvent
    {
        IocpReceiveSlot receive;
        IocpSendSlot    send;

        bool            closing;

        IocpTransferEvent()
        {
            std::memset(&receive.header.handle, 0, sizeof(receive.header.handle));
            std::memset(&send.header.handle, 0, sizeof(send.header.handle));

            closing = false;

            receive.header.kind    = IO_EVENT_NONE;
            receive.header.context = nullptr;
            receive.request.sock   = nullptr;
            receive.request.buffer = nullptr;
            receive.request.length = 0;

            send.header.kind    = IO_EVENT_NONE;
            send.header.context = nullptr;
            send.request.sock   = nullptr;
            send.request.buffer = nullptr;
            send.request.length = 0;
        }
    };

    inline void bindContext(IocpAcceptEvent& event, IOContext* context)
    {
        event.header.context = context;
    }

    inline void bindContext(IocpTransferEvent& event, IOContext* context)
    {
        event.receive.header.context = context;
        event.send.header.context    = context;
    }

    inline IocpTransferEvent& toTransferEvent(IocpReceiveSlot& slot)
    {
        return *reinterpret_cast<IocpTransferEvent*>(
            reinterpret_cast<char*>(&slot) - offsetof(IocpTransferEvent, receive));
    }

    inline IocpTransferEvent& toTransferEvent(IocpSendSlot& slot)
    {
        return *reinterpret_cast<IocpTransferEvent*>(
            reinterpret_cast<char*>(&slot) - offsetof(IocpTransferEvent, send));
    }

    inline bool isIdle(const IocpTransferEvent& event)
    {
        return event.receive.header.kind == IO_EVENT_NONE &&
               event.send.header.kind == IO_EVENT_NONE;
    }

    static_assert(std::is_standard_layout<IOEventHeader>::value &&
                  std::is_standard_layout<IocpAcceptEvent>::value &&
                  std::is_standard_layout<IocpReceiveSlot>::value &&
                  std::is_standard_layout<IocpSendSlot>::value &&
                  std::is_standard_layout<IocpTransferEvent>::value,
                  "a completion casts an OVERLAPPED* back to these, which needs "
                  "the header to really be at offset 0 - and toTransferEvent() "
                  "needs the slot offsets to be the ones offsetof reports");

    static_assert(offsetof(IocpAcceptEvent, header) == 0 &&
                  offsetof(IocpReceiveSlot, header) == 0 &&
                  offsetof(IocpSendSlot, header) == 0,
                  "the header must be first: a completion casts an OVERLAPPED* to "
                  "it and reads kind before it knows what it is holding");

    static_assert(sizeof(IocpAcceptEvent) <= IOAcceptEvent::HOLDER_SIZE,
                  "IocpAcceptEvent outgrew IOAcceptEvent::HOLDER_SIZE - raise it");
    static_assert(sizeof(IocpTransferEvent) <= IOTransferEvent::HOLDER_SIZE,
                  "IocpTransferEvent outgrew IOTransferEvent::HOLDER_SIZE - raise it");
}

#endif

#endif
