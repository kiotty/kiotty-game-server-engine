#include "kiotty_usecase_dispatcher.h"

namespace kiotty
{
    UsecaseDispatcher::UsecaseDispatcher(const UsecaseRegistry& registry, GameChannelPool& channels,
                                         const SessionRepository& sessions) :
        _registry(registry),
        _channels(channels),
        _sessions(sessions)
    {
    }

    bool UsecaseDispatcher::dispatch(const GameRequest& request)
    {
        IUsecase* const usecase = _registry.find(request.command);

        if (usecase == nullptr)
        {
            return false;
        }
        if (usecase->requiresSession() && !_sessions.find(request.channel_id).isOk())
        {
            return false;
        }

        ChannelAccess access = _channels.access(request.channel_id);

        if (!access)
        {
            return false;
        }

        BusinessGameChannel channel = access.channel().business();

        usecase->execute(request, channel);
        return true;
    }

    void UsecaseDispatcher::onStream(const GameRequest& request)
    {
        dispatch(request);
    }
}
