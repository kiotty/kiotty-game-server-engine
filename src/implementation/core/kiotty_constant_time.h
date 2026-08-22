#if !defined(KIOTTY_CORE_CONSTANT_TIME_H)
#define KIOTTY_CORE_CONSTANT_TIME_H

#include <core/kiotty_bytes.h>

#include <cstddef>
#include <cstdint>

namespace kiotty
{
    inline bool constantTimeEquals(ByteView lhs, ByteView rhs)
    {
        if (lhs.size() != rhs.size())
        {
            return false;
        }

        uint8_t difference = 0;

        for (size_t i = 0; i < lhs.size(); ++i)
        {
            difference |= static_cast<uint8_t>(lhs.data()[i] ^ rhs.data()[i]);
        }
        return difference == 0;
    }
}

#endif
