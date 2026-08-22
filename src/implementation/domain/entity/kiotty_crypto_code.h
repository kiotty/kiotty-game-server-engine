#if !defined(KIOTTY_DOMAIN_ENTITY_CRYPTO_CODE_H)
#define KIOTTY_DOMAIN_ENTITY_CRYPTO_CODE_H

#include <cstdint>

namespace kiotty
{
    enum class CryptoCode : int32_t
    {
        CRYPTO_SUCCESS = 0,

        CRYPTO_INVALID_ARGUMENT,
        CRYPTO_OUT_OF_MEMORY,
        CRYPTO_RANDOM_UNAVAILABLE,
        CRYPTO_TAG_MISMATCH,
        CRYPTO_FAILED,
    };
}

#endif
