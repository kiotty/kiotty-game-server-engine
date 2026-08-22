// =============================================================================
// kiotty_flow_io_uring.cpp
// -----------------------------------------------------------------------------
// io_uring 방식으로 "요청/응답"과 "이벤트"가 어떤 순서로 도는지만 보여주는 파일.
// 짝이 되는 파일은 kiotty_flow_iocp.cpp / kiotty_flow_epoll.cpp 이며, 절(節)
// 번호와 클래스 이름이 셋 다 같다. 나란히 놓고 diff 하면 백엔드가 무엇을
// 다르게 하는지가 그대로 보인다.
//
// 준비:
//   sudo apt install liburing-dev
//
// 빌드:
//   g++ -std=c++11 -Wall -Wextra -pthread kiotty_flow_io_uring.cpp -luring -o flow_uring
//
// 프로토콜(설명을 위해 최대한 줄였다):
//   [길이 4 byte (little endian)][payload]
//
// -----------------------------------------------------------------------------
// 한 줄 요약 - io_uring 은 어느 쪽을 닮았나
//
//   완료 모델이라는 점에서 IOCP 를 닮았다. 버퍼를 먼저 커널에 맡기고, 다
//   채워지면 "몇 byte 됐다"를 받는다. epoll 처럼 "읽을 수 있다"만 알려주고
//   내가 다시 recv() 를 부르는 방식이 아니다.
//
//   다른 점은 딱 하나다 - 제출(submit)이 스레드 안전하지 않다.
//   IOCP 는 아무 스레드나 WSASend 를 발행해도 되지만, io_uring 의 SQ ring 은
//   한 스레드만 만져야 한다. 그래서 다른 스레드에서 보내려면 넘겨야 한다.
//   epoll 의 epoll_ctl 같은 "스레드 안전한 뒷문"이 없다.
//
// -----------------------------------------------------------------------------
// 읽는 순서
//
//   §1  Connection  - socket + recv buffer + send buffer. 그게 전부다
//   §2  Endpoint    - 듣는 소켓
//   §3  IoLoop      - ring 하나 + 스레드 하나
//   §4  요청/응답 흐름  F1 ~ F8
//   §5  이벤트 흐름     E1 ~ E5
//   §6  main
//   §7  다른 두 파일과 다른 곳
//
// 흐름 표시(세 파일이 같은 번호다):
//   F1 accept 완료 -> Connection 생성
//   F2 길이 4 byte 수신 발행
//   F3 길이 수신 완료 -> 길이 추출
//   F4 payload 수신 발행
//   F5 payload 수신 완료 -> 처리
//   F6 응답 생성 -> send buffer 에 쌓기
//   F7 send 발행
//   F8 send 완료 -> 큐에 남은 것 처리, 그리고 F2 로 돌아간다
//
//   E1 워커 스레드를 만들기 "전에" 보내기 콜백을 등록한다
//   E2 워커가 일을 마치고 콜백을 부른다
//   E3 콜백이 payload 를 send buffer 에 쌓고 보내기를 요청한다
//   E4 io_uring 은 워커 스레드에서 제출할 수 없다. 큐에 넣고 eventfd 로 깨운다
//      <-- IOCP 와도, epoll 과도 다른 곳
//   E5 루프 스레드가 깨어나 제출 -> F8 과 같은 자리로 합류한다
// =============================================================================

#include <liburing.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

static const size_t   kLengthBytes = 4;
static const size_t   kMaxPayload  = 64 * 1024;
static const unsigned kRingEntries = 256;

// =============================================================================
// §0. 완료를 되찾기 위한 꼬리표
// -----------------------------------------------------------------------------
// io_uring 은 완료(CQE)에 우리가 넣어 둔 user_data 하나만 돌려준다.
//
//   IOCP     : completion key(누구) + OVERLAPPED(무슨 작업)  -> 둘을 준다
//   io_uring : user_data                                     -> 하나뿐이다
//   epoll    : epoll_data                                    -> 하나뿐이다
//
// 그래서 IOCP 파일과 달리 꼬리표가 "무슨 작업인가"와 "누구의 것인가"를 모두
// 들고 있어야 한다. 이것이 IoRequest 에 owner 가 붙은 이유다.
// =============================================================================

enum class OpKind
{
    Accept,
    Recv,
    Send,
    Wakeup,     // eventfd 감시 - 다른 스레드가 루프를 깨웠다
};

struct IoRequest
{
    OpKind kind;
    void*  owner;       // Connection* 또는 Endpoint*. Wakeup 이면 사용하지 않는다
};

// =============================================================================
// §1. Connection - socket + recv buffer + send buffer
// -----------------------------------------------------------------------------
// ★ 이 절은 IOCP 파일과 사실상 같은 코드다. 완료 모델이 같기 때문이다. ★
//   epoll 파일과도 같지만, 그쪽은 "맡긴 버퍼"가 아니라 "내가 읽을 자리"라는
//   점에서 의미가 조금 다르다.
// =============================================================================

struct ByteSpan
{
    uint8_t* data;
    size_t   size;
};

enum class RecvStep
{
    Length,     // 4 byte 를 모으는 중
    Payload,    // 그 길이만큼 모으는 중
};

class RecvBuffer
{
public:
    RecvBuffer()
        : _step(RecvStep::Length)
        , _filled(0)
        , _want(kLengthBytes)
    {
    }

    // 지금 커널에 맡길 자리. 남은 만큼만 정확히 받는다.
    ByteSpan spaceToFill()
    {
        ByteSpan span;
        span.data = _data + _filled;
        span.size = _want - _filled;
        return span;
    }

    bool filled(size_t bytes)
    {
        _filled += bytes;
        return _filled == _want;
    }

    uint32_t takeLengthAndExpectPayload()
    {
        const uint32_t length = static_cast<uint32_t>(_data[0])
                              | (static_cast<uint32_t>(_data[1]) << 8)
                              | (static_cast<uint32_t>(_data[2]) << 16)
                              | (static_cast<uint32_t>(_data[3]) << 24);

        _step   = RecvStep::Payload;
        _filled = 0;
        _want   = length;
        return length;
    }

    void expectNextLength()
    {
        _step   = RecvStep::Length;
        _filled = 0;
        _want   = kLengthBytes;
    }

    RecvStep       step() const    { return _step; }
    const uint8_t* payload() const { return _data; }
    size_t         size() const    { return _filled; }

private:
    RecvStep _step;
    size_t   _filled;
    size_t   _want;
    uint8_t  _data[kMaxPayload];
};

class SendBuffer
{
public:
    SendBuffer()
        : _sending(false)
        , _sent(0)
    {
    }

    void push(const std::vector<uint8_t>& packet)
    {
        std::lock_guard<std::mutex> guard(_lock);
        _queue.push_back(packet);
    }

    bool tryBeginSending()
    {
        std::lock_guard<std::mutex> guard(_lock);

        if (_sending || _queue.empty())
        {
            return false;
        }
        _sending = true;
        return true;
    }

    ByteSpan frontChunk()
    {
        std::lock_guard<std::mutex> guard(_lock);

        ByteSpan span;
        span.data = _queue.front().data() + _sent;
        span.size = _queue.front().size() - _sent;
        return span;
    }

    bool consume(size_t bytes)
    {
        std::lock_guard<std::mutex> guard(_lock);
        _sent += bytes;

        if (_sent < _queue.front().size())
        {
            return false;
        }
        _queue.pop_front();
        _sent = 0;
        return true;
    }

    void endSending()
    {
        std::lock_guard<std::mutex> guard(_lock);
        _sending = false;
    }

    bool empty()
    {
        std::lock_guard<std::mutex> guard(_lock);
        return _queue.empty();
    }

private:
    std::mutex                        _lock;
    std::deque<std::vector<uint8_t> > _queue;
    bool                              _sending;
    size_t                            _sent;
};

class IoLoop;

class Connection
{
public:
    Connection(int fd, IoLoop& loop)
        : _fd(fd)
        , _loop(loop)
    {
        // 꼬리표에 자기 자신을 박아 둔다. CQE 에는 이 포인터만 실려 온다.
        _recv_io.kind  = OpKind::Recv;
        _recv_io.owner = this;
        _send_io.kind  = OpKind::Send;
        _send_io.owner = this;
    }

    ~Connection()
    {
        if (_fd >= 0)
        {
            ::close(_fd);
        }
    }

    int         fd() const { return _fd; }
    RecvBuffer& recv()     { return _recv; }
    SendBuffer& send()     { return _send; }
    IoRequest*  recvIo()   { return &_recv_io; }
    IoRequest*  sendIo()   { return &_send_io; }

    // 소멸자보다 먼저 닫아야 할 때 쓴다. 두 번 닫지 않도록 -1 로 만든다.
    void close()
    {
        if (_fd >= 0)
        {
            ::close(_fd);
            _fd = -1;
        }
    }

private:
    int        _fd;
    IoLoop&    _loop;
    RecvBuffer _recv;
    SendBuffer _send;
    IoRequest  _recv_io;    // 커넥션당 수신 1개까지만 떠 있으므로 하나면 된다
    IoRequest  _send_io;    // 송신도 마찬가지
};

// =============================================================================
// §2. Endpoint - 듣는 소켓
// -----------------------------------------------------------------------------
// IOCP 의 AcceptEx 와 달리 "미리 만들어 둔 빈 소켓"이 필요 없다.
// IORING_OP_ACCEPT 가 완료되면 cqe->res 가 곧 새 fd 다. epoll 의 accept() 와 같다.
// =============================================================================

class Endpoint
{
public:
    Endpoint()
        : _fd(-1)
    {
        _accept_io.kind  = OpKind::Accept;
        _accept_io.owner = this;
    }

    bool open(uint16_t port)
    {
        _fd = ::socket(AF_INET, SOCK_STREAM, 0);

        if (_fd < 0)
        {
            return false;
        }

        int reuse = 1;
        ::setsockopt(_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        sockaddr_in address;
        std::memset(&address, 0, sizeof(address));
        address.sin_family      = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port        = ::htons(port);

        if (::bind(_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
        {
            return false;
        }
        return ::listen(_fd, 64) == 0;
    }

    ~Endpoint()
    {
        if (_fd >= 0)
        {
            ::close(_fd);
        }
    }

    int        fd() const { return _fd; }
    IoRequest* acceptIo() { return &_accept_io; }

private:
    int       _fd;
    IoRequest _accept_io;
};

// =============================================================================
// §3. IoLoop - ring 하나 + 스레드 하나
// -----------------------------------------------------------------------------
// ring 은 두 개의 원형 큐다.
//   SQ (submission queue) - 내가 "이 작업 해 줘"를 넣는 곳. 커널이 읽는다
//   CQ (completion queue) - 커널이 "이 작업 됐다"를 넣는 곳. 내가 읽는다
//
// 흐름은 IOCP 와 같다. 작업을 걸고, 완료를 받는다. 다른 것은 "거는" 방법이
// 함수 호출(WSARecv)이 아니라 SQ 에 항목을 채워 넣고 한 번에 제출하는 것이다.
// 그래서 여러 작업을 모아 한 번의 syscall 로 낼 수 있다.
// =============================================================================

class IoLoop
{
public:
    IoLoop()
        : _wakeup(-1)
        , _running(false)
    {
        _wakeup_io.kind  = OpKind::Wakeup;
        _wakeup_io.owner = 0;
        std::memset(&_ring, 0, sizeof(_ring));
    }

    bool open()
    {
        if (::io_uring_queue_init(kRingEntries, &_ring, 0) < 0)
        {
            return false;
        }

        // 다른 스레드가 루프를 깨우는 손잡이.
        //
        // epoll 에서는 이것이 필요 없었다 - epoll_ctl 이 스레드 안전한 데다
        // 관심을 추가하는 것만으로 epoll_wait 가 깨어났기 때문이다.
        // io_uring 에는 그런 뒷문이 없다. SQ 를 만질 수 있는 것은 루프 스레드
        // 하나뿐이라, 다른 스레드는 "깨워 달라"는 신호만 보낼 수 있다.
        _wakeup = ::eventfd(0, EFD_NONBLOCK);
        return _wakeup >= 0;
    }

    ~IoLoop()
    {
        for (size_t i = 0; i < _connections.size(); ++i)
        {
            delete _connections[i];
        }

        for (size_t i = 0; i < _closed.size(); ++i)
        {
            delete _closed[i];
        }

        if (_wakeup >= 0)
        {
            ::close(_wakeup);
        }
        ::io_uring_queue_exit(&_ring);
    }

    // ---------------------------------------------------------------------
    // 작업 걸기
    //
    // 셋 다 모양이 같다.
    //   1) io_uring_get_sqe()  - SQ 에서 빈 칸 하나를 얻는다
    //   2) io_uring_prep_*()   - 그 칸을 "무슨 작업인지"로 채운다
    //   3) set_data()          - 꼬리표를 단다 (완료 때 그대로 돌아온다)
    //   4) io_uring_submit()   - 커널에 낸다
    //
    // 4번을 매번 부르지 않고 여러 개를 채운 뒤 한 번만 불러도 된다.
    // 그것이 io_uring 이 syscall 을 줄이는 방식이다. 여기서는 순서를 보이려고
    // 매번 낸다.
    // ---------------------------------------------------------------------

    bool submitAccept(Endpoint& endpoint)
    {
        io_uring_sqe* sqe = ::io_uring_get_sqe(&_ring);

        if (sqe == 0)
        {
            return false;               // SQ 가 꽉 찼다
        }

        ::io_uring_prep_accept(sqe, endpoint.fd(), 0, 0, 0);
        ::io_uring_sqe_set_data(sqe, endpoint.acceptIo());
        return ::io_uring_submit(&_ring) >= 0;
    }

    bool submitRecv(Connection& connection)
    {
        io_uring_sqe* sqe = ::io_uring_get_sqe(&_ring);

        if (sqe == 0)
        {
            return false;
        }

        // ★ IOCP 의 WSARecv 와 같은 자리다 - 버퍼를 커널에 맡긴다 ★
        // 이 버퍼는 완료가 올 때까지 건드리면 안 된다. epoll 파일에는 이
        // 제약이 없었다(거기서는 내가 직접 recv 를 부르므로).
        ByteSpan space = connection.recv().spaceToFill();

        ::io_uring_prep_recv(sqe, connection.fd(), space.data, space.size, 0);
        ::io_uring_sqe_set_data(sqe, connection.recvIo());
        return ::io_uring_submit(&_ring) >= 0;
    }

    // ★ 여기가 다른 두 파일과 갈리는 첫 번째 지점 ★
    //
    // 이 함수는 반드시 루프 스레드에서 불려야 한다. SQ ring 이 스레드 안전하지
    // 않기 때문이다. IOCP 는 아무 스레드나 WSASend 를 부를 수 있었고, epoll 은
    // epoll_ctl 이라는 스레드 안전한 뒷문이 있었다. 여기는 둘 다 없다.
    bool submitSend(Connection& connection)
    {
        io_uring_sqe* sqe = ::io_uring_get_sqe(&_ring);

        if (sqe == 0)
        {
            return false;
        }

        ByteSpan chunk = connection.send().frontChunk();

        ::io_uring_prep_send(sqe, connection.fd(), chunk.data, chunk.size, MSG_NOSIGNAL);
        ::io_uring_sqe_set_data(sqe, connection.sendIo());
        return ::io_uring_submit(&_ring) >= 0;
    }

    // eventfd 를 감시한다. 완료되면 "누가 깨웠다"는 뜻이다.
    // 한 번 완료되면 사라지므로 완료를 처리한 뒤 다시 건다.
    bool submitWakeupWatch()
    {
        io_uring_sqe* sqe = ::io_uring_get_sqe(&_ring);

        if (sqe == 0)
        {
            return false;
        }

        ::io_uring_prep_read(sqe, _wakeup, &_wakeup_value, sizeof(_wakeup_value), 0);
        ::io_uring_sqe_set_data(sqe, &_wakeup_io);
        return ::io_uring_submit(&_ring) >= 0;
    }

    // 큐에 넣은 다음 부른다.
    // ★ 부르는 스레드가 루프 스레드인지에 따라 갈린다 ★
    void requestSend(Connection& connection)
    {
        if (!connection.send().tryBeginSending())
        {
            return;                     // 이미 누가 보내는 중이다
        }

        if (std::this_thread::get_id() == _loop_thread)
        {
            // 루프 스레드다. 그 자리에서 제출한다.
            if (!submitSend(connection))
            {
                connection.send().endSending();
            }
            return;
        }

        // E4 : 다른 스레드다. SQ 를 만질 수 없으니 넘기고 깨운다.
        {
            std::lock_guard<std::mutex> guard(_lock);
            _pending_sends.push_back(&connection);
        }
        wake();
    }

    void wake()
    {
        const uint64_t one = 1;
        ssize_t written = ::write(_wakeup, &one, sizeof(one));
        (void)written;
    }

    void addConnection(Connection* connection)
    {
        std::lock_guard<std::mutex> guard(_lock);
        _connections.push_back(connection);
    }

    void broadcast(const std::vector<uint8_t>& packet);

    void run(Endpoint& endpoint);

    void stop()
    {
        _running = false;
        wake();
    }

    // 끊긴 커넥션을 정리한다.
    //
    // epoll 과 달리 "관심을 내려놓는" 절차가 없다. 작업을 다시 걸지 않으면
    // 완료도 다시 오지 않기 때문이다. 그냥 닫고 목록에서 빼면 끝이다.
    void closeConnection(Connection& connection)
    {
        connection.close();

        std::lock_guard<std::mutex> guard(_lock);

        for (size_t i = 0; i < _connections.size(); ++i)
        {
            if (_connections[i] == &connection)
            {
                _connections.erase(_connections.begin() + i);
                break;
            }
        }
        // 객체는 지금 지우지 않는다. 워커 스레드가 broadcast() 안에서 이 포인터를
        // 들고 있을 수 있기 때문이다. 실제 엔진에서 이 수명 문제를 어떻게 다루는지는
        // docs/architecture.html 12.1 을 보라.
        _closed.push_back(&connection);
    }

    // E5 : 워커가 맡긴 것을 루프 스레드에서 제출한다.
    void drainPendingSends()
    {
        std::vector<Connection*> targets;

        {
            std::lock_guard<std::mutex> guard(_lock);
            targets.swap(_pending_sends);
        }

        for (size_t i = 0; i < targets.size(); ++i)
        {
            if (!submitSend(*targets[i]))
            {
                targets[i]->send().endSending();
            }
        }
    }

private:
    io_uring                 _ring;
    int                      _wakeup;
    uint64_t                 _wakeup_value;     // eventfd 를 읽어 담을 곳
    IoRequest                _wakeup_io;
    bool                     _running;
    std::thread::id          _loop_thread;
    std::mutex               _lock;
    std::vector<Connection*> _connections;
    std::vector<Connection*> _pending_sends;    // 다른 스레드가 맡긴 것
    std::vector<Connection*> _closed;           // 끊겼지만 아직 지우지 않은 것
};

// =============================================================================
// §4. 요청/응답 흐름  F1 ~ F8
// -----------------------------------------------------------------------------
// ★ 이 절의 함수들은 IOCP 파일과 거의 같다. 완료 모델이 같기 때문이다.
//    다른 것은 "몇 byte 됐나 / 실패했나"를 cqe->res 하나로 읽는다는 점뿐이다.
//      res >= 0 : 그 값이 byte 수
//      res <  0 : -errno
// =============================================================================

static std::vector<uint8_t> handleRequest(const uint8_t* payload, size_t size)
{
    std::printf("[F5] payload %zu byte 를 받았다: %.*s\n",
                size, static_cast<int>(size), reinterpret_cast<const char*>(payload));

    // F6 : 응답을 만든다. [길이 4 byte][payload] 로 감싼다.
    const char*  body      = "pong";
    const size_t body_size = 4;

    std::vector<uint8_t> response(kLengthBytes + body_size);
    response[0] = static_cast<uint8_t>(body_size);
    response[1] = 0;
    response[2] = 0;
    response[3] = 0;
    std::memcpy(&response[kLengthBytes], body, body_size);

    return response;
}

// -----------------------------------------------------------------------------
// F1 : accept 완료. cqe->res 가 곧 새 fd 다.
// -----------------------------------------------------------------------------
static void onAccepted(IoLoop& loop, Endpoint& endpoint, int result)
{
    if (result < 0)
    {
        std::printf("[--] accept 실패: %s\n", std::strerror(-result));
        loop.submitAccept(endpoint);
        return;
    }

    Connection* connection = new Connection(result, loop);

    loop.addConnection(connection);

    std::printf("[F1] 접속. Connection 을 만들었다\n");

    loop.submitRecv(*connection);       // F2 : 길이 4 byte 부터
    loop.submitAccept(endpoint);        // 다음 손님을 다시 받는다
}

// -----------------------------------------------------------------------------
// F3 / F5 : 수신 완료
// -----------------------------------------------------------------------------
static void onReceived(IoLoop& loop, Connection& connection, int result)
{
    if (result <= 0)
    {
        // 0 = 상대가 정상 종료, 음수 = 오류. 어느 쪽이든 답은 닫는 것이다.
        std::printf("[--] 상대가 끊었다\n");
        loop.closeConnection(connection);
        return;
    }

    if (!connection.recv().filled(static_cast<size_t>(result)))
    {
        loop.submitRecv(connection);     // 아직 덜 왔다. 같은 단계로 한 번 더
        return;
    }

    if (connection.recv().step() == RecvStep::Length)
    {
        // F3 : 4 byte 가 다 왔다.
        const uint32_t length = connection.recv().takeLengthAndExpectPayload();
        std::printf("[F3] 길이 = %u\n", length);

        loop.submitRecv(connection);     // F4 : 그 길이만큼 받는다
        return;
    }

    // F5 : payload 가 다 왔다.
    const std::vector<uint8_t> response =
        handleRequest(connection.recv().payload(), connection.recv().size());

    connection.recv().expectNextLength();
    loop.submitRecv(connection);         // F2 로 돌아간다 (로직보다 먼저 건다)

    connection.send().push(response);    // F6
    loop.requestSend(connection);        // F7
}

// -----------------------------------------------------------------------------
// F8 : 송신 완료
// -----------------------------------------------------------------------------
static void onSent(IoLoop& loop, Connection& connection, int result)
{
    if (result <= 0)
    {
        std::printf("[--] 전송 실패\n");
        connection.send().endSending();
        return;
    }

    if (!connection.send().consume(static_cast<size_t>(result)))
    {
        loop.submitSend(connection);     // 부분 전송. 이어서 보낸다
        return;
    }

    std::printf("[F8] 보냈다\n");

    connection.send().endSending();      // 먼저 자리를 내려놓고

    if (!connection.send().empty())
    {
        loop.requestSend(connection);    // 그 다음 큐를 한 번 더 본다
    }
    // 순서를 뒤집으면 그 사이 들어온 항목을 아무도 보내지 않는 상태가 된다.
}

// -----------------------------------------------------------------------------
// 루프 본체.
// -----------------------------------------------------------------------------
void IoLoop::run(Endpoint& endpoint)
{
    _running     = true;
    _loop_thread = std::this_thread::get_id();

    submitAccept(endpoint);
    submitWakeupWatch();

    while (_running)
    {
        // 완료가 하나라도 생길 때까지 기다린다.
        io_uring_cqe* first = 0;

        if (::io_uring_wait_cqe(&_ring, &first) < 0)
        {
            break;
        }

        // ★ 쌓여 있는 완료를 한꺼번에 훑는다 ★
        //
        // IOCP 의 GetQueuedCompletionStatusEx, epoll 의 epoll_wait 가 배열을
        // 돌려주는 것과 같은 자리다. 셋 다 "한 번 깨어나서 여러 개 처리"다.
        // 여기서는 syscall 조차 없다 - CQ 는 커널과 공유하는 메모리라 그냥 읽는다.
        unsigned      head  = 0;
        unsigned      count = 0;
        io_uring_cqe* cqe   = 0;

        io_uring_for_each_cqe(&_ring, head, cqe)
        {
            ++count;

            IoRequest* request = reinterpret_cast<IoRequest*>(::io_uring_cqe_get_data(cqe));
            const int  result  = cqe->res;

            if (request == 0)
            {
                continue;
            }

            switch (request->kind)
            {
            case OpKind::Accept:
                onAccepted(*this, *reinterpret_cast<Endpoint*>(request->owner), result);
                break;

            case OpKind::Recv:
                onReceived(*this, *reinterpret_cast<Connection*>(request->owner), result);
                break;

            case OpKind::Send:
                onSent(*this, *reinterpret_cast<Connection*>(request->owner), result);
                break;

            case OpKind::Wakeup:
                // E5 : 워커가 맡긴 것을 여기서 제출한다.
                drainPendingSends();
                submitWakeupWatch();     // 감시를 다시 건다
                break;
            }
        }

        // 훑은 만큼 CQ 에서 치운다. 이것을 빠뜨리면 같은 완료를 계속 다시 본다.
        ::io_uring_cq_advance(&_ring, count);
    }
}

// =============================================================================
// §5. 이벤트 흐름  E1 ~ E5
// =============================================================================

// E3 : 콜백이 하는 일. 세 파일 모두 코드가 같다 - 큐에 넣고 요청한다.
// 다른 것은 requestSend() 안에서 일어난다.
void IoLoop::broadcast(const std::vector<uint8_t>& packet)
{
    std::vector<Connection*> targets;

    {
        std::lock_guard<std::mutex> guard(_lock);
        targets = _connections;
    }

    for (size_t i = 0; i < targets.size(); ++i)
    {
        targets[i]->send().push(packet);

        // 이 함수는 워커 스레드에서 돈다. requestSend() 가 그것을 알아채고
        // E4 (큐에 넣고 eventfd 로 깨우기) 로 간다.
        requestSend(*targets[i]);
    }
}

class EventWorker
{
public:
    typedef std::function<void(const std::vector<uint8_t>&)> SendCallback;

    EventWorker()
        : _running(false)
    {
    }

    // E1 : 스레드를 만들기 "전에" 콜백을 등록한다.
    void setSendCallback(SendCallback callback)
    {
        _callback = callback;
    }

    void start()
    {
        _running = true;
        _thread  = std::thread(&EventWorker::loop, this);
    }

    ~EventWorker()
    {
        _running = false;

        if (_thread.joinable())
        {
            _thread.join();
        }
    }

private:
    void loop()
    {
        while (_running)
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));

            // E2 : 일이 끝났다. 이벤트를 만든다.
            const char*  body      = "tick";
            const size_t body_size = 4;

            std::vector<uint8_t> packet(kLengthBytes + body_size);
            packet[0] = static_cast<uint8_t>(body_size);
            packet[1] = 0;
            packet[2] = 0;
            packet[3] = 0;
            std::memcpy(&packet[kLengthBytes], body, body_size);

            std::printf("[E2] 이벤트를 만들어 콜백에 넘긴다\n");

            if (_callback)
            {
                _callback(packet);       // E3 로 간다
            }
        }
    }

    bool         _running;
    std::thread  _thread;
    SendCallback _callback;
};

// =============================================================================
// §6. main
// -----------------------------------------------------------------------------
// ★ 세 파일이 같은 코드다. ★
// =============================================================================

int main()
{
    // 출력을 버퍼링하지 않는다.
    // 이것이 없으면 표준 출력이 터미널이 아닐 때(파일로 넘기거나, Windows 에서
    // wsl 로 실행할 때) libc 가 4 KB 씩 모아 두므로 화면에 아무것도 안 나온다.
    // 서버가 안 도는 것으로 보이지만 실제로는 잘 돌고 있다.
    std::setvbuf(stdout, 0, _IONBF, 0);

    IoLoop loop;

    if (!loop.open())
    {
        std::printf("io_uring 을 못 만들었다\n");
        return 1;
    }

    Endpoint endpoint;

    if (!endpoint.open(7777))
    {
        std::printf("listen 실패\n");
        return 1;
    }

    EventWorker worker;
    worker.setSendCallback([&loop](const std::vector<uint8_t>& packet)   // E1
    {
        loop.broadcast(packet);
    });
    worker.start();

    std::printf("7777 에서 듣는다\n");
    loop.run(endpoint);

    return 0;
}

// =============================================================================
// §7. 다른 두 파일과 다른 곳
// -----------------------------------------------------------------------------
//                       IOCP              io_uring            epoll
//  모델                 완료              완료                준비
//  버퍼 주인            커널에 맡김       커널에 맡김         내가 들고 있다
//  작업 거는 법         WSARecv 호출      SQ 에 채우고 제출   관심 등록만
//  몇 byte 인지         완료가 들고 온다  cqe->res            recv() 반환값
//  실패 알기            (Ex 는 안 준다)   cqe->res 가 음수    errno
//  여러 완료 한 번에    GQCSEx            for_each_cqe        epoll_wait
//  다른 스레드 제출     된다              안 된다             epoll_ctl 로 우회
//
// 1) 꼬리표가 하나뿐이다
//      IOCP 는 완료마다 key(누구) + OVERLAPPED(무슨 작업) 둘을 준다.
//      io_uring 은 user_data 하나뿐이라, 꼬리표가 둘을 다 들어야 한다.
//      그래서 이 파일의 IoRequest 에는 owner 가 붙어 있다.
//
// 2) 제출이 스레드 안전하지 않다  <- 이 파일에서 가장 중요한 제약
//      IOCP  : 워커 스레드가 WSASend 를 그 자리에서 발행한다. 스레드 홉 0
//      epoll : 워커가 epoll_ctl 로 EPOLLOUT 만 걸면 되고, 그 호출이 루프를 깨운다
//      io_uring : 둘 다 안 된다. SQ 는 루프 스레드의 것이다.
//                 그래서 _pending_sends 에 넣고 eventfd 로 깨운다 (E4 -> E5)
//
//      실제 엔진에서 "IO 스레드마다 ring 하나 + 세션을 ring 에 고정"하는 이유가
//      이것이다. 그러면 제출이 ring 당 단일 스레드가 되어 이 제약이 문제가 되지
//      않는다.
//
// 3) syscall 이 가장 적다
//      SQ 도 CQ 도 커널과 공유하는 메모리다. 완료를 훑는 for_each_cqe 에는
//      syscall 이 아예 없다. 제출도 여러 개를 모아 한 번에 낼 수 있다.
//      (이 파일은 순서를 보이려고 매번 제출한다)
//      더 줄이려면 IORING_SETUP_SQPOLL 로 커널 스레드가 SQ 를 알아서 읽게 할 수
//      있지만, 8명 규모에서는 켤 이유가 없다 - 그 커널 스레드가 CPU 를 계속 쓴다.
//
// -----------------------------------------------------------------------------
// 이 파일이 실제 엔진과 다른 점 (일부러 줄인 것들)
//
//   - 헤더가 4 byte 길이뿐이다. 실제는 24 byte 고정 헤더
//   - 필요한 만큼만 정확히 recv 한다. 실제는 한 번에 더 받아 두고 버퍼에서 자른다
//   - 제출을 매번 한다. 실제는 여러 개를 모아 한 번에 낸다
//   - 커넥션을 지우지 않는다. 실제는 뜬 작업이 0 이 될 때까지 기다렸다 지운다
//   - 세션도 Usecase 도 없다. handleRequest() 자리가 그것들이 들어갈 곳이다
// =============================================================================
