#if defined(__linux__) && !defined(KIOTTY_HAS_IO_URING)

#if !defined(KIOTTY_PRESENTATION_EVENT_BACKEND_EPOLL_IO_EVENT_H)
#define KIOTTY_PRESENTATION_EVENT_BACKEND_EPOLL_IO_EVENT_H

#include "../kiotty_io_event.h"

#include <cstddef>

namespace kiotty
{
    struct IOEventHeader
    {
        IOEventKind kind;
        IOContext*  context;
    };

    struct EpollAcceptEvent : IOAcceptEvent
    {
        IOEventHeader    header;

        IOAcceptRequest  request;
        IOAcceptResponse response;

        EpollAcceptEvent()
        {
            header.kind       = IO_EVENT_NONE;
            header.context    = nullptr;
            request.listening = INVALID_SOCKET_HANDLE;
        }
    };

    struct EpollTransferEvent : IOTransferEvent
    {
        IOEventHeader     header;

        IOReceiveRequest  receive_request;
        IOReceiveResponse receive_response;
        IOSendRequest     send_request;
        IOSendResponse    send_response;

        bool              closing;

        EpollTransferEvent()
        {
            header.kind    = IO_EVENT_NONE;
            header.context = nullptr;
            closing        = false;

            receive_request.sock   = nullptr;
            receive_request.buffer = nullptr;
            receive_request.length = 0;

            send_request.sock   = nullptr;
            send_request.buffer = nullptr;
            send_request.length = 0;
        }
    };

    inline void bindContext(EpollAcceptEvent& event, IOContext* context)
    {
        event.header.context = context;
    }

    inline void bindContext(EpollTransferEvent& event, IOContext* context)
    {
        event.header.context = context;
    }

    inline bool isIdle(const EpollTransferEvent& event)
    {
        return (event.header.kind & IO_EVENT_TRANSFER) == 0;
    }

    static_assert(offsetof(EpollAcceptEvent, header) == 0 &&
                  offsetof(EpollTransferEvent, header) == 0,
                  "the header must be first: a wakeup casts a bare pointer to it "
                  "and reads kind before it knows which event it has");

    static_assert(sizeof(EpollAcceptEvent) <= IOAcceptEvent::HOLDER_SIZE,
                  "EpollAcceptEvent outgrew IOAcceptEvent::HOLDER_SIZE - raise it");
    static_assert(sizeof(EpollTransferEvent) <= IOTransferEvent::HOLDER_SIZE,
                  "EpollTransferEvent outgrew IOTransferEvent::HOLDER_SIZE - raise it");
}

#endif

#endif
