#if !defined(KIOTTY_DOMAIN_ENTITY_SESSION_TOKEN_H)
#define KIOTTY_DOMAIN_ENTITY_SESSION_TOKEN_H

#include <core/kiotty_bytes.h>
#include <core/kiotty_constant_time.h>

#include <cstddef>
#include <cstdint>

namespace kiotty
{
    static const size_t SESSION_TOKEN_SIZE = 32;

    struct SessionToken
    {
        uint8_t bytes[SESSION_TOKEN_SIZE] {0};

        ByteView view() const { return ByteView(bytes, SESSION_TOKEN_SIZE); }
        ByteSpan writableSpan() { return ByteSpan(bytes, SESSION_TOKEN_SIZE); }
    };

    inline bool operator==(const SessionToken& lhs, const SessionToken& rhs)
    {
        return constantTimeEquals(lhs.view(), rhs.view());
    }

    inline bool operator!=(const SessionToken& lhs, const SessionToken& rhs)
    {
        return !(lhs == rhs);
    }
}

#endif
