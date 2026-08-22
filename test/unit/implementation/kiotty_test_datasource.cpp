// IDataSource is one contract with two implementations, so the contract is
// written once as a typed test and both implementations are run through it.
// A row that passes on the in-memory map and fails on SQLite (or the other
// way round) is the whole point of testing them together: a caller must not
// be able to tell which one it has.
//
// The argument table is key (empty / short / 1 byte / binary with a NUL /
// long) x value (empty / short / binary with a NUL / long), which is also
// the table of "does this datasource respect that keys and values are bytes,
// not C strings". SQLite opens ":memory:" so no file is ever created.

#include <datalayer/datasource/kiotty_datasource.h>
#include <datalayer/datasource/kiotty_in_memory_datasource.h>

#if defined(KIOTTY_HAS_SQLITE)
#include <datalayer/datasource/kiotty_sqlite_datasource.h>
#endif

#include "support/kiotty_test_pools.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using kiotty::BlockPool;
using kiotty::Bytes;
using kiotty::ByteView;
using kiotty::DataSourceCode;
using kiotty::DataSourceReadResult;
using kiotty::IDataSource;
using kiotty::InMemoryDataSource;
using kiotty_test::SmallPool;

static_assert(static_cast<int32_t>(DataSourceCode::DATASOURCE_SUCCESS) == 0,
              "DataSourceCode::DATASOURCE_SUCCESS must be 0 for Result to read it as ok");

namespace
{
    typedef std::vector<uint8_t> Blob;

    Blob blob(const char* text)
    {
        return Blob(text, text + std::strlen(text));
    }

    Blob blobWithNul()
    {
        Blob out;
        out.push_back('a');
        out.push_back(0);
        out.push_back('b');
        return out;
    }

    Blob longBlob(uint8_t seed, size_t count)
    {
        Blob out(count);

        for (size_t i = 0; i < count; ++i)
        {
            out[i] = static_cast<uint8_t>(seed + i);
        }
        return out;
    }

    ByteView viewOf(const Blob& b)
    {
        return ByteView(b.empty() ? nullptr : b.data(), b.size());
    }

    bool sameBytes(const Blob& expected, const Bytes& actual)
    {
        return expected.size() == actual.size() &&
               (expected.empty() || std::memcmp(expected.data(), actual.data(), expected.size()) == 0);
    }

    // Each implementation is wrapped so the typed test can construct it the
    // same way and reach it as an IDataSource.
    class InMemoryUnderTest
    {
    public:
        InMemoryUnderTest() :
            _pool(64, 4),
            _source(_pool.pool())
        {
        }

        IDataSource& source() { return _source; }
        bool ready() const { return true; }

    private:
        SmallPool          _pool;
        InMemoryDataSource _source;
    };

#if defined(KIOTTY_HAS_SQLITE)
    class SqliteUnderTest
    {
    public:
        SqliteUnderTest() :
            _pool(64, 4),
            _source(":memory:", _pool.pool())
        {
        }

        IDataSource& source() { return _source; }
        bool ready() const { return static_cast<bool>(_source); }

    private:
        SmallPool                _pool;
        kiotty::SqliteDataSource _source;
    };

    typedef ::testing::Types<InMemoryUnderTest, SqliteUnderTest> Implementations;
#else
    typedef ::testing::Types<InMemoryUnderTest> Implementations;
#endif

    template <typename T>
    class DataSourceContract : public ::testing::Test
    {
    protected:
        T under_test;
    };

    TYPED_TEST_SUITE(DataSourceContract, Implementations);

    struct WriteReadCase
    {
        Blob           key;
        Blob           value;
        DataSourceCode expect_write;
        std::string    name;
    };

    // key (5) x value (4) = 20 rows. A write is rejected as soon as either
    // side is empty; everything else must round-trip byte for byte.
    std::vector<WriteReadCase> writeReadCases()
    {
        const Blob keys[] =
        {
            Blob(), blob("k"), blob("key"), blobWithNul(), longBlob(1, 300),
        };
        const char* const key_names[] =
        {
            "EmptyKey", "OneByteKey", "ShortKey", "KeyWithNul", "LongKey",
        };
        const Blob values[] =
        {
            Blob(), blob("value"), blobWithNul(), longBlob(7, 5000),
        };
        const char* const value_names[] =
        {
            "EmptyValue", "ShortValue", "ValueWithNul", "LongValue",
        };

        std::vector<WriteReadCase> cases;

        for (size_t k = 0; k < 5; ++k)
        {
            for (size_t v = 0; v < 4; ++v)
            {
                const bool rejected = keys[k].empty() || values[v].empty();

                WriteReadCase c;
                c.key          = keys[k];
                c.value        = values[v];
                c.expect_write = rejected ? DataSourceCode::DATASOURCE_INVALID_ARGUMENT
                                          : DataSourceCode::DATASOURCE_SUCCESS;
                c.name         = std::string(key_names[k]) + value_names[v];
                cases.push_back(c);
            }
        }
        return cases;
    }
}

// -----------------------------------------------------------------------------
// the contract, run on every implementation
// -----------------------------------------------------------------------------

TYPED_TEST(DataSourceContract, IsReadyAfterConstruction)
{
    EXPECT_TRUE(this->under_test.ready());
}

TYPED_TEST(DataSourceContract, WriteThenReadRoundTripsEveryKeyValueShape)
{
    ASSERT_TRUE(this->under_test.ready());
    IDataSource& source = this->under_test.source();

    const std::vector<WriteReadCase> cases = writeReadCases();

    for (size_t i = 0; i < cases.size(); ++i)
    {
        const WriteReadCase& c = cases[i];
        SCOPED_TRACE(c.name);

        EXPECT_EQ(c.expect_write, source.writeBlocking(viewOf(c.key), viewOf(c.value)));

        DataSourceReadResult read = source.readBlocking(viewOf(c.key));

        if (c.key.empty())
        {
            EXPECT_EQ(DataSourceCode::DATASOURCE_INVALID_ARGUMENT, read.code());
            continue;
        }
        if (c.expect_write != DataSourceCode::DATASOURCE_SUCCESS)
        {
            // The empty value is the first row for every key, so a rejected
            // write has nothing before it to fall back on: the key must not
            // exist.
            EXPECT_EQ(DataSourceCode::DATASOURCE_NOT_FOUND, read.code());
            continue;
        }

        ASSERT_TRUE(read.isOk()) << "code " << static_cast<int>(read.code());
        EXPECT_TRUE(sameBytes(c.value, read.value()));
    }
}

TYPED_TEST(DataSourceContract, ReadOfAnUnknownKeyIsNotFound)
{
    ASSERT_TRUE(this->under_test.ready());
    IDataSource& source = this->under_test.source();

    DataSourceReadResult read = source.readBlocking(viewOf(blob("missing")));

    EXPECT_FALSE(read.isOk());
    EXPECT_EQ(DataSourceCode::DATASOURCE_NOT_FOUND, read.code());
}

TYPED_TEST(DataSourceContract, ReadOfAnEmptyKeyIsInvalidArgumentEvenOnAnEmptyStore)
{
    ASSERT_TRUE(this->under_test.ready());
    IDataSource& source = this->under_test.source();

    EXPECT_EQ(DataSourceCode::DATASOURCE_INVALID_ARGUMENT, source.readBlocking(ByteView()).code());
}

TYPED_TEST(DataSourceContract, RejectedEmptyValueWriteLeavesNoEntryBehind)
{
    ASSERT_TRUE(this->under_test.ready());
    IDataSource& source = this->under_test.source();

    EXPECT_EQ(DataSourceCode::DATASOURCE_INVALID_ARGUMENT,
              source.writeBlocking(viewOf(blob("k")), ByteView()));
    EXPECT_EQ(DataSourceCode::DATASOURCE_NOT_FOUND, source.readBlocking(viewOf(blob("k"))).code());
}

TYPED_TEST(DataSourceContract, RejectedEmptyValueWriteDoesNotTouchAnExistingEntry)
{
    ASSERT_TRUE(this->under_test.ready());
    IDataSource& source = this->under_test.source();

    ASSERT_EQ(DataSourceCode::DATASOURCE_SUCCESS,
              source.writeBlocking(viewOf(blob("k")), viewOf(blob("first"))));
    EXPECT_EQ(DataSourceCode::DATASOURCE_INVALID_ARGUMENT,
              source.writeBlocking(viewOf(blob("k")), ByteView()));

    DataSourceReadResult read = source.readBlocking(viewOf(blob("k")));
    ASSERT_TRUE(read.isOk());
    EXPECT_TRUE(sameBytes(blob("first"), read.value()));
}

TYPED_TEST(DataSourceContract, WritingTheSameKeyAgainOverwritesTheValue)
{
    ASSERT_TRUE(this->under_test.ready());
    IDataSource& source = this->under_test.source();

    ASSERT_EQ(DataSourceCode::DATASOURCE_SUCCESS,
              source.writeBlocking(viewOf(blob("k")), viewOf(blob("first"))));
    ASSERT_EQ(DataSourceCode::DATASOURCE_SUCCESS,
              source.writeBlocking(viewOf(blob("k")), viewOf(blob("second, longer"))));

    DataSourceReadResult read = source.readBlocking(viewOf(blob("k")));
    ASSERT_TRUE(read.isOk());
    EXPECT_TRUE(sameBytes(blob("second, longer"), read.value()));

    // And shrinking works too - a stale tail would show as extra bytes.
    ASSERT_EQ(DataSourceCode::DATASOURCE_SUCCESS,
              source.writeBlocking(viewOf(blob("k")), viewOf(blob("3"))));

    DataSourceReadResult again = source.readBlocking(viewOf(blob("k")));
    ASSERT_TRUE(again.isOk());
    EXPECT_TRUE(sameBytes(blob("3"), again.value()));
}

TYPED_TEST(DataSourceContract, KeysThatSharePrefixesAreDistinct)
{
    ASSERT_TRUE(this->under_test.ready());
    IDataSource& source = this->under_test.source();

    ASSERT_EQ(DataSourceCode::DATASOURCE_SUCCESS,
              source.writeBlocking(viewOf(blob("ab")), viewOf(blob("1"))));
    ASSERT_EQ(DataSourceCode::DATASOURCE_SUCCESS,
              source.writeBlocking(viewOf(blob("abc")), viewOf(blob("2"))));

    DataSourceReadResult ab  = source.readBlocking(viewOf(blob("ab")));
    DataSourceReadResult abc = source.readBlocking(viewOf(blob("abc")));
    DataSourceReadResult a   = source.readBlocking(viewOf(blob("a")));

    ASSERT_TRUE(ab.isOk());
    ASSERT_TRUE(abc.isOk());
    EXPECT_TRUE(sameBytes(blob("1"), ab.value()));
    EXPECT_TRUE(sameBytes(blob("2"), abc.value()));
    EXPECT_EQ(DataSourceCode::DATASOURCE_NOT_FOUND, a.code());
}

TYPED_TEST(DataSourceContract, KeyWithAnEmbeddedNulIsNotTheSameAsItsPrefix)
{
    // A datasource that handed the key to a C-string API would stop at the
    // NUL and conflate "a\0b" with "a".
    ASSERT_TRUE(this->under_test.ready());
    IDataSource& source = this->under_test.source();

    ASSERT_EQ(DataSourceCode::DATASOURCE_SUCCESS,
              source.writeBlocking(viewOf(blobWithNul()), viewOf(blob("nul"))));

    EXPECT_EQ(DataSourceCode::DATASOURCE_NOT_FOUND, source.readBlocking(viewOf(blob("a"))).code());

    DataSourceReadResult read = source.readBlocking(viewOf(blobWithNul()));
    ASSERT_TRUE(read.isOk());
    EXPECT_TRUE(sameBytes(blob("nul"), read.value()));
}

TYPED_TEST(DataSourceContract, ReadReturnsACopyNotTheCallersBuffer)
{
    ASSERT_TRUE(this->under_test.ready());
    IDataSource& source = this->under_test.source();

    Blob value = blob("original");

    ASSERT_EQ(DataSourceCode::DATASOURCE_SUCCESS,
              source.writeBlocking(viewOf(blob("k")), viewOf(value)));

    // Scribble over what was written; the store must have its own copy.
    value[0] = 'X';

    DataSourceReadResult read = source.readBlocking(viewOf(blob("k")));
    ASSERT_TRUE(read.isOk());
    EXPECT_NE(static_cast<const void*>(value.data()), static_cast<const void*>(read.value().data()));
    EXPECT_TRUE(sameBytes(blob("original"), read.value()));
}

TYPED_TEST(DataSourceContract, TwoReadsOfTheSameKeyAreIndependentCopies)
{
    ASSERT_TRUE(this->under_test.ready());
    IDataSource& source = this->under_test.source();

    ASSERT_EQ(DataSourceCode::DATASOURCE_SUCCESS,
              source.writeBlocking(viewOf(blob("k")), viewOf(blob("shared"))));

    DataSourceReadResult first  = source.readBlocking(viewOf(blob("k")));
    DataSourceReadResult second = source.readBlocking(viewOf(blob("k")));

    ASSERT_TRUE(first.isOk());
    ASSERT_TRUE(second.isOk());
    EXPECT_NE(first.value().data(), second.value().data());

    first.value().writableSpan().data()[0] = 'X';
    EXPECT_TRUE(sameBytes(blob("shared"), second.value()));
}

// -----------------------------------------------------------------------------
// InMemoryDataSource only: size()
// -----------------------------------------------------------------------------

TEST(InMemoryDataSource, SizeCountsDistinctKeysOnly)
{
    SmallPool          pool(64, 4);
    InMemoryDataSource source(pool.pool());

    EXPECT_EQ(0u, source.size());

    EXPECT_EQ(DataSourceCode::DATASOURCE_SUCCESS, source.writeBlocking(viewOf(blob("a")), viewOf(blob("1"))));
    EXPECT_EQ(1u, source.size());

    EXPECT_EQ(DataSourceCode::DATASOURCE_SUCCESS, source.writeBlocking(viewOf(blob("a")), viewOf(blob("2"))));
    EXPECT_EQ(1u, source.size());

    EXPECT_EQ(DataSourceCode::DATASOURCE_SUCCESS, source.writeBlocking(viewOf(blob("b")), viewOf(blob("1"))));
    EXPECT_EQ(2u, source.size());
}

TEST(InMemoryDataSource, RejectedWritesDoNotChangeSize)
{
    SmallPool          pool(64, 4);
    InMemoryDataSource source(pool.pool());

    EXPECT_EQ(DataSourceCode::DATASOURCE_INVALID_ARGUMENT, source.writeBlocking(ByteView(), viewOf(blob("1"))));
    EXPECT_EQ(DataSourceCode::DATASOURCE_INVALID_ARGUMENT, source.writeBlocking(viewOf(blob("a")), ByteView()));
    EXPECT_EQ(0u, source.size());
}

TEST(InMemoryDataSource, ReadValueComesFromTheGivenPool)
{
    // One 64-byte block: the first read takes it, the second falls back to
    // the heap, and fallbackCount() shows that the pool was the source.
    SmallPool          pool(64, 1);
    InMemoryDataSource source(pool.pool());

    ASSERT_EQ(DataSourceCode::DATASOURCE_SUCCESS, source.writeBlocking(viewOf(blob("k")), viewOf(blob("v"))));

    DataSourceReadResult first = source.readBlocking(viewOf(blob("k")));
    ASSERT_TRUE(first.isOk());
    EXPECT_EQ(0u, pool.pool().fallbackCount());

    DataSourceReadResult second = source.readBlocking(viewOf(blob("k")));
    ASSERT_TRUE(second.isOk());
    EXPECT_EQ(1u, pool.pool().fallbackCount());
}

// -----------------------------------------------------------------------------
// SqliteDataSource only: opening
// -----------------------------------------------------------------------------

#if defined(KIOTTY_HAS_SQLITE)

using kiotty::SqliteDataSource;

namespace
{
    struct OpenCase
    {
        const char* path;
        bool        expect_open;
        const char* name;
    };

    // path: null / empty / in-memory / a directory that cannot be a database
    // file. No row creates a file on disk.
    const OpenCase kOpenCases[] =
    {
        { nullptr,    false, "NullPath" },
        { "",         false, "EmptyPath" },
        { ":memory:", true,  "InMemory" },
        { ".",        false, "DirectoryPath" },
    };

    std::string openNameOf(const ::testing::TestParamInfo<OpenCase>& info)
    {
        return info.param.name;
    }

    class SqliteOpen : public ::testing::TestWithParam<OpenCase>
    {
    };
}

TEST_P(SqliteOpen, IsUsableOnlyWhenThePathOpens)
{
    const OpenCase& c = GetParam();

    SmallPool        pool(64, 4);
    SqliteDataSource source(c.path, pool.pool());

    EXPECT_EQ(c.expect_open, static_cast<bool>(source));
}

INSTANTIATE_TEST_SUITE_P(AllPaths, SqliteOpen, ::testing::ValuesIn(kOpenCases), openNameOf);

TEST(SqliteDataSource, EveryCallOnAnUnopenedSourceIsUnavailable)
{
    SmallPool        pool(64, 4);
    SqliteDataSource source(nullptr, pool.pool());

    ASSERT_FALSE(static_cast<bool>(source));

    // Unavailable wins over invalid-argument: the empty key is not even looked at.
    EXPECT_EQ(DataSourceCode::DATASOURCE_UNAVAILABLE, source.readBlocking(ByteView()).code());
    EXPECT_EQ(DataSourceCode::DATASOURCE_UNAVAILABLE, source.readBlocking(viewOf(blob("k"))).code());
    EXPECT_EQ(DataSourceCode::DATASOURCE_UNAVAILABLE, source.writeBlocking(ByteView(), ByteView()));
    EXPECT_EQ(DataSourceCode::DATASOURCE_UNAVAILABLE, source.writeBlocking(viewOf(blob("k")), viewOf(blob("v"))));
}

TEST(SqliteDataSource, TwoInMemorySourcesDoNotShareData)
{
    SmallPool        pool(64, 4);
    SqliteDataSource first(":memory:", pool.pool());
    SqliteDataSource second(":memory:", pool.pool());

    ASSERT_TRUE(static_cast<bool>(first));
    ASSERT_TRUE(static_cast<bool>(second));

    ASSERT_EQ(DataSourceCode::DATASOURCE_SUCCESS, first.writeBlocking(viewOf(blob("k")), viewOf(blob("v"))));
    EXPECT_EQ(DataSourceCode::DATASOURCE_NOT_FOUND, second.readBlocking(viewOf(blob("k"))).code());
}

#endif // KIOTTY_HAS_SQLITE
