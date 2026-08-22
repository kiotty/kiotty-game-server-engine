#if !defined(KIOTTY_CORE_CONNECTION_BUFFER_H)
#define KIOTTY_CORE_CONNECTION_BUFFER_H

#include "kiotty_block_pool.h"
#include "kiotty_bytes.h"
#include "kiotty_packet_concept.h"
#include "kiotty_ring_buffer.h"

#include <cstddef>
#include <cstdint>
#include <utility>

namespace kiotty
{
    class ReceiveBuffer
    {
    public:
        explicit ReceiveBuffer(BlockPool& pool) :
            _pool(pool),
            _header(),
            _payload(),
            _header_received(0),
            _payload_received(0)
        {
        }

        ReceiveBuffer(const ReceiveBuffer&) = delete;
        ReceiveBuffer& operator=(const ReceiveBuffer&) = delete;

        ByteSpan headerSpace()
        {
            return wholeHeader().slice(_header_received,
                                       remaining(PACKET_HEADER_SIZE, _header_received));
        }

        void addHeaderReceived(size_t read_length)
        {
            _header_received += read_length;
        }

        bool isHeaderComplete() const { return _header_received >= PACKET_HEADER_SIZE; }

        const PacketHeader& header() const { return _header; }

        bool openPayload()
        {
            const size_t length = _header.payload_length.get();

            _payload_received = 0;
            _payload          = Bytes(_pool, length);

            return length == 0 || static_cast<bool>(_payload);
        }

        ByteSpan payloadSpace()
        {
            return _payload.writableSpan().slice(
                _payload_received, remaining(_payload.size(), _payload_received));
        }

        void addPayloadReceived(size_t read_length)
        {
            _payload_received += read_length;
        }

        bool isPayloadComplete() const { return _payload_received >= _payload.size(); }

        Bytes takePayload()
        {
            Bytes taken = std::move(_payload);

            reset();
            return taken;
        }

        void reset()
        {
            _payload          = Bytes();
            _header_received  = 0;
            _payload_received = 0;
        }

    private:
        static size_t remaining(size_t total, size_t done)
        {
            return (done < total) ? total - done : 0;
        }

        ByteSpan wholeHeader()
        {
            return ByteSpan(reinterpret_cast<uint8_t*>(&_header), PACKET_HEADER_SIZE);
        }

        BlockPool&   _pool;
        PacketHeader _header;
        Bytes        _payload;
        size_t       _header_received;
        size_t       _payload_received;
    };

    struct ReceivedPacket
    {
        PacketHeader header {};
        Bytes        payload;
    };

    enum class DropPolicy
    {
        Never,
        Oldest,
    };

    struct SentPacket
    {
        Bytes      bytes;
        DropPolicy policy {DropPolicy::Never};
    };

    class SendBuffer
    {
    public:
        explicit SendBuffer(size_t queue_capacity) :
            _queue(queue_capacity)
        {
        }

        SendBuffer(const SendBuffer&) = delete;
        SendBuffer& operator=(const SendBuffer&) = delete;

        explicit operator bool() const { return static_cast<bool>(_queue); }

        size_t capacity() const { return _queue.capacity(); }
        size_t size() const { return _queue.size(); }
        bool empty() const { return _queue.empty(); }
        bool full() const { return _queue.full(); }

        bool tryPush(SentPacket& packet)
        {
            if (_queue.full())
            {
                return false;
            }
            return _queue.tryPush(std::move(packet));
        }

        SentPacket pop()
        {
            SentPacket taken;

            _queue.tryPop(taken);
            return taken;
        }

        bool dropOldest(DropPolicy droppable)
        {
            for (size_t i = 0; i < _queue.size(); ++i)
            {
                if (_queue.at(i).policy == droppable)
                {
                    return _queue.removeAt(i);
                }
            }
            return false;
        }

    private:
        RingBuffer<SentPacket> _queue;
    };
}

#endif
