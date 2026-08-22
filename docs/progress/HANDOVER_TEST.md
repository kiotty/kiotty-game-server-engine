# 인수인계 — `test/unit` 유닛 테스트 (cpp-tester)

`core/` 와 `presentation/` 이 `IoLoop` 까지 올라왔고 세 백엔드가 모두 실제 왕복을
통과한다. **그런데 유닛 테스트가 하나도 없다.** 지금까지의 검증은 전부 손으로 만든
스크래치 하네스였고, 그것들은 `test/scratch/` 에 옮겨 두었다.

이 문서는 그 하네스를 정식 유닛 테스트로 바꾸는 다음 세션이 **무엇을, 어떤 계약으로,
어디까지 덮어야 하는지**를 적는다. 코드 자체의 설계는 [HANDOVER.md](HANDOVER.md) 에 있고
여기서는 반복하지 않는다.

## 1. 시작하기 전에 정해야 할 것

프레임워크와 테스트 경계 두 가지가 미정이고, 둘 다 첫 파일을 쓰기 전에 답이 있어야 한다.

### 1.1 프레임워크 — GoogleTest 도입 여부

`test/unit` 이 비어 있고 CMake 에 테스트 프레임워크가 들어와 있지 않다. 지금 있는 것은
`test/integration/kiotty_io_event_listener_smoke.cpp` 하나이며, `add_test` 로 직접 등록된
**프레임워크 없는 실행 파일**이다.

- GoogleTest 를 `FetchContent` 로 들이면 오프라인 빌드가 깨진다. 이 저장소는 지금까지
  외부 의존이 liburing 하나뿐이고 그것도 없으면 epoll 로 떨어진다.
- 스크래치 하네스들이 쓰는 `check(condition, "what")` 패턴은 이미 잘 돌아가고, 결과가
  사람이 읽을 수 있는 한 줄로 나온다.

**사용자에게 물을 것** — GoogleTest 를 들일 것인가, `check()` 패턴을 유지할 것인가.
파라미터 조합을 기계적으로 돌릴 계획이면 `TEST_P` 가 있는 쪽이 유리하고, 의존을 늘리지
않는 것이 우선이면 후자다.

### 1.2 무엇이 유닛 테스트이고 무엇이 아닌가

**`core/` 는 전부 운영체제를 타지 않는다.** 소켓도 스레드 부착도 없고, `BlockPool` 이
`malloc` 을 쓰는 것이 전부다. 여기가 `test/unit` 의 본체다.

**`presentation/` 은 그렇지 않다.** `Connection` 은 `ActiveSocket` 과
`IOMultiEventListener` 를 구체 타입으로 들고 있고 둘 다 인터페이스가 아니다. 소켓 없이
`Connection` 을 세울 방법이 지금은 없다.

| 대상 | 어디에 | 운영체제 의존 |
| --- | --- | --- |
| `Bytes` · `BlockPool` · `RingBuffer` · `PacketHeader` · `Holder` · `Result` | `test/unit` | 없음 |
| `ReceiveBuffer` · `SendBuffer` | `test/unit` | 없음 |
| `Connection` · `Endpoint` · `IoLoop` · `ConnectionTable` | `test/integration` | 루프백 소켓 |

`Connection` 을 억지로 유닛 테스트로 만들려면 `ActiveSocket` 과 `IOMultiEventListener` 를
인터페이스로 뽑아야 하는데, 그것은 테스트를 위해 뜨거운 경로에 가상 호출을 넣는 것이다.
**루프백 통합 테스트로 덮는 쪽을 권한다** (§4).

## 2. `core/` — 계약과 경계값

모듈마다 "깨지면 어디가 조용히 망가지는가"가 다르다. 그 지점이 곧 테스트해야 할 곳이다.

### 2.1 `Bytes` · `BlockPool`

상태가 둘뿐이라는 것(풀도 블록도 없거나, 둘 다 있거나)이 이 타입의 전부다. 그 불변식이
깨지는 경로를 전부 밟는다.

- `Bytes()` · `Bytes(pool, 0)` 이 모두 빈 상태로 떨어진다
- 이동 생성·이동 대입 뒤 **원본이 빈 상태**다 (이중 반납이 여기서 난다)
- 자기 자신에게 이동 대입해도 블록을 잃지 않는다
- 소멸 시 블록이 풀로 돌아간다 — `fallbackCount()` 와 반복 acquire/release 로 확인
- 요청 길이가 **가장 작은 맞는 클래스**로 간다. `64` 는 64 클래스, `65` 는 128 클래스
- 클래스가 비면 위 클래스를 빌리지 않고 **힙으로 간다** — `fallbackCount()` 가 증가
- `65536` 초과 요청이 힙으로 간다
- `release(nullptr)` 이 아무 일도 하지 않는다
- `BlockPool(nullptr, n)` 과 `class_count > MAX_REGIONS` 가 죽지 않는다

`pool_check.cpp` 가 이 항목 대부분을 이미 덮고 있다. 옮겨 오면 된다.

### 2.2 `RingBuffer<T>`

용량 고정 FIFO 라서 **랩어라운드와 슬롯 비우기** 두 가지가 전부다. 이번에 `at()` 과
`removeAt()` 이 추가되었고 **이 둘은 아직 유닛 테스트가 없다.**

- 용량까지 채우고, 가득 찬 뒤 `tryPush` 가 false 이며 **값을 가져가지 않는다**
- `tryPop` 이 슬롯을 `T()` 로 되돌린다 — `RingBuffer<Bytes>` 로 확인해야 의미가 있다
  (소유 타입이 아니면 되돌렸는지 안 되돌렸는지 보이지 않는다)
- 여러 바퀴 돌려도 순서가 유지된다
- `capacity == 0` 이 아무것도 받지 않는다
- **`removeAt(0)`** 이 `tryPop` 과 같은 결과를 낸다
- **`removeAt(size-1)`** 이 뒤를 밀지 않고 지운다
- **랩어라운드 상태에서의 `removeAt(중간)`** — `_head` 가 0이 아닐 때 인덱스 계산이
  틀리는 곳이 여기다. 반드시 head 를 앞으로 밀어 놓고 시험한다
- `removeAt(size)` · `removeAt(size+1)` 이 false 이고 아무것도 바꾸지 않는다
- `removeAt` 이후 마지막 슬롯이 `T()` 로 비워졌다 (`RingBuffer<Bytes>` 로 확인)

### 2.3 `PacketHeader` · `LittleEndian<T>`

**미정렬 접근이 없다는 것**이 이 타입들의 존재 이유다. 컴파일 타임 검사는 헤더에 이미
있으니, 런타임 테스트는 바이트 순서와 오프셋을 본다.

- `set()` 한 값이 `get()` 으로 그대로 나온다 — `uint16_t`/`uint32_t`/`uint64_t` 각각
  `0`, `1`, `최대값`, 그리고 바이트마다 값이 다른 패턴(`0x0102030405060708`)
- 메모리에 실제로 **리틀엔디안 순서로** 놓인다 — `reinterpret_cast<uint8_t*>` 로 확인
- **홀수 오프셋에서 읽고 쓴다.** `alignas(8)` 버퍼의 `+1`, `+3`, `+7` 위치에
  `PacketHeader` 를 놓고 전 필드를 왕복시킨다. UBSan 을 켜고 돌려야 의미가 있다
- `hasPacketMagic` — 정확히 `PACKET_MAGIC` 일 때만 true, 한 바이트만 달라도 false
- `hasSupportedVersion` — major 만 본다. minor 가 달라도 true, major 가 다르면 false
- `payload_length` 는 `uint16_t` 라 상한 검증이 필요 없다. **그 사실 자체를
  `static_assert` 로 남겨 두는 편이 테스트보다 낫다**

### 2.4 `ReceiveBuffer`

상태가 없는 것이 설계의 핵심이므로, 테스트도 **길이가 정확히 맞아떨어지는지**만 본다.
단계 판정과 magic 검사는 여기 없다 — `Connection` 의 몫이다.

- `headerSpace()` 가 처음에 24바이트, 10바이트 받은 뒤 14바이트
- **1바이트씩 24번** 넣어도 `isHeaderComplete()` 가 정확히 24번째에 true
- `openPayload()` 가 `payload_length` 와 **정확히 같은 크기** 블록을 잡는다
- `payload_length == 0` 이면 블록을 잡지 않고 `isPayloadComplete()` 가 즉시 true
- `payloadSpace()` 가 남은 만큼만 내놓는다
- `takePayload()` 가 두 카운터를 되돌리고, 그 뒤 `headerSpace()` 가 다시 24바이트
- **`reset()`** — 이번에 추가되었다. payload 를 놓고 두 카운터를 되돌린다.
  헤더를 절반만 받은 상태에서 `reset()` 한 뒤 `headerSpace()` 가 24바이트인지 확인한다
- 풀이 마른 상태에서 `openPayload()` 가 false 를 돌려준다

`recv_check.cpp` 가 이 중 `reset()` 을 뺀 나머지를 덮는다.

### 2.5 `SendBuffer` · `DropPolicy`

**항목 타입이 `Bytes` 에서 `SentPacket` 으로 바뀌었다.** 큐가 정책을 데이터로 들고,
판단은 `Connection` 이 한다. `dropOldest()` 가 이번에 새로 생겼고 여기가 테스트의 중심이다.

- `tryPush` 가 실패하면 **넘긴 패킷을 가져가지 않는다** (호출자가 다시 쓸 수 있다)
- `pop()` 이 빈 큐에서 빈 `SentPacket` 을 준다
- `dropOldest(Oldest)` 가 **가장 오래된 `Oldest` 항목 하나만** 지운다. 큐 앞쪽에 `Never`
  가 있어도 그것을 건너뛰고 뒤의 `Oldest` 를 찾아간다
- `Never` 만 들어 있는 큐에서 `dropOldest(Oldest)` 가 false
- 빈 큐에서 false
- **드롭 이후 남은 것들의 순서가 유지된다** — `Never, Oldest, Never, Oldest` 를 넣고
  하나 드롭한 뒤 `Never, Never, Oldest` 순으로 나오는지 본다
- 소멸 시 큐에 남은 블록이 전부 풀로 돌아간다 (`fallbackCount()` 대신 `reservedBytes()`
  가 아니라, 같은 크기를 다시 다 잡을 수 있는지로 확인한다)

`connection_check.cpp` 의 `checkSendQueue()` 가 이것들을 이미 돌리고 있다.

### 2.6 `Holder<Base>` · `Result<E, T>`

둘 다 힙 없이 다형성/에러를 다루는 도구라서, **소멸자가 정확히 한 번 불리는지**가 유일한
관심사다. 카운터를 든 테스트 타입 하나로 전부 덮인다.

- `Holder` — `make<Derived>()` 후 소멸 시 `~Derived` 가 한 번. 이동 후 원본은 비고
  소멸자가 두 번 불리지 않는다. `reset()` 이 멱등이다
- `Result` — `ok(v)` 는 `isOk()`, `error(e)` 는 `code() == e`. 실패 Result 는 `Value` 를
  **생성하지 않는다** (생성자 카운터로 확인). 복사·이동 대입의 네 조합
  (ok←ok, ok←fail, fail←ok, fail←fail)에서 소멸자 수가 맞는다

`core_check.cpp` 가 이 둘을 덮고 있다.

## 3. 파라미터 조합 — 군 분류는 컨펌을 받는다

`cpp-tester` 규약대로 enum × 숫자 군 × string 군을 빠짐없이 덮되, **군을 어떻게 나눌지는
사용자에게 먼저 확인한다.** 이 저장소에서 나눌 축은 다음과 같다.

| 축 | 잠정 군 |
| --- | --- |
| 블록 요청 길이 | `0` / `1` / 클래스 경계 정확값(`64`,`128`,…,`65536`) / 경계-1 / 경계+1 / `65537`(힙) |
| payload_length | `0` / `1` / `24` / `65535` |
| 큐 용량 | `0` / `1` / `2` / 설정 기본값 |
| 수신 분할 | 한 번에 전부 / 헤더·payload 둘로 / **1바이트씩** / 헤더 중간에서 끊김 |
| `DropPolicy` | `Never` / `Oldest` / 둘이 섞인 큐 |
| 정렬 오프셋 | `0` / `1` / `3` / `7` |

**`0` 과 `1` 은 어느 축에서도 빼지 않는다.** 지금까지 이 계층에서 나온 문제는 전부 그
둘이거나 랩어라운드였다.

## 4. `Connection` — 루프백 통합 테스트로 덮는다

소켓 없이 세울 수 없으므로 `test/integration` 에 둔다. 이미 돌아가는 하네스가 있으니
그것을 기준으로 시나리오를 늘리는 것이 가장 빠르다.

`test/scratch/connection_check.cpp` 가 실제 `Endpoint` + `IOMultiEventListener` +
`Connection` 을 세우고 진짜 클라이언트 소켓으로 왕복시킨다. 세 백엔드 전부에서 통과하고
TSan · ASan+UBSan 도 통과한다. 지금 덮고 있는 것:

- 헤더와 payload 를 **따로 보내는** 분할 수신 (`ReadingPayload` 경로)
- payload 왕복과 `correlation_id` 보존
- `emit()` → 송신 → 클라이언트 수신
- **magic 이 틀린 헤더 → 연결 종료 → `onDisconnected` 한 번**

아직 안 덮은 것, 그리고 늘려야 할 것:

- **1바이트씩 보내는 클라이언트.** 헤더 24회 + payload n회. 지금 가장 가치 있는 추가다
- **연속 패킷.** 한 번의 write 에 패킷 두 개를 붙여 보낸다. 커널에 남은 바이트가 다음
  수신으로 정확히 넘어가는지 — 길이를 딱 맞춰 요청하는 설계의 핵심 주장이 이것이다
- **`payload_length == 0`** 패킷
- **부분 전송.** 소켓 버퍼보다 큰 패킷을 보내 `onSent` 가 여러 번 오게 만든다.
  `_sent` 누적과 나머지 재제출 경로가 여기서만 돈다
- **큐 넘침.** `send_queue_limit` 을 1~2 로 잡고 `emit()` 을 연달아 불러 `Oldest` 드롭과
  `Never` 종료 두 갈래를 각각 확인한다
- **다른 스레드에서 `emit()`.** `SendState` 전이가 있는 이유가 이것이다. TSan 필수
- **다른 스레드에서 `close()`**
- 버전 major 가 다른 헤더 → 종료
- 클라이언트가 헤더 도중에 끊음 → `onDisconnected` 한 번

`Endpoint` 는 별도 테스트가 거의 필요 없다. 생성자가 socket·bind·listen 을 다 끝내므로
확인할 것은 **포트 0 이 커널 선택 포트로 채워지는가**와 **이미 쓰는 포트에서
`SOCKET_ADDRESS_IN_USE` 가 나오는가** 둘뿐이고, 나머지는 위 시나리오가 매번 지나간다.

### 4.1 `IoLoop` · `ConnectionTable`

`test/scratch/io_loop_check.cpp` 가 `EchoConnection` 하나로 서버를 세우고 클라이언트 여럿을
붙인다. 세 백엔드 · TSan · ASan+UBSan 전부 통과한다. 지금 덮고 있는 것:

- 동시 접속 셋이 각각 자기 슬롯을 받는다
- **접속 한도** — 표가 꽉 찬 상태의 네 번째가 거절되고, 거절이 기존 커넥션을 건드리지 않는다
- 전부 끊기면 표가 0으로 돌아온다
- **한 번 쓴 슬롯을 다시 써서** 왕복이 된다
- 각 커넥션이 자기 `correlation_id` 를 되돌려준다 (버퍼를 공유하지 않는다는 증거)
- 다른 스레드의 `stop()` 이 `run()` 을 끝낸다

늘려야 할 것:

- **표를 꽉 채운 뒤 하나만 끊고 새로 붙인다.** 빈 슬롯 탐색이 중간 구멍을 찾는지
- **`max_connections == 0`** 과 **`== 1`**
- `onOpened` 안에서 바로 `emit()` — 아직 아무것도 안 받은 커넥션에 보내기
- `onClosed` 가 커넥션이 파괴되기 **전에** 불리는지 (거기서 소켓 주소를 읽어 확인한다)
- **엔드포인트가 죽었을 때** `run()` 이 빠져나오고 `code()` 가 이유를 말하는지
- `run()` 전에 `stop()` 을 부르면 `run()` 이 즉시 돌아온다. 멈춘 루프는 멈춘 채로 있다
- **`reapClosed()` 가 콜백 밖에서만 돈다.** 끊긴 커넥션이 `onClosed` 직후가 아니라 다음
  `wait()` 사이에 파괴되는지 — ASan 으로 돌려야 반대 경우가 잡힌다
- **파생 커넥션이 자기 상태를 지킨다.** 슬롯을 재사용해도 이전 커넥션의 멤버가 새 것에
  비치지 않는지 (`ConnectionTable` 이 리셋이 아니라 새로 짓는다는 주장)

## 5. 회귀로 박아 둘 것 — 이번에 잡힌 버그 넷

넷 모두 리뷰로는 안 보였고 하네스를 돌려서 나왔다. 유닛 테스트가 생기면 **이 넷을 먼저
고정한다.**

### 5.1 떠 있는 연산이 없으면 `onDisconnected` 가 영영 안 온다

헤더 검증에 실패했을 때 소켓을 먼저 닫고 다음 수신을 걸지 않으면, 완료될 것이 하나도
없어 리스너가 끊김을 알릴 방법을 잃는다. **`Connection` 은 활성인 동안 항상 수신 하나를
띄워 둔다**로 고쳤다.

회귀 시나리오 — magic 이 틀린 헤더를 보내고 `onDisconnected` 가 **정해진 시간 안에**
오는지 본다. 오지 않는 것이 원래 증상이므로 타임아웃이 곧 실패 판정이다.

### 5.2 epoll 이 `cancel()` 을 못 듣는다

wakeup eventfd 를 epoll 에 등록할 때 엔진의 `IOEventKind` 자리에 `EPOLLIN` 을 넘기고
있었다. 두 값의 비트가 다르므로 관심 비트가 0으로 계산되어 **eventfd 가 영원히 보고되지
않았다.** `IO_EVENT_RECEIVE` 로 고쳤다.

회귀 시나리오 — 콜백 안에서 `cancel()` 을 부르고 다음 `wait()` 가 `CANCELLED` 를
돌려주는지, `onInterrupted` 가 오는지 본다. 세 백엔드 전부에서 돌려야 잡힌다.

### 5.3 IOCP 에서 accept 를 다시 걸면 그 소켓이 닫혔다

AcceptEx 는 다음 연결용 소켓을 미리 만들어 `response.accepting` 에 park 한다. 그런데
IOCP 디스패치는 콜백이 끝난 뒤 그 필드에 남은 핸들을 "안 가져간 것"으로 보고 닫았다.
**콜백 안에서 accept 를 다시 거는 순간 방금 무장한 accept 의 소켓이 닫혔고**, 그래서 첫
손님 이후로 아무도 못 들어왔다. 콜백 전에 제시한 핸들을 기억해 두고 **그것이 그대로 남아
있을 때만** 닫도록 고쳤다.

지금까지 어떤 테스트도 accept 를 두 번 걸지 않아서 살아 있던 버그다.

회귀 시나리오 — 클라이언트 둘 이상을 붙인다. 하나만 붙이는 테스트로는 절대 안 잡힌다.

### 5.4 epoll 에서 서버가 먼저 끊으면 완료가 사라진다

fd 를 닫으면 epoll 집합에서 조용히 빠지므로 걸려 있던 수신이 아무 통지 없이 증발한다.
`ActiveSocket::close()` 를 **`shutdown()`** 으로 바꿔, 핸들은 소멸자가 계속 소유하고
서버발 종료는 `shutdown(SHUT_RDWR)` 로 처리하게 했다. 세 백엔드 모두에서 걸려 있던
연산이 실패로 완료된다.

회귀 시나리오 — 서버가 먼저 `Connection::close()` 를 부르고 `onDisconnected` 가 오는지.
클라이언트가 끊는 경로와 **반드시 따로** 시험한다. 지금까지 통합 스모크는 클라이언트가
끊는 쪽만 덮고 있었고 그래서 이 버그가 살아 있었다.

## 6. `test/scratch/` — 물려받는 하네스

정식 테스트로 옮기고 나면 지운다. 그때까지는 이것이 이 계층의 유일한 검증이다.

| 파일 | 무엇을 덮는가 | 상태 |
| --- | --- | --- |
| `core_check.cpp` | `Bytes` · `Holder` · `Result` · `RingBuffer` · `SendBuffer` | **`SendBuffer` 절이 컴파일되지 않는다** |
| `pool_check.cpp` | `BlockPool` 크기 클래스 · 힙 fallback | 그대로 돈다 |
| `recv_check.cpp` | `ReceiveBuffer` 전 경로 | 그대로 돈다 |
| `connection_check.cpp` | `SendBuffer`+`DropPolicy`, `Connection` 루프백 | 세 백엔드 통과 |
| `io_loop_check.cpp` | `IoLoop` · `ConnectionTable` — 동시 접속, 접속 한도, 슬롯 회수·재사용, 외부 stop | 세 백엔드 통과 |
| `pool_bench.cpp` | 풀 대 malloc 지연 분포 | 테스트가 아니라 측정용 |

`core_check.cpp` 의 `checkSendBuffer()` 와 `checkSendBufferReleasesOnDestruction()` 은
`SendBuffer` 가 `Bytes` 를 담던 시절의 API 를 쓴다. 항목 타입이 `SentPacket` 으로
바뀌었으므로 그 두 함수만 고치면 되고, 고치는 김에 `connection_check.cpp` 의
`checkSendQueue()` 쪽으로 합치는 편이 낫다.

**어느 파일도 CMake 에 등록되어 있지 않다.** 손으로 컴파일해서 돌린다.

```
# Linux
g++ -std=c++14 -Wall -Wextra -Wpedantic -g -I src/implementation \
    test/scratch/connection_check.cpp -o cc <build>/libkiotty.a -lpthread [-luring]

# Windows (VsDevCmd 안에서)
cl /std:c++14 /W4 /EHsc /MDd /I src\implementation \
   test\scratch\connection_check.cpp /link <build>\kiotty.lib ws2_32.lib
```

## 7. 돌릴 때 지킬 것

이 계층에서 나온 문제는 전부 특정 백엔드에서만, 또는 새니타이저 아래에서만 보였다.
**한 곳에서 통과한 것을 통과했다고 보고하지 않는다.**

- **세 백엔드 전부** 돌린다 — Windows/IOCP, Linux/io_uring, Linux/epoll.
  epoll 은 `-DKIOTTY_USE_IO_URING=OFF` 로 따로 구성해야 나온다
- Linux 두 개는 **TSan** 과 **ASan+UBSan** 으로 각각 돌린다
- TSan 은 WSL 에서 `setarch $(uname -m) -R` 로 ASLR 을 끄고 돌린다
- **WSL 은 유휴 상태에서 `/tmp` 를 지운다.** 빌드 디렉토리는 홈 아래에 잡는다
- 워닝 0 을 유지한다 — `-Wall -Wextra -Wpedantic` / `/W4`
- 못 돌린 조합이 있으면 **못 돌렸다고 적는다.** 통과로 넘기지 않는다
