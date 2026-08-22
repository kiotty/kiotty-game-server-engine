#if !defined(KIOTTY_CORE_PACKET_WRITER_H)
#define KIOTTY_CORE_PACKET_WRITER_H

#include "kiotty_block_pool.h"
#include "kiotty_bytes.h"
#include "kiotty_packet_concept.h"

#include <chrono>
#include <cstdint>
#include <cstring>

namespace kiotty
{
    inline uint64_t monotonicMicroseconds()
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    inline Bytes writePacket(BlockPool& pool, uint16_t command, uint16_t flags,
                             uint32_t correlation_id, ByteView payload)
    {
        if (payload.size() > PACKET_MAX_PAYLOAD)
        {
            return Bytes();
        }

        Bytes packet(pool, PACKET_HEADER_SIZE + payload.size());

        if (!packet)
        {
            return packet;
        }

        PacketHeader header;
        header.magic.set(PACKET_MAGIC);
        header.correlation_id.set(correlation_id);
        header.timestamp.set(monotonicMicroseconds());
        header.command.set(command);
        header.flags.set(flags);
        header.version.set(PACKET_VERSION);
        header.payload_length.set(static_cast<uint16_t>(payload.size()));

        uint8_t* const out = packet.writableSpan().data();

        std::memcpy(out, &header, PACKET_HEADER_SIZE);

        if (payload.size() > 0)
        {
            std::memcpy(out + PACKET_HEADER_SIZE, payload.data(), payload.size());
        }
        return packet;
    }
}

#endif
