#if defined(__linux__) && !defined(KIOTTY_HAS_IO_URING)

#include "../kiotty_io_event_listener.h"
#include <presentation/event/backend/kiotty_epoll_io_event.h>

#include <cerrno>
#include <cstring>
#include <new>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <mutex>

namespace kiotty
{
    namespace
    {
        const int MAX_EVENTS_PER_WAIT = 64;

        struct EventListenerContext
        {
            int               epoll_descriptor;
            int               wakeup_descriptor;
            std::atomic<bool> cancel_requested;

            std::mutex        event_lock;
        };

        EventListenerContext* toContext(char* data)
        {
            return reinterpret_cast<EventListenerContext*>(data);
        }

        uint32_t toInterest(IOEventKind kind)
        {
            uint32_t interest = 0;

            if ((kind & (IO_EVENT_ACCEPT | IO_EVENT_RECEIVE)) != 0)
            {
                interest |= EPOLLIN;
            }
            if ((kind & IO_EVENT_SEND) != 0)
            {
                interest |= EPOLLOUT;
            }
            return interest;
        }

        bool addEvent(EventListenerContext& context, SocketHandle handle,
                      IOEventKind kind, void* owner)
        {
            epoll_event change;
            std::memset(&change, 0, sizeof(change));
            change.events   = toInterest(kind);
            change.data.ptr = owner;

            return ::epoll_ctl(context.epoll_descriptor, EPOLL_CTL_ADD,
                               static_cast<int>(handle), &change) == 0;
        }

        bool modifyEvent(EventListenerContext& context, SocketHandle handle,
                         IOEventKind kind, void* owner)
        {
            epoll_event change;
            std::memset(&change, 0, sizeof(change));
            change.events   = toInterest(kind);
            change.data.ptr = owner;

            if (::epoll_ctl(context.epoll_descriptor, EPOLL_CTL_MOD,
                            static_cast<int>(handle), &change) == 0)
            {
                return true;
            }

            if (errno != ENOENT)
            {
                return false;
            }
            return addEvent(context, handle, kind, owner);
        }

        bool removeEvent(EventListenerContext& context, SocketHandle handle)
        {
            ::epoll_ctl(context.epoll_descriptor, EPOLL_CTL_DEL,
                        static_cast<int>(handle), nullptr);
            return true;
        }

        bool applyKind(EventListenerContext& context, SocketHandle handle,
                       IOEventKind before, IOEventKind after, void* owner)
        {
            if (before == IO_EVENT_NONE)
            {
                return (after == IO_EVENT_NONE)
                           ? true
                           : addEvent(context, handle, after, owner);
            }
            return (after == IO_EVENT_NONE)
                       ? removeEvent(context, handle)
                       : modifyEvent(context, handle, after, owner);
        }

        int toWaitMilliseconds(uint64_t timeout_ms)
        {
            const uint64_t limit = 0x7fffffffull;

            if (timeout_ms >= limit)
            {
                return -1;
            }
            return static_cast<int>(timeout_ms);
        }

        void markClosingLocked(EpollTransferEvent& event, SocketCode code)
        {
            if (endsConnection(code))
            {
                event.closing = true;
            }
        }

        bool isDisconnectDue(EventListenerContext& listener, EpollTransferEvent& event)
        {
            std::lock_guard<std::mutex> guard(listener.event_lock);

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

        context->epoll_descriptor  = ::epoll_create1(EPOLL_CLOEXEC);
        context->wakeup_descriptor = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        context->cancel_requested.store(false, std::memory_order_relaxed);

        if (context->epoll_descriptor >= 0 && context->wakeup_descriptor >= 0)
        {
            addEvent(*context, static_cast<SocketHandle>(context->wakeup_descriptor),
                     IO_EVENT_RECEIVE, nullptr);
        }
    }

    IOMultiEventListener::~IOMultiEventListener()
    {
        EventListenerContext* context = toContext(_data);

        if (context->wakeup_descriptor >= 0)
        {
            ::close(context->wakeup_descriptor);
        }
        if (context->epoll_descriptor >= 0)
        {
            ::close(context->epoll_descriptor);
        }
        context->~EventListenerContext();
    }

    IOMultiEventListener::operator bool() const
    {
        const EventListenerContext* context = toContext(const_cast<char*>(_data));

        return context->epoll_descriptor >= 0 && context->wakeup_descriptor >= 0;
    }

    void IOMultiEventListener::cancel()
    {
        EventListenerContext* context = toContext(_data);

        context->cancel_requested.store(true, std::memory_order_release);

        const uint64_t one = 1;
        const ssize_t written = ::write(context->wakeup_descriptor, &one, sizeof(one));
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
        EpollAcceptEvent&     target   = static_cast<EpollAcceptEvent&>(event);

        std::lock_guard<std::mutex> guard(listener.event_lock);

        if ((target.header.kind & IO_EVENT_ACCEPT) != 0)
        {
            return SocketCode::SOCKET_OUT_OF_RESOURCE;
        }

        const IOEventKind before = target.header.kind;

        target.header.kind |= IO_EVENT_ACCEPT;
        target.request = request;

        if (!applyKind(listener, request.listening, before, target.header.kind, &target))
        {
            target.header.kind = before;
            return SocketCode::SOCKET_FAILED;
        }
        return SocketCode::SOCKET_SUCCESS;
    }

    SocketCode IOMultiEventListener::submit(IOTransferEvent& event,
                                            const IOReceiveRequest& request)
    {
        if (request.sock == nullptr || request.sock->handle() == INVALID_SOCKET_HANDLE)
        {
            return SocketCode::SOCKET_INVALID_HANDLE;
        }

        EventListenerContext& listener = *toContext(_data);
        EpollTransferEvent&   target   = static_cast<EpollTransferEvent&>(event);

        std::lock_guard<std::mutex> guard(listener.event_lock);

        if (target.closing)
        {
            return SocketCode::SOCKET_CLOSED;
        }
        if ((target.header.kind & IO_EVENT_RECEIVE) != 0)
        {
            return SocketCode::SOCKET_OUT_OF_RESOURCE;
        }

        const IOEventKind before = target.header.kind;

        target.header.kind |= IO_EVENT_RECEIVE;
        target.receive_request = request;

        if (!applyKind(listener, request.sock->handle(), before, target.header.kind, &target))
        {
            target.header.kind = before;
            return SocketCode::SOCKET_FAILED;
        }
        return SocketCode::SOCKET_SUCCESS;
    }

    SocketCode IOMultiEventListener::submit(IOTransferEvent& event,
                                            const IOSendRequest& request)
    {
        if (request.sock == nullptr || request.sock->handle() == INVALID_SOCKET_HANDLE)
        {
            return SocketCode::SOCKET_INVALID_HANDLE;
        }

        EventListenerContext& listener = *toContext(_data);
        EpollTransferEvent&   target   = static_cast<EpollTransferEvent&>(event);

        std::lock_guard<std::mutex> guard(listener.event_lock);

        if (target.closing)
        {
            return SocketCode::SOCKET_CLOSED;
        }
        if ((target.header.kind & IO_EVENT_SEND) != 0)
        {
            return SocketCode::SOCKET_OUT_OF_RESOURCE;
        }

        const IOEventKind before = target.header.kind;

        target.header.kind |= IO_EVENT_SEND;
        target.send_request = request;

        if (!applyKind(listener, request.sock->handle(), before, target.header.kind, &target))
        {
            target.header.kind = before;
            return SocketCode::SOCKET_FAILED;
        }
        return SocketCode::SOCKET_SUCCESS;
    }

    IOEventListenType IOMultiEventListener::wait(uint64_t timeout_ms)
    {
        EventListenerContext& listener = *toContext(_data);

        if (listener.epoll_descriptor < 0)
        {
            return IOEventListenType::CANCELLED;
        }

        epoll_event events[MAX_EVENTS_PER_WAIT];

        const int count = ::epoll_wait(listener.epoll_descriptor, events,
                                       MAX_EVENTS_PER_WAIT,
                                       toWaitMilliseconds(timeout_ms));

        if (count < 0)
        {
            return (errno == EINTR) ? IOEventListenType::TIMEOUT
                                    : IOEventListenType::CANCELLED;
        }

        bool dispatched = false;
        bool cancelled  = false;

        for (int i = 0; i < count; ++i)
        {
            if (events[i].data.ptr == nullptr)
            {
                uint64_t drained = 0;
                const ssize_t read_bytes =
                    ::read(listener.wakeup_descriptor, &drained, sizeof(drained));
                (void)read_bytes;

                cancelled = true;
                continue;
            }

            const uint32_t ready = events[i].events;

            const bool readable = (ready & (EPOLLIN | EPOLLERR | EPOLLHUP)) != 0;
            const bool writable = (ready & (EPOLLOUT | EPOLLERR | EPOLLHUP)) != 0;

            IOEventKind kind = IO_EVENT_NONE;

            {
                std::lock_guard<std::mutex> guard(listener.event_lock);
                kind = *static_cast<const IOEventKind*>(events[i].data.ptr);
            }

            if ((kind & IO_EVENT_ACCEPT) != 0)
            {
                if (!readable)
                {
                    continue;
                }

                EpollAcceptEvent& event =
                    *static_cast<EpollAcceptEvent*>(events[i].data.ptr);

                SocketHandle listening = INVALID_SOCKET_HANDLE;

                {
                    std::lock_guard<std::mutex> guard(listener.event_lock);

                    if ((event.header.kind & IO_EVENT_ACCEPT) == 0)
                    {
                        continue;
                    }

                    listening = event.request.listening;

                    const IOEventKind before = event.header.kind;
                    event.header.kind &= ~IO_EVENT_ACCEPT;
                    applyKind(listener, listening, before, event.header.kind, &event);
                }

                event.response.code = acceptConnection(listening, event);
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
                continue;
            }

            EpollTransferEvent& event =
                *static_cast<EpollTransferEvent*>(events[i].data.ptr);

            if (readable)
            {
                ActiveSocket* sock   = nullptr;
                char*         buffer = nullptr;
                size_t        length = 0;

                {
                    std::lock_guard<std::mutex> guard(listener.event_lock);

                    if ((event.header.kind & IO_EVENT_RECEIVE) != 0)
                    {
                        sock   = event.receive_request.sock;
                        buffer = event.receive_request.buffer;
                        length = event.receive_request.length;

                        const IOEventKind before = event.header.kind;
                        event.header.kind &= ~IO_EVENT_RECEIVE;
                        applyKind(listener, sock->handle(), before, event.header.kind, &event);
                    }
                }

                if (sock != nullptr)
                {
                    const TransferResult result = sock->receive(buffer, length, event);

                    event.receive_response.code   = result.code();
                    event.receive_response.length = result.isOk() ? result.value() : 0;

                    {
                        std::lock_guard<std::mutex> guard(listener.event_lock);
                        markClosingLocked(event, event.receive_response.code);
                    }

                    dispatched = true;

                    if (_callbacks.onReceived != nullptr)
                    {
                        _callbacks.onReceived(*this, *event.header.context,
                                              event.receive_response);
                    }

                    if (_callbacks.onDisconnected != nullptr &&
                        isDisconnectDue(listener, event))
                    {
                        _callbacks.onDisconnected(*this, *event.header.context);
                    }
                }
            }

            if (writable)
            {
                ActiveSocket* sock   = nullptr;
                const char*   buffer = nullptr;
                size_t        length = 0;

                {
                    std::lock_guard<std::mutex> guard(listener.event_lock);

                    if ((event.header.kind & IO_EVENT_SEND) != 0)
                    {
                        sock   = event.send_request.sock;
                        buffer = event.send_request.buffer;
                        length = event.send_request.length;

                        const IOEventKind before = event.header.kind;
                        event.header.kind &= ~IO_EVENT_SEND;
                        applyKind(listener, sock->handle(), before, event.header.kind, &event);
                    }
                }

                if (sock != nullptr)
                {
                    const TransferResult result = sock->send(buffer, length, event);

                    event.send_response.code   = result.code();
                    event.send_response.length = result.isOk() ? result.value() : 0;

                    {
                        std::lock_guard<std::mutex> guard(listener.event_lock);
                        markClosingLocked(event, event.send_response.code);
                    }

                    dispatched = true;

                    if (_callbacks.onSent != nullptr)
                    {
                        _callbacks.onSent(*this, *event.header.context, event.send_response);
                    }

                    if (_callbacks.onDisconnected != nullptr &&
                        isDisconnectDue(listener, event))
                    {
                        _callbacks.onDisconnected(*this, *event.header.context);
                    }
                }
            }
        }

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
