#include "kiotty_connection.h"

#include <utility>

namespace kiotty
{
    Connection::Connection(ActiveSocket&& sock,
                           const ConnectionInfo& info,
                           IoGameChannel channel,
                           IOMultiEventListener& listener,
                           BlockPool& pool,
                           size_t send_queue_limit,
                           IChannelBinder& binder,
                           IPacketCodec& codec) :
        _sock(std::move(sock)),
        _info(info),
        _channel(channel),
        _listener(listener),
        _binder(binder),
        _codec(codec),
        _event(makeTransferIOEvent(this)),
        _receive(pool),
        _send(send_queue_limit),
        _response_listener(*this, pool, codec),
        _event_listener(*this, pool, codec),
        _sending(),
        _sent(0),
        _recv_step(RecvStep::ReadingHeader),
        _send_state(SendState::Idle),
        _life_state(LifeState::Active)
    {
        _channel.response.addListener(_response_listener);
        _channel.event.addListener(_event_listener);
    }

    Connection::~Connection()
    {
        _channel.response.removeListener(_response_listener);
        _channel.event.removeListener(_event_listener);
        _binder.onDisconnected(_info, _channel);
    }

    Connection::operator bool() const
    {
        return _sock.handle() != INVALID_SOCKET_HANDLE &&
               static_cast<bool>(_event) &&
               static_cast<bool>(_send);
    }

    LifeState Connection::lifeState() const
    {
        return _life_state.load(std::memory_order_acquire);
    }

    void Connection::markClosing()
    {
        LifeState expected = LifeState::Active;

        _life_state.compare_exchange_strong(expected, LifeState::Closing);
    }

    void Connection::close()
    {
        markClosing();
        _sock.shutdown();
    }

    SocketCode Connection::submitReceive()
    {
        const ByteSpan space = receiveSpace();

        IOReceiveRequest request;
        request.sock   = &_sock;
        request.buffer = reinterpret_cast<char*>(space.data());
        request.length = space.size();

        return _listener.submit(*_event, request);
    }

    ByteSpan Connection::receiveSpace()
    {
        return (_recv_step == RecvStep::ReadingHeader)
                   ? _receive.headerSpace()
                   : _receive.payloadSpace();
    }

    void Connection::handleReceived(IOReceiveResponse& response)
    {
        const ReceiveOutcome outcome = advanceReceive(response);

        ReceivedPacket packet;

        if (outcome == ReceiveOutcome::Complete)
        {
            packet.header  = _receive.header();
            packet.payload = _receive.takePayload();
            _recv_step     = RecvStep::ReadingHeader;
        }

        if (endsConnection(response.code))
        {
            markClosing();
        }
        else
        {
            submitReceive();

            if (outcome == ReceiveOutcome::Rejected)
            {
                close();
            }
        }

        if (outcome == ReceiveOutcome::Complete)
        {
            dispatchPacket(packet);
        }
    }

    Connection::ReceiveOutcome Connection::advanceReceive(const IOReceiveResponse& response)
    {
        if (response.length == 0)
        {
            return ReceiveOutcome::Incomplete;
        }

        if (_recv_step == RecvStep::ReadingPayload)
        {
            _receive.addPayloadReceived(response.length);
            return _receive.isPayloadComplete() ? ReceiveOutcome::Complete
                                                : ReceiveOutcome::Incomplete;
        }

        _receive.addHeaderReceived(response.length);

        if (!_receive.isHeaderComplete())
        {
            return ReceiveOutcome::Incomplete;
        }
        return acceptHeader();
    }

    Connection::ReceiveOutcome Connection::acceptHeader()
    {
        const PacketHeader& header = _receive.header();

        if (!hasPacketMagic(header) ||
            !hasSupportedVersion(header) ||
            !_receive.openPayload())
        {
            _receive.reset();
            _recv_step = RecvStep::ReadingHeader;
            return ReceiveOutcome::Rejected;
        }

        _recv_step = RecvStep::ReadingPayload;

        return _receive.isPayloadComplete() ? ReceiveOutcome::Complete
                                            : ReceiveOutcome::Incomplete;
    }

    void Connection::dispatchPacket(ReceivedPacket& packet)
    {
        GameRequest request;

        if (!_codec.decode(packet, _channel.channel_id, request))
        {
            return;
        }
        _channel.request.emit(request);
    }

    void Connection::ResponseListener::onStream(const GameResponse& response)
    {
        Bytes packet = _codec.encode(_pool, response);

        if (!packet)
        {
            return;
        }
        _connection.emit(packet, DropPolicy::Never);
    }

    void Connection::EventListener::onStream(const GameEvent& event)
    {
        const uint32_t sequence = _sequence.fetch_add(1, std::memory_order_relaxed) + 1;

        Bytes packet = _codec.encode(_pool, event, sequence);

        if (!packet)
        {
            return;
        }
        _connection.emit(packet, DropPolicy::Oldest);
    }

    bool Connection::emit(Bytes& packet, DropPolicy policy)
    {
        if (queuePacket(packet, policy))
        {
            pumpSend();
            return true;
        }

        if (policy == DropPolicy::Never)
        {
            close();
        }
        return false;
    }

    bool Connection::queuePacket(Bytes& packet, DropPolicy policy)
    {
        SentPacket queued;
        queued.bytes  = std::move(packet);
        queued.policy = policy;

        std::lock_guard<std::mutex> guard(_send_lock);

        if (_send.tryPush(queued))
        {
            return true;
        }
        if (_send.dropOldest(DropPolicy::Oldest))
        {
            return _send.tryPush(queued);
        }
        return false;
    }

    SocketCode Connection::pumpSend()
    {
        IOSendRequest request = {};

        {
            std::lock_guard<std::mutex> guard(_send_lock);

            if (_send_state == SendState::Sending || _send.empty())
            {
                return SocketCode::SOCKET_SUCCESS;
            }

            SentPacket next = _send.pop();

            _sending    = std::move(next.bytes);
            _sent       = 0;
            _send_state = SendState::Sending;
            request     = pendingSendRequest();
        }

        const SocketCode code = _listener.submit(*_event, request);

        if (code != SocketCode::SOCKET_SUCCESS)
        {
            releaseSending();
        }
        return code;
    }

    void Connection::handleSent(IOSendResponse& response)
    {
        if (endsConnection(response.code))
        {
            markClosing();
            releaseSending();
            return;
        }

        if (submitRemainingSend(response.length))
        {
            return;
        }

        releaseSending();
        pumpSend();
    }

    bool Connection::submitRemainingSend(size_t sent_length)
    {
        IOSendRequest request = {};

        {
            std::lock_guard<std::mutex> guard(_send_lock);

            _sent += sent_length;

            if (_sent >= _sending.size())
            {
                return false;
            }
            request = pendingSendRequest();
        }

        if (_listener.submit(*_event, request) != SocketCode::SOCKET_SUCCESS)
        {
            releaseSending();
        }
        return true;
    }

    IOSendRequest Connection::pendingSendRequest()
    {
        IOSendRequest request;
        request.sock   = &_sock;
        request.buffer = reinterpret_cast<const char*>(_sending.data()) + _sent;
        request.length = _sending.size() - _sent;

        return request;
    }

    void Connection::releaseSending()
    {
        std::lock_guard<std::mutex> guard(_send_lock);

        _sending    = Bytes();
        _sent       = 0;
        _send_state = SendState::Idle;
    }

    void Connection::handleDisconnected()
    {
        _life_state.store(LifeState::Closed, std::memory_order_release);
        releaseSending();
    }
}
