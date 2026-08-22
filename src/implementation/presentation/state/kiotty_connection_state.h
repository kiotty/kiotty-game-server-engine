#if !defined(KIOTTY_PRESENTATION_STATE_CONNECTION_STATE_H)
#define KIOTTY_PRESENTATION_STATE_CONNECTION_STATE_H

#include <cstdint>

namespace kiotty
{
    enum class RecvStep : uint8_t
    {
        ReadingHeader,
        ReadingPayload,
    };

    enum class SendState : uint8_t
    {
        Idle,
        Sending,
    };

    enum class LifeState : uint8_t
    {
        Active,
        Closing,
        Closed,
    };
}

#endif
