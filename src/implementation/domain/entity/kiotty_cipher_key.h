#if !defined(KIOTTY_DOMAIN_ENTITY_CIPHER_KEY_H)
#define KIOTTY_DOMAIN_ENTITY_CIPHER_KEY_H

#include <core/kiotty_bytes.h>

#include <cstddef>
#include <cstdint>

namespace kiotty
{
    static const size_t CIPHER_KEY_SIZE = 32;

    struct CipherKey
    {
        uint8_t bytes[CIPHER_KEY_SIZE] {0};

        ByteView view() const { return ByteView(bytes, CIPHER_KEY_SIZE); }
        ByteSpan writableSpan() { return ByteSpan(bytes, CIPHER_KEY_SIZE); }
    };
}

#endif
