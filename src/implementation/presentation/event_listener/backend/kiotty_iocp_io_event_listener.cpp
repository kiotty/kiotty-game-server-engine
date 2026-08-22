#if defined(_WIN32)

#include "../kiotty_io_event_listener.h"
#include <presentation/event/backend/kiotty_iocp_io_event.h>

#include <mswsock.h>

#include <atomic>
#include <mutex>
#include <new>

namespace kiotty
{
    namespace
    {
        const ULONG MAX_COMPLETIONS_PER_WAIT = 64;


        struct EventListenerContext
        {
            HANDLE            port;
            std::atomic<bool> cancel_requested;

            std::mutex        operation_lock;
        };

        EventListenerContext* toContext(char* data)
        {
            return reinterpret_cast<EventListenerContext*>(data);
        }

        DWORD toWaitMilliseconds(uint64_t timeout_ms)
        {
            if (timeout_ms >= INFINITE)
            {
                return INFINITE;
            }
            return static_cast<DWORD>(timeout_ms);
        }

        bool attachToPort(HANDLE port, SocketHandle handle)
        {
            HANDLE const file = reinterpret_cast<HANDLE>(static_cast<SOCKET>(handle));

            if (::CreateIoCompletionPort(file, port, 0, 0) != nullptr)
            {
                return true;
            }
            return ::GetLastError() == ERROR_INVALID_PARAMETER;
        }

        void finishLocked(IOEventHeader& header, IocpTransferEvent& event,
                          SocketCode code)
        {
            header.kind = IO_EVENT_NONE;

            if (endsConnection(code))
            {
                event.closing = true;
            }
        }

        bool isDisconnectDue(EventListenerContext& listener, IocpTransferEvent& event)
        {
            std::lock_guard<std::mutex> guard(listener.operation_lock);

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
        context->port = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
        context->cancel_requested.store(false, std::memory_order_relaxed);
    }

    IOMultiEventListener::~IOMultiEventListener()
    {
        EventListenerContext* context = toContext(_data);

        if (context->port != nullptr)
        {
            ::CloseHandle(context->port);
        }
        context->~EventListenerContext();
    }

    IOMultiEventListener::operator bool() const
    {
        return toContext(const_cast<char*>(_data))->port != nullptr;
    }

    void IOMultiEventListener::cancel()
    {
        EventListenerContext* context = toContext(_data);

        context->cancel_requested.store(true, std::memory_order_release);

        ::PostQueuedCompletionStatus(context->port, 0, 0, nullptr);
    }

    SocketCode IOMultiEventListener::submit(IOAcceptEvent& event,
                                            const IOAcceptRequest& request)
    {
        EventListenerContext& listener = *toContext(_data);
        IocpAcceptEvent&      target   = static_cast<IocpAcceptEvent&>(event);

        if (request.listening == INVALID_SOCKET_HANDLE)
        {
            return SocketCode::SOCKET_INVALID_HANDLE;
        }
        if (!attachToPort(listener.port, request.listening))
        {
            return SocketCode::SOCKET_FAILED;
        }

        std::lock_guard<std::mutex> guard(listener.operation_lock);

        if (target.header.kind != IO_EVENT_NONE)
        {
            return SocketCode::SOCKET_OUT_OF_RESOURCE;
        }

        target.header.kind    = IO_EVENT_ACCEPT;
        target.request = request;
        std::memset(&target.header.handle, 0, sizeof(target.header.handle));

        const SocketCode code = acceptConnection(request.listening, event);

        if (code != SocketCode::SOCKET_PENDING)
        {
            target.header.kind = IO_EVENT_NONE;
            return code;
        }
        return SocketCode::SOCKET_SUCCESS;
    }

    SocketCode IOMultiEventListener::submit(IOTransferEvent& event,
                                            const IOReceiveRequest& request)
    {
        EventListenerContext& listener = *toContext(_data);
        IocpTransferEvent&    target   = static_cast<IocpTransferEvent&>(event);
        IocpReceiveSlot&      slot     = target.receive;

        if (request.sock == nullptr || request.sock->handle() == INVALID_SOCKET_HANDLE)
        {
            return SocketCode::SOCKET_INVALID_HANDLE;
        }
        if (!attachToPort(listener.port, request.sock->handle()))
        {
            return SocketCode::SOCKET_FAILED;
        }

        std::lock_guard<std::mutex> guard(listener.operation_lock);

        if (target.closing)
        {
            return SocketCode::SOCKET_CLOSED;
        }
        if (slot.header.kind != IO_EVENT_NONE)
        {
            return SocketCode::SOCKET_OUT_OF_RESOURCE;
        }

        slot.header.kind    = IO_EVENT_RECEIVE;
        slot.request = request;
        std::memset(&slot.header.handle, 0, sizeof(slot.header.handle));

        const TransferResult result =
            request.sock->receive(request.buffer, request.length, event);

        if (result.code() != SocketCode::SOCKET_PENDING)
        {
            slot.header.kind = IO_EVENT_NONE;
            return result.code();
        }
        return SocketCode::SOCKET_SUCCESS;
    }

    SocketCode IOMultiEventListener::submit(IOTransferEvent& event,
                                            const IOSendRequest& request)
    {
        EventListenerContext& listener = *toContext(_data);
        IocpTransferEvent&    target   = static_cast<IocpTransferEvent&>(event);
        IocpSendSlot&         slot     = target.send;

        if (request.sock == nullptr || request.sock->handle() == INVALID_SOCKET_HANDLE)
        {
            return SocketCode::SOCKET_INVALID_HANDLE;
        }
        if (!attachToPort(listener.port, request.sock->handle()))
        {
            return SocketCode::SOCKET_FAILED;
        }

        std::lock_guard<std::mutex> guard(listener.operation_lock);

        if (target.closing)
        {
            return SocketCode::SOCKET_CLOSED;
        }
        if (slot.header.kind != IO_EVENT_NONE)
        {
            return SocketCode::SOCKET_OUT_OF_RESOURCE;
        }

        slot.header.kind    = IO_EVENT_SEND;
        slot.request = request;
        std::memset(&slot.header.handle, 0, sizeof(slot.header.handle));

        const TransferResult result =
            request.sock->send(request.buffer, request.length, event);

        if (result.code() != SocketCode::SOCKET_PENDING)
        {
            slot.header.kind = IO_EVENT_NONE;
            return result.code();
        }
        return SocketCode::SOCKET_SUCCESS;
    }

    IOEventListenType IOMultiEventListener::wait(uint64_t timeout_ms)
    {
        EventListenerContext& listener = *toContext(_data);

        if (listener.port == nullptr)
        {
            return IOEventListenType::CANCELLED;
        }

        OVERLAPPED_ENTRY entries[MAX_COMPLETIONS_PER_WAIT];
        ULONG            removed = 0;

        const BOOL ok = ::GetQueuedCompletionStatusEx(listener.port,
                                                      entries,
                                                      MAX_COMPLETIONS_PER_WAIT,
                                                      &removed,
                                                      toWaitMilliseconds(timeout_ms),
                                                      FALSE);

        if (!ok)
        {
            return (::GetLastError() == WAIT_TIMEOUT)
                       ? IOEventListenType::TIMEOUT
                       : IOEventListenType::CANCELLED;
        }

        bool cancelled = false;

        for (ULONG i = 0; i < removed; ++i)
        {
            const OVERLAPPED_ENTRY& entry = entries[i];

            if (entry.lpOverlapped == nullptr)
            {
                cancelled = true;
                continue;
            }

            const LONG  status = static_cast<LONG>(entry.Internal);
            const DWORD bytes  = entry.dwNumberOfBytesTransferred;

            IOEventHeader* const header =
                reinterpret_cast<IOEventHeader*>(entry.lpOverlapped);

            IOEventKind kind = IO_EVENT_NONE;

            {
                std::lock_guard<std::mutex> guard(listener.operation_lock);
                kind = header->kind;
            }

            switch (kind)
            {
            case IO_EVENT_ACCEPT:
            {
                IocpAcceptEvent& event = *reinterpret_cast<IocpAcceptEvent*>(header);

                if (status < 0)
                {
                    ::closesocket(static_cast<SOCKET>(event.response.accepting));
                    event.response.accepting = INVALID_SOCKET_HANDLE;
                    event.response.code      = SocketCode::SOCKET_FAILED;
                }
                else
                {
                    const SOCKET listening =
                        static_cast<SOCKET>(event.request.listening);

                    ::setsockopt(static_cast<SOCKET>(event.response.accepting),
                                 SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                                 reinterpret_cast<const char*>(&listening),
                                 sizeof(listening));

                    event.response.code = SocketCode::SOCKET_SUCCESS;
                }

                {
                    std::lock_guard<std::mutex> guard(listener.operation_lock);
                    event.header.kind = IO_EVENT_NONE;
                }

                const SocketHandle offered = event.response.accepting;

                if (_callbacks.onAccepted != nullptr)
                {
                    _callbacks.onAccepted(*this, *event.header.context, event.response);
                }

                if (event.response.accepting == offered &&
                    offered != INVALID_SOCKET_HANDLE)
                {
                    ::closesocket(static_cast<SOCKET>(offered));
                    event.response.accepting = INVALID_SOCKET_HANDLE;
                }
                break;
            }

            case IO_EVENT_RECEIVE:
            {
                IocpReceiveSlot&   slot       = *reinterpret_cast<IocpReceiveSlot*>(header);
                IocpTransferEvent& connection = toTransferEvent(slot);

                slot.response.code =
                    (status < 0) ? SocketCode::SOCKET_FAILED :
                    (bytes == 0) ? SocketCode::SOCKET_CLOSED :
                                   SocketCode::SOCKET_SUCCESS;
                slot.response.length = bytes;

                {
                    std::lock_guard<std::mutex> guard(listener.operation_lock);
                    finishLocked(slot.header, connection, slot.response.code);
                }

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
                IocpSendSlot&      slot       = *reinterpret_cast<IocpSendSlot*>(header);
                IocpTransferEvent& connection = toTransferEvent(slot);

                slot.response.code   = (status < 0) ? SocketCode::SOCKET_FAILED
                                                    : SocketCode::SOCKET_SUCCESS;
                slot.response.length = bytes;

                {
                    std::lock_guard<std::mutex> guard(listener.operation_lock);
                    finishLocked(slot.header, connection, slot.response.code);
                }

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

        if (cancelled &&
            listener.cancel_requested.exchange(false, std::memory_order_acq_rel))
        {
            if (_callbacks.onInterrupted != nullptr)
            {
                _callbacks.onInterrupted(*this);
            }
            return IOEventListenType::CANCELLED;
        }

        return (removed > 0) ? IOEventListenType::SUCCESS : IOEventListenType::TIMEOUT;
    }
}

#endif
