#if !defined(KIOTTY_DOMAIN_CHANNEL_CHANNEL_BINDER_H)
#define KIOTTY_DOMAIN_CHANNEL_CHANNEL_BINDER_H

#include <core/kiotty_result.h>
#include <core/kiotty_stream.h>
#include <domain/channel/kiotty_game_channel.h>
#include <domain/channel/kiotty_game_channel_pool.h>
#include <domain/entity/kiotty_channel_id.h>
#include <domain/entity/kiotty_connection_info.h>
#include <domain/entity/kiotty_game_request.h>

namespace kiotty
{
    typedef Result<ChannelCode, IoGameChannel> IoChannelResult;

    class IChannelBinder
    {
    public:
        virtual ~IChannelBinder() {}

        virtual IoChannelResult onConnected(const ConnectionInfo& info) = 0;
        virtual void onDisconnected(const ConnectionInfo& info, const IoGameChannel& channel) = 0;
    };

    class ChannelPoolBinder : public IChannelBinder
    {
    public:
        ChannelPoolBinder(GameChannelPool& pool, StreamListener<GameRequest>& request_listener) :
            _pool(pool),
            _request_listener(request_listener)
        {
        }

        IoChannelResult onConnected(const ConnectionInfo&) override
        {
            ChannelResult created = _pool.create();

            if (!created.isOk())
            {
                return error(created.code());
            }

            GameChannel& channel = created.value();

            channel.business().request.addListener(_request_listener);
            return ok(channel.io());
        }

        void onDisconnected(const ConnectionInfo&, const IoGameChannel& channel) override
        {
            _pool.remove(channel.channel_id);
        }

    private:
        GameChannelPool&             _pool;
        StreamListener<GameRequest>& _request_listener;
    };
}

#endif
