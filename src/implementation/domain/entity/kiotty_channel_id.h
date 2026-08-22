#if !defined(KIOTTY_DOMAIN_ENTITY_CHANNEL_ID_H)
#define KIOTTY_DOMAIN_ENTITY_CHANNEL_ID_H

#include <cstdint>

namespace kiotty
{
    enum class ChannelCode : int32_t
    {
        CHANNEL_SUCCESS = 0,

        CHANNEL_POOL_EXHAUSTED,
        CHANNEL_NOT_FOUND,
        CHANNEL_STALE,
    };

    struct ChannelId
    {
        uint32_t index {0};
        uint32_t generation {0};
    };

    inline ChannelId makeChannelId(uint32_t index, uint32_t generation)
    {
        ChannelId id;
        id.index      = index;
        id.generation = generation;
        return id;
    }

    inline bool operator==(const ChannelId& lhs, const ChannelId& rhs)
    {
        return lhs.index == rhs.index && lhs.generation == rhs.generation;
    }

    inline bool operator!=(const ChannelId& lhs, const ChannelId& rhs)
    {
        return !(lhs == rhs);
    }
}

#endif
