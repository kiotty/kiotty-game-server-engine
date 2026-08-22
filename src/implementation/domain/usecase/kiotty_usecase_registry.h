#if !defined(KIOTTY_DOMAIN_USECASE_USECASE_REGISTRY_H)
#define KIOTTY_DOMAIN_USECASE_USECASE_REGISTRY_H

#include <core/kiotty_holder.h>
#include <domain/usecase/kiotty_usecase.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace kiotty
{
    class UsecaseRegistry
    {
    public:
        explicit UsecaseRegistry(std::vector<Holder<IUsecase> > usecases);

        UsecaseRegistry(const UsecaseRegistry&) = delete;
        UsecaseRegistry& operator=(const UsecaseRegistry&) = delete;

        explicit operator bool() const { return _usable; }

        size_t size() const { return _usecases.size(); }

        IUsecase* find(uint16_t command) const;

    private:
        bool hasNoDuplicates() const;

        std::vector<Holder<IUsecase> > _usecases;
        bool                           _usable;
    };
}

#endif
