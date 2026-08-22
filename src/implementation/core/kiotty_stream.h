#if !defined(KIOTTY_CORE_STREAM_H)
#define KIOTTY_CORE_STREAM_H

#include <cstddef>

namespace kiotty
{
    template <typename T>
    class ISink
    {
    public:
        virtual ~ISink() {}

        virtual bool emit(const T& item) = 0;
    };

    template <typename T>
    class StreamListener
    {
    public:
        virtual ~StreamListener() {}

        virtual void onStream(const T& item) = 0;
    };

    template <typename T>
    class IStream
    {
    public:
        virtual ~IStream() {}

        virtual bool addListener(StreamListener<T>& listener) = 0;
        virtual void removeListener(StreamListener<T>& listener) = 0;
        virtual void clear() = 0;
    };

    template <typename T, size_t MaxListeners = 4>
    class MutableStream : public ISink<T>, public IStream<T>
    {
        static_assert(MaxListeners > 0, "MutableStream needs room for at least one listener");

    public:
        MutableStream() :
            _listeners(),
            _count(0)
        {
        }

        MutableStream(const MutableStream&) = delete;
        MutableStream& operator=(const MutableStream&) = delete;

        ISink<T>& sink() { return *this; }
        IStream<T>& stream() { return *this; }

        size_t listenerCount() const { return _count; }

        bool emit(const T& item) override
        {
            if (_count == 0)
            {
                return false;
            }

            for (size_t i = 0; i < _count; ++i)
            {
                _listeners[i]->onStream(item);
            }
            return true;
        }

        bool addListener(StreamListener<T>& listener) override
        {
            if (_count == MaxListeners || indexOf(listener) != MaxListeners)
            {
                return false;
            }
            _listeners[_count] = &listener;
            ++_count;
            return true;
        }

        void removeListener(StreamListener<T>& listener) override
        {
            const size_t index = indexOf(listener);

            if (index == MaxListeners)
            {
                return;
            }

            for (size_t i = index + 1; i < _count; ++i)
            {
                _listeners[i - 1] = _listeners[i];
            }
            --_count;
            _listeners[_count] = nullptr;
        }

        void clear() override
        {
            for (size_t i = 0; i < _count; ++i)
            {
                _listeners[i] = nullptr;
            }
            _count = 0;
        }

    private:
        size_t indexOf(const StreamListener<T>& listener) const
        {
            for (size_t i = 0; i < _count; ++i)
            {
                if (_listeners[i] == &listener)
                {
                    return i;
                }
            }
            return MaxListeners;
        }

        StreamListener<T>* _listeners[MaxListeners];
        size_t             _count;
    };
}

#endif
