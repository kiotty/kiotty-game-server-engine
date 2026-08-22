// Aes256 and Cryptor are the engine's own AES-256-GCM. A cipher that round
// trips is not evidence of anything - a broken cipher round trips too - so
// the anchor here is the published vectors: FIPS-197 C.3 for the block
// cipher, NIST GCM tests 14 and 16 for the whole construction. They need a
// random source that hands out a chosen nonce; FixedNonce below is that.
//
// The rest is the wrapper's contract: the sealed layout (nonce | cipher |
// tag), the argument checks, and that every single bit of the sealed bytes
// is covered by the tag.

#include <core/kiotty_block_pool.h>
#include <datalayer/repository/cryptor/kiotty_aes256.h>
#include <datalayer/repository/cryptor/kiotty_cryptor.h>
#include <domain/entity/kiotty_cipher_key.h>

#include "support/kiotty_test_session.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using kiotty::AES_BLOCK_SIZE;
using kiotty::Aes256;
using kiotty::BlockPool;
using kiotty::Bytes;
using kiotty::ByteSpan;
using kiotty::ByteView;
using kiotty::CIPHER_KEY_SIZE;
using kiotty::CipherKey;
using kiotty::CRYPTOR_NONCE_SIZE;
using kiotty::CRYPTOR_OVERHEAD_SIZE;
using kiotty::CRYPTOR_TAG_SIZE;
using kiotty::CryptoCode;
using kiotty::Cryptor;
using kiotty::CryptoResult;
using kiotty::IRandomSource;
using kiotty_test::CounterRandom;

static_assert(CRYPTOR_OVERHEAD_SIZE == 28, "the tests below assume a 12-byte nonce and a 16-byte tag");
static_assert(CRYPTOR_NONCE_SIZE == 12, "the tests below assume a 12-byte nonce");
static_assert(CRYPTOR_TAG_SIZE == 16, "the tests below assume a 16-byte tag");
static_assert(AES_BLOCK_SIZE == 16, "the tests below assume a 16-byte block");
static_assert(CIPHER_KEY_SIZE == 32, "the tests below assume a 32-byte key");

namespace
{
    typedef std::vector<uint8_t> Blob;

    uint8_t hexDigit(char c)
    {
        if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
        return static_cast<uint8_t>(c - 'A' + 10);
    }

    Blob fromHex(const char* hex)
    {
        Blob out;

        for (size_t i = 0; hex[i] != 0 && hex[i + 1] != 0; i += 2)
        {
            out.push_back(static_cast<uint8_t>(hexDigit(hex[i]) * 16 + hexDigit(hex[i + 1])));
        }
        return out;
    }

    Blob pattern(uint8_t seed, size_t count)
    {
        Blob out(count);

        for (size_t i = 0; i < count; ++i)
        {
            out[i] = static_cast<uint8_t>(seed + i * 7);
        }
        return out;
    }

    ByteView viewOf(const Blob& b)
    {
        return ByteView(b.empty() ? nullptr : b.data(), b.size());
    }

    Blob toBlob(const Bytes& bytes)
    {
        return Blob(bytes.data(), bytes.data() + bytes.size());
    }

    CipherKey keyFromHex(const char* hex)
    {
        const Blob bytes = fromHex(hex);
        CipherKey  key;

        std::memcpy(key.bytes, bytes.data(), CIPHER_KEY_SIZE);
        return key;
    }

    CipherKey keyOfCountingBytes()
    {
        CipherKey key;

        for (size_t i = 0; i < CIPHER_KEY_SIZE; ++i)
        {
            key.bytes[i] = static_cast<uint8_t>(i);
        }
        return key;
    }

    // Hands out one chosen nonce on every fill. What makes a published GCM
    // vector reproducible: the nonce is an input there, not a random draw.
    class FixedNonce : public IRandomSource
    {
    public:
        explicit FixedNonce(const char* hex) :
            nonce(fromHex(hex))
        {
        }

        bool fill(ByteSpan out) override
        {
            for (size_t i = 0; i < out.size(); ++i)
            {
                out.data()[i] = nonce[i % nonce.size()];
            }
            return true;
        }

        Blob nonce;
    };

    // The default pool: its largest class is 65536, so a 70000-byte message
    // goes to the heap fallback - one of the length rows below relies on it.
    class DefaultPool
    {
    public:
        DefaultPool() :
            _pool(kiotty::defaultBlockClasses(), kiotty::defaultBlockClassCount())
        {
        }

        BlockPool& pool() { return _pool; }

    private:
        BlockPool _pool;
    };
}

// -----------------------------------------------------------------------------
// Aes256: the block cipher alone
// -----------------------------------------------------------------------------

TEST(Aes256, EncryptBlockMatchesFips197AppendixC3)
{
    const Aes256 cipher(keyOfCountingBytes());
    const Blob   plain    = fromHex("00112233445566778899aabbccddeeff");
    const Blob   expected = fromHex("8ea2b7ca516745bfeafc49904b496089");
    uint8_t      out[AES_BLOCK_SIZE];

    cipher.encryptBlock(plain.data(), out);

    EXPECT_EQ(expected, Blob(out, out + AES_BLOCK_SIZE));
}

TEST(Aes256, EncryptBlockIsDeterministicAndKeyDependent)
{
    const Aes256 first(keyOfCountingBytes());
    const Aes256 same(keyOfCountingBytes());

    CipherKey other_key = keyOfCountingBytes();
    other_key.bytes[CIPHER_KEY_SIZE - 1] ^= 0x01;
    const Aes256 other(other_key);

    const Blob plain = fromHex("00112233445566778899aabbccddeeff");
    uint8_t    a[AES_BLOCK_SIZE];
    uint8_t    b[AES_BLOCK_SIZE];
    uint8_t    c[AES_BLOCK_SIZE];

    first.encryptBlock(plain.data(), a);
    same.encryptBlock(plain.data(), b);
    other.encryptBlock(plain.data(), c);

    EXPECT_EQ(0, std::memcmp(a, b, AES_BLOCK_SIZE));
    EXPECT_NE(0, std::memcmp(a, c, AES_BLOCK_SIZE));
}

TEST(Aes256, EncryptBlockInPlaceGivesTheSameAnswer)
{
    // Callers may pass the same buffer for in and out.
    const Aes256 cipher(keyOfCountingBytes());
    const Blob   expected = fromHex("8ea2b7ca516745bfeafc49904b496089");
    Blob         buffer   = fromHex("00112233445566778899aabbccddeeff");

    cipher.encryptBlock(buffer.data(), buffer.data());

    EXPECT_EQ(expected, buffer);
}

// -----------------------------------------------------------------------------
// Cryptor: published GCM vectors
// -----------------------------------------------------------------------------

namespace
{
    struct GcmVector
    {
        const char* key;
        const char* nonce;
        const char* plain;
        const char* cipher;
        const char* tag;
        const char* name;
    };

    const GcmVector kGcmVectors[] =
    {
        {
            "0000000000000000000000000000000000000000000000000000000000000000",
            "000000000000000000000000",
            "00000000000000000000000000000000",
            "cea7403d4d606b6e074ec5d3baf39d18",
            "d0d1c8a799996bf0265b98b5d48ab919",
            "NistGcmTest14",
        },
        {
            "feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308",
            "cafebabefacedbaddecaf888",
            "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
            "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b391aafd255",
            "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa"
            "8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662898015ad",
            "b094dac5d93471bdec1a502270e3cc6c",
            "NistGcmTest16",
        },
    };

    std::string gcmNameOf(const ::testing::TestParamInfo<GcmVector>& info)
    {
        return info.param.name;
    }

    class GcmVectors : public ::testing::TestWithParam<GcmVector>
    {
    };
}

TEST_P(GcmVectors, EncryptProducesThePublishedCiphertextAndTag)
{
    const GcmVector& v = GetParam();

    DefaultPool pool;
    FixedNonce  random(v.nonce);
    Cryptor     cryptor(keyFromHex(v.key), random, pool.pool());

    const Blob plain  = fromHex(v.plain);
    const Blob cipher = fromHex(v.cipher);
    const Blob tag    = fromHex(v.tag);

    CryptoResult sealed = cryptor.encrypt(viewOf(plain));

    ASSERT_TRUE(sealed.isOk());
    ASSERT_EQ(plain.size() + CRYPTOR_OVERHEAD_SIZE, sealed.value().size());

    const Blob out = toBlob(sealed.value());

    EXPECT_EQ(random.nonce, Blob(out.begin(), out.begin() + CRYPTOR_NONCE_SIZE));
    EXPECT_EQ(cipher, Blob(out.begin() + CRYPTOR_NONCE_SIZE, out.end() - CRYPTOR_TAG_SIZE));
    EXPECT_EQ(tag, Blob(out.end() - CRYPTOR_TAG_SIZE, out.end()));
}

TEST_P(GcmVectors, DecryptOfThePublishedBytesGivesThePlaintext)
{
    const GcmVector& v = GetParam();

    DefaultPool   pool;
    CounterRandom random;   // decrypt must not need the nonce source
    Cryptor       cryptor(keyFromHex(v.key), random, pool.pool());

    Blob sealed = fromHex(v.nonce);
    const Blob cipher = fromHex(v.cipher);
    const Blob tag    = fromHex(v.tag);
    sealed.insert(sealed.end(), cipher.begin(), cipher.end());
    sealed.insert(sealed.end(), tag.begin(), tag.end());

    CryptoResult opened = cryptor.decrypt(viewOf(sealed));

    ASSERT_TRUE(opened.isOk());
    EXPECT_EQ(fromHex(v.plain), toBlob(opened.value()));
}

INSTANTIATE_TEST_SUITE_P(Published, GcmVectors, ::testing::ValuesIn(kGcmVectors), gcmNameOf);

// -----------------------------------------------------------------------------
// Cryptor: round trip over the length classes
// -----------------------------------------------------------------------------

namespace
{
    struct LengthCase
    {
        size_t      length;
        const char* name;
    };

    // 1 (smallest) / 15, 16, 17 (around one AES block) / 32 (two blocks) /
    // 1000 / 70000 (above the largest pool class: heap fallback).
    const LengthCase kLengthCases[] =
    {
        { 1,     "OneByte" },
        { 15,    "OneBelowABlock" },
        { 16,    "ExactlyABlock" },
        { 17,    "OneAboveABlock" },
        { 32,    "TwoBlocks" },
        { 1000,  "OneThousand" },
        { 70000, "AboveLargestPoolClass" },
    };

    std::string lengthNameOf(const ::testing::TestParamInfo<LengthCase>& info)
    {
        return info.param.name;
    }

    class RoundTrip : public ::testing::TestWithParam<LengthCase>
    {
    };
}

TEST_P(RoundTrip, DecryptOfEncryptGivesTheOriginalBytes)
{
    const LengthCase& c = GetParam();

    DefaultPool   pool;
    CounterRandom random;
    Cryptor       cryptor(keyOfCountingBytes(), random, pool.pool());

    const Blob plain = pattern(3, c.length);

    CryptoResult sealed = cryptor.encrypt(viewOf(plain));
    ASSERT_TRUE(sealed.isOk());
    EXPECT_EQ(c.length + CRYPTOR_OVERHEAD_SIZE, sealed.value().size());

    // The ciphertext is not the plaintext (a keystream of zeros would pass
    // the round trip).
    EXPECT_NE(plain, Blob(sealed.value().data() + CRYPTOR_NONCE_SIZE,
                          sealed.value().data() + CRYPTOR_NONCE_SIZE + c.length));

    CryptoResult opened = cryptor.decrypt(sealed.value().view());
    ASSERT_TRUE(opened.isOk());
    EXPECT_EQ(plain, toBlob(opened.value()));
}

INSTANTIATE_TEST_SUITE_P(AllLengths, RoundTrip, ::testing::ValuesIn(kLengthCases), lengthNameOf);

// -----------------------------------------------------------------------------
// Cryptor: encrypt
// -----------------------------------------------------------------------------

TEST(Cryptor, EncryptOfEmptyPlaintextIsInvalidArgument)
{
    DefaultPool   pool;
    CounterRandom random;
    Cryptor       cryptor(keyOfCountingBytes(), random, pool.pool());

    CryptoResult sealed = cryptor.encrypt(ByteView());

    EXPECT_FALSE(sealed.isOk());
    EXPECT_EQ(CryptoCode::CRYPTO_INVALID_ARGUMENT, sealed.code());
    EXPECT_EQ(1u, random.next);   // no nonce was drawn
}

TEST(Cryptor, EncryptWhenRandomFailsIsRandomUnavailable)
{
    DefaultPool   pool;
    CounterRandom random;
    Cryptor       cryptor(keyOfCountingBytes(), random, pool.pool());

    random.fails = true;

    const Blob   plain  = pattern(1, 10);
    CryptoResult sealed = cryptor.encrypt(viewOf(plain));

    EXPECT_FALSE(sealed.isOk());
    EXPECT_EQ(CryptoCode::CRYPTO_RANDOM_UNAVAILABLE, sealed.code());
}

TEST(Cryptor, EncryptDrawsExactlyTheNonceFromTheRandomSource)
{
    DefaultPool   pool;
    CounterRandom random;
    Cryptor       cryptor(keyOfCountingBytes(), random, pool.pool());

    const Blob   plain  = pattern(1, 10);
    CryptoResult sealed = cryptor.encrypt(viewOf(plain));

    ASSERT_TRUE(sealed.isOk());
    EXPECT_EQ(1u + CRYPTOR_NONCE_SIZE, random.next);

    // And the nonce is the first 12 bytes of the output, verbatim.
    for (size_t i = 0; i < CRYPTOR_NONCE_SIZE; ++i)
    {
        EXPECT_EQ(static_cast<uint8_t>(1 + i), sealed.value().data()[i]) << "at index " << i;
    }
}

TEST(Cryptor, SamePlaintextTwiceGivesDifferentCiphertextAndTag)
{
    DefaultPool   pool;
    CounterRandom random;
    Cryptor       cryptor(keyOfCountingBytes(), random, pool.pool());

    const Blob   plain  = pattern(1, 40);
    CryptoResult first  = cryptor.encrypt(viewOf(plain));
    CryptoResult second = cryptor.encrypt(viewOf(plain));

    ASSERT_TRUE(first.isOk());
    ASSERT_TRUE(second.isOk());

    const Blob a = toBlob(first.value());
    const Blob b = toBlob(second.value());

    EXPECT_NE(Blob(a.begin(), a.begin() + 12), Blob(b.begin(), b.begin() + 12));
    EXPECT_NE(Blob(a.begin() + 12, a.end() - 16), Blob(b.begin() + 12, b.end() - 16));
    EXPECT_NE(Blob(a.end() - 16, a.end()), Blob(b.end() - 16, b.end()));

    // Both still open.
    EXPECT_TRUE(cryptor.decrypt(first.value().view()).isOk());
    EXPECT_TRUE(cryptor.decrypt(second.value().view()).isOk());
}

TEST(Cryptor, SameNonceAndPlaintextGiveTheSameSealedBytes)
{
    // Determinism given the nonce: the "different ciphertext" test above
    // means "different nonce", not "hidden state".
    DefaultPool pool;
    FixedNonce  random_a("0102030405060708090a0b0c");
    FixedNonce  random_b("0102030405060708090a0b0c");
    Cryptor     a(keyOfCountingBytes(), random_a, pool.pool());
    Cryptor     b(keyOfCountingBytes(), random_b, pool.pool());

    const Blob plain = pattern(9, 33);

    CryptoResult sealed_a = a.encrypt(viewOf(plain));
    CryptoResult sealed_b = b.encrypt(viewOf(plain));

    ASSERT_TRUE(sealed_a.isOk());
    ASSERT_TRUE(sealed_b.isOk());
    EXPECT_EQ(toBlob(sealed_a.value()), toBlob(sealed_b.value()));
}

TEST(Cryptor, EncryptDoesNotModifyThePlaintext)
{
    DefaultPool   pool;
    CounterRandom random;
    Cryptor       cryptor(keyOfCountingBytes(), random, pool.pool());

    const Blob plain = pattern(5, 50);
    Blob       copy  = plain;

    ASSERT_TRUE(cryptor.encrypt(viewOf(copy)).isOk());
    EXPECT_EQ(plain, copy);
}

// -----------------------------------------------------------------------------
// Cryptor: decrypt argument checks
// -----------------------------------------------------------------------------

namespace
{
    struct ShortCase
    {
        size_t      length;
        const char* name;
    };

    // sealed length: 0 / 1 / one below overhead / exactly overhead (no
    // ciphertext at all) - all rejected before any crypto runs.
    const ShortCase kShortCases[] =
    {
        { 0,                         "Empty" },
        { 1,                         "OneByte" },
        { CRYPTOR_OVERHEAD_SIZE - 1, "OneBelowOverhead" },
        { CRYPTOR_OVERHEAD_SIZE,     "ExactlyOverhead" },
    };

    std::string shortNameOf(const ::testing::TestParamInfo<ShortCase>& info)
    {
        return info.param.name;
    }

    class DecryptTooShort : public ::testing::TestWithParam<ShortCase>
    {
    };
}

TEST_P(DecryptTooShort, IsInvalidArgument)
{
    const ShortCase& c = GetParam();

    DefaultPool   pool;
    CounterRandom random;
    Cryptor       cryptor(keyOfCountingBytes(), random, pool.pool());

    const Blob   sealed = pattern(0, c.length);
    CryptoResult opened = cryptor.decrypt(viewOf(sealed));

    EXPECT_FALSE(opened.isOk());
    EXPECT_EQ(CryptoCode::CRYPTO_INVALID_ARGUMENT, opened.code());
}

INSTANTIATE_TEST_SUITE_P(AllLengths, DecryptTooShort, ::testing::ValuesIn(kShortCases), shortNameOf);

TEST(Cryptor, DecryptOfOverheadPlusOneByteIsAccepted)
{
    DefaultPool   pool;
    CounterRandom random;
    Cryptor       cryptor(keyOfCountingBytes(), random, pool.pool());

    const Blob   plain  = pattern(1, 1);
    CryptoResult sealed = cryptor.encrypt(viewOf(plain));
    ASSERT_TRUE(sealed.isOk());
    ASSERT_EQ(CRYPTOR_OVERHEAD_SIZE + 1, sealed.value().size());

    CryptoResult opened = cryptor.decrypt(sealed.value().view());
    ASSERT_TRUE(opened.isOk());
    EXPECT_EQ(plain, toBlob(opened.value()));
}

// -----------------------------------------------------------------------------
// Cryptor: tamper detection
// -----------------------------------------------------------------------------

TEST(Cryptor, EveryFlippedBitOfTheSealedBytesIsATagMismatch)
{
    // 20 bytes of plaintext: sealed is 48 bytes, 384 bits, each flipped in
    // turn. Covers the nonce, both partial and full ciphertext blocks, and
    // the tag itself.
    DefaultPool   pool;
    CounterRandom random;
    Cryptor       cryptor(keyOfCountingBytes(), random, pool.pool());

    const Blob   plain  = pattern(1, 20);
    CryptoResult sealed = cryptor.encrypt(viewOf(plain));
    ASSERT_TRUE(sealed.isOk());

    const Blob good = toBlob(sealed.value());

    for (size_t byte = 0; byte < good.size(); ++byte)
    {
        for (int bit = 0; bit < 8; ++bit)
        {
            Blob tampered = good;
            tampered[byte] ^= static_cast<uint8_t>(1 << bit);

            CryptoResult opened = cryptor.decrypt(viewOf(tampered));

            EXPECT_FALSE(opened.isOk()) << "byte " << byte << " bit " << bit;
            EXPECT_EQ(CryptoCode::CRYPTO_TAG_MISMATCH, opened.code()) << "byte " << byte << " bit " << bit;
        }
    }

    // The untouched bytes still open, so the loop above was not rejecting
    // everything.
    EXPECT_TRUE(cryptor.decrypt(viewOf(good)).isOk());
}

TEST(Cryptor, TruncatedSealedBytesAreATagMismatch)
{
    DefaultPool   pool;
    CounterRandom random;
    Cryptor       cryptor(keyOfCountingBytes(), random, pool.pool());

    const Blob   plain  = pattern(1, 20);
    CryptoResult sealed = cryptor.encrypt(viewOf(plain));
    ASSERT_TRUE(sealed.isOk());

    const Blob good = toBlob(sealed.value());

    // Drop the last ciphertext byte but keep a full-length tag: the tag is
    // over different bytes now.
    Blob shorter(good.begin(), good.end() - CRYPTOR_TAG_SIZE - 1);
    shorter.insert(shorter.end(), good.end() - CRYPTOR_TAG_SIZE, good.end());

    CryptoResult opened = cryptor.decrypt(viewOf(shorter));

    EXPECT_FALSE(opened.isOk());
    EXPECT_EQ(CryptoCode::CRYPTO_TAG_MISMATCH, opened.code());
}

TEST(Cryptor, ExtendedSealedBytesAreATagMismatch)
{
    DefaultPool   pool;
    CounterRandom random;
    Cryptor       cryptor(keyOfCountingBytes(), random, pool.pool());

    const Blob   plain  = pattern(1, 20);
    CryptoResult sealed = cryptor.encrypt(viewOf(plain));
    ASSERT_TRUE(sealed.isOk());

    Blob longer = toBlob(sealed.value());
    longer.push_back(0);

    CryptoResult opened = cryptor.decrypt(viewOf(longer));

    EXPECT_FALSE(opened.isOk());
    EXPECT_EQ(CryptoCode::CRYPTO_TAG_MISMATCH, opened.code());
}

TEST(Cryptor, DifferentKeyIsATagMismatch)
{
    DefaultPool   pool;
    CounterRandom random;
    Cryptor       sealer(keyOfCountingBytes(), random, pool.pool());

    CipherKey other_key = keyOfCountingBytes();
    other_key.bytes[0] ^= 0x01;
    Cryptor other(other_key, random, pool.pool());

    const Blob   plain  = pattern(1, 20);
    CryptoResult sealed = sealer.encrypt(viewOf(plain));
    ASSERT_TRUE(sealed.isOk());

    CryptoResult opened = other.decrypt(sealed.value().view());

    EXPECT_FALSE(opened.isOk());
    EXPECT_EQ(CryptoCode::CRYPTO_TAG_MISMATCH, opened.code());
}

TEST(Cryptor, SameKeyInAnotherCryptorOpensTheSealedBytes)
{
    // Key material is the only shared secret: a second instance built from
    // the same key, with its own random source, can read what the first
    // sealed. That is what lets the server restart and keep its data.
    DefaultPool   pool;
    CounterRandom random_a;
    CounterRandom random_b;
    Cryptor       a(keyOfCountingBytes(), random_a, pool.pool());
    Cryptor       b(keyOfCountingBytes(), random_b, pool.pool());

    const Blob   plain  = pattern(1, 20);
    CryptoResult sealed = a.encrypt(viewOf(plain));
    ASSERT_TRUE(sealed.isOk());

    CryptoResult opened = b.decrypt(sealed.value().view());
    ASSERT_TRUE(opened.isOk());
    EXPECT_EQ(plain, toBlob(opened.value()));
}

TEST(Cryptor, DecryptDoesNotTouchTheRandomSource)
{
    DefaultPool   pool;
    CounterRandom random;
    Cryptor       cryptor(keyOfCountingBytes(), random, pool.pool());

    const Blob   plain  = pattern(1, 20);
    CryptoResult sealed = cryptor.encrypt(viewOf(plain));
    ASSERT_TRUE(sealed.isOk());

    const uint32_t before = random.next;
    random.fails = true;

    EXPECT_TRUE(cryptor.decrypt(sealed.value().view()).isOk());
    EXPECT_EQ(before, random.next);
}

TEST(Cryptor, DecryptDoesNotModifyTheSealedBytes)
{
    DefaultPool   pool;
    CounterRandom random;
    Cryptor       cryptor(keyOfCountingBytes(), random, pool.pool());

    const Blob   plain  = pattern(1, 20);
    CryptoResult sealed = cryptor.encrypt(viewOf(plain));
    ASSERT_TRUE(sealed.isOk());

    const Blob before = toBlob(sealed.value());
    ASSERT_TRUE(cryptor.decrypt(sealed.value().view()).isOk());
    EXPECT_EQ(before, toBlob(sealed.value()));
}
