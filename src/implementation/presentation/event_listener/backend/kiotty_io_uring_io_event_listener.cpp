#if defined(KIOTTY_HAS_IO_URING)

#include "../kiotty_io_event_listener.h"
#include <presentation/event/backend/kiotty_io_uring_io_event.h>

#include <presentation/socket/backend/kiotty_posix_socket_internal.h>

#include <cerrno>
#include <cstring>
#include <new>
#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <mutex>
#include <thread>

namespace kiotty
{
    namespace
    {
        const unsigned RING_ENTRIES = 256;


        struct EventListenerContext
        {
            io_uring          ring;
            bool              ring_initialized;
            bool              timeout_without_sqe;

            int               wakeup_descriptor;
            uint64_t          wakeup_value;
            bool              wakeup_armed;

            std::atomic<bool> cancel_requested;

            std::mutex        submission_lock;

            std::thread::id   loop_thread;
            bool              loop_thread_known;
        };

        EventListenerContext* toContext(char* data)
        {
            return reinterpret_cast<EventListenerContext*>(data);
        }

        void prepare(io_uring_sqe* sqe, IOEventKind kind, void* event)
        {
            switch (kind)
            {
            case IO_EVENT_ACCEPT:
            {
                IoUringAcceptEvent& target = *static_cast<IoUringAcceptEvent*>(event);

                target.header.handle = sqe;
                acceptConnection(target.request.listening, target);
                break;
            }

            case IO_EVENT_RECEIVE:
            {
                IoUringTransferEvent& target =
                    *static_cast<IoUringTransferEvent*>(event);

                target.receive.header.handle = sqe;
                target.receive.request.sock->receive(target.receive.request.buffer,
                                                     target.receive.request.length,
                                                     target);
                break;
            }

            case IO_EVENT_SEND:
            {
                IoUringTransferEvent& target =
                    *static_cast<IoUringTransferEvent*>(event);

                target.send.header.handle = sqe;
                target.send.request.sock->send(target.send.request.buffer,
                                               target.send.request.length,
                                               target);
                break;
            }

            default:
                break;
            }
        }

        SocketCode issueLocked(EventListenerContext& context, IOEventKind kind, void* event)
        {
            io_uring_sqe* sqe = ::io_uring_get_sqe(&context.ring);

            if (sqe == nullptr)
            {
                ::io_uring_submit(&context.ring);
                sqe = ::io_uring_get_sqe(&context.ring);

                if (sqe == nullptr)
                {
                    return SocketCode::SOCKET_OUT_OF_RESOURCE;
                }
            }

            prepare(sqe, kind, event);

            const bool on_loop_thread =
                context.loop_thread_known &&
                context.loop_thread == std::this_thread::get_id();

            if (!on_loop_thread)
            {
                ::io_uring_submit(&context.ring);
            }
            return SocketCode::SOCKET_SUCCESS;
        }

        void armWakeupWatch(EventListenerContext& context)
        {
            if (context.wakeup_armed)
            {
                return;
            }

            io_uring_sqe* const sqe = ::io_uring_get_sqe(&context.ring);

            if (sqe == nullptr)
            {
                return;
            }

            ::io_uring_prep_read(sqe, context.wakeup_descriptor,
                                 &context.wakeup_value, sizeof(context.wakeup_value), 0);
            ::io_uring_sqe_set_data(sqe, nullptr);

            context.wakeup_armed = true;
        }

        void flush(EventListenerContext& context)
        {
            std::lock_guard<std::mutex> guard(context.submission_lock);
            ::io_uring_submit(&context.ring);
        }

        void finishLocked(IOEventHeader& header, IoUringTransferEvent& event,
                          SocketCode code)
        {
            header.kind = IO_EVENT_NONE;

            if (endsConnection(code))
            {
                event.closing = true;
            }
        }

        bool isDisconnectDue(EventListenerContext& listener, IoUringTransferEvent& event)
        {
            std::lock_guard<std::mutex> guard(listener.submission_lock);

            return event.closing && isIdle(event);
        }
    }

    static_assert(sizeof(EventListenerContext) <= IOMultiEventListener::STORAGE_SIZE,
                  "EventListenerContext outgrew IOMultiEventListener::STORAGE_SIZE - raise it");
    static_assert(alignof(EventListenerContext) <= IOMultiEventListener::STORAGE_ALIGN,
                  "EventListenerContext needs stricter alignment than STORAGE_ALIGN");

    IOMultiEventListener::IOMultiEventListener(const IOEventCallback& callbacks) :
        _callbacks(callbacks)
    {
        EventListenerContext* context = ::new (_data) EventListenerContext();

        std::memset(&context->ring, 0, sizeof(context->ring));
        context->ring_initialized =
            ::io_uring_queue_init(RING_ENTRIES, &context->ring, 0) == 0;

        context->timeout_without_sqe =
            context->ring_initialized &&
            (context->ring.features & IORING_FEAT_EXT_ARG) != 0;

        context->wakeup_descriptor = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        context->wakeup_value      = 0;
        context->wakeup_armed      = false;
        context->loop_thread_known = false;

        context->cancel_requested.store(false, std::memory_order_relaxed);
    }

    IOMultiEventListener::~IOMultiEventListener()
    {
        EventListenerContext* context = toContext(_data);

        if (context->wakeup_descriptor >= 0)
        {
            ::close(context->wakeup_descriptor);
        }
        if (context->ring_initialized)
        {
            ::io_uring_queue_exit(&context->ring);
        }
        context->~EventListenerContext();
    }

    IOMultiEventListener::operator bool() const
    {
        const EventListenerContext* context = toContext(const_cast<char*>(_data));

        return context->ring_initialized &&
               context->timeout_without_sqe &&
               context->wakeup_descriptor >= 0;
    }

    void IOMultiEventListener::cancel()
    {
        EventListenerContext* context = toContext(_data);

        context->cancel_requested.store(true, std::memory_order_release);

        const uint64_t one = 1;
        const ssize_t written =
            ::write(context->wakeup_descriptor, &one, sizeof(one));
        (void)written;
    }

    SocketCode IOMultiEventListener::submit(IOAcceptEvent& event,
                                            const IOAcceptRequest& request)
    {
        if (request.listening == INVALID_SOCKET_HANDLE)
        {
            return SocketCode::SOCKET_INVALID_HANDLE;
        }

        EventListenerContext& listener = *toContext(_data);
        IoUringAcceptEvent&   target   = static_cast<IoUringAcceptEvent&>(event);

        std::lock_guard<std::mutex> guard(listener.submission_lock);

        if (target.header.kind != IO_EVENT_NONE)
        {
            return SocketCode::SOCKET_OUT_OF_RESOURCE;
        }

        target.header.kind    = IO_EVENT_ACCEPT;
        target.request = request;

        const SocketCode code = issueLocked(listener, IO_EVENT_ACCEPT, &target);

        if (code != SocketCode::SOCKET_SUCCESS)
        {
            target.header.kind = IO_EVENT_NONE;
        }
        return code;
    }

    SocketCode IOMultiEventListener::submit(IOTransferEvent& event,
                                            const IOReceiveRequest& request)
    {
        if (request.sock == nullptr || request.sock->handle() == INVALID_SOCKET_HANDLE)
        {
            return SocketCode::SOCKET_INVALID_HANDLE;
        }

        EventListenerContext& listener = *toContext(_data);
        IoUringTransferEvent& target   = static_cast<IoUringTransferEvent&>(event);

        std::lock_guard<std::mutex> guard(listener.submission_lock);

        if (target.closing)
        {
            return SocketCode::SOCKET_CLOSED;
        }
        if (target.receive.header.kind != IO_EVENT_NONE)
        {
            return SocketCode::SOCKET_OUT_OF_RESOURCE;
        }

        target.receive.header.kind    = IO_EVENT_RECEIVE;
        target.receive.request = request;

        const SocketCode code = issueLocked(listener, IO_EVENT_RECEIVE, &target);

        if (code != SocketCode::SOCKET_SUCCESS)
        {
            target.receive.header.kind = IO_EVENT_NONE;
        }
        return code;
    }

    SocketCode IOMultiEventListener::submit(IOTransferEvent& event,
                                            const IOSendRequest& request)
    {
        if (request.sock == nullptr || request.sock->handle() == INVALID_SOCKET_HANDLE)
        {
            return SocketCode::SOCKET_INVALID_HANDLE;
        }

        EventListenerContext& listener = *toContext(_data);
        IoUringTransferEvent& target   = static_cast<IoUringTransferEvent&>(event);

        std::lock_guard<std::mutex> guard(listener.submission_lock);

        if (target.closing)
        {
            return SocketCode::SOCKET_CLOSED;
        }
        if (target.send.header.kind != IO_EVENT_NONE)
        {
            return SocketCode::SOCKET_OUT_OF_RESOURCE;
        }

        target.send.header.kind    = IO_EVENT_SEND;
        target.send.request = request;

        const SocketCode code = issueLocked(listener, IO_EVENT_SEND, &target);

        if (code != SocketCode::SOCKET_SUCCESS)
        {
            target.send.header.kind = IO_EVENT_NONE;
        }
        return code;
    }

    IOEventListenType IOMultiEventListener::wait(uint64_t timeout_ms)
    {
        EventListenerContext& listener = *toContext(_data);

        if (!static_cast<bool>(*this))
        {
            return IOEventListenType::CANCELLED;
        }

        {
            std::lock_guard<std::mutex> guard(listener.submission_lock);

            listener.loop_thread       = std::this_thread::get_id();
            listener.loop_thread_known = true;

            armWakeupWatch(listener);
            ::io_uring_submit(&listener.ring);
        }

        io_uring_cqe* first  = nullptr;
        int           waited = 0;

        if (timeout_ms >= static_cast<uint64_t>(0x7fffffff))
        {
            waited = ::io_uring_wait_cqe(&listener.ring, &first);
        }
        else
        {
            __kernel_timespec deadline;
            deadline.tv_sec  = static_cast<__s64>(timeout_ms / 1000u);
            deadline.tv_nsec = static_cast<long long>((timeout_ms % 1000u) * 1000000u);

            waited = ::io_uring_wait_cqe_timeout(&listener.ring, &first, &deadline);
        }

        if (waited < 0 && waited != -ETIME)
        {
            return (waited == -EINTR) ? IOEventListenType::TIMEOUT
                                      : IOEventListenType::CANCELLED;
        }

        bool dispatched = false;
        bool cancelled  = false;

        unsigned      head = 0;
        unsigned      seen = 0;
        io_uring_cqe* cqe  = nullptr;

        io_uring_for_each_cqe(&listener.ring, head, cqe)
        {
            ++seen;

            void* const data   = ::io_uring_cqe_get_data(cqe);
            const int   result = cqe->res;

            if (data == nullptr)
            {
                listener.wakeup_armed = false;
                cancelled = true;
                continue;
            }

            IOEventHeader* const header = static_cast<IOEventHeader*>(data);

            const SocketCode code = (result < 0) ? toSocketCode(-result)
                                                 : SocketCode::SOCKET_SUCCESS;
            const size_t transferred = (result > 0) ? static_cast<size_t>(result) : 0;

            IOEventKind kind = IO_EVENT_NONE;

            {
                std::lock_guard<std::mutex> guard(listener.submission_lock);
                kind = header->kind;
            }

            switch (kind)
            {
            case IO_EVENT_ACCEPT:
            {
                IoUringAcceptEvent& event =
                    *reinterpret_cast<IoUringAcceptEvent*>(header);

                if (result >= 0)
                {
                    event.response.accepting = static_cast<SocketHandle>(result);
                }
                event.response.code = code;

                {
                    std::lock_guard<std::mutex> guard(listener.submission_lock);
                    event.header.kind = IO_EVENT_NONE;
                }

                dispatched = true;

                if (_callbacks.onAccepted != nullptr)
                {
                    _callbacks.onAccepted(*this, *event.header.context, event.response);
                }

                if (event.response.accepting != INVALID_SOCKET_HANDLE)
                {
                    ::close(static_cast<int>(event.response.accepting));
                    event.response.accepting = INVALID_SOCKET_HANDLE;
                }
                break;
            }

            case IO_EVENT_RECEIVE:
            {
                IoUringReceiveSlot& slot =
                    *reinterpret_cast<IoUringReceiveSlot*>(header);
                IoUringTransferEvent& connection = toTransferEvent(slot);

                slot.response.code =
                    (result == 0) ? SocketCode::SOCKET_CLOSED : code;
                slot.response.length = transferred;

                {
                    std::lock_guard<std::mutex> guard(listener.submission_lock);
                    finishLocked(slot.header, connection, slot.response.code);
                }

                dispatched = true;

                if (_callbacks.onReceived != nullptr)
                {
                    _callbacks.onReceived(*this, *slot.header.context, slot.response);
                }

                if (_callbacks.onDisconnected != nullptr &&
                    isDisconnectDue(listener, connection))
                {
                    _callbacks.onDisconnected(*this, *slot.header.context);
                }
                break;
            }

            case IO_EVENT_SEND:
            {
                IoUringSendSlot&      slot       = *reinterpret_cast<IoUringSendSlot*>(header);
                IoUringTransferEvent& connection = toTransferEvent(slot);

                slot.response.code   = code;
                slot.response.length = transferred;

                {
                    std::lock_guard<std::mutex> guard(listener.submission_lock);
                    finishLocked(slot.header, connection, slot.response.code);
                }

                dispatched = true;

                if (_callbacks.onSent != nullptr)
                {
                    _callbacks.onSent(*this, *slot.header.context, slot.response);
                }

                if (_callbacks.onDisconnected != nullptr &&
                    isDisconnectDue(listener, connection))
                {
                    _callbacks.onDisconnected(*this, *slot.header.context);
                }
                break;
            }

            default:
                break;
            }
        }

        ::io_uring_cq_advance(&listener.ring, seen);

        flush(listener);

        if (cancelled &&
            listener.cancel_requested.exchange(false, std::memory_order_acq_rel))
        {
            if (_callbacks.onInterrupted != nullptr)
            {
                _callbacks.onInterrupted(*this);
            }
            return IOEventListenType::CANCELLED;
        }

        return dispatched ? IOEventListenType::SUCCESS : IOEventListenType::TIMEOUT;
    }
}

#endif
