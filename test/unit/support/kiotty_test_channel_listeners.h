#if !defined(KIOTTY_TEST_CHANNEL_LISTENERS_H)
#define KIOTTY_TEST_CHANNEL_LISTENERS_H

#include <core/kiotty_stream.h>
#include <domain/entity/kiotty_game_event.h>
#include <domain/entity/kiotty_game_request.h>
#include <domain/entity/kiotty_game_response.h>

#include <cstddef>
#include <cstdint>

namespace kiotty_test
{
    // A listener that remembers how many times it was called and the address
    // of the last item. The address is what matters: the stream contract is
    // "the same object reaches every listener", and the entities carry a
    // move-only Bytes, so identity is the only thing worth recording.
    template <typename T>
    class CountingListener : public kiotty::StreamListener<T>
    {
    public:
        CountingListener() :
            calls(0),
            last(nullptr)
        {
        }

        void onStream(const T& item) override
        {
            ++calls;
            last = &item;
        }

        int      calls;
        const T* last;
    };

    typedef CountingListener<kiotty::GameRequest>  RequestListener;
    typedef CountingListener<kiotty::GameResponse> ResponseListener;
    typedef CountingListener<kiotty::GameEvent>    EventListener;
}

#endif // KIOTTY_TEST_CHANNEL_LISTENERS_H
