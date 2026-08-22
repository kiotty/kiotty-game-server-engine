#if !defined(KIOTTY_CORE_RANDOM_SOURCE_H)
#define KIOTTY_CORE_RANDOM_SOURCE_H

#include <core/kiotty_bytes.h>

namespace kiotty
{
    class IRandomSource
    {
    public:
        virtual ~IRandomSource() {}

        virtual bool fill(ByteSpan out) = 0;
    };
}

#endif
