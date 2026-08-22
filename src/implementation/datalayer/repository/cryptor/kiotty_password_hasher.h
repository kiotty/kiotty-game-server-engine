#if !defined(KIOTTY_DATALAYER_REPOSITORY_CRYPTOR_PASSWORD_HASHER_H)
#define KIOTTY_DATALAYER_REPOSITORY_CRYPTOR_PASSWORD_HASHER_H

#include <core/kiotty_bytes.h>
#include <core/kiotty_random_source.h>
#include <core/kiotty_result.h>
#include <domain/entity/kiotty_crypto_code.h>

#include <cstddef>
#include <cstdint>

namespace kiotty
{
    struct HashParameters
    {
        uint32_t time_cost {3};
        uint32_t memory_kib {65536};
        uint32_t parallelism {1};
    };

    static const size_t PASSWORD_HASH_SALT_SIZE    = 16;
    static const size_t PASSWORD_HASH_DIGEST_SIZE  = 32;
    static const size_t PASSWORD_HASH_ENCODED_SIZE = 160;

    struct PasswordHash
    {
        char encoded[PASSWORD_HASH_ENCODED_SIZE] {0};
    };

    typedef Result<CryptoCode, PasswordHash> PasswordHashResult;

    class PasswordHasher
    {
    public:
        PasswordHasher(const HashParameters& parameters, IRandomSource& random);

        explicit operator bool() const { return _usable; }

        const HashParameters& parameters() const { return _parameters; }

        PasswordHashResult hashBlocking(ByteView password) const;
        bool               matchesBlocking(ByteView password, const PasswordHash& stored) const;

    private:
        static bool fitsEncodedSize(const HashParameters& parameters);

        HashParameters _parameters;
        IRandomSource& _random;
        bool           _usable;
    };
}

#endif
