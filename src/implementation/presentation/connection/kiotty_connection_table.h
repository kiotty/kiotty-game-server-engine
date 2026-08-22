#if !defined(KIOTTY_PRESENTATION_CONNECTION_CONNECTION_TABLE_H)
#define KIOTTY_PRESENTATION_CONNECTION_CONNECTION_TABLE_H

#include <domain/channel/kiotty_channel_binder.h>
#include <domain/codec/kiotty_packet_codec.h>
#include <presentation/connection/kiotty_connection.h>

#include <atomic>
#include <cstddef>
#include <type_traits>

namespace kiotty
{
    class ConnectionTable
    {
    public:
        ConnectionTable(size_t capacity, BlockPool& pool, size_t send_queue_limit,
                        IChannelBinder& binder, IPacketCodec& codec);
        ~ConnectionTable();

        ConnectionTable(const ConnectionTable&) = delete;
        ConnectionTable& operator=(const ConnectionTable&) = delete;
        ConnectionTable(ConnectionTable&&) = delete;
        ConnectionTable& operator=(ConnectionTable&&) = delete;

        explicit operator bool() const { return _slots != nullptr; }

        size_t capacity() const { return _capacity; }
        size_t size() const { return _size.load(std::memory_order_acquire); }
        bool full() const { return size() == _capacity; }

        Connection* open(SocketHandle& accepted, IOMultiEventListener& listener);
        void close(Connection& connection);
        void reapClosed();

        Connection* at(size_t index);

    private:
        struct Slot
        {
            typename std::aligned_storage<sizeof(Connection), alignof(Connection)>::type bytes;
            bool live {false};

            void* storage() { return static_cast<void*>(&bytes); }
            Connection* connection() { return reinterpret_cast<Connection*>(&bytes); }
        };

        Slot* findFreeSlot();
        Slot* findSlotOf(Connection& connection);
        static void destroy(Slot& slot);
        void closeEveryLiveSlot();

        Slot*               _slots;
        size_t              _capacity;
        std::atomic<size_t> _size;
        BlockPool&          _pool;
        size_t              _send_queue_limit;
        IChannelBinder&     _binder;
        IPacketCodec&       _codec;
    };
}

#endif
