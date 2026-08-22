#if !defined(KIOTTY_DATALAYER_REPOSITORY_SESSION_SESSION_REPOSITORY_H)
#define KIOTTY_DATALAYER_REPOSITORY_SESSION_SESSION_REPOSITORY_H

#include <core/kiotty_random_source.h>
#include <core/kiotty_result.h>
#include <datalayer/repository/session/kiotty_session.h>
#include <datalayer/repository/session/kiotty_session_policy.h>
#include <domain/channel/kiotty_game_channel_pool.h>
#include <domain/entity/kiotty_account_id.h>
#include <domain/entity/kiotty_channel_id.h>
#include <domain/entity/kiotty_session_code.h>
#include <domain/entity/kiotty_session_token.h>

#include <cstddef>
#include <cstdint>
#include <mutex>

namespace kiotty
{
    typedef Result<SessionCode, Session> SessionResult;

    class SessionRepository
    {
    public:
        SessionRepository(GameChannelPool& channels, ISessionPolicy& policy,
                          IRandomSource& random, size_t max_sessions);
        ~SessionRepository();

        SessionRepository(const SessionRepository&) = delete;
        SessionRepository& operator=(const SessionRepository&) = delete;

        explicit operator bool() const { return _slots != nullptr; }

        size_t capacity() const { return _capacity; }
        size_t size() const;

        GameChannelPool& channels() const { return _channels; }
        ChannelId        channelOf(const SessionToken& token) const;

        SessionResult open(ChannelId channel, const AccountId& account);
        SessionResult rebind(ChannelId channel, const SessionToken& token);
        SessionResult find(ChannelId channel) const;
        void          close(const Session& session);

        void detach(ChannelId channel);
        void sweep(uint64_t now_ms);

    private:
        struct Slot
        {
            ChannelId    channel;
            AccountId    account;
            SessionToken token;
            uint64_t     detached_at_ms {0};
            uint32_t     orphan_lifetime_ms {0};
            bool         live {false};
            bool         attached {false};
            bool         detach_time_known {false};
        };

        Slot*   findAttached(ChannelId channel) const;
        Slot*   findByAccount(const AccountId& account) const;
        Slot*   findByToken(const SessionToken& token) const;
        Slot*   findFree() const;
        Session sessionOf(const Slot& slot) const;
        void    release(Slot& slot);

        mutable std::mutex _lock;
        GameChannelPool&   _channels;
        ISessionPolicy&    _policy;
        IRandomSource&     _random;
        Slot*              _slots;
        size_t             _capacity;
        size_t             _size;
    };
}

#endif
