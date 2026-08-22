#if !defined(KIOTTY_DOMAIN_ENTITY_DATASOURCE_CODE_H)
#define KIOTTY_DOMAIN_ENTITY_DATASOURCE_CODE_H

#include <cstdint>

namespace kiotty
{
    enum class DataSourceCode : int32_t
    {
        DATASOURCE_SUCCESS = 0,

        DATASOURCE_NOT_FOUND,
        DATASOURCE_INVALID_ARGUMENT,
        DATASOURCE_OUT_OF_MEMORY,
        DATASOURCE_UNAVAILABLE,
        DATASOURCE_FAILED,
    };
}

#endif
