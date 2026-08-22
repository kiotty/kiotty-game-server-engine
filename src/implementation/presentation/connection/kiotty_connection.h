#if !defined(KIOTTY_PRESENTATION_CONNECTION_CONNECTION_H)
#define KIOTTY_PRESENTATION_CONNECTION_CONNECTION_H

#include <core/kiotty_block_pool.h>
#include <core/kiotty_bytes.h>
#include <core/kiotty_connection_buffer.h>
#include <core/kiotty_holder.h>
#include <core/kiotty_stream.h>
#include <domain/channel/kiotty_channel_binder.h>
#include <domain/channel/kiotty_game_channel.h>
#include <domain/codec/kiotty_packet_codec.h>
#include <domain/entity/kiotty_connection_info.h>
#include <domain/entity/kiotty_socket_code.h>
#include <presentation/event/kiotty_io_event.h>
#include <presentation/event_listener/kiotty_io_event_listener.h>
#include <presentation/socket/kiotty_socket.h>
#include <presentation/state/kiotty_connection_state.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace kiotty
{
    class Connection : public IOContext
    {
    public:
        Connection(ActiveSocket&& sock,
                   const ConnectionInfo& info,
                   IoGameChannel channel,
                   IOMultiEventListener& listener,
                   BlockPool& pool,
                   size_t send_queue_limit,
                   IChannelBinder& binder,
                   IPacketCodec& codec);

        ~Connection();

        Connection(const Connection&) = delete;
        Connection& operator=(const Connection&) = delete;
        Connection(Connection&&) = delete;
        Connection& operator=(Connection&&) = delete;

        explicit operator bool() const;

        const char* ip() const { return _info.ip; }
        uint16_t port() const { return _info.port; }
        const ConnectionInfo& info() const { return _info; }
        ChannelId channelId() const { return _channel.channel_id; }

        LifeState lifeState() const;

        SocketCode submitReceive();

        bool emit(Bytes& packet, DropPolicy policy);

        void close();

        void handleReceived(IOReceiveResponse& response);
        void handleSent(IOSendResponse& response);
        void handleDisconnected();

    private:
        enum class ReceiveOutcome
        {
            Incomplete,
            Complete,
            Rejected,
        };

        class ResponseListener : public StreamListener<GameResponse>
        {
        public:
            ResponseListener(Connection& connection, BlockPool& pool, IPacketCodec& codec) :
                _connection(connection),
                _pool(pool),
                _codec(codec)
            {
            }

            void onStream(const GameResponse& response) override;

        private:
            Connection&   _connection;
            BlockPool&    _pool;
            IPacketCodec& _codec;
        };

        class EventListener : public StreamListener<GameEvent>
        {
        public:
            EventListener(Connection& connection, BlockPool& pool, IPacketCodec& codec) :
                _connection(connection),
                _pool(pool),
                _codec(codec),
                _sequence(0)
            {
            }

            void onStream(const GameEvent& event) override;

        private:
            Connection&           _connection;
            BlockPool&            _pool;
            IPacketCodec&         _codec;
            std::atomic<uint32_t> _sequence;
        };

        void markClosing();

        ReceiveOutcome advanceReceive(const IOReceiveResponse& response);
        ReceiveOutcome acceptHeader();
        ByteSpan receiveSpace();
        void dispatchPacket(ReceivedPacket& packet);

        bool queuePacket(Bytes& packet, DropPolicy policy);
        SocketCode pumpSend();
        bool submitRemainingSend(size_t sent_length);
        IOSendRequest pendingSendRequest();
        void releaseSending();

        ActiveSocket            _sock;
        ConnectionInfo          _info;
        IoGameChannel           _channel;
        IOMultiEventListener&   _listener;
        IChannelBinder&         _binder;
        IPacketCodec&           _codec;
        Holder<IOTransferEvent> _event;
        ReceiveBuffer           _receive;
        SendBuffer              _send;
        ResponseListener        _response_listener;
        EventListener           _event_listener;

        Bytes  _sending;
        size_t _sent;

        RecvStep               _recv_step;
        SendState              _send_state;
        std::atomic<LifeState> _life_state;

        std::mutex _send_lock;
    };
}

#endif
