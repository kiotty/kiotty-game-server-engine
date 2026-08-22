#if !defined(KIOTTY_DOMAIN_USECASE_USECASE_DISPATCHER_H)
#define KIOTTY_DOMAIN_USECASE_USECASE_DISPATCHER_H

#include <core/kiotty_stream.h>
#include <domain/channel/kiotty_game_channel_pool.h>
#include <domain/entity/kiotty_game_request.h>
#include <domain/usecase/kiotty_usecase.h>
#include <domain/usecase/kiotty_usecase_registry.h>

namespace kiotty
{
    class UsecaseDispatcher : public StreamListener<GameRequest>
    {
    public:
        UsecaseDispatcher(const UsecaseRegistry& registry, GameChannelPool& channels);

        bool dispatch(const GameRequest& request);

        void onStream(const GameRequest& request) override;

    private:
        const UsecaseRegistry& _registry;
        GameChannelPool&       _channels;
    };
}

#endif
