# 리팩토링 계획 — Stream · GameChannel · ChannelBinder

[code_refactoring.md](code_refactoring.md) 의 요구를 **지금 코드에 맞춰 실행 가능한 순서**로
푼 문서다. 목표는 하나다 — presentation(`Connection`)과 domain(usecase·repository)이
서로를 모른 채 **`GameChannel` 위의 스트림으로만** 데이터를 주고받게 만든다.
`Request` 구조체 안에 `Sink<Response>*` 를 실어 보내는 지금 방식은 사라진다.

각 단계는 **빌드 워닝 0 · 기존 scratch 하네스 통과**를 유지한 채 끝나야 다음 단계로 간다.

## 1. 설계 원칙과 초안에서 달라진 점

nullable 을 만들지 않는다 — 실패할 수 있는 호출은 `Result` 로 돌려주고, 성공한 값은
**참조**다. 호출자가 `nullptr` 검사를 떠안는 API 는 하나도 두지 않는다.

| # | 초안 | 결정 | 이유 |
| --- | --- | --- | --- |
| 1 | `IMutableStream<T>` 인터페이스 + `MutableStream<T>` | `IMutableStream` 은 만들지 않는다. `MutableStream<T>` 가 `ISink<T>` 와 `IStream<T>` 를 직접 상속 | 구현이 하나뿐인 인터페이스는 비용만 든다. 두 번째 구현이 생기면 그때 뽑는다 |
| 2 | `emit(void* user_context, const T&)` / `addOnStream(OnData<T>)` | `emit(const T&)` / `addListener(StreamListener<T>&)` 로 통일 | `ISink::emit` 시그니처와 맞아야 `MutableStream` 이 `ISink` 가 된다 |
| 3 | `GameChannel& find(size_t)` | 실패할 수 있는 호출은 `Result<ChannelCode, GameChannel&>` | 끊긴 채널을 찾는 일은 정상 경로다. 실패는 `Result`, 성공은 참조. `Result` 에 참조 특수화를 추가한다 (§2.3) |
| 4 | `IoGameChannel` 의 `ISink&` / `IStream&` 멤버 | **그대로 참조.** `Connection` 이 생성자 인자로 완성된 채널을 받는다 | 채널 획득을 `Connection` 생성 **앞**으로 빼면 참조 멤버가 된다 (§5.2) |
| 5 | `void* onDisconnected(...)` | `void onDisconnected(...)` | 오타로 본다 |
| 6 | `ConnectionHandler` | `IChannelBinder` | "연결될 때 채널을 빌려주고 끊길 때 돌려받는다"가 역할 전부다 |
| 7 | `size_t channel_id` | `struct ChannelId { uint32_t index; uint32_t generation; }` | 회수된 슬롯이 재사용돼도 옛 id 로 새 채널을 잡지 못하게 한다 |
| 8 | `Connection::onPacket` 순수 가상 | **삭제.** codec 을 주입받아 `Connection` 이 직접 `GameRequest` 로 바꾼다 | `Connection` 을 상속할 이유가 없어진다. `ConnectionTable<T>` 템플릿도 비템플릿이 된다 (§5) |

**"Event 반납을 reference_count 로?"** 는 **필요 없다**는 것이 결론이다. §4.3 에서 푼다.
**"SendBuffer 가 필요한가?"** 는 §5.6 에서 답한다 — 큐는 필요하고, "보낸 뒤 삭제"는 이미
그렇게 돌고 있다.

## 2. Core — `kiotty_stream.h`

푸시 기반 스트림 한 벌을 만든다. `ISink<T>` 에 넣으면 등록된 `StreamListener<T>` 전원에게
**같은 호출 스택 안에서** 전달되고, 호출이 끝나면 아이템에 대한 스트림의 관심은 끝난다.
힙 할당은 0 이다.

### 2.1 인터페이스

```cpp
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
public:
    ISink<T>&   sink()   { return *this; }
    IStream<T>& stream() { return *this; }

    bool emit(const T& item) override;                        // false: 리스너 0 명
    bool addListener(StreamListener<T>& listener) override;   // false: 가득 참
    void removeListener(StreamListener<T>& listener) override;
    void clear() override;

private:
    StreamListener<T>* _listeners[MaxListeners];
    size_t             _count = 0;
};
```

**아이템은 `const T&` 다.** 리스너가 여럿이면 누구도 아이템을 가져갈(move) 수 없다.
`GameResponse::payload` 가 move-only `Bytes` 이므로 이 결정이 곧 §4.3 의 소유권 규칙이다.

`removeListener` 는 초안에 없지만 넣는다 — 리스너(`Connection`)가 채널보다 먼저 죽는
경로가 있어 떼는 방법이 있어야 한다 (§5.3).

### 2.2 콜백 저장 — `std::function` 대신 무엇을

| 방식 | 할당 | C++11 | 판단 |
| --- | --- | --- | --- |
| `std::function<void(const T&)>` | 캡처 크기에 따라 힙 | ○ | 제외 — 요구사항 위반 |
| 함수 포인터 + `void* user_context` | 0 | ○ | 동작하지만 C 스타일. 캐스팅이 호출부마다 생긴다 |
| **`StreamListener<T>&` 가상 인터페이스 + 고정 배열** | 0 | ○ | **채택.** `IOContext`·`IUsecase` 와 같은 모양이라 새로 배울 것이 없다 |
| `FunctionRef<void(const T&)>` (비소유 type-erasure, C++26 `function_ref` 의 C++11 구현) | 0 | ○ | 가장 modern. 다만 리스너는 채널 수명 내내 등록되므로 어차피 이름 있는 객체가 필요하다 — 얻는 것이 없다 |

리스너 수명은 **등록한 쪽이 책임진다** (`Holder`·`IConnectionTable&` 와 같은 비소유 규약).

### 2.3 `Result<E, T&>` — 참조 특수화

`Result<E, V>` 는 `V` 를 union 안에 두므로 참조를 담을 수 없고, `E` 는 `bool` 이 막혀 있다.
**참조용 부분 특수화**를 `kiotty_result.h` 에 추가한다 — 내부는 `T*`, 바깥은 `T&` 만 보인다.
union 이 필요 없어 본체보다 짧다.

```cpp
template<typename ErrorCode, typename Value>
class Result<ErrorCode, Value&>
{
public:
    struct Success {};
    struct Fail {};

    Result(const Success&, Value& value) : _code(static_cast<ErrorCode>(0)), _value(&value) {}
    Result(const Fail&, ErrorCode error_code) : _code(error_code), _value(nullptr) {}

    bool      isOk() const { return _code == static_cast<ErrorCode>(0); }
    ErrorCode code() const { return _code; }

    Value& value() const
    {
        assert(_value != nullptr && "Result<E, T&>::value() on a failed result");
        return *_value;
    }

private:
    ErrorCode _code;
    Value*    _value;
};
```

복사·대입은 컴파일러 기본(포인터 하나)으로 충분하다. `ok()` 는 `std::decay` 로 참조를
벗기므로 **참조용 `okRef(value)` 헬퍼**를 하나 더 둔다 — `ok(channel)` 이 `GameChannel`
을 복사하려 드는 사고를 막는다. `error(code)` 는 그대로 쓴다.

```cpp
enum class ChannelCode : int32_t
{
    SUCCESS = 0,
    POOL_EXHAUSTED,
    NOT_FOUND,
    STALE,          // index 는 맞지만 generation 이 다르다
};

typedef Result<ChannelCode, GameChannel&> ChannelResult;

ChannelResult created = pool.create();

if (!created.isOk())
{
    return;
}
GameChannel& channel = created.value();
```

이 특수화는 §7 의 `ConnectionTable::open` 정리(`Result<SocketCode, Connection&>`)에도
그대로 쓰인다.

### 2.4 기존 `Sink` / `Stream` 의 처리

`Sink<T>::emit(T&)` 와 pull 형 `Stream<T>::next(T&)` 는 **삭제**한다. 사용처는 셋이고
전부 §4·§5 에서 대체된다.

| 사용처 | 대체 |
| --- | --- |
| `Request::replies` / `Request::events` (`Sink*`) | 필드 제거 → `BusinessGameChannel` 의 sink |
| `UsecaseDispatcher : Sink<Request>` | `StreamListener<GameRequest>` (§4.4) |
| `ResponseSink` / `EventSink` (`kiotty_connection_sinks.h`) | `Connection` 내부 리스너 (§5.4), 파일 삭제 |

## 3. Domain — entity 와 channel

presentation 과 business 사이에 놓이는 **데이터(entity)** 와 **통로(channel)** 를 만든다.
채널 하나는 커넥션 하나와 1:1 이고, 같은 채널을 두 쪽이 **서로 반대 방향의 뷰**로 본다.
domain 은 presentation 의 메모리를 가리키지 않는다 — `ConnectionInfo` 는 값 복사다.

### 3.1 파일

```
domain/entity/kiotty_channel_id.h          ChannelId, ChannelCode
domain/entity/kiotty_game_request.h        GameRequest
domain/entity/kiotty_game_response.h       GameResponse
domain/entity/kiotty_game_event.h          GameEvent
domain/entity/kiotty_connection_info.h     ConnectionInfo
domain/channel/kiotty_game_channel.h       GameChannel, IoGameChannel, BusinessGameChannel
domain/channel/kiotty_game_channel_pool.h  GameChannelPool  (+ .cpp, KIOTTY_SOURCES 등록)
domain/channel/kiotty_channel_binder.h     IChannelBinder, ChannelPoolBinder
domain/codec/kiotty_packet_codec.h         IPacketCodec, DefaultPacketCodec
```

`kiotty_usecase.h` 의 `Request`·`Response`·`Event` 는 entity 파일로 **옮기고 이름을 바꾼다.**
`kiotty_usecase.h` 는 `IUsecase` 만 남긴다.

### 3.2 entity

```cpp
struct ChannelId
{
    uint32_t index      {0};
    uint32_t generation {0};
};

struct ConnectionInfo
{
    char     ip[16];        // IPv4 dotted-decimal, NUL 종료. 값 복사 — presentation 수명과 무관
    uint16_t port {0};
};

struct GameRequest
{
    uint64_t  state_sequence {0};
    ChannelId channel_id;
    uint32_t  correlation_id {0};
    uint16_t  command {0};
    bool      authenticated {false};
    Bytes     payload;
};

struct GameResponse
{
    uint32_t correlation_id {0};
    uint16_t command {0};
    Bytes    payload;
};

struct GameEvent
{
    uint16_t command {0};
    Bytes    payload;
};
```

`GameRequest` 에서 `Sink*` 두 개가 빠지고 **`channel_id` 가 들어간다.** usecase 가 응답할
채널을 이것으로 찾는다 (§4.4).

### 3.3 channel

```cpp
struct IoGameChannel
{
    ChannelId              channel_id;
    ISink<GameRequest>&    request;
    IStream<GameResponse>& response;
    IStream<GameEvent>&    event;
};

struct BusinessGameChannel
{
    ChannelId              channel_id;
    IStream<GameRequest>&  request;
    ISink<GameResponse>&   response;
    ISink<GameEvent>&      event;
};

class GameChannel
{
public:
    explicit GameChannel(ChannelId channel_id);

    ChannelId           id() const { return _channel_id; }
    IoGameChannel       io();
    BusinessGameChannel business();

private:
    ChannelId                   _channel_id;
    MutableStream<GameRequest>  _request;
    MutableStream<GameResponse> _response;
    MutableStream<GameEvent>    _event;
};
```

두 뷰는 **참조 멤버뿐인 값 타입**이다 — 복사 생성은 되고 대입은 안 된다. 그래서 이 뷰를
멤버로 갖는 쪽(`Connection`)은 **생성자 초기화 목록에서** 받아야 하고, §5.2 가 그렇게 한다.
`GameChannel` 이 살아 있는 동안만 유효하다.

### 3.4 `GameChannelPool`

고정 용량, 슬롯 인덱스가 `ChannelId::index`, 슬롯마다 세대 카운터. `remove` 가 세대를
올리므로 옛 id 로는 `STALE` 이 돌아온다.

```cpp
class GameChannelPool
{
public:
    explicit GameChannelPool(size_t capacity);

    ChannelResult create();                  // POOL_EXHAUSTED
    void          remove(ChannelId id);      // NOT_FOUND / STALE 이면 아무 일도 하지 않는다
    ChannelAccess access(ChannelId id);      // NOT_FOUND / STALE

    size_t size() const;
    size_t capacity() const;

private:
    struct Slot
    {
        std::aligned_storage<sizeof(GameChannel), alignof(GameChannel)>::type bytes;
        uint32_t generation {0};
        bool     live {false};
    };

    std::recursive_mutex _lock;
    ...
};
```

**락.** `create`·`remove`·`access` 는 모두 `_lock` 안에서 돈다. `remove` 가 락을 지나야 워커의 `access` 스코프 중에 채널이 파괴되지 않고, `create` 가 락을 지나야 워커가 읽는 `live`·`generation` 이 찢어지지 않는다. 찾기만 하는 `find` 는 두지 않는다 — 돌려준 참조를
락 밖에서 쓰면 그 사이 `remove` 가 들어올 수 있다 — 워커 스레드가 생기는 순간 터지는
문제다. 그래서 **접근을 RAII 로 묶는다.**

```cpp
class ChannelAccess
{
public:
    explicit operator bool() const { return _channel != nullptr; }
    ChannelCode  code() const      { return _code; }
    GameChannel& channel()         { return *_channel; }     // precondition: bool

private:
    std::unique_lock<std::recursive_mutex> _guard;   // 풀의 _lock 을 잡은 채 산다
    GameChannel*                 _channel;
    ChannelCode                  _code;
};

ChannelAccess access(ChannelId id);   // GameChannelPool 의 공개 API
```

```cpp
ChannelAccess access = pool.access(session.channel_id);

if (access)
{
    access.channel().business().event.emit(event);   // 이 블록 동안 remove 가 막힌다
}
```

`ChannelAccess` 안의 포인터는 외부에 나가지 않는 구현 세부이고, 사용자는 `bool` 검사 한 번
뒤 참조만 쓴다 — §1 의 원칙과 어긋나지 않는다. 락은 풀 하나에 하나(coarse) 다. 워커가
늘어 경합이 측정되면 슬롯 단위로 내린다. 지금은 IO 루프 스레드 혼자 쓰므로 **비경합
뮤텍스 비용(수십 ns)** 만 낸다.

`remove` 는 `IChannelBinder::onDisconnected` 가 부른다 — 같은 락을 지나므로 워커의
`emit` 도중에 채널이 사라지지 않는다. 워커는 **채널 접근을 작업의 가장 마지막에** 두어
락을 쥔 시간을 줄인다.

### 3.5 `IChannelBinder`

```cpp
class IChannelBinder
{
public:
    virtual ~IChannelBinder() {}

    virtual Result<ChannelCode, IoGameChannel> onConnected(const ConnectionInfo& info) = 0;
    virtual void onDisconnected(const ConnectionInfo& info, const IoGameChannel& channel) = 0;
};
```

`Result<…, IoGameChannel>` 은 값 타입이라 문제없다 (참조 멤버를 가진 구조체를 union 에
넣는 것은 허용된다 — 대입만 안 될 뿐이고 `Result` 는 구성·파괴만 한다. **`Result` 의
`operator=` 가 이 타입에서 컴파일되지 않으니 `ChannelBinder` 결과를 대입하지 않는다.**)

기본 구현 `ChannelPoolBinder` 를 같이 둔다 — `GameChannelPool&` 과 `UsecaseDispatcher&`
를 들고, `onConnected` 에서 `create()` → `business().request.addListener(dispatcher)` →
`io()` 반환, `onDisconnected` 에서 `remove(channel.channel_id)`. 세션
repository 가 생기면 이것을 감싸거나 대체하는 구현을 domain 쪽이 만든다.

### 3.6 `IPacketCodec`

와이어 `ReceivedPacket` ↔ entity 변환은 사용자가 정의한다. `IChannelBinder` 와 함께
`ConnectionTable` 에 주입한다 (§5.1).

```cpp
class IPacketCodec
{
public:
    virtual ~IPacketCodec() {}

    virtual bool  decode(ReceivedPacket& packet, ChannelId channel_id, GameRequest& out) = 0;
    virtual Bytes encode(BlockPool& pool, const GameResponse& response) = 0;
    virtual Bytes encode(BlockPool& pool, const GameEvent& event, uint32_t sequence) = 0;
};
```

`decode` 가 `false` 면 `Connection` 은 패킷을 버린다(연결은 유지). `encode` 가 빈 `Bytes`
면 리스너가 송신을 건너뛴다 — 지금 `ResponseSink` 의 실패 경로와 같다.
`DefaultPacketCodec` 은 지금 `writePacket` 과 헤더 필드 복사를 그대로 옮긴 것이고, scratch
하네스와 테스트가 쓴다. `authenticated`·`state_sequence` 는 세션이 들어오기 전까지
codec 이 채운다(지금 하네스가 하는 방식).

## 4. 데이터 흐름과 소유권

스트림이 **동기 푸시**라는 사실 하나에서 소유권 규칙이 전부 나온다 — `emit` 이 반환되면
아이템은 더 이상 참조되지 않으므로, **만든 쪽이 그대로 들고 있다가 버리면 된다.**

### 4.1 요청 (presentation → business)

```
Connection::handleReceived → 패킷 완성
  → GameRequest request 를 스택에 만든다: _codec.decode(packet, _channel.channel_id, request)
  → _channel.request.emit(request)
      → UsecaseDispatcher::onStream(request)                 // 같은 스택
          → pool.access(request.channel_id) → IUsecase::execute(request, channel.business())
  → 반환, request 소멸
```

### 4.2 응답 (business → presentation)

```
usecase 안:
  GameResponse response { ... };                               // 스택
  channel.response.emit(response)
      → Connection::ResponseListener::onStream(response)      // 같은 스택
          → _codec.encode(pool, response) → 새 Bytes
          → Connection::emit(packet, DropPolicy::Never)
  emit 반환, response 소멸                                      // payload 반납
```

### 4.3 이벤트 팬아웃 — reference count 가 필요 없는 이유

```cpp
GameEvent event = repository.createEvent();     // payload 소유자는 이 스택 변수

for (GameSession& session : related_sessions)
{
    ChannelAccess access = pool.access(session.channel_id);

    if (!access)
    {
        continue;                                // 이미 끊긴 세션 (NOT_FOUND / STALE)
    }
    access.channel().business().event.emit(event);   // 리스너가 여기서 복사를 끝낸다
}
// 루프 끝 → event 소멸 → payload 가 풀로 돌아간다
```

`Connection` 의 이벤트 리스너는 `encode` 로 **송신 패킷을 새로 만든다.** `emit` 이
돌아오는 시점에 채널 N 개 모두가 자기 몫의 복사본을 `SendBuffer` 에 넣었다. 원본을 더
오래 살려 둘 이유가 없고, 그래서 reference count 도 없다.

대가는 **인코딩이 채널 수만큼 반복**되는 것이다. 한 번 인코딩한 `Bytes` 를 N 개 커넥션이
공유하려면 refcount 있는 `Bytes` 가 필요한데, 그것은 **측정으로 병목이라고 나올 때**
별도 작업으로 한다.

### 4.4 `IUsecase::execute` 와 디스패처

```cpp
virtual void execute(const GameRequest& request, BusinessGameChannel& channel) = 0;
```

`UsecaseDispatcher` 는 `StreamListener<GameRequest>` 가 되고 `UsecaseRegistry&` 와
`GameChannelPool&` 을 든다. `request.channel_id` 로 `access` 해 `business()` 를 넘긴다.
채널이 없으면 요청을 버린다.

디스패처 인스턴스는 **하나**이고 모든 채널의 `request` 스트림에 리스너로 붙는다 —
`ChannelPoolBinder::onConnected` 가 그 등록을 한다. 즉 **디스패처가 어느 채널에 붙는지는
binder 의 일**이고, 디스패처 자신은 채널을 모른다.

**재진입 주의.** §4.1 의 경로에서 디스패처는 이미 `access` 로 풀 락을 쥔 채 usecase 를
실행한다. usecase 가 그 안에서 다시 `pool.access` (§4.3 의 브로드캐스트) 를 부르면
`std::mutex` 는 데드락이다. 그래서 풀 락은 **`std::recursive_mutex`** 다 (결정 B). `access` 스코프 안에서 같은 스레드가 `create`·`remove` 를 불러도 된다.

| 안 | 내용 | 비용 |
| --- | --- | --- |
| A | 디스패처는 `access` 를 **`business()` 뷰를 꺼내는 동안만** 쥐고, 락을 푼 뒤 `execute` 한다 | `execute` 중 자기 채널이 `remove` 될 수 있다 → `Connection` 파괴는 IO 루프 스레드이고 디스패처도 IO 루프 스레드이므로 **지금은** 불가능. 워커가 생기면 A 는 깨진다 |
| B | `std::recursive_mutex` | 같은 스레드의 재진입은 허용, 비용은 `mutex` 와 비슷. 워커가 생겨도 규칙이 그대로다 |

B 를 택한 이유는 규칙이 스레드 모델과 무관하게 유지되기 때문이다.

## 5. Presentation — `Endpoint` · `ConnectionTable` · `Connection`

`ConnectionTable` 이 binder 와 codec 을 주입받아, 소켓을 만들고 → 채널을 받고 → 그 둘로
`Connection` 을 구성한다. `Connection` 은 완성된 재료만 받으므로 참조 멤버로 둘 수 있고,
파괴될 때 채널을 돌려준다. `Connection` 은 더 이상 추상 클래스가 아니다.

### 5.1 주입 경로

```
ConnectionTable(capacity, pool, send_queue_limit, IChannelBinder&, IPacketCodec&)
Endpoint(ip, port, waiting_limit, IConnectionTable&)            // 불변
IoLoop(Endpoint&)                                                // 불변
```

`Endpoint` 는 binder 를 모른다. 초안(§Presentation-1)은 `Endpoint` 와 `Connection` 양쪽이
받도록 했지만 같은 객체를 두 군데 주입하면 어긋날 수 있어 **`ConnectionTable` 한 곳**에서
받는다. `ConnectionTable<TConnection>` 템플릿 파라미터는 사라지고 `IConnectionTable` 은
구현이 하나뿐이 되므로 **함께 제거**한다 — `Endpoint` 가 `ConnectionTable&` 를 직접 받는다.

### 5.2 `ConnectionTable::open` — 재료를 먼저, 객체는 마지막에

```cpp
Connection* ConnectionTable::open(SocketHandle accepted, IOMultiEventListener& listener)
{
    Slot* const slot = findFreeSlot();
    if (slot == nullptr) { return nullptr; }                      // IoLoop 의 기존 규약

    ActiveSocket sock(accepted);                                  // ip/port 가 여기서 정해진다
    if (sock.handle() == INVALID_SOCKET_HANDLE) { return nullptr; }

    const ConnectionInfo info = connectionInfoOf(sock);          // char[16] 에 복사
    Result<ChannelCode, IoGameChannel> bound = _binder.onConnected(info);
    if (!bound.isOk()) { return nullptr; }                        // 풀 고갈 → 소켓은 sock 소멸자가 닫는다

    Connection* const connection = ::new (slot->storage())
        Connection(std::move(sock), info, bound.value(), listener, _pool, _send_queue_limit,
                   _binder, _codec);
    ...
}
```

`open` 의 반환형이 아직 포인터인 것은 `Endpoint::openConnection` 의 기존 계약이다. 이
계획의 범위 밖이지만 §1 원칙에 맞추려면 `Result<SocketCode, Connection&>`
로 바꾸는 후속 작업이 있다 — §7 에 둔다.

### 5.3 `Connection`

```cpp
class Connection : public IOContext
{
public:
    Connection(ActiveSocket&& sock, const ConnectionInfo& info, IoGameChannel channel,
               IOMultiEventListener& listener, BlockPool& pool, size_t send_queue_limit,
               IChannelBinder& binder, IPacketCodec& codec);
    ~Connection();
    ...
private:
    class ResponseListener : public StreamListener<GameResponse> { ... };
    class EventListener    : public StreamListener<GameEvent>    { ... };

    void dispatchPacket(ReceivedPacket& packet);      // 기존 onPacket 자리

    ActiveSocket      _sock;
    ConnectionInfo    _info;
    IoGameChannel     _channel;                       // 참조 멤버 — 초기화 목록에서만
    IChannelBinder&   _binder;
    IPacketCodec&     _codec;
    ResponseListener  _response_listener;
    EventListener     _event_listener;
    ...
};
```

생성자 본문은 `_channel.response.addListener(_response_listener)` 와 `event` 등록 둘뿐이다.
`onOpened`/`onClosed` 가상 훅은 상속이 사라지므로 **함께 제거**한다.

소멸자:

```cpp
Connection::~Connection()
{
    _channel.response.removeListener(_response_listener);
    _channel.event.removeListener(_event_listener);
    _binder.onDisconnected(_info, _channel);
}
```

`removeListener` 를 먼저 부르는 것은 채널을 풀에 돌려주지 않는 binder 구현(재접속 대기용
보존 등)을 허용하기 위해서다. 파괴 시점은 `ConnectionTable::reapClosed` / `close` /
테이블 소멸자 — 모두 IO 루프 스레드다.

### 5.4 리스너 — `kiotty_connection_sinks.h` 를 `Connection` 안으로

`ResponseSink`·`EventSink` 본문이 `ResponseListener`·`EventListener` 가 되고, `writePacket`
호출이 `_codec.encode(...)` 로 바뀐다. 파일 `kiotty_connection_sinks.h` 는 지운다. 이벤트
시퀀스 카운터는 `EventListener` 안에 그대로 둔다.

### 5.5 `onPacket` 의 자리

`Connection::handleReceived` 가 패킷을 완성하면 `dispatchPacket` 을 부른다:

```cpp
void Connection::dispatchPacket(ReceivedPacket& packet)
{
    GameRequest request;

    if (!_codec.decode(packet, _channel.channel_id, request))
    {
        return;
    }
    _channel.request.emit(request);
}
```

사용자 확장점은 `IUsecase`·`IPacketCodec`·`IChannelBinder` 셋이고, `Connection` 상속은
없다.

### 5.6 `SendBuffer` 는 필요한가 — 필요하다

질문: "`submitSend` 로 보낸 패킷을 `onSent` 에서 추적할 수 있으면 거기서 지우면 되지
않나. 풀이 이미 그 일을 하는 것 아닌가."

**보낸 패킷의 추적·삭제는 이미 그렇게 돌고 있다.** `pumpSend` 가 큐에서 하나를 꺼내
`_sending` 으로 옮기고, `handleSent` → `releaseSending` 이 `_sending = Bytes()` 로 풀에
돌려준다. `SendBuffer` 는 그 역할이 아니다.

`SendBuffer` 가 하는 일은 **소켓에 한 번에 하나만 실을 수 있는 동안 나머지를 순서대로
세워 두는 것**이다. `BlockPool` 은 메모리를 빌려줄 뿐 "다음에 보낼 것이 무엇인가"를 모른다.
usecase 가 응답 하나와 이벤트 둘을 연달아 `emit` 하면 첫 번째가 전송 중인 동안 나머지 둘이
어딘가에 살아 있어야 하고, 그 어딘가가 `SendBuffer` 다. 거기에 `DropPolicy::Oldest`
(이벤트는 밀리면 버린다) 와 큐 한도(백프레셔)가 얹혀 있다.

따라서 **그대로 둔다.** 다만 이름이 역할을 말하지 않는다 — `SendQueue` 로 바꾸는 것을
§7 에 둔다.

## 6. 실행 순서

단계마다 빌드·하네스를 돌려 **워닝 0 · PASS** 를 확인하고 다음으로 간다. 한 단계가
컴파일되는 시점에 `cpp-tester` 를 불러 유닛 테스트를 받는다.

| 단계 | 작업 | 건드리는 파일 | 완료 조건 |
| --- | --- | --- | --- |
| 0 | 기준선 — 현재 빌드·scratch 전부 돌려 결과 기록 | — | HANDOVER §1 표와 일치 |
| 1 | §2 스트림 추가, `Result<E, T&>` 특수화 + `okRef`. 구 `Sink`/`Stream` 은 아직 남김 | `core/kiotty_stream.h`, `core/kiotty_result.h` | `MutableStream` 유닛 테스트 (등록 한도·remove 중 emit·clear), `Result<E, T&>` 테스트 (ok/fail·복사·const) |
| 2 | §3 entity · channel · pool(+락·세대·`ChannelAccess`) · binder · codec 추가. 기존 `Request` 등은 아직 남김 | `domain/entity/*`, `domain/channel/*`, `domain/codec/*`, `CMakeLists.txt` | 풀 테스트 (create/remove/access/슬롯 재사용/STALE/고갈), 양방향 뷰 테스트, `DefaultPacketCodec` 왕복 테스트 |
| 3 | §4.4 usecase 시그니처 변경, 디스패처를 리스너로, 구 `Request`·`Sink` 삭제 | `domain/usecase/*`, `core/kiotty_stream.h` | `usecase_check` offline 통과 |
| 4 | §5 `Connection` 비추상화, `ConnectionTable` 비템플릿화 + 주입, `Endpoint` 시그니처, `kiotty_connection_sinks.h` 삭제, scratch 하네스의 `Connection` 파생 클래스 제거 | `presentation/connection/*`, `presentation/endpoint/*`, `test/scratch/*` | 모든 scratch 하네스 PASS |
| 5 | 검증 매트릭스 — g++ `-Wall -Wextra -Wpedantic`, MSVC `/W4`, ASan+UBSan, TSan(epoll·io_uring) | — | HANDOVER §1 표 갱신 |
| 6 | 문서 — HANDOVER.md 의 `core/`·`domain/`·`presentation/` 계약 절 갱신 | `docs/progress/*` | — |

3 단계와 4 단계 사이에는 빌드가 깨진다 (`Request`·`onPacket` 사용처가 `Connection` 과
하네스에 있다). **3·4 는 한 커밋으로 묶는다.**

## 7. 후속 작업

이번 범위에서 풀지 않는 것들이다.

| 항목 | 언제 | 방향 |
| --- | --- | --- |
| `ConnectionTable::open` 의 포인터 반환 | `Endpoint` 를 `Result` 로 정리할 때 | `Result<SocketCode, Connection&>` |
| `SendBuffer` → `SendQueue` 이름 | 한가할 때 | 동작 변경 없음 |
| 이벤트 N 회 인코딩 (§4.3) | 브로드캐스트 세션 수가 커질 때 | 측정 후 refcount `Bytes` 검토 |
| 풀 락의 슬롯 단위 분할 (§3.4) | 워커 스레드 경합이 측정될 때 | 슬롯마다 `std::mutex`, `ChannelAccess` 인터페이스는 불변 |
| 워커 스레드의 TSan 검증 | 워커 도입 시 | 소켓 없는 테스트 더블로 `access`/`remove` 교차를 Linux TSan 에서 돌린다 |

## 8. 실행 결과 (2026-08-22)

§6 의 0–6 단계를 모두 마쳤다. 검증은 실제로 돌린 것만 적는다.

| 검증 | 결과 |
| --- | --- |
| g++ `-Wall -Wextra -Wpedantic` (epoll · io_uring) / MSVC `/W4` (iocp) | 워닝 0 |
| scratch 하네스 `connection_check` · `io_loop_check` · `usecase_check` | 3 백엔드 전부 PASS |
| 통합 스모크 `kiotty_io_event_listener_smoke` | 3 백엔드 PASS |
| ASan+UBSan · TSan (epoll · io_uring, 하네스 3종) | 리포트 0 |
| 유닛 테스트 (cpp-tester, 신규 177개) | Linux 334/335 · Windows 333/334 — 실패 1건은 기존 `RingBuffer` 테스트 |

cpp-tester 가 테스트로 고정한 동작 두 가지는 계약이 아니라 **현재 구현**이다.

- `MutableStream` 리스너가 자기 `onStream` 안에서 자신을 `removeListener` 하면 그 `emit` 에서
  다음 리스너 하나를 건너뛴다. `Connection` 은 소멸자에서만 떼므로 지금은 닿지 않는다.
- `Result<E, T&>::operator bool` 은 본체 템플릿과 같이 **실패일 때 true** 다. `ChannelAccess`
  의 `operator bool` 은 성공일 때 true 라 극성이 반대다 — `isOk()` 를 쓰면 혼동이 없다.
