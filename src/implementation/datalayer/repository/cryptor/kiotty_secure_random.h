#if !defined(KIOTTY_DATALAYER_REPOSITORY_CRYPTOR_SECURE_RANDOM_H)
#define KIOTTY_DATALAYER_REPOSITORY_CRYPTOR_SECURE_RANDOM_H

#include <core/kiotty_random_source.h>

namespace kiotty
{
    class SecureRandom : public IRandomSource
    {
    public:
        bool fill(ByteSpan out) override;
    };
}

#endif
