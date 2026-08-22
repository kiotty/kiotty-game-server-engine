#include "kiotty_usecase_registry.h"

#include <utility>

namespace kiotty
{
    UsecaseRegistry::UsecaseRegistry(std::vector<Holder<IUsecase> > usecases) :
        _usecases(std::move(usecases)),
        _usable(false)
    {
        for (size_t i = 0; i < _usecases.size(); ++i)
        {
            if (!static_cast<bool>(_usecases[i]))
            {
                return;
            }
        }
        _usable = hasNoDuplicates();
    }

    bool UsecaseRegistry::hasNoDuplicates() const
    {
        for (size_t i = 0; i < _usecases.size(); ++i)
        {
            for (size_t j = i + 1; j < _usecases.size(); ++j)
            {
                if (_usecases[i]->command() == _usecases[j]->command())
                {
                    return false;
                }
            }
        }
        return true;
    }

    IUsecase* UsecaseRegistry::find(uint16_t command) const
    {
        for (size_t i = 0; i < _usecases.size(); ++i)
        {
            if (_usecases[i]->command() == command)
            {
                return &(*_usecases[i]);
            }
        }
        return nullptr;
    }
}
