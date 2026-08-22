#if !defined(KIOTTY_DATALAYER_REPOSITORY_SESSION_SESSION_POLICY_H)
#define KIOTTY_DATALAYER_REPOSITORY_SESSION_SESSION_POLICY_H

#include <datalayer/repository/session/kiotty_session.h>
#include <domain/entity/kiotty_account_id.h>

#include <cstdint>

namespace kiotty
{
    class ISessionPolicy
    {
    public:
        virtual ~ISessionPolicy() {}

        virtual uint32_t orphanLifetimeMs(const Session& session) const = 0;
        virtual bool     replacesPreviousLogin(const AccountId& account) const = 0;
    };
}

#endif
