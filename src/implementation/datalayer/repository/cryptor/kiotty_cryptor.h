#if !defined(KIOTTY_DATALAYER_REPOSITORY_CRYPTOR_CRYPTOR_H)
#define KIOTTY_DATALAYER_REPOSITORY_CRYPTOR_CRYPTOR_H

#include <core/kiotty_block_pool.h>
#include <core/kiotty_bytes.h>
#include <core/kiotty_random_source.h>
#include <core/kiotty_result.h>
#include <datalayer/repository/cryptor/kiotty_aes256.h>
#include <domain/entity/kiotty_cipher_key.h>
#include <domain/entity/kiotty_crypto_code.h>

#include <cstddef>
#include <cstdint>

namespace kiotty
{
    static const size_t CRYPTOR_NONCE_SIZE    = 12;
    static const size_t CRYPTOR_TAG_SIZE      = 16;
    static const size_t CRYPTOR_OVERHEAD_SIZE = CRYPTOR_NONCE_SIZE + CRYPTOR_TAG_SIZE;

    typedef Result<CryptoCode, Bytes> CryptoResult;

    class Cryptor
    {
    public:
        Cryptor(const CipherKey& key, IRandomSource& random, BlockPool& pool);

        Cryptor(const Cryptor&) = delete;
        Cryptor& operator=(const Cryptor&) = delete;

        CryptoResult encrypt(ByteView plain) const;
        CryptoResult decrypt(ByteView sealed) const;

    private:
        struct Block128
        {
            uint64_t hi;
            uint64_t lo;
        };

        void     keystreamXor(const uint8_t nonce[CRYPTOR_NONCE_SIZE],
                              const uint8_t* in, size_t length, uint8_t* out) const;
        void     computeTag(const uint8_t nonce[CRYPTOR_NONCE_SIZE],
                            const uint8_t* cipher, size_t length,
                            uint8_t tag[CRYPTOR_TAG_SIZE]) const;
        Block128 ghash(const uint8_t* cipher, size_t length) const;

        static Block128 multiply(Block128 x, Block128 y);
        static Block128 loadBlock(const uint8_t* bytes, size_t length);
        static void     storeBlock(Block128 block, uint8_t out[AES_BLOCK_SIZE]);

        Aes256         _cipher;
        Block128       _hash_key;
        IRandomSource& _random;
        BlockPool&     _pool;
    };
}

#endif
