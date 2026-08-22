#if !defined(KIOTTY_DOMAIN_CODEC_PACKET_CODEC_H)
#define KIOTTY_DOMAIN_CODEC_PACKET_CODEC_H

#include <core/kiotty_block_pool.h>
#include <core/kiotty_bytes.h>
#include <core/kiotty_connection_buffer.h>
#include <core/kiotty_packet_writer.h>
#include <domain/entity/kiotty_channel_id.h>
#include <domain/entity/kiotty_game_event.h>
#include <domain/entity/kiotty_game_request.h>
#include <domain/entity/kiotty_game_response.h>

#include <cstdint>
#include <utility>

namespace kiotty
{
    class IPacketCodec
    {
    public:
        virtual ~IPacketCodec() {}

        virtual bool  decode(ReceivedPacket& packet, ChannelId channel_id, GameRequest& out) = 0;
        virtual Bytes encode(BlockPool& pool, const GameResponse& response) = 0;
        virtual Bytes encode(BlockPool& pool, const GameEvent& event, uint32_t sequence) = 0;
    };

    class DefaultPacketCodec : public IPacketCodec
    {
    public:
        bool decode(ReceivedPacket& packet, ChannelId channel_id, GameRequest& out) override
        {
            out.channel_id     = channel_id;
            out.correlation_id = packet.header.correlation_id.get();
            out.command        = packet.header.command.get();
            out.state_sequence = packet.header.timestamp.get();
            out.payload        = std::move(packet.payload);
            return true;
        }

        Bytes encode(BlockPool& pool, const GameResponse& response) override
        {
            return writePacket(pool, response.command, 0,
                               response.correlation_id, response.payload.view());
        }

        Bytes encode(BlockPool& pool, const GameEvent& event, uint32_t sequence) override
        {
            return writePacket(pool, event.command, PACKET_FLAG_EVENT,
                               sequence, event.payload.view());
        }
    };
}

#endif
