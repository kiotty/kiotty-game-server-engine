#if !defined(KIOTTY_DATALAYER_REPOSITORY_SESSION_SESSION_H)
#define KIOTTY_DATALAYER_REPOSITORY_SESSION_SESSION_H

#include <core/kiotty_bytes.h>
#include <domain/entity/kiotty_account_id.h>
#include <domain/entity/kiotty_channel_id.h>
#include <domain/entity/kiotty_session_token.h>

#include <cstdint>

namespace kiotty
{
    class SessionRepository;

    class Session
    {
    public:
        Session();
        Session(const SessionRepository& sessions, const AccountId& account, const SessionToken& token);

        explicit operator bool() const { return _sessions != nullptr; }

        ChannelId           channel() const;
        const AccountId&    account() const { return _account; }
        const SessionToken& token() const { return _token; }

        bool reply(uint32_t correlation_id, uint16_t command, Bytes payload);
        bool notify(uint16_t command, Bytes payload);

    private:
        const SessionRepository* _sessions;
        AccountId                _account;
        SessionToken             _token;
    };
}

#endif
