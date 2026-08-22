#if defined(KIOTTY_HAS_IO_URING)

#if !defined(KIOTTY_PRESENTATION_EVENT_BACKEND_IO_URING_IO_EVENT_H)
#define KIOTTY_PRESENTATION_EVENT_BACKEND_IO_URING_IO_EVENT_H

#include "../kiotty_io_event.h"

#include <cstddef>
#include <type_traits>

#include <liburing.h>

namespace kiotty
{
    struct IOEventHeader
    {
        io_uring_sqe* handle;
        IOEventKind   kind;
        IOContext*    context;
    };

    struct IoUringAcceptEvent : IOAcceptEvent
    {
        IOEventHeader    header;

        IOAcceptRequest  request;
        IOAcceptResponse response;

        IoUringAcceptEvent()
        {
            header.handle     = nullptr;
            header.kind       = IO_EVENT_NONE;
            header.context    = nullptr;
            request.listening = INVALID_SOCKET_HANDLE;
        }
    };

    struct IoUringReceiveSlot
    {
        IOEventHeader     header;

        IOReceiveRequest  request;
        IOReceiveResponse response;
    };

    struct IoUringSendSlot
    {
        IOEventHeader  header;

        IOSendRequest  request;
        IOSendResponse response;
    };

    struct IoUringTransferEvent : IOTransferEvent
    {
        IoUringReceiveSlot receive;
        IoUringSendSlot    send;

        bool               closing;

        IoUringTransferEvent()
        {
            closing = false;

            receive.header.handle  = nullptr;
            receive.header.kind    = IO_EVENT_NONE;
            receive.header.context = nullptr;
            receive.request.sock   = nullptr;
            receive.request.buffer = nullptr;
            receive.request.length = 0;

            send.header.handle  = nullptr;
            send.header.kind    = IO_EVENT_NONE;
            send.header.context = nullptr;
            send.request.sock   = nullptr;
            send.request.buffer = nullptr;
            send.request.length = 0;
        }
    };

    inline void bindContext(IoUringAcceptEvent& event, IOContext* context)
    {
        event.header.context = context;
    }

    inline void bindContext(IoUringTransferEvent& event, IOContext* context)
    {
        event.receive.header.context = context;
        event.send.header.context    = context;
    }

    inline IoUringTransferEvent& toTransferEvent(IoUringReceiveSlot& slot)
    {
        return *reinterpret_cast<IoUringTransferEvent*>(
            reinterpret_cast<char*>(&slot) - offsetof(IoUringTransferEvent, receive));
    }

    inline IoUringTransferEvent& toTransferEvent(IoUringSendSlot& slot)
    {
        return *reinterpret_cast<IoUringTransferEvent*>(
            reinterpret_cast<char*>(&slot) - offsetof(IoUringTransferEvent, send));
    }

    inline bool isIdle(const IoUringTransferEvent& event)
    {
        return event.receive.header.kind == IO_EVENT_NONE &&
               event.send.header.kind == IO_EVENT_NONE;
    }

    static_assert(std::is_standard_layout<IoUringTransferEvent>::value,
                  "toTransferEvent() needs the slot offsets to be the ones "
                  "offsetof reports");

    static_assert(offsetof(IoUringAcceptEvent, header) == 0 &&
                  offsetof(IoUringReceiveSlot, header) == 0 &&
                  offsetof(IoUringSendSlot, header) == 0,
                  "the header must be first: a completion casts user_data to it "
                  "and reads kind before it knows what it is holding");

    static_assert(sizeof(IoUringAcceptEvent) <= IOAcceptEvent::HOLDER_SIZE,
                  "IoUringAcceptEvent outgrew IOAcceptEvent::HOLDER_SIZE - raise it");
    static_assert(sizeof(IoUringTransferEvent) <= IOTransferEvent::HOLDER_SIZE,
                  "IoUringTransferEvent outgrew IOTransferEvent::HOLDER_SIZE - raise it");
}

#endif

#endif
