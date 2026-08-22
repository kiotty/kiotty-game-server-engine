#if !defined(KIOTTY_TEST_SESSION_H)
#define KIOTTY_TEST_SESSION_H

#include <core/kiotty_random_source.h>
#include <datalayer/repository/session/kiotty_session_policy.h>
#include <domain/entity/kiotty_account_id.h>

#include <cstddef>
#include <cstdint>

namespace kiotty_test
{
    // An AccountId from a literal the test knows is valid. tryMakeAccountId
    // refuses null and over-long names; a test that wants those goes through
    // it directly and looks at the bool.
    inline kiotty::AccountId accountOf(const char* name)
    {
        kiotty::AccountId id;
        tryMakeAccountId(name, id);
        return id;
    }

    // Deterministic bytes so a token can be predicted and compared. Every fill
    // continues the same counter, so two tokens from one source never collide.
    class CounterRandom : public kiotty::IRandomSource
    {
    public:
        CounterRandom() :
            next(1),
            fails(false)
        {
        }

        bool fill(kiotty::ByteSpan out) override
        {
            if (fails)
            {
                return false;
            }

            for (size_t i = 0; i < out.size(); ++i)
            {
                out.data()[i] = static_cast<uint8_t>(next);
                ++next;
            }
            return true;
        }

        uint32_t next;
        bool     fails;
    };

    // A policy whose answers are plain fields, so a test states the rule it is
    // exercising instead of deriving it.
    class FixedPolicy : public kiotty::ISessionPolicy
    {
    public:
        FixedPolicy() :
            orphan_lifetime_ms(0),
            replaces(true)
        {
        }

        uint32_t orphanLifetimeMs(const kiotty::Session&) const override
        {
            return orphan_lifetime_ms;
        }

        bool replacesPreviousLogin(const kiotty::AccountId&) const override
        {
            return replaces;
        }

        uint32_t orphan_lifetime_ms;
        bool     replaces;
    };
}

#endif
