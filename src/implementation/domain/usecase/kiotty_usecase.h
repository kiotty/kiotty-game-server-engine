#if !defined(KIOTTY_DOMAIN_USECASE_USECASE_H)
#define KIOTTY_DOMAIN_USECASE_USECASE_H

#include <domain/channel/kiotty_game_channel.h>
#include <domain/entity/kiotty_game_request.h>

#include <cstddef>
#include <cstdint>

namespace kiotty
{
    class IUsecase
    {
    public:
        static const size_t HOLDER_SIZE  = 64;
        static const size_t HOLDER_ALIGN = 8;

        virtual ~IUsecase() {}

        virtual uint16_t command() const = 0;

        virtual void execute(const GameRequest& request, BusinessGameChannel& channel) = 0;

        virtual bool requiresSession() const { return true; }
    };

    class IPublicUsecase : public IUsecase
    {
    public:
        bool requiresSession() const override { return false; }
    };
}

#endif
