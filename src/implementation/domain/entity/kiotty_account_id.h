#if !defined(KIOTTY_DOMAIN_ENTITY_ACCOUNT_ID_H)
#define KIOTTY_DOMAIN_ENTITY_ACCOUNT_ID_H

#include <cstddef>

namespace kiotty
{
    static const size_t ACCOUNT_NAME_SIZE     = 64;
    static const size_t ACCOUNT_NAME_MAX_CHARS = ACCOUNT_NAME_SIZE - 1;

    struct AccountId
    {
        char name[ACCOUNT_NAME_SIZE] {0};
    };

    inline bool tryMakeAccountId(const char* name, AccountId& out)
    {
        out = AccountId();

        if (name == nullptr)
        {
            return false;
        }

        size_t length = 0;

        while (name[length] != 0)
        {
            if (length == ACCOUNT_NAME_MAX_CHARS)
            {
                return false;
            }
            ++length;
        }

        for (size_t i = 0; i < length; ++i)
        {
            out.name[i] = name[i];
        }
        return true;
    }

    inline bool operator==(const AccountId& lhs, const AccountId& rhs)
    {
        for (size_t i = 0; i < ACCOUNT_NAME_SIZE; ++i)
        {
            if (lhs.name[i] != rhs.name[i])
            {
                return false;
            }
        }
        return true;
    }

    inline bool operator!=(const AccountId& lhs, const AccountId& rhs)
    {
        return !(lhs == rhs);
    }
}

#endif
