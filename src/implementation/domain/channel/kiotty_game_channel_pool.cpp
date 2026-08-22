#include "kiotty_game_channel_pool.h"

#include <new>

namespace kiotty
{
    GameChannelPool::GameChannelPool(size_t capacity) :
        _slots(nullptr),
        _capacity(capacity),
        _size(0)
    {
        if (capacity > 0)
        {
            _slots = new (std::nothrow) Slot[capacity];
        }

        if (_slots == nullptr)
        {
            _capacity = 0;
        }
    }

    GameChannelPool::~GameChannelPool()
    {
        for (size_t i = 0; i < _capacity; ++i)
        {
            if (_slots[i].live)
            {
                _slots[i].channel()->~GameChannel();
            }
        }
        delete[] _slots;
    }

    size_t GameChannelPool::size() const
    {
        std::lock_guard<std::recursive_mutex> guard(_lock);

        return _size;
    }

    ChannelResult GameChannelPool::create()
    {
        std::lock_guard<std::recursive_mutex> guard(_lock);

        for (size_t i = 0; i < _capacity; ++i)
        {
            Slot& slot = _slots[i];

            if (slot.live)
            {
                continue;
            }

            const ChannelId id = makeChannelId(static_cast<uint32_t>(i), slot.generation);

            GameChannel* const channel = ::new (static_cast<void*>(&slot.bytes)) GameChannel(id);

            slot.live = true;
            ++_size;
            return okRef(*channel);
        }
        return error(ChannelCode::CHANNEL_POOL_EXHAUSTED);
    }

    void GameChannelPool::remove(ChannelId id)
    {
        std::lock_guard<std::recursive_mutex> guard(_lock);

        ChannelCode code = ChannelCode::CHANNEL_SUCCESS;
        Slot* const slot = findLiveSlot(id, code);

        if (slot == nullptr)
        {
            return;
        }

        slot->channel()->~GameChannel();
        slot->live = false;
        ++slot->generation;
        --_size;
    }

    ChannelAccess GameChannelPool::access(ChannelId id)
    {
        std::unique_lock<std::recursive_mutex> guard(_lock);

        ChannelCode code = ChannelCode::CHANNEL_SUCCESS;
        Slot* const slot = findLiveSlot(id, code);

        if (slot == nullptr)
        {
            guard.unlock();
            return ChannelAccess(std::move(guard), nullptr, code);
        }
        return ChannelAccess(std::move(guard), slot->channel(), code);
    }

    GameChannelPool::Slot* GameChannelPool::findLiveSlot(ChannelId id, ChannelCode& code)
    {
        if (id.index >= _capacity || !_slots[id.index].live)
        {
            code = ChannelCode::CHANNEL_NOT_FOUND;
            return nullptr;
        }

        Slot& slot = _slots[id.index];

        if (slot.generation != id.generation)
        {
            code = ChannelCode::CHANNEL_STALE;
            return nullptr;
        }
        return &slot;
    }
}
