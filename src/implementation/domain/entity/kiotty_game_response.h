#if !defined(KIOTTY_DOMAIN_ENTITY_GAME_RESPONSE_H)
#define KIOTTY_DOMAIN_ENTITY_GAME_RESPONSE_H

#include <core/kiotty_bytes.h>

#include <cstdint>

namespace kiotty
{
    struct GameResponse
    {
        uint32_t correlation_id {0};
        uint16_t command {0};
        Bytes    payload;
    };
}

#endif
