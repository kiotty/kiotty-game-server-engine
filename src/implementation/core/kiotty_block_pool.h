#if !defined(KIOTTY_CORE_BLOCK_POOL_H)
#define KIOTTY_CORE_BLOCK_POOL_H

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace kiotty
{
    struct BlockClass
    {
        size_t block_size;
        size_t num_of_block;
    };

    inline const BlockClass* defaultBlockClasses()
    {
        static const BlockClass table[] =
        {
            {    64, 2048 },
            {   128, 2048 },
            {   256, 1024 },
            {   512,  512 },
            {  1024,  256 },
            {  4096,  128 },
            { 16384,   64 },
            { 65536,   32 },
        };
        return table;
    }

    inline size_t defaultBlockClassCount()
    {
        return 8;
    }

    class BlockRegionPool
    {
    public:
        BlockRegionPool() :
            _storage(nullptr),
            _block_size(0),
            _num_of_block(0),
            _free_head(nullptr)
        {
        }

        ~BlockRegionPool()
        {
            discard();
        }

        BlockRegionPool(const BlockRegionPool&) = delete;
        BlockRegionPool& operator=(const BlockRegionPool&) = delete;

        void reserve(size_t block_size, size_t num_of_block)
        {
            discard();

            if (block_size < sizeof(void*) || num_of_block == 0)
            {
                return;
            }

            _storage = static_cast<uint8_t*>(std::malloc(block_size * num_of_block));

            if (_storage == nullptr)
            {
                return;
            }

            _block_size   = block_size;
            _num_of_block = num_of_block;

            preFaultPages();
            linkEveryBlockFree();
        }

        size_t blockSize() const { return _block_size; }
        size_t numOfBlock() const { return _num_of_block; }
        size_t reservedBytes() const { return _block_size * _num_of_block; }

        bool full() const { return _free_head == nullptr; }

        bool holds(const void* ptr) const
        {
            if (_storage == nullptr)
            {
                return false;
            }

            const uintptr_t address = reinterpret_cast<uintptr_t>(ptr);
            const uintptr_t start   = reinterpret_cast<uintptr_t>(_storage);

            return address >= start && address < start + reservedBytes();
        }

        void* allocate()
        {
            void* const block = _free_head;

            if (block == nullptr)
            {
                return nullptr;
            }

            _free_head = nextFreeAfter(block);
            return block;
        }

        void release(void* block)
        {
            nextFreeAfter(block) = _free_head;
            _free_head           = block;
        }

    private:
        static void*& nextFreeAfter(void* block)
        {
            return *reinterpret_cast<void**>(block);
        }

        void discard()
        {
            std::free(_storage);

            _storage      = nullptr;
            _block_size   = 0;
            _num_of_block = 0;
            _free_head    = nullptr;
        }

        void preFaultPages()
        {
            std::memset(_storage, 0, reservedBytes());
        }

        void linkEveryBlockFree()
        {
            for (size_t i = _num_of_block; i > 0; --i)
            {
                release(_storage + (i - 1) * _block_size);
            }
        }

        uint8_t* _storage;
        size_t   _block_size;
        size_t   _num_of_block;
        void*    _free_head;
    };

    class HeapFallback
    {
    public:
        HeapFallback() :
            _count(0)
        {
        }

        void* allocate(size_t length)
        {
            ++_count;
            return std::malloc(length);
        }

        void release(void* block)
        {
            std::free(block);
        }

        size_t count() const { return _count; }

    private:
        size_t _count;
    };

    class BlockPool
    {
    public:
        static const size_t MAX_REGIONS = 16;

        BlockPool(const BlockClass* classes, size_t class_count) :
            _region_count(0)
        {
            if (classes == nullptr)
            {
                return;
            }

            _region_count = (class_count < MAX_REGIONS) ? class_count : MAX_REGIONS;

            for (size_t i = 0; i < _region_count; ++i)
            {
                _regions[i].reserve(classes[i].block_size, classes[i].num_of_block);
            }
        }

        BlockPool(const BlockPool&) = delete;
        BlockPool& operator=(const BlockPool&) = delete;

        void* acquire(size_t length)
        {
            if (length == 0)
            {
                return nullptr;
            }

            std::lock_guard<std::mutex> guard(_lock);

            BlockRegionPool* const region = findRegion(length);

            if (region == nullptr || region->full())
            {
                return _heap.allocate(length);
            }
            return region->allocate();
        }

        void release(void* released_ptr)
        {
            if (released_ptr == nullptr)
            {
                return;
            }

            std::lock_guard<std::mutex> guard(_lock);

            BlockRegionPool* const region = findRegion(released_ptr);

            if (region == nullptr)
            {
                _heap.release(released_ptr);
                return;
            }
            region->release(released_ptr);
        }

        size_t regionCount() const
        {
            std::lock_guard<std::mutex> guard(_lock);
            return _region_count;
        }

        size_t reservedBytes() const
        {
            std::lock_guard<std::mutex> guard(_lock);

            size_t total = 0;

            for (size_t i = 0; i < _region_count; ++i)
            {
                total += _regions[i].reservedBytes();
            }
            return total;
        }

        size_t fallbackCount() const
        {
            std::lock_guard<std::mutex> guard(_lock);
            return _heap.count();
        }

    private:
        BlockRegionPool* findRegion(size_t length)
        {
            BlockRegionPool* smallest = nullptr;

            for (size_t i = 0; i < _region_count; ++i)
            {
                BlockRegionPool& region = _regions[i];

                if (region.blockSize() < length)
                {
                    continue;
                }
                if (smallest == nullptr || region.blockSize() < smallest->blockSize())
                {
                    smallest = &region;
                }
            }
            return smallest;
        }

        BlockRegionPool* findRegion(void* released_ptr)
        {
            for (size_t i = 0; i < _region_count; ++i)
            {
                BlockRegionPool& region = _regions[i];

                if (region.holds(released_ptr))
                {
                    return &region;
                }
            }
            return nullptr;
        }

        BlockRegionPool    _regions[MAX_REGIONS];
        size_t             _region_count;
        HeapFallback       _heap;
        mutable std::mutex _lock;
    };
}

#endif
