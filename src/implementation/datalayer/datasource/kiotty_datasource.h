#if !defined(KIOTTY_DATALAYER_DATASOURCE_DATASOURCE_H)
#define KIOTTY_DATALAYER_DATASOURCE_DATASOURCE_H

#include <core/kiotty_bytes.h>
#include <core/kiotty_result.h>
#include <domain/entity/kiotty_datasource_code.h>

namespace kiotty
{
    typedef Result<DataSourceCode, Bytes> DataSourceReadResult;

    class IDataSource
    {
    public:
        virtual ~IDataSource() {}

        virtual DataSourceReadResult readBlocking(ByteView key) = 0;
        virtual DataSourceCode       writeBlocking(ByteView key, ByteView value) = 0;
    };
}

#endif
