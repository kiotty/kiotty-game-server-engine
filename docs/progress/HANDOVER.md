# 인수인계 — GameChannel 경계까지 완료, 다음은 Session 과 Worker

`presentation/` 이 socket · event · event_listener · endpoint · connection · **io_loop**
까지 다 올라왔고 `core/` 의 버퍼·풀·패킷 타입이 끝났다. **서버가 실제로 돈다** — 클라이언트
여럿이 붙고, 패킷이 올라오고, 응답이 나가고, 끊기면 슬롯이 회수된다. presentation 과 domain
은 **`GameChannel` 위의 스트림으로만** 데이터를 주고받고, 애플리케이션이 채우는 자리는
`IUsecase` · `IPacketCodec` · `IChannelBinder` 셋이다 (§5.2, §5.6). `Connection` 을 상속하는
일은 없다.

이 문서는 다음 세션이 바로 이어 쓰도록 **지금 있는 것의 계약**을 적는다. 설계 결정의 근거는
[code_refactoring_plan.md](code_refactoring_plan.md) 에 있다. 유닛 테스트를 붙이는 일은
[HANDOVER_TEST.md](HANDOVER_TEST.md) 로 따로 넘겼다.

## 1. 현재 상태

세 백엔드가 모두 빌드되고 실제 왕복이 통과한다. 검증은 `test/scratch/` 의 하네스로 했다.

| 검증 대상 | g++ `-Wall -Wextra -Wpedantic` | MSVC `/W4` | ASan+UBSan | TSan |
| --- | --- | --- | --- | --- |
| 라이브러리 3 백엔드 + 스모크 | 워닝 0, PASS | 워닝 0, PASS | epoll·io_uring PASS | epoll·io_uring clean |
| core 타입 (Bytes·BlockPool·RingBuffer·PacketHeader·SendBuffer) | 워닝 0, PASS | 워닝 0, PASS | PASS | PASS |
| ReceiveBuffer | 워닝 0, PASS | 워닝 0, PASS | PASS | — |
| Connection (루프백 왕복 · 분할 수신 · 헤더 거부 · 드롭 정책) | 워닝 0, PASS | 워닝 0, PASS | epoll·io_uring PASS | epoll·io_uring clean |
| IoLoop (동시 접속 3 · 접속 한도 · 슬롯 회수·재사용 · 외부 stop) | 워닝 0, PASS | 워닝 0, PASS | epoll·io_uring PASS | epoll·io_uring clean |
| domain 경계 (Registry · Dispatcher · GameChannel 배선 · 응답/이벤트 왕복 · 끊긴 채널 거절) | 워닝 0, PASS | 워닝 0, PASS | epoll·io_uring PASS | epoll·io_uring clean |

빌드는 Linux 가 `cmake -S . -B <dir>`, Windows 가 `bash script/build.sh` 다.
epoll 백엔드는 `-DKIOTTY_USE_IO_URING=OFF` 로 따로 구성해야 나온다.
TSan 은 WSL 에서 `setarch $(uname -m) -R` 로 ASLR 을 끄고 돌린다.
**WSL 은 유휴 상태에서 VM 이 내려가며 `/tmp` 를 지운다** — 빌드 디렉토리는 홈 아래에 잡는다.

**유닛 테스트는 `test/unit/` 에 있다** — core 타입과 `GameChannel` 경계(스트림 · `Result<E, T&>` ·
채널 · 풀 · codec · binder · dispatcher) 334개. `RingBuffer.TryPushOnFullBufferLeavesAMovedValueWithTheCaller`
1건이 리팩토링 이전부터 실패 상태다 — `RingBuffer::tryPush` 가 가득 찼을 때 인자를 소비하는
문제로, 아직 손대지 않았다. presentation 의 소켓 경로는 여전히 `test/scratch/` 하네스로만 검증한다 (§6.1).

## 2. `core/` — 무엇이 있고 계약이 무엇인가

바이트를 소유하는 방법과 패킷을 와이어에 싣는 방법이 여기서 정해진다. 전부 헤더 전용이고
`kiotty_bytes.cpp` 하나만 `.cpp` 다.

### 2.1 `Bytes` — 단일 소유자, 풀 뒷받침

```cpp
Bytes();
Bytes(BlockPool& pool, size_t length);   // 블록만 잡는다. 내용은 미정

Bytes(const Bytes&) = delete;            // 복사 없음
Bytes(Bytes&&) noexcept;                 // 이동만

ByteSpan       writableSpan();           // 쓰기 노출은 이것 하나
const uint8_t* data() const;
size_t         size() const;
ByteView       view() const;
```

**상태가 둘뿐이다** — 풀도 블록도 없거나, 둘 다 있거나. `length == 0` 이면 블록을 잡지
않고 빈 상태로 떨어진다. 이동·해제 뒤에도 반드시 첫 번째 상태로 돌아간다.

**참조 카운트가 없다.** 블록에 헤더가 붙지 않으므로 `Bytes(pool, 64)` 는 정확히 64바이트를
요청하고, 크기 클래스가 어긋나지 않는다. 유일한 소유자가 놓는 순간 블록이 풀로 돌아간다.

### 2.2 `BlockPool` — 크기 클래스 + 힙 fallback

```cpp
BlockPool(const BlockClass* classes, size_t class_count);
void*  acquire(size_t length);
void   release(void* released_ptr);
size_t fallbackCount() const;
```

`BlockRegionPool`(한 크기의 블록들) · `HeapFallback`(malloc/free + 횟수) · `BlockPool`
(둘 중 어디로 보낼지) 셋으로 나뉜다.

기본 표는 8개 클래스 · 4.625 MiB 다.

| 블록 | 개수 | 담는 것 |
| ---: | ---: | --- |
| 64 B | 2048 | 입력 이벤트, 작은 델타 |
| 128 B | 2048 | 트래픽 대부분 |
| 256 B | 1024 | |
| 512 B | 512 | |
| 1 KiB | 256 | |
| 4 KiB | 128 | 큰 상태 갱신 |
| 16 KiB | 64 | 스냅샷 |
| 64 KiB | 32 | 최대 크기 payload 하나 |

개수는 "커넥션 64개 × 큐 깊이 64 = 동시 4096개"를 트래픽 분포로 나눈 값이다.
**`fallbackCount()` 가 0 이 아니면 그 표가 모자란 것이다** — 튜닝은 그 숫자를 보고 한다.

**규칙 둘.** 클래스가 비면 위 클래스를 빌리지 않고 힙으로 간다 (작은 요청이 최대 payload
용 블록을 말리지 않게). 늘리는 기능은 없다 — `realloc` 은 밖에 나가 있는 포인터를
dangling 으로 만들고, 런타임 확장은 뜨거운 경로에서 할당하는 것이라 존재 이유를 지운다.

**측정 근거.** 혼합 크기 acquire+release 의 꼬리가 malloc 보다 짧다. 평균은 무승부다.

| | mean | p99 | p99.9 |
| --- | ---: | ---: | ---: |
| Linux malloc | 34 | 111 | 202 ns |
| Linux pool | 27 | 40 | 51 ns |
| Windows malloc | 62 | 200 | 1000 ns |
| Windows pool | 37 | 100 | 100 ns |

**한계.** 풀 전체에 뮤텍스가 하나다. 4스레드가 전력으로 할당하면 malloc 에 진다
(637 vs 42 ns). 교차점이 초당 100만 건 근처이고 8명 60Hz 는 초당 1000건 수준이라
실사용에서는 락이 부딪히지 않는다. 그 영역에 가면 **클래스 앞에 스레드별 free list** 를
붙이는 것이 답이지 락을 키우는 것이 아니다.

### 2.3 `PacketHeader` · `LittleEndian<T>` · `WireStruct<T,N>`

헤더 24바이트를 **수신 버퍼의 임의 위치에서 그대로 읽을 수 있게** 만드는 것이 전부다.

모든 필드가 `LittleEndian<T>` 라 구조체 정렬 요구가 1 이고, 그래서 어느 오프셋에서
캐스팅해도 미정렬 접근이 없다. `get()`/`set()` 만 있고 암묵 변환이 없다 — 와이어 필드를
읽는 것은 변환이고, 변환처럼 보여야 한다.

리틀엔디안 호스트에서는 `memcpy` 한 번으로 접힌다 (`static_cast` 는 미정렬 UB 라 안 된다).
빅엔디안 폴백은 남아 있고, 그쪽에서는 느릴 뿐 틀리지 않는다.

`PACKET_MAGIC = 0x544F494B` — 와이어에서 `K I O T` 로 읽힌다. **문서에 값이 없어 정한
것이므로 바꾸려면 지금이 싸다.**

`hasPacketMagic()` / `hasSupportedVersion()` 이 같은 헤더에 있다. major 만 보고 minor 는
통과시킨다. "우리 패킷인가"는 버퍼의 지식도 커넥션의 지식도 아니라 패킷의 지식이다.

`payload_length` 상한 검증은 **필요 없다** — 필드가 `uint16_t` 라 최대값이 정확히
`PACKET_MAX_PAYLOAD`(65535)다. 요구사항 §5 의 검증이 타입으로 이미 만족된다.

### 2.4 `ReceiveBuffer` — 상태가 없다

```cpp
explicit ReceiveBuffer(BlockPool& pool);

ByteSpan headerSpace();                  // 아직 안 받은 헤더 자리
void     addHeaderReceived(size_t read_length);
bool     isHeaderComplete() const;
const PacketHeader& header() const;

bool     openPayload();                  // header 의 길이로 블록을 잡는다. false = 실패
ByteSpan payloadSpace();
void     addPayloadReceived(size_t read_length);
bool     isPayloadComplete() const;

Bytes    takePayload();                  // 통째로 넘기고 두 카운터를 되돌린다
```

**길이를 딱 맞춰 요청하는 것이 설계의 핵심이다.** 헤더 단계에서는 남은 헤더 바이트만,
payload 단계에서는 남은 payload 바이트만 내놓는다. 커널에 "24바이트 자리 있음"이라고
말하면 25바이트를 주지 않으므로 **다음 패킷을 넘겨 읽는 일 자체가 없다.** 그래서 앞당김도
프레이밍도 뷰 수명 규칙도 필요 없다.

**복사가 0이다.** payload 는 길이를 알고 나서 풀에서 딱 그 크기 블록을 잡고, 커널이 그
블록에 직접 쓴다. `takePayload()` 가 그 `Bytes` 를 move 로 넘기므로 **워커가 payload 를
들고 일하는 동안 커넥션은 새 패킷을 받는다.**

`payload_length == 0` 이면 블록을 아예 잡지 않는다.

**단계 판정과 magic·version 검사는 Connection 이 한다.** 이 클래스에는 `Step` 도
`broken` 도 없다.

### 2.5 `SendBuffer` — `Bytes` 하나가 패킷 전체

```cpp
struct SentPacket { Bytes bytes; DropPolicy policy; };

explicit SendBuffer(size_t queue_capacity);
bool       tryPush(SentPacket& packet);  // 실패하면 packet 을 그대로 남긴다
SentPacket pop();                        // 비었으면 빈 SentPacket
bool       dropOldest(DropPolicy droppable);
bool       empty() const;
```

**헤더 24바이트와 payload 가 한 블록에 들어간다.** 만드는 쪽이 `Bytes(pool, 24 + payload_length)`
로 잡고 앞 24바이트에 헤더를, 뒤에 payload 를 직접 직렬화한다. 그래서 송신도 복사가 0이고,
`IOSendRequest{ sock, buffer, length }` 에 버퍼 하나로 그대로 들어간다 — **소켓 계층에
scatter-gather 를 넣을 필요가 없다.**

`RingBuffer::tryPush(T value)` 를 그대로 노출하지 않은 이유가 있다. 그것은 값으로 받아서
가득 찬 큐에 `std::move` 로 밀면 packet 이 파라미터로 옮겨졌다가 그대로 죽는다. 소유
타입에는 위험한 서명이라 참조로 감쌌다.

**정책은 데이터로만 들고, 판단은 Connection 이 한다.** 큐는 `dropOldest()` 로 "이 정책인
것 중 가장 오래된 하나"를 지워 줄 뿐이고, 언제 그것을 부를지와 그래도 자리가 없을 때 어떻게
할지는 `Connection::emit()` 이 정한다 (§4.4).

`SentPacket` 은 `ReceivedPacket` 과 짝이다. 받은 쪽은 헤더와 payload 가 따로 오므로 둘로
나뉘고, 보내는 쪽은 한 블록이므로 `Bytes` 하나다. **`architecture.html` 과
`requirement.md` 는 아직 이것을 `OutgoingPacket` 이라 부른다** — `IStateSink<OutgoingPacket>`
을 만들 때 이름을 맞춘다.

### 2.6 `RingBuffer<T>` — 용량 고정 FIFO

용량은 생성자에서 한 번 정해지고 늘지 않는다(`ServerConfig.send_queue_limit`). 동기화하지
않는다 — 소유자가 락을 잡는다. 2의 거듭제곱으로 반올림하지 않으므로 설정한 숫자가 곧
실제 한도다.

`at(offset)` 과 `removeAt(offset)` 이 있다. 중간을 지우면 뒤가 밀리므로 O(n) 이고, 그래서
패킷마다 도는 경로에서는 쓰지 않는다 — 큐가 꽉 찬 순간에만 `dropOldest()` 가 쓴다.

## 3. `presentation/` — 커넥션이 기대는 계약

`Connection` 이 지켜야 할 규칙이 전부 리스너 계약에서 나온다. 이 절이 그것들이다.

### 3.1 `IOMultiEventListener`

`submit` 이 `SOCKET_SUCCESS` 를 돌려주면 **콜백이 정확히 한 번 온다.** 다른 코드면
**콜백이 안 온다.** 소켓과 이벤트는 그 콜백보다 오래 살아야 한다. 수신 1개와 송신 1개가
동시에 떠 있을 수 있고, 같은 방향의 두 번째 제출은 `SOCKET_OUT_OF_RESOURCE` 로 거절된다.

### 3.2 `onDisconnected` — 정리해도 되는 유일한 시점

끊김은 **떠 있는 연산이 0 이 된 뒤에** 알려진다. receive 가 `SOCKET_CLOSED` 로 끝난
순간 정리하면 send 가 아직 커널 안이고 커널이 그 이벤트 주소를 들고 있다 — IOCP 와
io_uring 에서 use-after-free 다.

- 종료 판정은 `endsConnection()` 한 곳에 있다 (`SOCKET_CLOSED`·`SOCKET_FAILED`·`SOCKET_INVALID_HANDLE`).
- 끊긴 뒤 `submit` 은 전부 `SOCKET_CLOSED` 로 거절된다. **이 거절이 없으면 떠 있는 수가
  0 으로 안 내려가고 `onDisconnected` 가 영영 안 온다.**
- 거절은 영구적이다. 커넥션 풀이 슬롯을 다시 내줄 때는 **Connection 을 소멸시키고 그
  자리에 새로 생성한다** — 이벤트는 생성자가 만들므로 리셋 코드가 필요 없다.
- 서버가 먼저 끊고 싶으면 `ActiveSocket::shutdown()` 을 부른다. 떠 있던 연산이 실패로
  완료되고 그 경로로 `onDisconnected` 가 온다. **핸들을 닫는 것이 아니다** — epoll 은
  닫힌 디스크립터를 아무 통지 없이 집합에서 빼므로, 닫으면 걸려 있던 수신이 증발한다.
  핸들은 소멸자가 계속 소유한다.

**`onDisconnected` 가 오면 커널이 더 이상 그 이벤트를 가리키지 않고 더 이상 어떤 콜백도
오지 않는다. 그 시점부터 Connection 을 파괴하거나 풀에 반납해도 안전하다.**

**그 대가로 지켜야 할 것이 하나 있다 — 활성인 동안 무엇이든 하나는 떠 있어야 한다.**
`onDisconnected` 는 떠 있던 것이 0 이 될 때 오므로, 아무것도 안 띄운 커넥션은 끊겼다고
말할 방법이 없다. `Connection` 은 수신을 accept 직후에 걸고 완료마다 다시 건다 (§4.2).

### 3.3 `Endpoint`

생성자가 socket · bind · listen 을 다 끝낸다. `operator bool()` 이 true 면 곧바로
`submitAccept(listener)` 를 걸 수 있고 반쯤 만들어진 상태가 없다. 실패 이유는 `code()` 다.

`onAccepted` 의 context 는 **accept 를 건 Endpoint** 다. 받아 낸 핸들은
`IOAcceptResponse.accepting` 으로 나가고, **커넥션 모음은 IoLoop 가 소유한다**
(`architecture.html` §5.8). Endpoint 는 커넥션이 어디서 왔는지를 말해 줄 뿐이다.

## 4. `Connection` — 무엇을 하고 무엇을 모르는가

소켓 하나, 전송 이벤트 하나, 버퍼 둘, 채널 뷰 하나. 그것으로 수신 상태 기계를 돌리고 송신
체인을 유지한다. **Registry 도 Session 도 모른다** — 완성된 패킷을 `IPacketCodec` 으로
`GameRequest` 로 바꿔 자기 `IoGameChannel.request` 에 올리고, 채널의 `response` · `event`
에서 내려오는 것을 다시 패킷으로 내보낼 뿐이다. 그래서 서버 없이도 커넥션 하나만 세워 돌릴
수 있다 (`test/scratch/connection_check.cpp`).

### 4.1 인터페이스

```cpp
Connection(ActiveSocket&& sock, const ConnectionInfo& info, IoGameChannel channel,
           IOMultiEventListener& listener, BlockPool& pool, size_t send_queue_limit,
           IChannelBinder& binder, IPacketCodec& codec);   // ConnectionTable::open 만 부른다

SocketCode submitReceive();                 // accept 직후 한 번. 소유자가 부른다
bool       emit(Bytes& packet, DropPolicy); // 아무 스레드나. 블록하지 않는다
void       close();                         // 아무 스레드나
LifeState  lifeState() const;

void handleReceived(IOReceiveResponse&);                       // 루프 스레드. 완성된 패킷은 채널로
void handleSent(IOSendResponse&);                              // 루프 스레드
void handleDisconnected();                                     // 루프 스레드
```

`handle*` 셋은 리스너 콜백이 `IOContext&` 를 `Connection&` 로 캐스팅해서 그대로 넘긴다.
`emit()` 은 큐에 넣고 송신 체인까지 시동한다 — 요구사항 §4 의 "push 후 requestSend" 두
줄을 하나로 합친 것이고, 하나만 부르고 마는 사고가 생길 수 없다.

**송신 락 하나가 `_send` · `_send_state` · `_sending` · `_sent` 를 함께 지킨다.** 수신 쪽은
루프 스레드만 만지므로 지키지 않는다. `submit()` 은 항상 락 밖에서 부른다.

### 4.2 수신 루프

```
포스트할 자리:
    ReadingHeader  -> _receive.headerSpace()
    ReadingPayload -> _receive.payloadSpace()

handleReceived(response, out):
    ReadingHeader:
        addHeaderReceived
        isHeaderComplete() 이면
            magic / version / openPayload 검사 -> 실패면 Rejected
            isPayloadComplete() ? Complete : ReadingPayload
    ReadingPayload:
        addPayloadReceived
        isPayloadComplete() 이면 Complete

    Complete 이면 header 를 복사하고 takePayload() 를 out 에 실어 준다
    다음 수신을 **여기서** 발행한다 (요구사항 §4)
    Rejected 였으면 그 다음에 close()
```

**헤더는 복사해서 넘긴다.** 다음 수신이 `_receive` 의 헤더 자리에 바로 걸리므로, 참조로
넘기면 부르는 쪽이 읽는 동안 커널이 그 위에 쓴다. 24바이트짜리 복사 하나가 그 값이다.

**발행이 `close()` 보다 먼저다.** 순서가 뒤집히면 죽은 핸들에 건 submit 이 거절되고,
거절된 submit 은 완료를 만들지 않으므로 `onDisconnected` 가 영영 오지 않는다.

### 4.3 송신 루프와 해제 시점

**보내는 중인 하나는 큐가 아니라 Connection 이 든다.** 큐에 남겨 두면 다 나갈 때까지
pop 을 못 하고, 그러면 큐가 "대기열"과 "전송 중"을 둘 다 뜻하게 된다.

| 시점 | 할 일 |
| --- | --- |
| `submit` → SUCCESS | `_sending` 이 그 `Bytes` 를 붙들고 있는다 |
| `onSent` 에서 다 나갔음 | **여기서 해제.** `_sending = Bytes()` 하면 블록이 풀로 돌아간다 |
| `onSent` 인데 일부만 | 안 놓고 `_sent` 를 올린 뒤 남은 만큼 다시 `submit` |
| `submit` 이 SUCCESS 아님 | 콜백이 안 오므로 즉시 해제한다 |
| `onDisconnected` | 떠 있는 것이 0 이므로 Connection 통째로 파괴해도 안전 |

`SendState` 전이는 요구사항 §4 의 순서를 지킨다 — 생산자는 큐에 push 를 **먼저** 하고
`Idle -> Sending`, 완료 핸들러는 `Sending -> Idle` 로 **먼저** 되돌리고 큐를 한 번 더
확인한다. CAS 대신 송신 락 안에서 읽고 쓰는데, 큐와 상태를 따로 지키면 그 둘 사이가
비는 순간이 생기기 때문이다. 지켜야 할 순서는 그대로다.

### 4.4 드롭 정책

`emit()` 이 정한다. 자리가 없으면 **큐 어디에 있든 가장 오래된 `Oldest` 항목 하나를
버리고** 다시 넣는다. 그래도 안 되면 — 큐가 전부 `Never` 라는 뜻이고 —
`Never` 는 연결 종료, `Oldest` 는 자기 자신을 버린다.

앞의 것만 보지 않고 큐를 뒤지는 이유가 있다. 응답 하나가 머리에 걸려 있다는 이유로 뒤에
쌓인 실시간 이벤트 63개를 못 버리면, 이벤트가 큐를 채우는 정상 상황에서 응답이 올 때마다
연결이 끊긴다. 뒤지는 비용은 O(n) 이지만 **큐가 꽉 찬 순간에만** 든다.

**이미 커널에 넘어간 것(`_sending`)은 버릴 수 없다** — 커널이 그 블록을 읽고 있다.

## 5. 조립 — `IoLoop` · `Endpoint` · `ConnectionTable`

셋을 밖에서 쌓아 올린다. 어느 것도 다른 것을 등록하지 않고, 완성된 패킷을 어디로 올릴지
알려 주는 핸들러도 없다 — **완료가 들고 오는 `IOContext` 가 이미 그 일을 할 객체다.**

```cpp
BlockPool pool(defaultBlockClasses(), defaultBlockClassCount());

GameChannelPool   channels(64);
UsecaseRegistry   registry(makeUsecases(pool));
UsecaseDispatcher dispatcher(registry, channels);
ChannelPoolBinder binder(channels, dispatcher);
DefaultPacketCodec codec;

ConnectionTable connections(64, pool, 64, binder, codec);
Endpoint        endpoint("0.0.0.0", 7777, 64, connections);
IoLoop          loop(endpoint);

if (loop) loop.run();
```

**백엔드별로 갈리지 않는다.** 설계 문서(`architecture.html` §6.9)는 `IIoLoop` 와 구현 셋을
그렸지만, 백엔드 차이는 이미 `IOMultiEventListener` 안에서 다 쓰였다. 그래서 루프 셋이 아니라
루프 하나를 세 번 빌드한다.

### 5.1 라우팅 — `IOContext` 하나로 끝난다

콜백은 맨 함수 포인터이고 리스너는 소유자를 모른다. 그래도 되는 이유는 **모든 완료가
`IOContext&` 를 들고 오고, 그것이 곧 그 일을 할 객체**이기 때문이다.

| 콜백 | context | 캐스팅해서 부르는 것 |
| --- | --- | --- |
| `onAccepted` | `Endpoint` | `handleAccepted()` — 표에 커넥션을 짓고 accept 를 다시 건다 |
| `onReceived` | `Connection` | `handleReceived()` |
| `onSent` | `Connection` | `handleSent()` |
| `onDisconnected` | `Connection` | `handleDisconnected()` |

`onInterrupted` 은 쓰지 않는다 — cancel 은 어떤 연산의 것도 아니라서 캐스팅할 context 가
없고, `run()` 은 `wait()` 의 반환값으로 이미 안다.

**`reinterpret_cast` 가 아니라 `static_cast` 다.** `IOContext` 는 빈 베이스지만 `Connection`
에는 vptr 이 있어서 베이스 서브오브젝트가 오프셋 0 이라는 보장이 없다. `static_cast` 는 그
보정을 컴파일 타임에 하고 비용은 같다.

### 5.2 애플리케이션이 채우는 자리 셋 — `IUsecase` · `IPacketCodec` · `IChannelBinder`

`Connection` 은 구체 클래스이고 상속하지 않는다. 커넥션이 하는 일은 바이트를 패킷으로
모으고, 패킷을 `IPacketCodec` 으로 `GameRequest` 로 바꿔 자기 채널의 `request` 스트림에
올리고, 채널의 `response` · `event` 스트림에서 내려오는 것을 다시 패킷으로 내보내는
것뿐이다.

| 자리 | 받는 곳 | 하는 일 |
| --- | --- | --- |
| `IUsecase::execute(const GameRequest&, BusinessGameChannel&)` | `UsecaseRegistry` | 요청 처리. 응답·이벤트는 넘겨받은 채널의 sink 에 `emit` |
| `IPacketCodec` | `ConnectionTable` 생성자 | `decode(ReceivedPacket&, ChannelId, GameRequest&)` 와 `encode(BlockPool&, GameResponse/GameEvent)`. `DefaultPacketCodec` 이 헤더 필드를 그대로 옮기는 기본값 |
| `IChannelBinder` | `ConnectionTable` 생성자 | `onConnected(ConnectionInfo) → Result<ChannelCode, IoGameChannel>`, `onDisconnected(ConnectionInfo, IoGameChannel)`. `ChannelPoolBinder` 가 풀에서 채널을 만들고 주어진 `StreamListener<GameRequest>` (보통 dispatcher) 를 붙이는 기본값 |

셋 다 루프 스레드에서 돈다. `decode` 는 **다음 수신이 이미 걸린 뒤에** 불리므로 여기서
쓰는 시간이 자기 수신 윈도우를 갉아먹지는 않지만, 같은 루프의 다른 커넥션은 그만큼 기다린다
(요구사항 §8). 커넥션별로 기억할 것(세션 플래그 등)은 `ChannelId.index` 를 키로 codec 이나
binder 가 붙이는 쪽에 둔다 — `test/scratch/usecase_check.cpp` 의 `SessionCodec` 이 그 예다.

### 5.3 커넥션 슬롯 — `ConnectionTable`

`Connection` 은 이동할 수 없다. 이벤트가 자기 주소를 들고 있고 커널이 그 이벤트를 들고
있어서다. 그래서 `std::vector<Connection>` 도 `std::deque` 도 못 쓴다.

남는 답이 **자리가 고정된 슬롯 한 줄**이다. 생성 시 `sizeof(Connection)` 으로 한 번 잡고,
커넥션은 그 자리에 placement new 로 짓고 그 자리에서 소멸시킨다. 슬롯을 다시 내줄 때는
**리셋이 아니라 새로 짓는다** — closing 으로 넘어간 전송 이벤트는 되돌아오지 않고, 그
이벤트는 생성자가 만든다.

**`open()` 은 재료를 먼저 갖추고 객체를 마지막에 짓는다.** `ActiveSocket` 을 만들어
ip/port 를 얻고 → `IChannelBinder::onConnected` 로 채널을 받고 → 그 둘을 `Connection`
생성자에 넘긴다. 그래서 `Connection` 의 `IoGameChannel` 멤버는 참조 멤버로 둘 수 있고,
채널 풀 고갈은 커넥션이 생기기 전에 거절된다. `open(SocketHandle&, …)` 은 핸들을
가져가는 순간 호출자의 핸들을 `INVALID` 로 비운다 — 실패 경로에서 `ActiveSocket` 소멸자와
리스너가 같은 핸들을 두 번 닫지 않게 하기 위해서다.

**빈 슬롯이 없으면 그것이 곧 접속 한도다.** `open()` 이 nullptr 을 주고, 엔드포인트는
`response.accepting` 을 그대로 두며, 리스너가 콜백이 끝날 때 그 핸들을 닫는다. 거절하는
코드가 따로 없다 — 안 가져가는 것이 거절이다.

`~Connection` 은 채널에서 자기 리스너를 떼고 `onDisconnected` 를 부른다. 파괴는 §5.4 의
`reapClosed` 에서, 즉 루프 스레드에서만 일어난다.

빈 슬롯 찾기와 슬롯 찾기가 둘 다 선형 탐색이다. accept 와 disconnect 에서만 도는 경로이고
기본 64칸이라 그대로 둔다.

### 5.4 슬롯 회수는 콜백 밖에서

`onDisconnected` 안에서 커넥션을 파괴하지 않는다. 그 시점에 백엔드는 아직 자기 완료 배치를
걷고 있고, 걷고 있는 이벤트가 바로 그 커넥션의 것이다.

`handleDisconnected()` 는 `LifeState::Closed` 로만 표시하고, `run()` 이 `wait()` 사이마다
`reapClosed()` 를 불러 그때 파괴한다.

### 5.5 accept 를 다시 거는 자리

`Endpoint::handleAccepted()` 안에서 다시 건다. 그것을 빼먹으면 첫 손님 이후로 귀가 먹는다.

다시 걸기가 실패하면 false 를 돌려주고 루프가 `cancel()` 한다. 한 커넥션이 거절되거나
실패한 것은 듣기를 그만둘 이유가 아니지만, 엔드포인트가 죽은 것은 이유다.

**엔드포인트는 지금 하나다.** 여럿을 붙이려면 `IoLoop` 가 목록을 들고 각각에 accept 를
걸고 각각을 reap 하면 된다 — `onAccepted` 의 context 가 이미 어느 엔드포인트인지 말해 주므로
그쪽 라우팅은 바꿀 것이 없다.


### 5.6 domain 경계 — `GameChannel` 과 푸시 스트림

presentation 과 domain 은 서로의 타입을 모른다. 사이를 오가는 것은 순수 데이터 셋 —
`GameRequest` · `GameResponse` · `GameEvent` (`domain/entity/`) — 뿐이고, 건너는 길은
`GameChannel` 하나가 들고 있는 `MutableStream<T>` 셋이다 (`core/kiotty_stream.h`,
`domain/channel/`).

| 타입 | 뜻 |
| --- | --- |
| `ISink<T>::emit(const T&)` | 넣는 끝. 등록된 리스너 전원에게 **같은 호출 스택에서** 전달. 리스너 0 명이면 false |
| `IStream<T>::addListener / removeListener / clear` | 듣는 끝. `StreamListener<T>::onStream(const T&)` 를 등록. 고정 배열(기본 4), 힙 없음, 비소유 |
| `MutableStream<T>` | 둘 다 구현. `sink()` / `stream()` 으로 한쪽만 내준다 |
| `IoGameChannel` | presentation 이 보는 뷰 — `request` 는 sink, `response` · `event` 는 stream |
| `BusinessGameChannel` | domain 이 보는 뷰 — `request` 는 stream, `response` · `event` 는 sink |
| `GameChannelPool` | 고정 용량 슬롯 + `ChannelId{index, generation}`. `create()` 는 `Result<ChannelCode, GameChannel&>`, `remove(id)`, `access(id)` 는 풀 락을 쥔 RAII `ChannelAccess` |

```
Connection::handleReceived → 패킷 완성
  → codec.decode → GameRequest (payload 는 수신 블록을 move — 복사 없음)
  → channel.request.emit  →  UsecaseDispatcher::onStream
       → pool.access(request.channel_id) → usecase.execute(request, business)
            → business.response.emit  →  Connection::ResponseListener → codec.encode → emit(Never)
            → business.event.emit     →  Connection::EventListener    → codec.encode → emit(Oldest)
```

**소유권은 동기 푸시에서 나온다.** `emit` 이 돌아오면 아이템은 더 이상 참조되지 않는다.
리스너는 자기 몫을 그 자리에서 복사(인코딩)하고, 만든 쪽은 스택 변수를 그냥 버린다.
브로드캐스트도 같은 `GameEvent` 하나를 채널 N 개에 차례로 `emit` 하면 끝이고 refcount 는
없다.

**채널 접근은 `access()` 로만 한다.** `ChannelAccess` 가 살아 있는 동안 풀의
`std::recursive_mutex` 를 쥐므로 그 사이 `remove` 가 막힌다 — 워커 스레드가 `emit` 하는
도중 커넥션이 파괴되는 일이 없다. 같은 스레드의 재진입(디스패처가 락을 쥔 채 usecase 가
다른 채널에 `access`)은 허용된다. 끊긴 채널의 옛 id 는 슬롯이 비었으면 `CHANNEL_NOT_FOUND`,
재사용됐으면 `CHANNEL_STALE` 이다.

헤더 직렬화는 `core/kiotty_packet_writer.h` 의 `writePacket` 하나이고 `DefaultPacketCodec`
이 그것을 부른다 — magic · version · timestamp 를 엔진이 찍고, 응답은 correlation_id 반사,
이벤트는 커넥션별 시퀀스를 받는다 (요구사항 §2 10번).

## 6. 미결·주의

아직 정식 테스트로 옮기지 않은 검증 하네스, 정해지지 않은 값, 규모가 커지면 다시 볼
지점을 적는다.

### 6.1 검증 하네스가 `test/scratch/` 에 있다

`core_check.cpp` · `pool_check.cpp` · `recv_check.cpp` · `connection_check.cpp` ·
`io_loop_check.cpp` · `usecase_check.cpp` 와 벤치마크 `pool_bench.cpp` 가 있다. **어느 것도
CMake 에 등록되어 있지 않다** — 손으로 컴파일해서 돌린다. 정식 유닛 테스트로 옮기고 나면
지운다 ([HANDOVER_TEST.md](HANDOVER_TEST.md) §6).

`core_check.cpp` 의 `SendBuffer` 두 절은 항목 타입이 `SentPacket` 으로 바뀌기 전 API 를
써서 **지금 컴파일되지 않는다.** 나머지는 그대로 돈다. `connection_check` · `io_loop_check` ·
`usecase_check` 는 `GameChannel` 경계 위에서 돈다 — `StreamListener<GameRequest>` 와
`IChannelBinder` 를 하네스가 직접 구현한다.

### 6.2 정해지지 않은 것

- **`PACKET_MAGIC` 값** — 문서에 없어 정한 것 (§2.3). 바꾸려면 아직 싸다
- **송신 큐 깊이** — `ServerConfig.send_queue_limit` 의 기본값
- **`MutableStream` 의 리스너 수** — 기본 4. 채널당 스트림별 리스너가 1 명이라 여유가 있다

드롭 정책 엄격도는 정해졌다 — 큐 중간에서 버린다 (§4.4).

### 6.3 규모가 커지면 다시 볼 것

- **수신이 패킷당 read 2회다** (헤더 한 번, payload 한 번). 송신은 한 블록이라 1회다.
  8명 60Hz 면 초당 2000회 수준이라 무해하고, 수천 명이 되면 배칭이 필요해진다.
- **브로드캐스트가 커넥션마다 인코딩한다.** 이벤트의 `correlation_id` 가 커넥션별 시퀀스라
  헤더가 커넥션마다 다르고, 그래서 한 블록을 나눠 가질 수 없다. 실시간 이벤트 크기(수백
  바이트)에서는 복사가 오히려 빠르므로 지금은 이쪽이 맞다. 큰 payload 를 여러 명에게
  동시에 뿌리는 요구가 생기면 헤더를 분리하고 refcount `Bytes` 로 공유를 되살리는 것이 답이다.
- **풀의 단일 뮤텍스** — `BlockPool` (§2.2) 과 `GameChannelPool` (§5.6) 둘 다

## 7. 다음 세션이 먼저 볼 것

1. **Session 을 실물로 만든다.** 지금은 `GameRequest.authenticated` 플래그 하나다 —
   codec 이 정하고 dispatcher 가 거른다. 세션 생성·토큰·재바인딩이 들어오면 이 플래그가
   세션 조회로 바뀌고, 세션은 `ChannelId` 를 기억한다 (generation 까지 — 재사용된 슬롯을
   잘못 잡지 않도록). 재바인딩 시 `state_sequence` 가 이어져야 하는지도 그때 정한다.
2. **Worker.** 채널 접근 규칙(§5.6 `access`)은 워커를 전제로 이미 잡혀 있다. 워커는
   채널 접근을 작업의 **가장 마지막**에 두어 락을 쥔 시간을 줄인다. 풀 락은 풀 하나에
   하나(coarse) 이므로 경합이 측정되면 슬롯 단위로 내린다. 워커가 생기면 소켓 없는 테스트
   더블로 `access`/`remove` 교차를 Linux TSan 에서 돌린다.
3. **`ConnectionTable::open` 의 포인터 반환** 을 `Result<SocketCode, Connection&>` 로
   정리한다 (`Endpoint::openConnection` 의 계약과 함께).
4. **`SendBuffer` → `SendQueue` 개명.** 동작 변경 없음.
5. 무엇을 고치든 **세 백엔드 전부** 빌드하고, Linux 두 개는 TSan 과 ASan+UBSan 으로
   돌린다. 이 계층에서 잡힌 버그는 전부 특정 백엔드에서만 또는 새니타이저 아래에서만
   보였고, 리뷰로는 하나도 안 보였다.
