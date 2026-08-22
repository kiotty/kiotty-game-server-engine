#include "kiotty_password_hasher.h"

#include <argon2.h>

#include <cstring>

namespace kiotty
{
    PasswordHasher::PasswordHasher(const HashParameters& parameters, IRandomSource& random) :
        _parameters(parameters),
        _random(random),
        _usable(false)
    {
        const bool parameters_in_range =
            parameters.time_cost >= ARGON2_MIN_TIME &&
            parameters.parallelism >= ARGON2_MIN_LANES &&
            parameters.memory_kib >= 8 * parameters.parallelism;

        _usable = parameters_in_range && fitsEncodedSize(parameters);
    }

    PasswordHashResult PasswordHasher::hashBlocking(ByteView password) const
    {
        if (!_usable || password.size() == 0)
        {
            return error(CryptoCode::CRYPTO_INVALID_ARGUMENT);
        }

        uint8_t salt[PASSWORD_HASH_SALT_SIZE];

        if (!_random.fill(ByteSpan(salt, PASSWORD_HASH_SALT_SIZE)))
        {
            return error(CryptoCode::CRYPTO_RANDOM_UNAVAILABLE);
        }

        PasswordHash hash;

        const int status = argon2id_hash_encoded(_parameters.time_cost,
                                                 _parameters.memory_kib,
                                                 _parameters.parallelism,
                                                 password.data(), password.size(),
                                                 salt, PASSWORD_HASH_SALT_SIZE,
                                                 PASSWORD_HASH_DIGEST_SIZE,
                                                 hash.encoded, PASSWORD_HASH_ENCODED_SIZE);

        if (status == ARGON2_MEMORY_ALLOCATION_ERROR)
        {
            return error(CryptoCode::CRYPTO_OUT_OF_MEMORY);
        }
        if (status != ARGON2_OK)
        {
            return error(CryptoCode::CRYPTO_FAILED);
        }
        return ok(hash);
    }

    bool PasswordHasher::matchesBlocking(ByteView password, const PasswordHash& stored) const
    {
        if (password.size() == 0)
        {
            return false;
        }
        if (std::memchr(stored.encoded, 0, PASSWORD_HASH_ENCODED_SIZE) == nullptr)
        {
            return false;
        }
        return argon2id_verify(stored.encoded, password.data(), password.size()) == ARGON2_OK;
    }

    bool PasswordHasher::fitsEncodedSize(const HashParameters& parameters)
    {
        const size_t needed = argon2_encodedlen(parameters.time_cost,
                                                parameters.memory_kib,
                                                parameters.parallelism,
                                                PASSWORD_HASH_SALT_SIZE,
                                                PASSWORD_HASH_DIGEST_SIZE,
                                                Argon2_id);
        return needed <= PASSWORD_HASH_ENCODED_SIZE;
    }
}
