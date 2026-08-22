#if !defined(KIOTTY_CORE_BYTES_H)
#define KIOTTY_CORE_BYTES_H

#include <cassert>
#include <cstddef>
#include <cstdint>

namespace kiotty
{
    class ByteView
    {
    public:
        ByteView() :
            _data(nullptr),
            _size(0)
        {
        }

        ByteView(const uint8_t* data, size_t size) :
            _data(data),
            _size(size)
        {
        }

        const uint8_t* data() const { return _data; }
        size_t size() const { return _size; }

        explicit operator bool() const { return _data != nullptr; }

        ByteView slice(size_t offset, size_t count) const
        {
            assert(offset <= _size && count <= _size - offset && "slice() out of range");
            return ByteView(_data + offset, count);
        }

    private:
        const uint8_t* _data;
        size_t         _size;
    };

    class ByteSpan
    {
    public:
        ByteSpan() :
            _data(nullptr),
            _size(0)
        {
        }

        ByteSpan(uint8_t* data, size_t size) :
            _data(data),
            _size(size)
        {
        }

        uint8_t* data() const { return _data; }
        size_t size() const { return _size; }

        explicit operator bool() const { return _data != nullptr; }

        ByteSpan slice(size_t offset, size_t count) const
        {
            assert(offset <= _size && count <= _size - offset && "slice() out of range");
            return ByteSpan(_data + offset, count);
        }

        ByteView view() const { return ByteView(_data, _size); }

    private:
        uint8_t* _data;
        size_t   _size;
    };

    class BlockPool;

    class Bytes
    {
    public:
        Bytes();
        Bytes(BlockPool& pool, size_t length);
        ~Bytes();

        Bytes(const Bytes&) = delete;
        Bytes& operator=(const Bytes&) = delete;

        Bytes(Bytes&& other) noexcept;
        Bytes& operator=(Bytes&& other) noexcept;

        ByteSpan writableSpan() { return _span; }

        const uint8_t* data() const { return _span.data(); }
        size_t size() const { return _span.size(); }

        explicit operator bool() const { return _span.data() != nullptr; }

        ByteView view() const { return _span.view(); }

    private:
        void releaseToPool();
        void adopt(Bytes& source);
        void clear();

        BlockPool* _pool;
        ByteSpan   _span;
    };
}

#endif
