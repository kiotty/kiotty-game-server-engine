#include "kiotty_session.h"

#include <datalayer/repository/session/kiotty_session_repository.h>
#include <domain/channel/kiotty_game_channel_pool.h>
#include <domain/entity/kiotty_game_event.h>
#include <domain/entity/kiotty_game_response.h>

#include <utility>

namespace kiotty
{
    Session::Session() :
        _sessions(nullptr),
        _account(),
        _token()
    {
    }

    Session::Session(const SessionRepository& sessions, const AccountId& account, const SessionToken& token) :
        _sessions(&sessions),
        _account(account),
        _token(token)
    {
    }

    ChannelId Session::channel() const
    {
        if (_sessions == nullptr)
        {
            return ChannelId();
        }
        return _sessions->channelOf(_token);
    }

    bool Session::reply(uint32_t correlation_id, uint16_t command, Bytes payload)
    {
        const ChannelId id = channel();

        if (isNull(id))
        {
            return false;
        }

        ChannelAccess access = _sessions->channels().access(id);

        if (!access)
        {
            return false;
        }

        GameResponse response;
        response.correlation_id = correlation_id;
        response.command        = command;
        response.payload        = std::move(payload);

        return access.channel().business().response.emit(response);
    }

    bool Session::notify(uint16_t command, Bytes payload)
    {
        const ChannelId id = channel();

        if (isNull(id))
        {
            return false;
        }

        ChannelAccess access = _sessions->channels().access(id);

        if (!access)
        {
            return false;
        }

        GameEvent event;
        event.command = command;
        event.payload = std::move(payload);

        return access.channel().business().event.emit(event);
    }
}
