#include "kiotty_bytes.h"

#include "kiotty_block_pool.h"

namespace kiotty
{
    Bytes::Bytes() :
        _pool(nullptr),
        _span()
    {
    }

    Bytes::Bytes(BlockPool& pool, size_t length) :
        _pool(nullptr),
        _span()
    {
        if (length == 0)
        {
            return;
        }

        void* const block = pool.acquire(length);

        if (block == nullptr)
        {
            return;
        }

        _pool = &pool;
        _span = ByteSpan(static_cast<uint8_t*>(block), length);
    }

    Bytes::~Bytes()
    {
        releaseToPool();
    }

    Bytes::Bytes(Bytes&& other) noexcept :
        _pool(nullptr),
        _span()
    {
        adopt(other);
    }

    Bytes& Bytes::operator=(Bytes&& other) noexcept
    {
        if (this != &other)
        {
            releaseToPool();
            adopt(other);
        }
        return *this;
    }

    void Bytes::releaseToPool()
    {
        if (_span.data() == nullptr)
        {
            return;
        }

        _pool->release(_span.data());
        clear();
    }

    void Bytes::adopt(Bytes& source)
    {
        _pool = source._pool;
        _span = source._span;

        source.clear();
    }

    void Bytes::clear()
    {
        _pool = nullptr;
        _span = ByteSpan();
    }
}
