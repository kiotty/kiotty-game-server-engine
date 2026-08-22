#if !defined(KIOTTY_DOMAIN_ENTITY_GAME_REQUEST_H)
#define KIOTTY_DOMAIN_ENTITY_GAME_REQUEST_H

#include <core/kiotty_bytes.h>
#include <domain/entity/kiotty_channel_id.h>

#include <cstdint>

namespace kiotty
{
    struct GameRequest
    {
        uint64_t  state_sequence {0};
        ChannelId channel_id;
        uint32_t  correlation_id {0};
        uint16_t  command {0};
        Bytes     payload;
    };
}

#endif
