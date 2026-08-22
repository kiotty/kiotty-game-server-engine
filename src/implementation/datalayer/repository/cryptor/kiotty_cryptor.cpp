#include "kiotty_cryptor.h"

#include <core/kiotty_constant_time.h>

#include <cstring>
#include <utility>

namespace kiotty
{
    namespace
    {
        const uint64_t GHASH_REDUCTION = 0xE100000000000000ull;

        void buildCounterBlock(const uint8_t nonce[CRYPTOR_NONCE_SIZE], uint32_t counter,
                               uint8_t out[AES_BLOCK_SIZE])
        {
            std::memcpy(out, nonce, CRYPTOR_NONCE_SIZE);
            out[12] = static_cast<uint8_t>(counter >> 24);
            out[13] = static_cast<uint8_t>(counter >> 16);
            out[14] = static_cast<uint8_t>(counter >> 8);
            out[15] = static_cast<uint8_t>(counter);
        }
    }

    Cryptor::Cryptor(const CipherKey& key, IRandomSource& random, BlockPool& pool) :
        _cipher(key),
        _hash_key(),
        _random(random),
        _pool(pool)
    {
        uint8_t zero[AES_BLOCK_SIZE] = {0};
        uint8_t hash_key[AES_BLOCK_SIZE];

        _cipher.encryptBlock(zero, hash_key);
        _hash_key = loadBlock(hash_key, AES_BLOCK_SIZE);
    }

    CryptoResult Cryptor::encrypt(ByteView plain) const
    {
        if (plain.size() == 0)
        {
            return error(CryptoCode::CRYPTO_INVALID_ARGUMENT);
        }

        Bytes sealed(_pool, CRYPTOR_NONCE_SIZE + plain.size() + CRYPTOR_TAG_SIZE);

        if (!sealed)
        {
            return error(CryptoCode::CRYPTO_OUT_OF_MEMORY);
        }

        uint8_t* const nonce  = sealed.writableSpan().data();
        uint8_t* const cipher = nonce + CRYPTOR_NONCE_SIZE;
        uint8_t* const tag    = cipher + plain.size();

        if (!_random.fill(ByteSpan(nonce, CRYPTOR_NONCE_SIZE)))
        {
            return error(CryptoCode::CRYPTO_RANDOM_UNAVAILABLE);
        }

        keystreamXor(nonce, plain.data(), plain.size(), cipher);
        computeTag(nonce, cipher, plain.size(), tag);

        return CryptoResult(CryptoResult::Success(), std::move(sealed));
    }

    CryptoResult Cryptor::decrypt(ByteView sealed) const
    {
        if (sealed.size() <= CRYPTOR_OVERHEAD_SIZE)
        {
            return error(CryptoCode::CRYPTO_INVALID_ARGUMENT);
        }

        const size_t         length = sealed.size() - CRYPTOR_OVERHEAD_SIZE;
        const uint8_t* const nonce  = sealed.data();
        const uint8_t* const cipher = nonce + CRYPTOR_NONCE_SIZE;
        const uint8_t* const tag    = cipher + length;

        uint8_t expected[CRYPTOR_TAG_SIZE];
        computeTag(nonce, cipher, length, expected);

        if (!constantTimeEquals(ByteView(expected, CRYPTOR_TAG_SIZE), ByteView(tag, CRYPTOR_TAG_SIZE)))
        {
            return error(CryptoCode::CRYPTO_TAG_MISMATCH);
        }

        Bytes plain(_pool, length);

        if (!plain)
        {
            return error(CryptoCode::CRYPTO_OUT_OF_MEMORY);
        }

        keystreamXor(nonce, cipher, length, plain.writableSpan().data());

        return CryptoResult(CryptoResult::Success(), std::move(plain));
    }

    void Cryptor::keystreamXor(const uint8_t nonce[CRYPTOR_NONCE_SIZE],
                               const uint8_t* in, size_t length, uint8_t* out) const
    {
        uint8_t  counter_block[AES_BLOCK_SIZE];
        uint8_t  keystream[AES_BLOCK_SIZE];
        uint32_t counter = 2;

        for (size_t offset = 0; offset < length; offset += AES_BLOCK_SIZE)
        {
            buildCounterBlock(nonce, counter, counter_block);
            _cipher.encryptBlock(counter_block, keystream);
            ++counter;

            const size_t chunk = (length - offset < AES_BLOCK_SIZE) ? length - offset : AES_BLOCK_SIZE;

            for (size_t i = 0; i < chunk; ++i)
            {
                out[offset + i] = static_cast<uint8_t>(in[offset + i] ^ keystream[i]);
            }
        }
    }

    void Cryptor::computeTag(const uint8_t nonce[CRYPTOR_NONCE_SIZE],
                             const uint8_t* cipher, size_t length,
                             uint8_t tag[CRYPTOR_TAG_SIZE]) const
    {
        uint8_t counter_block[AES_BLOCK_SIZE];
        uint8_t mask[AES_BLOCK_SIZE];

        buildCounterBlock(nonce, 1, counter_block);
        _cipher.encryptBlock(counter_block, mask);

        storeBlock(ghash(cipher, length), tag);

        for (size_t i = 0; i < CRYPTOR_TAG_SIZE; ++i)
        {
            tag[i] ^= mask[i];
        }
    }

    Cryptor::Block128 Cryptor::ghash(const uint8_t* cipher, size_t length) const
    {
        Block128 x = { 0, 0 };

        for (size_t offset = 0; offset < length; offset += AES_BLOCK_SIZE)
        {
            const size_t   chunk = (length - offset < AES_BLOCK_SIZE) ? length - offset : AES_BLOCK_SIZE;
            const Block128 block = loadBlock(cipher + offset, chunk);

            x.hi ^= block.hi;
            x.lo ^= block.lo;
            x = multiply(x, _hash_key);
        }

        const Block128 lengths = { 0, static_cast<uint64_t>(length) * 8 };

        x.hi ^= lengths.hi;
        x.lo ^= lengths.lo;
        return multiply(x, _hash_key);
    }

    Cryptor::Block128 Cryptor::multiply(Block128 x, Block128 y)
    {
        Block128 z = { 0, 0 };
        Block128 v = y;

        for (int bit = 0; bit < 128; ++bit)
        {
            const uint64_t selector = (bit < 64) ? (x.hi >> (63 - bit)) : (x.lo >> (127 - bit));

            if (selector & 1)
            {
                z.hi ^= v.hi;
                z.lo ^= v.lo;
            }

            const bool carry = (v.lo & 1) != 0;

            v.lo = (v.lo >> 1) | (v.hi << 63);
            v.hi = v.hi >> 1;

            if (carry)
            {
                v.hi ^= GHASH_REDUCTION;
            }
        }
        return z;
    }

    Cryptor::Block128 Cryptor::loadBlock(const uint8_t* bytes, size_t length)
    {
        uint8_t padded[AES_BLOCK_SIZE] = {0};
        std::memcpy(padded, bytes, length);

        Block128 block = { 0, 0 };

        for (size_t i = 0; i < 8; ++i)
        {
            block.hi = (block.hi << 8) | padded[i];
            block.lo = (block.lo << 8) | padded[8 + i];
        }
        return block;
    }

    void Cryptor::storeBlock(Block128 block, uint8_t out[AES_BLOCK_SIZE])
    {
        for (size_t i = 0; i < 8; ++i)
        {
            out[i]     = static_cast<uint8_t>(block.hi >> (56 - 8 * i));
            out[8 + i] = static_cast<uint8_t>(block.lo >> (56 - 8 * i));
        }
    }
}
