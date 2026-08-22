// =============================================================================
// kiotty_flow_epoll.cpp
// -----------------------------------------------------------------------------
// epoll 방식으로 "요청/응답"과 "이벤트"가 어떤 순서로 도는지만 보여주는 파일.
// 짝이 되는 파일은 kiotty_flow_iocp.cpp 이며, 절(節) 번호와 클래스 이름이 같다.
// 나란히 놓고 diff 하면 백엔드가 무엇을 다르게 하는지가 그대로 보인다.
//
// 빌드:
//   g++ -std=c++11 -Wall -Wextra -pthread kiotty_flow_epoll.cpp -o flow_epoll
//
// 프로토콜(설명을 위해 최대한 줄였다):
//   [길이 4 byte (little endian)][payload]
//
// -----------------------------------------------------------------------------
// 읽는 순서
//
//   §1  Connection  - socket + recv buffer + send buffer. 그게 전부다
//   §2  Endpoint    - 듣는 소켓
//   §3  IoLoop      - epoll 인스턴스 하나 + 스레드 하나
//   §4  요청/응답 흐름  F1 ~ F8
//   §5  이벤트 흐름     E1 ~ E5
//   §6  main
//   §7  IOCP 파일과 다른 곳
//
// 흐름 표시(IOCP 파일과 같은 번호다):
//   F1 accept -> Connection 생성
//   F2 길이 4 byte 수신 대기
//   F3 길이 수신 완료 -> 길이 추출
//   F4 payload 수신 대기
//   F5 payload 수신 완료 -> 처리
//   F6 응답 생성 -> send buffer 에 쌓기
//   F7 send 시도
//   F8 send 완료 -> 큐에 남은 것 처리, 그리고 F2 로 돌아간다
//
//   E1 워커 스레드를 만들기 "전에" 보내기 콜백을 등록한다
//   E2 워커가 일을 마치고 콜백을 부른다
//   E3 콜백이 payload 를 send buffer 에 쌓고 보내기를 요청한다
//   E4 epoll 은 워커 스레드에서 직접 보내지 못한다. epoll_ctl 로 EPOLLOUT 만 건다
//      <-- IOCP 와 갈리는 곳
//   E5 그 epoll_ctl 이 루프를 깨운다 -> 루프 스레드가 send -> F8 로 합류한다
// =============================================================================

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
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

static const size_t kLengthBytes = 4;
static const size_t kMaxPayload  = 64 * 1024;

// =============================================================================
// §0. 통지를 되찾기 위한 꼬리표
// -----------------------------------------------------------------------------
// epoll 은 통지를 줄 때 우리가 등록해 둔 epoll_data 하나만 돌려준다.
// IOCP 의 completion key 에 해당한다. "무슨 작업이었나"(IOCP 의 OVERLAPPED)는
// 주지 않는다 - epoll 은 작업을 거는 물건이 아니라 "읽을 수 있다 / 쓸 수 있다"만
// 알려주기 때문이다.
//
// 그래서 등록하는 것마다 앞에 종류 꼬리표를 붙여 두고, 통지가 오면 그것을 보고
// 어디로 갈지 정한다.
// =============================================================================

enum class WatchKind
{
    Listen,         // 듣는 소켓
    Connection,     // 커넥션
    Wakeup,         // eventfd - fd 의 준비 상태로 표현할 수 없는 일 (지금은 stop 뿐)
};

struct Watch
{
    WatchKind kind;     // 반드시 첫 멤버여야 한다
};

// =============================================================================
// §1. Connection - socket + recv buffer + send buffer
// -----------------------------------------------------------------------------
// ★ 이 절은 IOCP 파일과 사실상 같은 코드다. 타입만 WSABUF -> ByteSpan 이다. ★
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
    // Watch 를 첫 멤버로 둔다. epoll 통지에서 이 포인터가 그대로 돌아온다.
    Connection(int fd, IoLoop& loop)
        : _fd(fd)
        , _loop(loop)
    {
        _watch.kind = WatchKind::Connection;
    }

    ~Connection()
    {
        if (_fd >= 0)
        {
            ::close(_fd);
        }
    }

    Watch*      watch()    { return &_watch; }
    int         fd() const { return _fd; }
    RecvBuffer& recv()     { return _recv; }
    SendBuffer& send()     { return _send; }

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
    Watch      _watch;      // 반드시 첫 멤버
    int        _fd;
    IoLoop&    _loop;
    RecvBuffer _recv;
    SendBuffer _send;
};

// =============================================================================
// §2. Endpoint - 듣는 소켓
// -----------------------------------------------------------------------------
// IOCP 와 달리 "미리 만들어 둔 빈 소켓"이 필요 없다. accept() 가 그 자리에서
// 새 fd 를 만들어 주기 때문이다.
// =============================================================================

class Endpoint
{
public:
    Endpoint()
        : _fd(-1)
    {
        _watch.kind = WatchKind::Listen;
    }

    bool open(uint16_t port)
    {
        _fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);

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

    Watch* watch()    { return &_watch; }
    int    fd() const { return _fd; }

private:
    Watch _watch;       // 반드시 첫 멤버
    int   _fd;
};

// =============================================================================
// §3. IoLoop - epoll 인스턴스 하나 + 스레드 하나
// -----------------------------------------------------------------------------
// IOCP 파일과 겉모습(submitAccept / submitRecv / submitSend)은 같지만, 속은
// 다르다. epoll 은 작업을 "거는" 물건이 아니라 "언제 할 수 있는지 알려주는"
// 물건이라서, 실제 recv/send 는 통지가 온 뒤 이 스레드가 직접 부른다.
// =============================================================================

class IoLoop
{
public:
    IoLoop()
        : _epoll(-1)
        , _wakeup(-1)
        , _running(false)
    {
        _wakeup_watch.kind = WatchKind::Wakeup;
    }

    bool open()
    {
        _epoll = ::epoll_create1(0);

        if (_epoll < 0)
        {
            return false;
        }

        // 다른 스레드가 루프를 깨우는 손잡이. IOCP 에는 대응물이 필요 없다.
        _wakeup = ::eventfd(0, EFD_NONBLOCK);

        if (_wakeup < 0)
        {
            return false;
        }
        return add(_wakeup, EPOLLIN, &_wakeup_watch);
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

        if (_wakeup >= 0) { ::close(_wakeup); }
        if (_epoll >= 0)  { ::close(_epoll); }
    }

    bool add(int fd, uint32_t events, void* watch)
    {
        epoll_event ev;
        std::memset(&ev, 0, sizeof(ev));
        ev.events   = events;
        ev.data.ptr = watch;
        return ::epoll_ctl(_epoll, EPOLL_CTL_ADD, fd, &ev) == 0;
    }

    bool modify(int fd, uint32_t events, void* watch)
    {
        epoll_event ev;
        std::memset(&ev, 0, sizeof(ev));
        ev.events   = events;
        ev.data.ptr = watch;
        return ::epoll_ctl(_epoll, EPOLL_CTL_MOD, fd, &ev) == 0;
    }

    // ---------------------------------------------------------------------
    // 작업 걸기 - 이름은 IOCP 와 같지만 하는 일이 다르다.
    // ---------------------------------------------------------------------

    // IOCP 는 여기서 AcceptEx 를 건다. epoll 은 관심만 유지한다.
    bool submitAccept(Endpoint& endpoint)
    {
        return modify(endpoint.fd(), EPOLLIN, endpoint.watch());
    }

    // IOCP 는 여기서 WSARecv 로 버퍼를 커널에 맡긴다.
    // epoll 은 맡길 곳이 없다. "읽을 수 있으면 알려 달라"고만 한다.
    bool submitRecv(Connection& connection)
    {
        return modify(connection.fd(), EPOLLIN, connection.watch());
    }

    // ★ 여기가 IOCP 파일과 갈리는 첫 번째 지점 ★
    //
    // 이 함수는 반드시 루프 스레드에서 불려야 한다. epoll 의 관심 집합은 루프가
    // 소유한 상태라, 다른 스레드가 건드리면 꼬인다.
    // 그래서 지금 그 자리에서 send() 를 시도하고, 다 못 보내면 EPOLLOUT 을 건다.
    // 보낼 수 있는 만큼 보낸다.
    //
    // ★ "보내는 사람 자리"(_sending)를 내려놓는 것도 이 함수의 책임이다. ★
    //   IOCP·io_uring 에서는 완료 핸들러 onSent() 가 그 일을 한다. epoll 에는
    //   완료라는 것이 없어서 - 여기서 그 자리에 보내 버리므로 - 내려놓을 곳이
    //   여기밖에 없다. 이것을 빠뜨리면 첫 응답 이후 _sending 이 true 로 굳어
    //   그 커넥션은 두 번 다시 아무것도 못 보낸다.
    bool submitSend(Connection& connection)
    {
        for (;;)
        {
            if (connection.send().empty())
            {
                // F8 : 다 보냈다. EPOLLOUT 은 내려놓는다 - 걸어 두면 보낼 것이
                // 없는데도 계속 깨어나 CPU 를 태운다.
                modify(connection.fd(), EPOLLIN, connection.watch());
                std::printf("[F8] 보냈다\n");

                connection.send().endSending();     // 먼저 자리를 내려놓고

                // 그 다음 큐를 한 번 더 본다. 내려놓기 직전에 다른 스레드가
                // 넣은 것이 있으면 그것을 아무도 안 보내는 상태가 되기 때문이다.
                if (connection.send().empty() || !connection.send().tryBeginSending())
                {
                    return true;
                }
                continue;                           // 자리를 다시 잡았다. 이어서 보낸다
            }

            ByteSpan chunk = connection.send().frontChunk();

            const ssize_t sent = ::send(connection.fd(), chunk.data, chunk.size, MSG_NOSIGNAL);

            if (sent < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    // 커널 송신 버퍼가 찼다. 비면 알려 달라고 걸어 두고,
                    // 보내는 사람 자리는 든 채로 물러난다.
                    return modify(connection.fd(), EPOLLIN | EPOLLOUT, connection.watch());
                }
                connection.send().endSending();
                return false;
            }

            connection.send().consume(static_cast<size_t>(sent));
        }
    }

    // 끊긴 커넥션을 epoll 에서 뺀다.
    //
    // ★ 이것을 빠뜨리면 무한 루프가 된다. ★
    //   닫힌 소켓은 "읽을 수 있는" 상태로 영원히 남고, epoll 은 level-trigger 라
    //   epoll_wait 가 그 fd 를 끝없이 돌려준다. recv() 는 계속 0 을 주고.
    //   IOCP·io_uring 에는 이 문제가 없다 - 작업을 다시 걸지 않으면 완료도
    //   다시 오지 않기 때문이다. epoll 만 "관심을 명시적으로 내려놔야" 한다.
    void closeConnection(Connection& connection)
    {
        ::epoll_ctl(_epoll, EPOLL_CTL_DEL, connection.fd(), 0);
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

    // 큐에 넣은 다음 부른다.
    // ★ 부르는 스레드가 루프 스레드인지에 따라 갈린다. IOCP 에는 없는 분기다. ★
    void requestSend(Connection& connection)
    {
        if (!connection.send().tryBeginSending())
        {
            return;                          // 이미 누가 보내는 중이다
        }

        if (std::this_thread::get_id() == _loop_thread)
        {
            // 루프 스레드다. 그 자리에서 보낸다 - 왕복이 없으니 가장 짧다.
            // 보내는 사람 자리를 내려놓는 것은 submitSend() 가 한다.
            submitSend(connection);
            return;
        }

        // E4 : 다른 스레드다. send() 를 직접 부르면 안 되므로 EPOLLOUT 만 건다.
        //
        // 이것으로 충분한 이유 두 가지 (측정으로 확인함, 커널 6.6):
        //   1) epoll_ctl 은 스레드 안전하다. epoll_wait 에 블록된 스레드가 있어도
        //      다른 스레드에서 관심 집합을 바꿀 수 있다.
        //   2) EPOLL_CTL_MOD 로 추가한 이벤트가 "이미 준비된" 상태이면, 커널이
        //      그 자리에서 ready 목록에 넣고 블록된 epoll_wait 를 깨운다.
        //      소켓 송신 버퍼는 보통 비어 있으므로 EPOLLOUT 은 거의 항상 준비 상태다.
        //
        // 즉 이 한 줄이 "넘기기"와 "깨우기"를 동시에 한다. 따로 깨우는 수단이
        // 필요 없다. 루프가 깨어나 onWritable() 로 들어가는 것이 E5 다.
        //
        // 여기서 mask 를 건드리는 것이 루프 스레드와 부딪히지 않는 이유는
        // _sending 플래그다. 그 자리를 잡은 스레드만 mask 를 만진다.
        modify(connection.fd(), EPOLLIN | EPOLLOUT, connection.watch());
    }

    // fd 의 준비 상태로 표현할 수 없는 일에만 쓴다.
    // 지금은 stop() 하나뿐이다 - "루프를 그만 돌아라"에 대응하는 소켓이 없기 때문.
    // 나중에 "워커가 커넥션을 닫아 달라고 한다", "타이머가 됐다" 같은 것이
    // 생기면 그것들도 이 손잡이를 쓴다. 보내기는 이제 여기 오지 않는다.
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

private:
    int                      _epoll;
    int                      _wakeup;         // stop() 전용 (위 wake() 주석 참고)
    Watch                    _wakeup_watch;
    bool                     _running;
    std::thread::id          _loop_thread;
    std::mutex               _lock;
    std::vector<Connection*> _connections;
    std::vector<Connection*> _closed;        // 끊겼지만 아직 지우지 않은 것
};

// =============================================================================
// §4. 요청/응답 흐름  F1 ~ F8
// -----------------------------------------------------------------------------
// ★ 이 절의 함수들은 IOCP 파일과 거의 같다. 다른 것은 "몇 byte 왔는지"를
//    완료가 알려주느냐, recv() 를 불러 봐야 아느냐뿐이다. ★
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
// F1 : accept
// -----------------------------------------------------------------------------
void onAccepted(IoLoop& loop, Endpoint& endpoint)
{
    for (;;)
    {
        const int accepted = ::accept4(endpoint.fd(), 0, 0, SOCK_NONBLOCK);

        if (accepted < 0)
        {
            return;                          // EAGAIN - 더 없다
        }

        Connection* connection = new Connection(accepted, loop);

        loop.add(accepted, EPOLLIN, connection->watch());
        loop.addConnection(connection);

        std::printf("[F1] 접속. Connection 을 만들었다\n");
        // F2 : EPOLLIN 을 걸어 둔 것이 곧 "길이 4 byte 를 기다린다"이다
    }
}

// -----------------------------------------------------------------------------
// F3 / F5 : 읽을 수 있다는 통지가 왔다
// -----------------------------------------------------------------------------
void onReadable(IoLoop& loop, Connection& connection)
{
    for (;;)
    {
        ByteSpan space = connection.recv().spaceToFill();

        // ★ IOCP 와 갈리는 두 번째 지점 ★
        // IOCP 는 완료가 "몇 byte 됐다"를 들고 온다. epoll 은 여기서 직접
        // 읽어 봐야 안다. 이 한 줄이 두 모델의 차이 전부다.
        const ssize_t received = ::recv(connection.fd(), space.data, space.size, 0);

        if (received < 0)
        {
            return;                          // EAGAIN - 지금은 여기까지
        }

        if (received == 0)
        {
            std::printf("[--] 상대가 끊었다\n");
            loop.closeConnection(connection);   // 안 빼면 무한히 깨어난다
            return;
        }

        if (!connection.recv().filled(static_cast<size_t>(received)))
        {
            continue;                        // 아직 덜 왔다
        }

        if (connection.recv().step() == RecvStep::Length)
        {
            // F3 : 4 byte 가 다 왔다.
            const uint32_t length = connection.recv().takeLengthAndExpectPayload();
            std::printf("[F3] 길이 = %u\n", length);
            continue;                        // F4 : 그 길이만큼 이어서 받는다
        }

        // F5 : payload 가 다 왔다.
        const std::vector<uint8_t> response =
            handleRequest(connection.recv().payload(), connection.recv().size());

        connection.recv().expectNextLength();    // F2 로 돌아갈 준비

        connection.send().push(response);        // F6
        loop.requestSend(connection);            // F7
    }
}

// -----------------------------------------------------------------------------
// F8 : 쓸 수 있다는 통지가 왔다 (부분 전송으로 EPOLLOUT 을 걸어 둔 경우)
// -----------------------------------------------------------------------------
void onWritable(IoLoop& loop, Connection& connection)
{
    // 이어서 보내는 것도, 다 보낸 뒤 자리를 내려놓는 것도 submitSend() 안에 있다.
    loop.submitSend(connection);
}

// -----------------------------------------------------------------------------
// 루프 본체.
// -----------------------------------------------------------------------------
void IoLoop::run(Endpoint& endpoint)
{
    _running     = true;
    _loop_thread = std::this_thread::get_id();

    submitAccept(endpoint);

    epoll_event events[64];

    while (_running)
    {
        // ★ 완료가 아니라 "준비됨"이 온다. accept / recv / send 가 같은 통로다 ★
        const int count = ::epoll_wait(_epoll, events, 64, -1);

        for (int i = 0; i < count; ++i)
        {
            Watch* watch = reinterpret_cast<Watch*>(events[i].data.ptr);

            switch (watch->kind)
            {
            case WatchKind::Listen:
                onAccepted(*this, *reinterpret_cast<Endpoint*>(watch));
                break;

            case WatchKind::Connection:
            {
                Connection& connection = *reinterpret_cast<Connection*>(watch);

                if (events[i].events & EPOLLIN)
                {
                    onReadable(*this, connection);
                }

                if (events[i].events & EPOLLOUT)
                {
                    onWritable(*this, connection);
                }
                break;
            }

            case WatchKind::Wakeup:
            {
                // stop() 이 깨운 것이다. 값을 비워 두지 않으면 계속 깨어난다.
                uint64_t value = 0;
                ssize_t  read_bytes = ::read(_wakeup, &value, sizeof(value));
                (void)read_bytes;
                break;
            }
            }
        }
    }
}

// =============================================================================
// §5. 이벤트 흐름  E1 ~ E5
// =============================================================================

// E3 : 콜백이 하는 일. IOCP 파일과 코드가 같다 - 큐에 넣고 요청한다.
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
        // E4 (epoll_ctl 로 EPOLLOUT 걸기) 로 간다.
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
                _callback(packet);           // E3 로 간다
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
// ★ IOCP 파일과 같은 코드다. WSAStartup 만 없다. ★
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
        std::printf("epoll 을 못 만들었다\n");
        return 1;
    }

    Endpoint endpoint;

    if (!endpoint.open(7777))
    {
        std::printf("listen 실패\n");
        return 1;
    }
    loop.add(endpoint.fd(), EPOLLIN, endpoint.watch());

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
// §7. IOCP 파일과 다른 곳
// -----------------------------------------------------------------------------
// kiotty_flow_iocp.cpp 와 비교하면 다른 곳은 아래 넷뿐이다.
// 나머지 - RecvBuffer, SendBuffer, Connection, handleRequest, EventWorker,
// main - 은 두 파일에서 사실상 같은 코드다.
//
//   1) 작업을 거는 방법
//        IOCP  : AcceptEx / WSARecv / WSASend 로 커널에 버퍼를 맡긴다
//        epoll : 맡길 곳이 없다. submitRecv() 는 EPOLLIN 을 거는 것이 전부이고,
//                실제 recv() 는 통지가 온 뒤 onReadable() 안에서 부른다
//
//   2) 몇 byte 인지 아는 방법
//        IOCP  : 완료가 들고 온다
//        epoll : recv() / send() 의 반환값이 그것이다
//
//   3) 다른 스레드에서 보내기  <- 실무에서 가장 자주 틀리는 곳
//        IOCP  : 아무 스레드나 WSASend 를 발행해도 된다. E4 가 그 자리에서 끝난다
//        epoll : send() 는 루프 스레드만 부른다. 다른 스레드는 epoll_ctl 로
//                EPOLLOUT 만 걸고 끝낸다 (E4). 그 epoll_ctl 이 루프를 깨우므로
//                따로 깨우는 수단이 필요 없다 - 소켓은 거의 항상 쓰기 준비
//                상태라 커널이 즉시 ready 목록에 넣고 epoll_wait 를 깨운다.
//                대가는 왕복 한 번(EPOLLOUT 걸기 -> 깨어나기 -> 보내고 내려놓기)
//                이다. IOCP 는 그 왕복이 없다
//
//   4) 부분 전송
//        IOCP  : 완료가 몇 byte 나갔는지 알려준다. 남으면 그냥 다시 WSASend
//        epoll : send() 가 EAGAIN 을 주면 EPOLLOUT 을 걸어 두고, 통지가 오면
//                onWritable() 에서 이어 보낸다. 다 보내면 EPOLLOUT 을 내려놓아야
//                한다 - 걸어 둔 채로 두면 보낼 것이 없는데 계속 깨어난다
//
// 즉 Connection 은 두 백엔드에서 한 글자도 다르지 않다. 추상화의 경계를
// "작업을 건다 / 통지를 받는다" 로 그으면 그렇게 된다.
//
// -----------------------------------------------------------------------------
// 이 두 파일이 실제 엔진과 다른 점 (일부러 줄인 것들)
//
//   - 헤더가 4 byte 길이뿐이다. 실제는 24 byte 고정 헤더
//   - 필요한 만큼만 정확히 recv 한다. 실제는 한 번에 더 받아 두고 버퍼에서 자른다
//     (그래서 실제 ReceiveBuffer 에는 "파싱 구간 / 커널이 쓰는 구간" 이 따로 있다)
//   - 커넥션을 지우지 않는다. 실제는 pending io 가 0 이 될 때까지 기다렸다 지운다
//   - 세션도 Usecase 도 없다. handleRequest() 자리가 그것들이 들어갈 곳이다
//   - std::function 과 std::vector 로 매번 할당한다. 실제는 고정 버퍼와 풀을 쓴다
// =============================================================================
