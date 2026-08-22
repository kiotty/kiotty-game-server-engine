#if !defined(KIOTTY_CORE_RING_BUFFER_H)
#define KIOTTY_CORE_RING_BUFFER_H

#include <cstddef>
#include <new>
#include <utility>

namespace kiotty
{
    template <typename T>
    class RingBuffer
    {
    public:
        explicit RingBuffer(size_t capacity) :
            _items(nullptr),
            _capacity(capacity),
            _head(0),
            _size(0)
        {
            if (capacity > 0)
            {
                _items = new (std::nothrow) T[capacity];
            }

            if (_items == nullptr)
            {
                _capacity = 0;
            }
        }

        ~RingBuffer()
        {
            delete[] _items;
        }

        RingBuffer(const RingBuffer&) = delete;
        RingBuffer& operator=(const RingBuffer&) = delete;
        RingBuffer(RingBuffer&&) = delete;
        RingBuffer& operator=(RingBuffer&&) = delete;

        explicit operator bool() const { return _items != nullptr; }

        size_t capacity() const { return _capacity; }
        size_t size() const { return _size; }
        bool empty() const { return _size == 0; }
        bool full() const { return _size == _capacity; }

        bool tryPush(T&& value)
        {
            if (_size == _capacity)
            {
                return false;
            }

            _items[indexOf(_size)] = std::move(value);
            ++_size;
            return true;
        }

        bool tryPush(const T& value)
        {
            if (_size == _capacity)
            {
                return false;
            }

            _items[indexOf(_size)] = value;
            ++_size;
            return true;
        }

        bool tryPop(T& out)
        {
            if (_size == 0)
            {
                return false;
            }

            out = std::move(_items[_head]);

            _items[_head] = T();

            _head = wrap(_head + 1);
            --_size;
            return true;
        }

        T& at(size_t offset) { return _items[indexOf(offset)]; }
        const T& at(size_t offset) const { return _items[indexOf(offset)]; }

        bool removeAt(size_t offset)
        {
            if (offset >= _size)
            {
                return false;
            }

            for (size_t i = offset; i + 1 < _size; ++i)
            {
                _items[indexOf(i)] = std::move(_items[indexOf(i + 1)]);
            }

            _items[indexOf(_size - 1)] = T();
            --_size;
            return true;
        }

    private:
        size_t wrap(size_t index) const
        {
            return (index >= _capacity) ? index - _capacity : index;
        }

        size_t indexOf(size_t offset) const
        {
            return wrap(_head + offset);
        }

        T*     _items;
        size_t _capacity;
        size_t _head;
        size_t _size;
    };
}

#endif
