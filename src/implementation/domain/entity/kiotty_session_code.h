#if !defined(KIOTTY_DOMAIN_ENTITY_SESSION_CODE_H)
#define KIOTTY_DOMAIN_ENTITY_SESSION_CODE_H

#include <cstdint>

namespace kiotty
{
    enum class SessionCode : int32_t
    {
        SESSION_SUCCESS = 0,

        SESSION_NOT_FOUND,
        SESSION_UNAVAILABLE,
        SESSION_ALREADY_AUTHENTICATED,
        SESSION_LOGIN_REJECTED,
        SESSION_TOO_MANY,
        SESSION_RANDOM_UNAVAILABLE,
    };
}

#endif
