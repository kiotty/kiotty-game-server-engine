#if !defined(KIOTTY_DATALAYER_DATASOURCE_SQLITE_DATASOURCE_H)
#define KIOTTY_DATALAYER_DATASOURCE_SQLITE_DATASOURCE_H

#include <core/kiotty_block_pool.h>
#include <datalayer/datasource/kiotty_datasource.h>

#include <mutex>

struct sqlite3;
struct sqlite3_stmt;

namespace kiotty
{
    class SqliteDataSource : public IDataSource
    {
    public:
        SqliteDataSource(const char* path, BlockPool& pool);
        ~SqliteDataSource();

        SqliteDataSource(const SqliteDataSource&) = delete;
        SqliteDataSource& operator=(const SqliteDataSource&) = delete;

        explicit operator bool() const { return _write != nullptr; }

        DataSourceReadResult readBlocking(ByteView key) override;
        DataSourceCode       writeBlocking(ByteView key, ByteView value) override;

    private:
        bool open(const char* path);
        bool execute(const char* sql);
        bool prepare(const char* sql, sqlite3_stmt*& out);
        void closeEverything();

        static DataSourceCode codeOf(int sqlite_status);

        std::mutex    _lock;
        BlockPool&    _pool;
        sqlite3*      _db;
        sqlite3_stmt* _read;
        sqlite3_stmt* _write;
    };
}

#endif
