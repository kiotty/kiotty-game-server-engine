#if !defined(KIOTTY_DOMAIN_CHANNEL_GAME_CHANNEL_POOL_H)
#define KIOTTY_DOMAIN_CHANNEL_GAME_CHANNEL_POOL_H

#include <core/kiotty_result.h>
#include <domain/channel/kiotty_game_channel.h>
#include <domain/entity/kiotty_channel_id.h>

#include <cassert>
#include <cstddef>
#include <mutex>
#include <type_traits>

namespace kiotty
{
    typedef Result<ChannelCode, GameChannel&> ChannelResult;

    class ChannelAccess
    {
    public:
        ChannelAccess(std::unique_lock<std::recursive_mutex>&& guard,
                      GameChannel* channel, ChannelCode code) :
            _guard(std::move(guard)),
            _channel(channel),
            _code(code)
        {
        }

        ChannelAccess(const ChannelAccess&) = delete;
        ChannelAccess& operator=(const ChannelAccess&) = delete;

        ChannelAccess(ChannelAccess&& other) noexcept :
            _guard(std::move(other._guard)),
            _channel(other._channel),
            _code(other._code)
        {
            other._channel = nullptr;
        }

        explicit operator bool() const { return _channel != nullptr; }

        ChannelCode code() const { return _code; }

        GameChannel& channel() const
        {
            assert(_channel != nullptr && "ChannelAccess::channel() on a failed access");
            return *_channel;
        }

    private:
        std::unique_lock<std::recursive_mutex> _guard;
        GameChannel*                           _channel;
        ChannelCode                            _code;
    };

    class GameChannelPool
    {
    public:
        explicit GameChannelPool(size_t capacity);
        ~GameChannelPool();

        GameChannelPool(const GameChannelPool&) = delete;
        GameChannelPool& operator=(const GameChannelPool&) = delete;

        explicit operator bool() const { return _slots != nullptr; }

        size_t capacity() const { return _capacity; }
        size_t size() const;

        ChannelResult create();
        void          remove(ChannelId id);
        ChannelAccess access(ChannelId id);

    private:
        struct Slot
        {
            typename std::aligned_storage<sizeof(GameChannel), alignof(GameChannel)>::type bytes;
            uint32_t generation {1};
            bool     live {false};

            GameChannel* channel() { return reinterpret_cast<GameChannel*>(&bytes); }
        };

        Slot* findLiveSlot(ChannelId id, ChannelCode& code);

        mutable std::recursive_mutex _lock;
        Slot*                _slots;
        size_t               _capacity;
        size_t               _size;
    };
}

#endif
