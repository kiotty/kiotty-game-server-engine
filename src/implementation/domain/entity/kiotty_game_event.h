#if !defined(KIOTTY_DOMAIN_ENTITY_GAME_EVENT_H)
#define KIOTTY_DOMAIN_ENTITY_GAME_EVENT_H

#include <core/kiotty_bytes.h>

#include <cstdint>

namespace kiotty
{
    struct GameEvent
    {
        uint16_t command {0};
        Bytes    payload;
    };
}

#endif
