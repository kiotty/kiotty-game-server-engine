#include "kiotty_io_event.h"

#include "backend/kiotty_iocp_io_event.h"
#include "backend/kiotty_epoll_io_event.h"
#include "backend/kiotty_io_uring_io_event.h"

namespace kiotty
{
    namespace
    {
#if defined(_WIN32)
        typedef IocpAcceptEvent      ConcreteAcceptEvent;
        typedef IocpTransferEvent    ConcreteTransferEvent;
#elif defined(KIOTTY_HAS_IO_URING)
        typedef IoUringAcceptEvent   ConcreteAcceptEvent;
        typedef IoUringTransferEvent ConcreteTransferEvent;
#elif defined(__linux__)
        typedef EpollAcceptEvent     ConcreteAcceptEvent;
        typedef EpollTransferEvent   ConcreteTransferEvent;
#else
#  error "kiotty: no IO event backend for this platform"
#endif
    }

    Holder<IOAcceptEvent> makeAcceptIOEvent(IOContext* context)
    {
        Holder<IOAcceptEvent> event =
            Holder<IOAcceptEvent>::make<ConcreteAcceptEvent>();

        bindContext(static_cast<ConcreteAcceptEvent&>(*event), context);
        return event;
    }

    Holder<IOTransferEvent> makeTransferIOEvent(IOContext* context)
    {
        Holder<IOTransferEvent> event =
            Holder<IOTransferEvent>::make<ConcreteTransferEvent>();

        bindContext(static_cast<ConcreteTransferEvent&>(*event), context);
        return event;
    }
}
