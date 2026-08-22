#if !defined(KIOTTY_DATALAYER_REPOSITORY_CRYPTOR_AES256_H)
#define KIOTTY_DATALAYER_REPOSITORY_CRYPTOR_AES256_H

#include <domain/entity/kiotty_cipher_key.h>

#include <cstddef>
#include <cstdint>

namespace kiotty
{
    static const size_t AES_BLOCK_SIZE = 16;

    class Aes256
    {
    public:
        explicit Aes256(const CipherKey& key);
        ~Aes256();

        Aes256(const Aes256&) = delete;
        Aes256& operator=(const Aes256&) = delete;

        void encryptBlock(const uint8_t in[AES_BLOCK_SIZE], uint8_t out[AES_BLOCK_SIZE]) const;

    private:
        static const size_t ROUNDS          = 14;
        static const size_t ROUND_KEY_WORDS = 4 * (ROUNDS + 1);

        void expandKey(const CipherKey& key);

        uint32_t _round_keys[ROUND_KEY_WORDS];
    };
}

#endif
