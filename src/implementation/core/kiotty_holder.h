#if !defined(KIOTTY_HOLDER_H)
#define KIOTTY_HOLDER_H

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace kiotty
{
    template<typename Base>
    class Holder
    {
    public:
        Holder() :
            _it(nullptr),
            _object_lifecycle(nullptr)
        {
        }

        template <typename Derived, typename ...Args>
        static Holder make(Args&&... args)
        {
            static_assert(std::is_base_of<Base, Derived>::value,
                "Derived must derive from Base");
            static_assert(sizeof(Derived) <= Base::HOLDER_SIZE,
                "Derived outgrew Base::HOLDER_SIZE - raise it (this changes sizeof(Holder))");
            static_assert(alignof(Derived) <= Base::HOLDER_ALIGN,
                "Derived needs stricter alignment than Base::HOLDER_ALIGN");
            static_assert(std::is_nothrow_move_constructible<Derived>::value,
                "Holder lives in std::vector and must move without throwing");

            Holder holder;

            Derived* object = ::new (holder.storage()) Derived(std::forward<Args>(args)...);

            holder._it = object;
            holder._object_lifecycle = lifeCycleOf<Derived>();
            return holder;
        }

        Holder(const Holder& other) = delete;
        Holder& operator=(const Holder& other) = delete;

        Holder(Holder&& other) noexcept :
            _it(nullptr),
            _object_lifecycle(nullptr)
        {
            moveFrom(other);
        }

        Holder& operator=(Holder&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                moveFrom(other);
            }
            return *this;
        }

        ~Holder()
        {
            reset();
        }

        void reset() noexcept
        {
            if (_object_lifecycle != nullptr)
            {
                _object_lifecycle->destroy(_it);
                _it = nullptr;
                _object_lifecycle = nullptr;
            }
        }

        explicit operator bool() const
        {
            return _object_lifecycle != nullptr;
        }

        Base& operator*() const
        {
            return *_it;
        }

        Base* operator->() const
        {
            return _it;
        }

    private:
        struct ObjectLifeCycle
        {
            Base* (*onMove)(Base* from, void* to);
            void  (*destroy)(Base* it);
        };

        template<typename Derived>
        static Base* onMoveOf(Base* from, void* to)
        {
            Derived* source = static_cast<Derived*>(from);
            Derived* moved = ::new (to) Derived(std::move(*source));
            source->~Derived();
            return moved;
        }

        template<typename Derived>
        static void destroyOf(Base* it)
        {
            static_cast<Derived*>(it)->~Derived();
        }

        template<typename Derived>
        static const ObjectLifeCycle* lifeCycleOf()
        {
            static const ObjectLifeCycle table = { &onMoveOf<Derived>, &destroyOf<Derived> };
            return &table;
        }

        void* storage()
        {
            return static_cast<void*>(&_storage);
        }

        void moveFrom(Holder& other) noexcept
        {
            if (other._object_lifecycle == nullptr)
            {
                return;
            }
            _it = other._object_lifecycle->onMove(other._it, storage());
            _object_lifecycle = other._object_lifecycle;
            other._it = nullptr;
            other._object_lifecycle = nullptr;
        }

        typename std::aligned_storage<Base::HOLDER_SIZE, Base::HOLDER_ALIGN>::type _storage;

        Base* _it {nullptr};
        const ObjectLifeCycle* _object_lifecycle {nullptr};
    };
}

#endif
