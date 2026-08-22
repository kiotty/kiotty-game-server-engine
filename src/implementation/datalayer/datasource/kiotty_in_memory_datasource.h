#if !defined(KIOTTY_DATALAYER_DATASOURCE_IN_MEMORY_DATASOURCE_H)
#define KIOTTY_DATALAYER_DATASOURCE_IN_MEMORY_DATASOURCE_H

#include <core/kiotty_block_pool.h>
#include <datalayer/datasource/kiotty_datasource.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

namespace kiotty
{
    class InMemoryDataSource : public IDataSource
    {
    public:
        explicit InMemoryDataSource(BlockPool& pool);

        InMemoryDataSource(const InMemoryDataSource&) = delete;
        InMemoryDataSource& operator=(const InMemoryDataSource&) = delete;

        size_t size() const;

        DataSourceReadResult readBlocking(ByteView key) override;
        DataSourceCode       writeBlocking(ByteView key, ByteView value) override;

    private:
        typedef std::vector<uint8_t> Blob;
        typedef std::map<Blob, Blob> Table;

        static Blob toBlob(ByteView view);

        mutable std::mutex _lock;
        BlockPool&         _pool;
        Table              _table;
    };
}

#endif
