#include "kiotty_session_repository.h"

#include <new>

namespace kiotty
{
    SessionRepository::SessionRepository(GameChannelPool& channels, ISessionPolicy& policy,
                                         IRandomSource& random, size_t max_sessions) :
        _lock(),
        _channels(channels),
        _policy(policy),
        _random(random),
        _slots(nullptr),
        _capacity(max_sessions),
        _size(0)
    {
        if (max_sessions > 0)
        {
            _slots = new (std::nothrow) Slot[max_sessions];
        }

        if (_slots == nullptr)
        {
            _capacity = 0;
        }
    }

    SessionRepository::~SessionRepository()
    {
        delete[] _slots;
    }

    size_t SessionRepository::size() const
    {
        std::lock_guard<std::mutex> guard(_lock);

        return _size;
    }

    SessionResult SessionRepository::open(ChannelId channel, const AccountId& account)
    {
        std::lock_guard<std::mutex> guard(_lock);

        if (findAttached(channel) != nullptr)
        {
            return error(SessionCode::SESSION_ALREADY_AUTHENTICATED);
        }

        Slot* const previous = findByAccount(account);

        if (previous != nullptr && !_policy.replacesPreviousLogin(account))
        {
            return error(SessionCode::SESSION_LOGIN_REJECTED);
        }
        if (previous == nullptr && findFree() == nullptr)
        {
            return error(SessionCode::SESSION_TOO_MANY);
        }

        SessionToken token;

        if (!_random.fill(token.writableSpan()))
        {
            return error(SessionCode::SESSION_RANDOM_UNAVAILABLE);
        }

        if (previous != nullptr)
        {
            release(*previous);
        }

        Slot* const slot = findFree();

        slot->channel            = channel;
        slot->account            = account;
        slot->token              = token;
        slot->detach_time_known  = false;
        slot->orphan_lifetime_ms = 0;
        slot->live               = true;
        slot->attached           = true;
        ++_size;

        return ok(sessionOf(*slot));
    }

    SessionResult SessionRepository::rebind(ChannelId channel, const SessionToken& token)
    {
        std::lock_guard<std::mutex> guard(_lock);

        if (findAttached(channel) != nullptr)
        {
            return error(SessionCode::SESSION_ALREADY_AUTHENTICATED);
        }

        Slot* const slot = findByToken(token);

        if (slot == nullptr)
        {
            return error(SessionCode::SESSION_UNAVAILABLE);
        }

        slot->channel            = channel;
        slot->detach_time_known  = false;
        slot->orphan_lifetime_ms = 0;
        slot->attached           = true;

        return ok(sessionOf(*slot));
    }

    SessionResult SessionRepository::find(ChannelId channel) const
    {
        std::lock_guard<std::mutex> guard(_lock);

        const Slot* const slot = findAttached(channel);

        if (slot == nullptr)
        {
            return error(SessionCode::SESSION_NOT_FOUND);
        }
        return ok(sessionOf(*slot));
    }

    void SessionRepository::close(const Session& session)
    {
        std::lock_guard<std::mutex> guard(_lock);

        Slot* const slot = findByToken(session.token());

        if (slot != nullptr)
        {
            release(*slot);
        }
    }

    void SessionRepository::detach(ChannelId channel)
    {
        Session detaching;

        {
            std::lock_guard<std::mutex> guard(_lock);

            const Slot* const slot = findAttached(channel);

            if (slot == nullptr)
            {
                return;
            }
            detaching = sessionOf(*slot);
        }

        const uint32_t lifetime_ms = _policy.orphanLifetimeMs(detaching);

        std::lock_guard<std::mutex> guard(_lock);

        Slot* const slot = findByToken(detaching.token());

        if (slot == nullptr || !slot->attached || slot->channel != channel)
        {
            return;
        }

        if (lifetime_ms == 0)
        {
            release(*slot);
            return;
        }

        slot->attached           = false;
        slot->detach_time_known  = false;
        slot->orphan_lifetime_ms = lifetime_ms;
    }

    void SessionRepository::sweep(uint64_t now_ms)
    {
        std::lock_guard<std::mutex> guard(_lock);

        for (size_t i = 0; i < _capacity; ++i)
        {
            Slot& slot = _slots[i];

            if (!slot.live || slot.attached)
            {
                continue;
            }
            if (!slot.detach_time_known)
            {
                slot.detached_at_ms    = now_ms;
                slot.detach_time_known = true;
                continue;
            }
            if (now_ms - slot.detached_at_ms >= slot.orphan_lifetime_ms)
            {
                release(slot);
            }
        }
    }

    SessionRepository::Slot* SessionRepository::findAttached(ChannelId channel) const
    {
        for (size_t i = 0; i < _capacity; ++i)
        {
            Slot& slot = _slots[i];

            if (slot.live && slot.attached && slot.channel == channel)
            {
                return &slot;
            }
        }
        return nullptr;
    }

    SessionRepository::Slot* SessionRepository::findByAccount(const AccountId& account) const
    {
        for (size_t i = 0; i < _capacity; ++i)
        {
            Slot& slot = _slots[i];

            if (slot.live && slot.account == account)
            {
                return &slot;
            }
        }
        return nullptr;
    }

    SessionRepository::Slot* SessionRepository::findByToken(const SessionToken& token) const
    {
        Slot* found = nullptr;

        for (size_t i = 0; i < _capacity; ++i)
        {
            Slot& slot = _slots[i];

            if (slot.live && slot.token == token)
            {
                found = &slot;
            }
        }
        return found;
    }

    SessionRepository::Slot* SessionRepository::findFree() const
    {
        for (size_t i = 0; i < _capacity; ++i)
        {
            if (!_slots[i].live)
            {
                return &_slots[i];
            }
        }
        return nullptr;
    }

    ChannelId SessionRepository::channelOf(const SessionToken& token) const
    {
        std::lock_guard<std::mutex> guard(_lock);

        const Slot* const slot = findByToken(token);

        if (slot == nullptr || !slot->attached)
        {
            return ChannelId();
        }
        return slot->channel;
    }

    Session SessionRepository::sessionOf(const Slot& slot) const
    {
        return Session(*this, slot.account, slot.token);
    }

    void SessionRepository::release(Slot& slot)
    {
        slot.live     = false;
        slot.attached = false;
        slot.token    = SessionToken();
        --_size;
    }
}
