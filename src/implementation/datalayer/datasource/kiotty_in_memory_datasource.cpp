#include "kiotty_in_memory_datasource.h"

#include <cstring>
#include <new>
#include <utility>

namespace kiotty
{
    InMemoryDataSource::InMemoryDataSource(BlockPool& pool) :
        _lock(),
        _pool(pool),
        _table()
    {
    }

    size_t InMemoryDataSource::size() const
    {
        std::lock_guard<std::mutex> guard(_lock);

        return _table.size();
    }

    DataSourceReadResult InMemoryDataSource::readBlocking(ByteView key)
    {
        if (key.size() == 0)
        {
            return error(DataSourceCode::DATASOURCE_INVALID_ARGUMENT);
        }

        std::lock_guard<std::mutex> guard(_lock);

        const Table::const_iterator found = _table.find(toBlob(key));

        if (found == _table.end())
        {
            return error(DataSourceCode::DATASOURCE_NOT_FOUND);
        }

        const Blob& stored = found->second;

        Bytes value(_pool, stored.size());

        if (!value)
        {
            return error(DataSourceCode::DATASOURCE_OUT_OF_MEMORY);
        }

        std::memcpy(value.writableSpan().data(), stored.data(), stored.size());
        return DataSourceReadResult(DataSourceReadResult::Success(), std::move(value));
    }

    DataSourceCode InMemoryDataSource::writeBlocking(ByteView key, ByteView value)
    {
        if (key.size() == 0 || value.size() == 0)
        {
            return DataSourceCode::DATASOURCE_INVALID_ARGUMENT;
        }

        std::lock_guard<std::mutex> guard(_lock);

        try
        {
            _table[toBlob(key)] = toBlob(value);
        }
        catch (const std::bad_alloc&)
        {
            return DataSourceCode::DATASOURCE_OUT_OF_MEMORY;
        }
        return DataSourceCode::DATASOURCE_SUCCESS;
    }

    InMemoryDataSource::Blob InMemoryDataSource::toBlob(ByteView view)
    {
        return Blob(view.data(), view.data() + view.size());
    }
}
