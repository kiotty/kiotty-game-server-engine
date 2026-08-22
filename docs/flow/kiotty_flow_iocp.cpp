// =============================================================================
// kiotty_flow_iocp.cpp
// -----------------------------------------------------------------------------
// IOCP 방식으로 "요청/응답"과 "이벤트"가 어떤 순서로 도는지만 보여주는 파일.
// 엔진 코드가 아니라 흐름을 읽기 위한 문서다. 짝이 되는 파일은
// kiotty_flow_epoll.cpp 이며, 두 파일은 절(節) 번호와 클래스 이름이 같다.
// 나란히 놓고 diff 하면 백엔드가 무엇을 다르게 하는지가 그대로 보인다.
//
// 빌드:
//   cl /std:c++14 /W4 /EHsc kiotty_flow_iocp.cpp ws2_32.lib
//
// 프로토콜(설명을 위해 최대한 줄였다):
//   [길이 4 byte (little endian)][payload]
//
// -----------------------------------------------------------------------------
// 읽는 순서
//
//   §1  Connection  - socket + recv buffer + send buffer. 그게 전부다
//   §2  Endpoint    - 듣는 소켓
//   §3  IoLoop      - IOCP 포트 하나 + 스레드 하나
//   §4  요청/응답 흐름  F1 ~ F8
//   §5  이벤트 흐름     E1 ~ E5
//   §6  main
//   §7  epoll 파일과 다른 곳
//
// 흐름 표시:
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
//   E4 IOCP 는 그 자리에서 WSASend 를 발행한다  <-- epoll 과 갈리는 곳
//   E5 send 완료 -> F8 과 같은 자리로 합류한다
// =============================================================================

// GetQueuedCompletionStatusEx 와 OVERLAPPED_ENTRY 는 Vista 부터다.
#ifndef _WIN32_WINNT
#  define _WIN32_WINNT 0x0600
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>            // AcceptEx 의 GUID (WSAID_ACCEPTEX)

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

static const size_t kLengthBytes = 4;
static const size_t kMaxPayload  = 64 * 1024;

// 한 번의 GetQueuedCompletionStatusEx 로 꺼낼 완료 개수의 상한.
// 클수록 syscall 이 줄지만, 그만큼 스택에 배열이 커진다.
static const ULONG  kMaxCompletionsPerCall = 64;

// =============================================================================
// §0. 완료를 되찾기 위한 꼬리표
// -----------------------------------------------------------------------------
// IOCP 는 완료를 알려줄 때 두 가지를 준다.
//   1) completion key  - 소켓을 포트에 등록할 때 우리가 넣어 둔 값. 여기서는
//                        Connection* 또는 Endpoint* 를 넣는다. "누구의 완료인가"
//   2) OVERLAPPED*     - 작업을 걸 때 우리가 넘긴 포인터. "무슨 작업이었나"
//
// 그래서 OVERLAPPED 를 구조체 맨 앞에 두고 뒤에 종류를 붙여 둔다. 완료가 오면
// OVERLAPPED* 를 그대로 IoRequest* 로 캐스팅해 종류를 되찾는다.
// =============================================================================

enum class OpKind
{
    Accept,
    Recv,
    Send,
};

struct IoRequest
{
    OVERLAPPED overlapped;      // 반드시 첫 멤버여야 한다
    OpKind     kind;

    void reset(OpKind k)
    {
        ZeroMemory(&overlapped, sizeof(overlapped));
        kind = k;
    }
};

// =============================================================================
// §1. Connection - socket + recv buffer + send buffer
// -----------------------------------------------------------------------------
// 커넥션이 아는 것은 이 셋뿐이다. Usecase 도, Codec 도, 세션도 모른다.
// "다음에 무엇을 받아야 하는가"(길이냐 payload 냐)는 recv buffer 의 상태다.
// =============================================================================

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
    //
    // 실전에서는 한 번에 더 많이 받아 두고 버퍼에서 잘라 쓰는 편이 빠르다.
    // 여기서는 "4 byte 받고 -> 길이만큼 받고" 순서를 눈으로 보려고 필요한
    // 만큼만 받는다.
    WSABUF spaceToFill()
    {
        WSABUF buffer;
        buffer.buf = reinterpret_cast<char*>(_data + _filled);
        buffer.len = static_cast<ULONG>(_want - _filled);
        return buffer;
    }

    // 커널이 채워 준 만큼 알린다. 이번 단계가 끝났으면 true.
    bool filled(size_t bytes)
    {
        _filled += bytes;
        return _filled == _want;
    }

    // F3 에서 부른다. 4 byte 를 길이로 읽고 payload 단계로 넘어간다.
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

    // F5 가 끝난 뒤. 다시 길이 4 byte 를 기다리는 상태로 되돌린다.
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

// 보낼 것들이 줄 서는 곳. 여러 스레드가 넣고, 보내는 것은 한 번에 하나다.
class SendBuffer
{
public:
    SendBuffer()
        : _sending(false)
        , _sent(0)
    {
    }

    // 아무 스레드나 부른다 (IO 스레드, 이벤트 워커 스레드 둘 다).
    void push(const std::vector<uint8_t>& packet)
    {
        std::lock_guard<std::mutex> guard(_lock);
        _queue.push_back(packet);
    }

    // "지금 내가 보내는 사람이 된다"를 한 번에 하나만 성공시킨다.
    // 이미 누가 보내는 중이면 false - 그 사람이 내 것까지 가져간다.
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

    // 지금 보낼 조각. 부분 전송이면 보낸 만큼 건너뛴 자리를 준다.
    WSABUF frontChunk()
    {
        std::lock_guard<std::mutex> guard(_lock);

        WSABUF buffer;
        buffer.buf = reinterpret_cast<char*>(_queue.front().data() + _sent);
        buffer.len = static_cast<ULONG>(_queue.front().size() - _sent);
        return buffer;
    }

    // 보낸 만큼 소비한다. 이번 항목을 다 보냈으면 true.
    bool consume(size_t bytes)
    {
        std::lock_guard<std::mutex> guard(_lock);
        _sent += bytes;

        if (_sent < _queue.front().size())
        {
            return false;       // 부분 전송. 같은 항목을 이어서 보낸다
        }
        _queue.pop_front();
        _sent = 0;
        return true;
    }

    // 보내는 사람 자리를 내려놓는다.
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

class IoLoop;   // 아래에서 정의한다

class Connection
{
public:
    Connection(SOCKET socket, IoLoop& loop)
        : _socket(socket)
        , _loop(loop)
    {
        _recv_io.reset(OpKind::Recv);
        _send_io.reset(OpKind::Send);
    }

    ~Connection()
    {
        if (_socket != INVALID_SOCKET)
        {
            ::closesocket(_socket);
        }
    }

    SOCKET      socket() const { return _socket; }
    RecvBuffer& recv()         { return _recv; }
    SendBuffer& send()         { return _send; }
    IoRequest&  recvIo()       { return _recv_io; }
    IoRequest&  sendIo()       { return _send_io; }

    // 소멸자보다 먼저 닫아야 할 때 쓴다. 두 번 닫지 않도록 표시를 남긴다.
    void close()
    {
        if (_socket != INVALID_SOCKET)
        {
            ::closesocket(_socket);
            _socket = INVALID_SOCKET;
        }
    }

private:
    SOCKET     _socket;
    IoLoop&    _loop;
    RecvBuffer _recv;
    SendBuffer _send;
    IoRequest  _recv_io;    // 커넥션당 수신 1개까지만 떠 있으므로 하나면 된다
    IoRequest  _send_io;    // 송신도 마찬가지
};

// =============================================================================
// §2. Endpoint - 듣는 소켓
// -----------------------------------------------------------------------------
// AcceptEx 는 "받을 소켓을 미리 만들어 두고" 거는 방식이다. 그래서 Endpoint 가
// 다음 손님이 앉을 빈 소켓을 하나 들고 있는다.
// =============================================================================

class Endpoint
{
public:
    Endpoint()
        : _listening(INVALID_SOCKET)
        , _pending(INVALID_SOCKET)
        , _accept_ex(0)
    {
        _accept_io.reset(OpKind::Accept);
    }

    bool open(uint16_t port)
    {
        _listening = ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP,
                                  0, 0, WSA_FLAG_OVERLAPPED);
        if (_listening == INVALID_SOCKET)
        {
            return false;
        }

        sockaddr_in address;
        ZeroMemory(&address, sizeof(address));
        address.sin_family      = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port        = ::htons(port);

        if (::bind(_listening, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
        {
            return false;
        }

        if (::listen(_listening, 64) != 0)
        {
            return false;
        }

        // AcceptEx 는 표준 함수가 아니라 확장 함수라 런타임에 주소를 물어봐야 한다.
        GUID  guid  = WSAID_ACCEPTEX;
        DWORD bytes = 0;

        if (::WSAIoctl(_listening, SIO_GET_EXTENSION_FUNCTION_POINTER,
                       &guid, sizeof(guid),
                       &_accept_ex, sizeof(_accept_ex),
                       &bytes, 0, 0) != 0)
        {
            return false;
        }
        return true;
    }

    ~Endpoint()
    {
        if (_pending != INVALID_SOCKET)   { ::closesocket(_pending); }
        if (_listening != INVALID_SOCKET) { ::closesocket(_listening); }
    }

    SOCKET      listening() const { return _listening; }
    SOCKET      pending() const   { return _pending; }
    IoRequest&  acceptIo()        { return _accept_io; }
    LPFN_ACCEPTEX acceptEx() const { return _accept_ex; }
    char*       addressBuffer()   { return _address_buffer; }

    void makePendingSocket()
    {
        _pending = ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP,
                                0, 0, WSA_FLAG_OVERLAPPED);
    }

    // F1 에서 부른다. 앉은 손님을 데려가고 자리를 비운다.
    SOCKET takeAccepted()
    {
        const SOCKET accepted = _pending;
        _pending = INVALID_SOCKET;
        return accepted;
    }

private:
    SOCKET        _listening;
    SOCKET        _pending;         // 다음 손님이 앉을 빈 소켓
    LPFN_ACCEPTEX _accept_ex;
    IoRequest     _accept_io;
    char          _address_buffer[(sizeof(sockaddr_in) + 16) * 2];  // AcceptEx 가 요구한다
};

// =============================================================================
// §3. IoLoop - IOCP 포트 하나 + 스레드 하나
// -----------------------------------------------------------------------------
// 이 클래스가 하는 일은 셋이다.
//   1) 작업을 건다     submitAccept / submitRecv / submitSend
//   2) 완료를 받는다   run() 안의 GetQueuedCompletionStatus
//   3) 받은 완료를 §4, §5 의 흐름 함수로 넘긴다
// =============================================================================

class IoLoop
{
public:
    IoLoop()
        : _port(0)
        , _running(false)
    {
    }

    bool open()
    {
        _port = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, 0, 0, 0);
        return _port != 0;
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

        if (_port != 0)
        {
            ::CloseHandle(_port);
        }
    }

    // 소켓을 포트에 붙인다. key 가 곧 "누구의 완료인가"가 된다.
    bool attach(SOCKET socket, void* key)
    {
        return ::CreateIoCompletionPort(reinterpret_cast<HANDLE>(socket),
                                        _port,
                                        reinterpret_cast<ULONG_PTR>(key),
                                        0) != 0;
    }

    // ---------------------------------------------------------------------
    // 작업 걸기 - 세 개 모두 "걸어 두고 즉시 반환"이다. 여기서 기다리지 않는다.
    // ---------------------------------------------------------------------

    bool submitAccept(Endpoint& endpoint)
    {
        endpoint.makePendingSocket();
        endpoint.acceptIo().reset(OpKind::Accept);

        DWORD received = 0;

        // dwReceiveDataLength = 0 : 연결만 받고 데이터는 받지 않는다.
        // 0 이 아니면 첫 데이터가 올 때까지 완료가 안 떠서, 접속만 하고 조용한
        // 클라이언트가 accept 를 붙잡고 있게 된다.
        const BOOL ok = endpoint.acceptEx()(
            endpoint.listening(),
            endpoint.pending(),
            endpoint.addressBuffer(),
            0,
            sizeof(sockaddr_in) + 16,
            sizeof(sockaddr_in) + 16,
            &received,
            &endpoint.acceptIo().overlapped);

        return ok || ::WSAGetLastError() == ERROR_IO_PENDING;
    }

    bool submitRecv(Connection& connection)
    {
        connection.recvIo().reset(OpKind::Recv);

        WSABUF buffer = connection.recv().spaceToFill();
        DWORD  flags  = 0;

        const int result = ::WSARecv(connection.socket(), &buffer, 1,
                                     0, &flags,
                                     &connection.recvIo().overlapped, 0);

        return result == 0 || ::WSAGetLastError() == WSA_IO_PENDING;
    }

    // ★ 여기가 epoll 파일과 갈리는 첫 번째 지점 ★
    //
    // IOCP 에서는 "아무 스레드나" 이 함수를 부를 수 있다. 작업이 발행 시점에
    // 자기완결적이고, 소켓이 스레드가 아니라 포트에 등록되어 있기 때문이다.
    // 그래서 이벤트 워커 스레드가 E4 에서 이 함수를 그대로 부른다. 스레드 홉 0.
    bool submitSend(Connection& connection)
    {
        connection.sendIo().reset(OpKind::Send);

        WSABUF buffer = connection.send().frontChunk();

        const int result = ::WSASend(connection.socket(), &buffer, 1,
                                     0, 0,
                                     &connection.sendIo().overlapped, 0);

        return result == 0 || ::WSAGetLastError() == WSA_IO_PENDING;
    }

    // 큐에 넣은 다음 부른다. 보내는 사람이 아직 없으면 내가 된다.
    void requestSend(Connection& connection)
    {
        if (!connection.send().tryBeginSending())
        {
            return;             // 이미 누가 보내는 중이다
        }

        if (!submitSend(connection))
        {
            connection.send().endSending();
        }
    }

    void addConnection(Connection* connection)
    {
        std::lock_guard<std::mutex> guard(_lock);
        _connections.push_back(connection);
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

    // E3 에서 쓴다. 지금 붙어 있는 모두에게 같은 payload 를 흘린다.
    void broadcast(const std::vector<uint8_t>& packet);

    void run(Endpoint& endpoint);

    void stop()
    {
        _running = false;
        ::PostQueuedCompletionStatus(_port, 0, 0, 0);   // 루프를 깨운다
    }

private:
    HANDLE                    _port;
    bool                      _running;
    std::mutex                _lock;
    std::vector<Connection*>  _connections;
    std::vector<Connection*>  _closed;      // 끊겼지만 아직 지우지 않은 것
};

// =============================================================================
// §4. 요청/응답 흐름  F1 ~ F8
// =============================================================================

// F5 에서 부른다. 여기가 "작업"이다 - 엔진이라면 Codec -> Usecase 가 오는 자리.
static std::vector<uint8_t> handleRequest(const uint8_t* payload, size_t size)
{
    std::printf("[F5] payload %zu byte 를 받았다: %.*s\n",
                size, static_cast<int>(size), reinterpret_cast<const char*>(payload));

    // F6 : 응답을 만든다. [길이 4 byte][payload] 로 감싼다.
    const char*  body      = "pong";
    const size_t body_size = 4;

    std::vector<uint8_t> response(kLengthBytes + body_size);
    response[0] = static_cast<uint8_t>(body_size);
    response[1] = static_cast<uint8_t>(body_size >> 8);
    response[2] = static_cast<uint8_t>(body_size >> 16);
    response[3] = static_cast<uint8_t>(body_size >> 24);
    std::memcpy(&response[kLengthBytes], body, body_size);

    return response;
}

// -----------------------------------------------------------------------------
// F1 : accept 완료
// -----------------------------------------------------------------------------
static void onAccepted(IoLoop& loop, Endpoint& endpoint)
{
    const SOCKET accepted = endpoint.takeAccepted();

    // AcceptEx 로 받은 소켓은 이 한 줄을 해 줘야 보통 소켓처럼 동작한다.
    SOCKET listening = endpoint.listening();
    ::setsockopt(accepted, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                 reinterpret_cast<char*>(&listening), sizeof(listening));

    Connection* connection = new Connection(accepted, loop);

    loop.attach(accepted, connection);      // 이제 이 소켓의 완료는 connection 이 받는다
    loop.addConnection(connection);

    std::printf("[F1] 접속. Connection 을 만들었다\n");

    loop.submitRecv(*connection);           // F2 : 길이 4 byte 부터
    loop.submitAccept(endpoint);            // 다음 손님을 다시 받는다
}

// -----------------------------------------------------------------------------
// F3 / F5 : 수신 완료
// -----------------------------------------------------------------------------
static void onReceived(IoLoop& loop, Connection& connection, DWORD bytes)
{
    if (bytes == 0)
    {
        std::printf("[--] 상대가 끊었다\n");
        loop.closeConnection(connection);
        return;
    }

    if (!connection.recv().filled(bytes))
    {
        // 아직 덜 왔다. 같은 단계로 한 번 더 건다.
        loop.submitRecv(connection);
        return;
    }

    if (connection.recv().step() == RecvStep::Length)
    {
        // F3 : 4 byte 가 다 왔다. 길이를 꺼내고 payload 단계로 넘어간다.
        const uint32_t length = connection.recv().takeLengthAndExpectPayload();
        std::printf("[F3] 길이 = %u\n", length);

        loop.submitRecv(connection);         // F4 : 그 길이만큼 받는다
        return;
    }

    // F5 : payload 가 다 왔다.
    const std::vector<uint8_t> response =
        handleRequest(connection.recv().payload(), connection.recv().size());

    connection.recv().expectNextLength();    // 다음 요청을 기다릴 준비
    loop.submitRecv(connection);             // F2 로 돌아간다 (로직보다 먼저 건다)

    connection.send().push(response);        // F6
    loop.requestSend(connection);            // F7
}

// -----------------------------------------------------------------------------
// F8 : 송신 완료
// -----------------------------------------------------------------------------
static void onSent(IoLoop& loop, Connection& connection, DWORD bytes)
{
    // GetQueuedCompletionStatusEx 는 완료별 성공/실패를 따로 주지 않는다.
    // 보낼 것이 있었는데 0 byte 가 나갔다면 실패다.
    if (bytes == 0)
    {
        std::printf("[--] 전송 실패\n");
        connection.send().endSending();
        return;
    }

    if (!connection.send().consume(bytes))
    {
        // 부분 전송. 남은 만큼 이어서 보낸다. 보내는 사람 자리는 그대로 둔다.
        loop.submitSend(connection);
        return;
    }

    std::printf("[F8] 보냈다\n");

    connection.send().endSending();          // 먼저 자리를 내려놓고

    if (!connection.send().empty())
    {
        loop.requestSend(connection);        // 그 다음 큐를 한 번 더 본다
    }
    // 순서를 뒤집으면 - 큐를 먼저 보고 자리를 내려놓으면 - 그 사이에 들어온
    // 항목을 아무도 보내지 않는 상태로 남길 수 있다 (lost wakeup).
}

// -----------------------------------------------------------------------------
// 완료 하나를 알맞은 흐름 함수로 넘긴다.
//
// IOCP 는 완료마다 두 가지를 준다.
//   lpCompletionKey  - 누구의 완료인가 (소켓을 포트에 붙일 때 넣어 둔 값)
//   lpOverlapped     - 무슨 작업이었나 (작업을 걸 때 넘긴 IoRequest)
// -----------------------------------------------------------------------------
static void dispatchCompletion(IoLoop& loop, const OVERLAPPED_ENTRY& entry)
{
    if (entry.lpOverlapped == 0)
    {
        return;                              // stop() 이 던진 가짜 완료
    }

    IoRequest*  request = reinterpret_cast<IoRequest*>(entry.lpOverlapped);
    const DWORD bytes   = entry.dwNumberOfBytesTransferred;

    switch (request->kind)
    {
    case OpKind::Accept:
        onAccepted(loop, *reinterpret_cast<Endpoint*>(entry.lpCompletionKey));
        break;

    case OpKind::Recv:
        onReceived(loop, *reinterpret_cast<Connection*>(entry.lpCompletionKey), bytes);
        break;

    case OpKind::Send:
        onSent(loop, *reinterpret_cast<Connection*>(entry.lpCompletionKey), bytes);
        break;
    }
}

// -----------------------------------------------------------------------------
// 루프 본체. 백엔드 차이가 드러나는 곳은 이 while 안 한 덩이뿐이다.
// -----------------------------------------------------------------------------
void IoLoop::run(Endpoint& endpoint)
{
    _running = true;

    submitAccept(endpoint);                  // 첫 accept 를 건다

    OVERLAPPED_ENTRY entries[kMaxCompletionsPerCall];

    while (_running)
    {
        ULONG removed = 0;

        // ★ 완료가 여기로 온다. accept / recv / send 가 모두 같은 통로다 ★
        //
        // GetQueuedCompletionStatus 는 완료를 하나씩 꺼낸다. 완료 하나당
        // syscall 하나다. Ex 는 쌓여 있는 만큼을 한 번에 꺼내 온다 -
        // epoll_wait 가 준비된 fd 를 배열로 돌려주는 것과 같은 자리다.
        //
        // 대신 잃는 것이 하나 있다. 하나짜리 버전은 반환값(BOOL)으로
        // "이 작업이 실패했다"를 알려주는데, Ex 는 그것을 주지 않는다.
        // 정확한 실패 이유가 필요하면 WSAGetOverlappedResult() 를 부르거나
        // entry.Internal(NTSTATUS)을 본다. 여기서는 그럴 필요가 없다 -
        // recv 든 send 든 0 byte 면 닫는 것이 답이기 때문이다.
        const BOOL ok = ::GetQueuedCompletionStatusEx(
            _port, entries, kMaxCompletionsPerCall, &removed, INFINITE, FALSE);

        if (!ok)
        {
            break;                           // 포트가 닫혔다
        }

        for (ULONG i = 0; i < removed; ++i)
        {
            dispatchCompletion(*this, entries[i]);
        }
    }
}

// =============================================================================
// §5. 이벤트 흐름  E1 ~ E5
// -----------------------------------------------------------------------------
// 요청이 시작점이 아니다. 워커 스레드가 스스로 만들어 낸다.
// 나가는 길은 요청/응답과 완전히 같다 - send buffer 에 쌓고 보내기를 요청한다.
// =============================================================================

// E3 : 콜백이 하는 일. 큐에 넣고 요청하는 두 줄이 전부다.
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

        // ★ 여기가 epoll 파일과 갈리는 두 번째 지점 ★
        //
        // 이 함수는 워커 스레드에서 돈다. IOCP 는 아무 스레드나 발행할 수 있으므로
        // requestSend() 안에서 WSASend 가 그대로 나간다 (E4). 루프 스레드를
        // 깨울 필요가 없다.
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
    // 스레드가 돌기 시작한 뒤에 등록하면, 그 사이에 나온 이벤트가 갈 곳이 없다.
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
// 생성 순서가 곧 종속성 순서다. 아래에서 위로 만들고, 소멸은 그 역순으로 일어난다.
// =============================================================================

int main()
{
    // 출력을 버퍼링하지 않는다. 표준 출력이 콘솔이 아닐 때(파일로 넘기거나
    // 다른 셸을 거칠 때) 출력이 모여 있다가 한꺼번에 나오는 것을 막는다.
    std::setvbuf(stdout, 0, _IONBF, 0);

    WSADATA data;
    ::WSAStartup(MAKEWORD(2, 2), &data);

    IoLoop loop;

    if (!loop.open())
    {
        std::printf("IOCP 포트를 못 만들었다\n");
        return 1;
    }

    Endpoint endpoint;

    if (!endpoint.open(7777))
    {
        std::printf("listen 실패\n");
        return 1;
    }
    loop.attach(endpoint.listening(), &endpoint);

    EventWorker worker;
    worker.setSendCallback([&loop](const std::vector<uint8_t>& packet)   // E1
    {
        loop.broadcast(packet);
    });
    worker.start();

    std::printf("7777 에서 듣는다\n");
    loop.run(endpoint);                      // stop() 전까지 이 스레드가 루프다

    ::WSACleanup();
    return 0;
}

// =============================================================================
// §7. epoll 파일과 다른 곳
// -----------------------------------------------------------------------------
// kiotty_flow_epoll.cpp 와 비교하면 다른 곳은 아래 넷뿐이다.
// 나머지 - RecvBuffer, SendBuffer, Connection, onAccepted, onReceived, onSent,
// handleRequest, EventWorker - 는 두 파일에서 사실상 같은 코드다.
//
//   1) 작업을 거는 방법
//        IOCP  : AcceptEx / WSARecv / WSASend 로 커널에 버퍼를 맡긴다
//        epoll : 거는 것이 아니라 "관심"만 등록한다. 실제 읽기/쓰기는 통지가 온
//                뒤에 루프 스레드가 직접 한다
//
//   2) 완료를 받는 방법
//        IOCP  : GetQueuedCompletionStatus 가 "몇 byte 됐다"를 준다
//        epoll : epoll_wait 는 "읽을 수 있다"만 준다. 몇 byte 인지는 recv() 를
//                불러 봐야 안다
//
//   3) 다른 스레드에서 보내기  <- 실무에서 가장 자주 틀리는 곳
//        IOCP  : 아무 스레드나 WSASend 를 발행해도 된다. E4 가 그 자리에서 끝난다
//        epoll : 관심 집합은 루프 스레드의 것이라 아무나 건드리면 꼬인다.
//                워커는 큐에 넣고 eventfd 를 깨우고, 루프 스레드가 꺼내 보낸다
//
//   4) 부분 전송
//        IOCP  : 완료가 몇 byte 나갔는지 알려준다
//        epoll : send() 의 반환값이 그것이다. 남았으면 EPOLLOUT 을 걸어 둔다
//
// 즉 Connection 은 두 백엔드에서 한 글자도 다르지 않다. 추상화의 경계를
// "작업을 건다 / 완료를 받는다" 로 그으면 그렇게 된다.
// =============================================================================
