#include "kiotty_connection_table.h"

#include <new>
#include <utility>

namespace kiotty
{
    ConnectionTable::ConnectionTable(size_t capacity, BlockPool& pool, size_t send_queue_limit,
                                     IChannelBinder& binder, IPacketCodec& codec) :
        _slots(nullptr),
        _capacity(capacity),
        _size(0),
        _pool(pool),
        _send_queue_limit(send_queue_limit),
        _binder(binder),
        _codec(codec)
    {
        if (capacity > 0)
        {
            _slots = new (std::nothrow) Slot[capacity];
        }

        if (_slots == nullptr)
        {
            _capacity = 0;
        }
    }

    ConnectionTable::~ConnectionTable()
    {
        closeEveryLiveSlot();
        delete[] _slots;
    }

    Connection* ConnectionTable::open(SocketHandle& accepted, IOMultiEventListener& listener)
    {
        Slot* const slot = findFreeSlot();

        if (slot == nullptr)
        {
            return nullptr;
        }

        ActiveSocket sock(accepted);
        accepted = INVALID_SOCKET_HANDLE;

        if (sock.handle() == INVALID_SOCKET_HANDLE)
        {
            return nullptr;
        }

        const ConnectionInfo info = makeConnectionInfo(sock.ip(), sock.port());
        const IoChannelResult bound = _binder.onConnected(info);

        if (!bound.isOk())
        {
            return nullptr;
        }

        Connection* const connection = ::new (slot->storage())
            Connection(std::move(sock), info, bound.value(), listener,
                       _pool, _send_queue_limit, _binder, _codec);

        slot->live = true;
        _size.fetch_add(1, std::memory_order_release);
        return connection;
    }

    void ConnectionTable::close(Connection& connection)
    {
        Slot* const slot = findSlotOf(connection);

        if (slot == nullptr)
        {
            return;
        }

        destroy(*slot);
        _size.fetch_sub(1, std::memory_order_release);
    }

    void ConnectionTable::reapClosed()
    {
        for (size_t i = 0; i < _capacity; ++i)
        {
            Slot& slot = _slots[i];

            if (slot.live && slot.connection()->lifeState() == LifeState::Closed)
            {
                destroy(slot);
                _size.fetch_sub(1, std::memory_order_release);
            }
        }
    }

    Connection* ConnectionTable::at(size_t index)
    {
        if (index >= _capacity || !_slots[index].live)
        {
            return nullptr;
        }
        return _slots[index].connection();
    }

    ConnectionTable::Slot* ConnectionTable::findFreeSlot()
    {
        for (size_t i = 0; i < _capacity; ++i)
        {
            if (!_slots[i].live)
            {
                return &_slots[i];
            }
        }
        return nullptr;
    }

    ConnectionTable::Slot* ConnectionTable::findSlotOf(Connection& connection)
    {
        for (size_t i = 0; i < _capacity; ++i)
        {
            if (_slots[i].live && _slots[i].connection() == &connection)
            {
                return &_slots[i];
            }
        }
        return nullptr;
    }

    void ConnectionTable::destroy(Slot& slot)
    {
        slot.connection()->~Connection();
        slot.live = false;
    }

    void ConnectionTable::closeEveryLiveSlot()
    {
        for (size_t i = 0; i < _capacity; ++i)
        {
            if (_slots[i].live)
            {
                destroy(_slots[i]);
            }
        }
        _size.store(0, std::memory_order_release);
    }
}
