#if !defined(KIOTTY_DOMAIN_CHANNEL_GAME_CHANNEL_H)
#define KIOTTY_DOMAIN_CHANNEL_GAME_CHANNEL_H

#include <core/kiotty_stream.h>
#include <domain/entity/kiotty_channel_id.h>
#include <domain/entity/kiotty_game_event.h>
#include <domain/entity/kiotty_game_request.h>
#include <domain/entity/kiotty_game_response.h>

namespace kiotty
{
    struct IoGameChannel
    {
        ChannelId              channel_id;
        ISink<GameRequest>&    request;
        IStream<GameResponse>& response;
        IStream<GameEvent>&    event;
    };

    struct BusinessGameChannel
    {
        ChannelId              channel_id;
        IStream<GameRequest>&  request;
        ISink<GameResponse>&   response;
        ISink<GameEvent>&      event;
    };

    class GameChannel
    {
    public:
        explicit GameChannel(ChannelId channel_id) :
            _channel_id(channel_id)
        {
        }

        GameChannel(const GameChannel&) = delete;
        GameChannel& operator=(const GameChannel&) = delete;

        ChannelId id() const { return _channel_id; }

        IoGameChannel io()
        {
            IoGameChannel view = { _channel_id, _request.sink(), _response.stream(), _event.stream() };
            return view;
        }

        BusinessGameChannel business()
        {
            BusinessGameChannel view = { _channel_id, _request.stream(), _response.sink(), _event.sink() };
            return view;
        }

    private:
        ChannelId                   _channel_id;
        MutableStream<GameRequest>  _request;
        MutableStream<GameResponse> _response;
        MutableStream<GameEvent>    _event;
    };
}

#endif
