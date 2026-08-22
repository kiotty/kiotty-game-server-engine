#include "kiotty_sqlite_datasource.h"

#include <sqlite3.h>

#include <cstring>
#include <utility>

namespace kiotty
{
    namespace
    {
        const char* const kCreateTable =
            "CREATE TABLE IF NOT EXISTS kv (key BLOB PRIMARY KEY, value BLOB NOT NULL)";
        const char* const kEnableWal  = "PRAGMA journal_mode=WAL";
        const char* const kSelect     = "SELECT value FROM kv WHERE key = ?1";
        const char* const kUpsert     = "INSERT OR REPLACE INTO kv (key, value) VALUES (?1, ?2)";

        class StatementReset
        {
        public:
            explicit StatementReset(sqlite3_stmt* statement) :
                _statement(statement)
            {
            }

            ~StatementReset()
            {
                sqlite3_clear_bindings(_statement);
                sqlite3_reset(_statement);
            }

            StatementReset(const StatementReset&) = delete;
            StatementReset& operator=(const StatementReset&) = delete;

        private:
            sqlite3_stmt* _statement;
        };

        int bindBlob(sqlite3_stmt* statement, int index, ByteView view)
        {
            return sqlite3_bind_blob(statement, index, view.data(),
                                     static_cast<int>(view.size()), SQLITE_STATIC);
        }
    }

    SqliteDataSource::SqliteDataSource(const char* path, BlockPool& pool) :
        _lock(),
        _pool(pool),
        _db(nullptr),
        _read(nullptr),
        _write(nullptr)
    {
        if (path == nullptr || path[0] == 0)
        {
            return;
        }

        const bool ready =
            open(path) &&
            execute(kEnableWal) &&
            execute(kCreateTable) &&
            prepare(kSelect, _read) &&
            prepare(kUpsert, _write);

        if (!ready)
        {
            closeEverything();
        }
    }

    SqliteDataSource::~SqliteDataSource()
    {
        closeEverything();
    }

    DataSourceReadResult SqliteDataSource::readBlocking(ByteView key)
    {
        if (_read == nullptr)
        {
            return error(DataSourceCode::DATASOURCE_UNAVAILABLE);
        }
        if (key.size() == 0)
        {
            return error(DataSourceCode::DATASOURCE_INVALID_ARGUMENT);
        }

        std::lock_guard<std::mutex> guard(_lock);
        StatementReset              reset(_read);

        if (bindBlob(_read, 1, key) != SQLITE_OK)
        {
            return error(DataSourceCode::DATASOURCE_FAILED);
        }

        const int status = sqlite3_step(_read);

        if (status == SQLITE_DONE)
        {
            return error(DataSourceCode::DATASOURCE_NOT_FOUND);
        }
        if (status != SQLITE_ROW)
        {
            return error(codeOf(status));
        }

        const void* const stored = sqlite3_column_blob(_read, 0);
        const size_t      length = static_cast<size_t>(sqlite3_column_bytes(_read, 0));

        Bytes value(_pool, length);

        if (!value)
        {
            return error(DataSourceCode::DATASOURCE_OUT_OF_MEMORY);
        }

        std::memcpy(value.writableSpan().data(), stored, length);
        return DataSourceReadResult(DataSourceReadResult::Success(), std::move(value));
    }

    DataSourceCode SqliteDataSource::writeBlocking(ByteView key, ByteView value)
    {
        if (_write == nullptr)
        {
            return DataSourceCode::DATASOURCE_UNAVAILABLE;
        }
        if (key.size() == 0 || value.size() == 0)
        {
            return DataSourceCode::DATASOURCE_INVALID_ARGUMENT;
        }

        std::lock_guard<std::mutex> guard(_lock);
        StatementReset              reset(_write);

        if (bindBlob(_write, 1, key) != SQLITE_OK || bindBlob(_write, 2, value) != SQLITE_OK)
        {
            return DataSourceCode::DATASOURCE_FAILED;
        }

        const int status = sqlite3_step(_write);

        if (status != SQLITE_DONE)
        {
            return codeOf(status);
        }
        return DataSourceCode::DATASOURCE_SUCCESS;
    }

    bool SqliteDataSource::open(const char* path)
    {
        const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX;

        return sqlite3_open_v2(path, &_db, flags, nullptr) == SQLITE_OK;
    }

    bool SqliteDataSource::execute(const char* sql)
    {
        return sqlite3_exec(_db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
    }

    bool SqliteDataSource::prepare(const char* sql, sqlite3_stmt*& out)
    {
        return sqlite3_prepare_v2(_db, sql, -1, &out, nullptr) == SQLITE_OK;
    }

    void SqliteDataSource::closeEverything()
    {
        sqlite3_finalize(_write);
        sqlite3_finalize(_read);
        sqlite3_close(_db);

        _write = nullptr;
        _read  = nullptr;
        _db    = nullptr;
    }

    DataSourceCode SqliteDataSource::codeOf(int sqlite_status)
    {
        switch (sqlite_status)
        {
        case SQLITE_NOMEM:
            return DataSourceCode::DATASOURCE_OUT_OF_MEMORY;
        case SQLITE_BUSY:
        case SQLITE_LOCKED:
        case SQLITE_IOERR:
        case SQLITE_FULL:
        case SQLITE_READONLY:
            return DataSourceCode::DATASOURCE_UNAVAILABLE;
        default:
            return DataSourceCode::DATASOURCE_FAILED;
        }
    }
}
