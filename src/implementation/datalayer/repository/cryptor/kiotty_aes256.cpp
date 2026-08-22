#include "kiotty_aes256.h"

#include <cstring>

namespace kiotty
{
    namespace
    {
        uint8_t rotateLeft8(uint8_t value, unsigned shift)
        {
            return static_cast<uint8_t>((value << shift) | (value >> (8 - shift)));
        }

        uint8_t xtime(uint8_t value)
        {
            return static_cast<uint8_t>((value << 1) ^ ((value & 0x80) ? 0x1B : 0x00));
        }

        struct SBox
        {
            uint8_t table[256];

            SBox()
            {
                uint8_t p = 1;
                uint8_t q = 1;

                do
                {
                    p = static_cast<uint8_t>(p ^ xtime(p));

                    q = static_cast<uint8_t>(q ^ (q << 1));
                    q = static_cast<uint8_t>(q ^ (q << 2));
                    q = static_cast<uint8_t>(q ^ (q << 4));
                    q = static_cast<uint8_t>(q ^ ((q & 0x80) ? 0x09 : 0x00));

                    const uint8_t affine = static_cast<uint8_t>(
                        q ^ rotateLeft8(q, 1) ^ rotateLeft8(q, 2) ^ rotateLeft8(q, 3) ^ rotateLeft8(q, 4));

                    table[p] = static_cast<uint8_t>(affine ^ 0x63);
                }
                while (p != 1);

                table[0] = 0x63;
            }
        };

        const uint8_t* sbox()
        {
            static const SBox box;
            return box.table;
        }

        uint32_t subWord(uint32_t word)
        {
            const uint8_t* const s = sbox();

            return (static_cast<uint32_t>(s[(word >> 24) & 0xFF]) << 24) |
                   (static_cast<uint32_t>(s[(word >> 16) & 0xFF]) << 16) |
                   (static_cast<uint32_t>(s[(word >> 8) & 0xFF]) << 8) |
                   (static_cast<uint32_t>(s[word & 0xFF]));
        }

        uint32_t rotWord(uint32_t word)
        {
            return (word << 8) | (word >> 24);
        }

        uint32_t loadWord(const uint8_t* bytes)
        {
            return (static_cast<uint32_t>(bytes[0]) << 24) |
                   (static_cast<uint32_t>(bytes[1]) << 16) |
                   (static_cast<uint32_t>(bytes[2]) << 8) |
                   (static_cast<uint32_t>(bytes[3]));
        }

        void addRoundKey(uint8_t state[AES_BLOCK_SIZE], const uint32_t* round_key)
        {
            for (size_t column = 0; column < 4; ++column)
            {
                const uint32_t word = round_key[column];

                state[4 * column + 0] ^= static_cast<uint8_t>(word >> 24);
                state[4 * column + 1] ^= static_cast<uint8_t>(word >> 16);
                state[4 * column + 2] ^= static_cast<uint8_t>(word >> 8);
                state[4 * column + 3] ^= static_cast<uint8_t>(word);
            }
        }

        void subBytes(uint8_t state[AES_BLOCK_SIZE])
        {
            const uint8_t* const s = sbox();

            for (size_t i = 0; i < AES_BLOCK_SIZE; ++i)
            {
                state[i] = s[state[i]];
            }
        }

        void shiftRows(uint8_t state[AES_BLOCK_SIZE])
        {
            uint8_t shifted[AES_BLOCK_SIZE];

            for (size_t row = 0; row < 4; ++row)
            {
                for (size_t column = 0; column < 4; ++column)
                {
                    shifted[row + 4 * column] = state[row + 4 * ((column + row) % 4)];
                }
            }
            std::memcpy(state, shifted, AES_BLOCK_SIZE);
        }

        void mixColumns(uint8_t state[AES_BLOCK_SIZE])
        {
            for (size_t column = 0; column < 4; ++column)
            {
                uint8_t* const c = state + 4 * column;

                const uint8_t a0 = c[0];
                const uint8_t a1 = c[1];
                const uint8_t a2 = c[2];
                const uint8_t a3 = c[3];
                const uint8_t all = static_cast<uint8_t>(a0 ^ a1 ^ a2 ^ a3);

                c[0] = static_cast<uint8_t>(a0 ^ all ^ xtime(static_cast<uint8_t>(a0 ^ a1)));
                c[1] = static_cast<uint8_t>(a1 ^ all ^ xtime(static_cast<uint8_t>(a1 ^ a2)));
                c[2] = static_cast<uint8_t>(a2 ^ all ^ xtime(static_cast<uint8_t>(a2 ^ a3)));
                c[3] = static_cast<uint8_t>(a3 ^ all ^ xtime(static_cast<uint8_t>(a3 ^ a0)));
            }
        }
    }

    Aes256::Aes256(const CipherKey& key)
    {
        expandKey(key);
    }

    Aes256::~Aes256()
    {
        volatile uint32_t* const keys = _round_keys;

        for (size_t i = 0; i < ROUND_KEY_WORDS; ++i)
        {
            keys[i] = 0;
        }
    }

    void Aes256::encryptBlock(const uint8_t in[AES_BLOCK_SIZE], uint8_t out[AES_BLOCK_SIZE]) const
    {
        uint8_t state[AES_BLOCK_SIZE];
        std::memcpy(state, in, AES_BLOCK_SIZE);

        addRoundKey(state, _round_keys);

        for (size_t round = 1; round < ROUNDS; ++round)
        {
            subBytes(state);
            shiftRows(state);
            mixColumns(state);
            addRoundKey(state, _round_keys + 4 * round);
        }

        subBytes(state);
        shiftRows(state);
        addRoundKey(state, _round_keys + 4 * ROUNDS);

        std::memcpy(out, state, AES_BLOCK_SIZE);
    }

    void Aes256::expandKey(const CipherKey& key)
    {
        static const size_t KEY_WORDS = CIPHER_KEY_SIZE / 4;

        for (size_t i = 0; i < KEY_WORDS; ++i)
        {
            _round_keys[i] = loadWord(key.bytes + 4 * i);
        }

        uint32_t rcon = 0x01000000;

        for (size_t i = KEY_WORDS; i < ROUND_KEY_WORDS; ++i)
        {
            uint32_t temp = _round_keys[i - 1];

            if (i % KEY_WORDS == 0)
            {
                temp = subWord(rotWord(temp)) ^ rcon;
                rcon = static_cast<uint32_t>(xtime(static_cast<uint8_t>(rcon >> 24))) << 24;
            }
            else if (i % KEY_WORDS == 4)
            {
                temp = subWord(temp);
            }
            _round_keys[i] = _round_keys[i - KEY_WORDS] ^ temp;
        }
    }
}
