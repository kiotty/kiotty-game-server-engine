#if !defined(KIOTTY_PRESENTATION_EVENT_LISTENER_IO_EVENT_LISTENER_H)
#define KIOTTY_PRESENTATION_EVENT_LISTENER_IO_EVENT_LISTENER_H

#include "../event/kiotty_io_event.h"
#include <presentation/socket/kiotty_socket.h>

namespace kiotty
{
    class IOMultiEventListener;

    enum class IOEventListenType
    {
        SUCCESS,
        TIMEOUT,
        CANCELLED,
    };

    struct IOEventCallback {
        void (*onAccepted)(IOMultiEventListener& listener, IOContext& context, IOAcceptResponse& response) {nullptr};
        void (*onReceived)(IOMultiEventListener& listener, IOContext& context, IOReceiveResponse& response) {nullptr};
        void (*onSent)(IOMultiEventListener& listener, IOContext& context, IOSendResponse& response) {nullptr};

        void (*onDisconnected)(IOMultiEventListener& listener, IOContext& context) {nullptr};

        void (*onInterrupted)(IOMultiEventListener& listener) {nullptr};
    };


    class IOMultiEventListener
    {
    public:
        static const size_t STORAGE_SIZE = 512;
        static const size_t STORAGE_ALIGN = 16;

        explicit IOMultiEventListener(const IOEventCallback& callbacks);

        ~IOMultiEventListener();
        IOMultiEventListener(const IOMultiEventListener&) = delete;
        IOMultiEventListener& operator=(const IOMultiEventListener&) = delete;
        IOMultiEventListener(IOMultiEventListener&&) = delete;
        IOMultiEventListener& operator=(IOMultiEventListener&&) = delete;

        explicit operator bool() const;

        IOEventListenType wait(uint64_t timeout_ms);
        void cancel();

        SocketCode submit(IOAcceptEvent& event, const IOAcceptRequest& request);
        SocketCode submit(IOTransferEvent& event, const IOReceiveRequest& request);
        SocketCode submit(IOTransferEvent& event, const IOSendRequest& request);

    private:
        alignas(STORAGE_ALIGN) char _data[STORAGE_SIZE];

        IOEventCallback _callbacks;
    };

}

#endif
