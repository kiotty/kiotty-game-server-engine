# C++ 게임 서버 개발 지시서 (kiotty-mini-game) — kiotty 엔진판

> **이 문서는 서버 개발자의 AI 에이전트(Claude)에게 그대로 전달하는 프롬프트다.**
> 이 문서 하나와 이 저장소의 `src/`(kiotty 엔진)만으로 서버를 완성할 수 있도록 필요한 정보를 모두 담았다.
> 클라이언트 저장소에 접근할 필요는 없다.
>
> 원본: 클라이언트(Godot 4.7) 팀 / 프로토콜 버전 **2** / 2026-08-15
> 개정: 2026-08-22 — 자체 소켓 계층 대신 **kiotty 엔진(`src/`) 위에 올리도록** 전송 계층·구조·구현 순서를 바꿨다.
> 게임 규칙(§5·§6의 검증 표·§9·§10)은 원본 그대로다.

> **클라이언트 팀에 통보가 필요한 변경 (이 개정으로 생김)**
>
> 1. 프레이밍이 6바이트 헤더(`u32 payloadLen + u16 msgId`)에서 **엔진의 24바이트 고정 헤더**(§2)로 바뀐다.
>    엔진의 헤더 파서는 엔진 인터페이스이므로 서버 쪽에서 바꿀 수 없다.
> 2. 서버→클라이언트 메시지가 **응답(Response)** 과 **이벤트(Event)** 로 나뉘고 헤더 `flags`로 구분된다(§2.4).
> 3. 그래서 `protocolVersion`(HELLO 페이로드)을 **2 → 3** 으로 올린다. 클라이언트가 보내는 값이 3이 아니면 `KICK(VersionMismatch)`.
>
> 이 세 가지가 합의되기 전까지 실제 클라이언트 검증(§9.1)은 통과하지 않는다. 그 전까지는 §9.2의 자체 테스트 클라이언트로 검증한다.

---

## 0. 당신의 역할과 목표

당신은 2D 멀티플레이 게임의 **C++ 서버**를 kiotty 엔진 위에 개발한다.
클라이언트(Godot 4.7, 별도 팀)는 **이미 완성되어 동작 중**이며, 위 통보 사항을 반영하면 이 문서의 프로토콜을 그대로 구현한다.
**클라이언트 게임 규칙은 수정할 수 없다.** 서버가 이 문서에 맞춰야 한다.

최종 목표: **4명이 동시에 접속해** 각자 캐릭터를 고르고, 초원을 돌아다니며 서로를 공격하고,
상자를 열어 아이템을 얻고, 그 아이템을 사용해 체력을 회복하고,
**클라이언트를 강제 종료한 뒤 재접속해도 인벤토리가 남아 있는** 상태.

### 절대 원칙

1. **서버가 authoritative.** 좌표, HP, 사망, 공격 판정, 상자 개방, 아이템 지급/소모, 인벤토리는
   전부 서버가 결정한다. 클라이언트는 "의도"만 보낸다(방향 입력, 공격, 상호작용 대상 id, 사용할 슬롯).
2. **클라이언트 값을 신뢰하지 않는다.** 클라이언트는 좌표나 수량을 주장하지 않으며,
   설령 조작된 패킷이 와도 서버가 거리·쿨다운·중복·상한을 검증해 막아야 한다.
3. **인원수를 하드코딩하지 않는다.** 지금 목표는 4명이지만, 자료구조는 임의 인원으로 확장 가능해야 한다.
   정원은 조립 시점의 숫자 셋(`ConnectionTable`·`GameChannelPool`·`SessionRepository` 용량)으로만 정한다.
4. **영속 데이터는 서버 DB 가 기준이다.** 클라이언트 로컬 저장은 인벤토리 근거로 쓰지 않는다.
5. **엔진(`src/`)의 코드는 한 줄도 바꾸지 않는다.** 엔진이 제공하는 것은 그대로 쓰고, 없는 것은 게임 쪽 코드에 만든다.
   엔진에 손대야만 풀리는 문제가 나오면 **고치지 말고 §11의 "엔진에 요청할 것"에 적고 보고한다.** 이미 알려진 것이 거기 있다.

---

## 1. 개발 환경 (확정 사항)

- 언어/표준: **C++11** (엔진의 `CMAKE_CXX_STANDARD 11`, 확장 OFF 를 그대로 따른다).
  `std::optional`·`string_view`·구조적 바인딩·`if constexpr` 은 쓸 수 없다. 엔진 `Result<E,T>` 와 `bool tryGet(T&)` 로 대신한다.
- 빌드: **CMake** (Windows MSVC / Linux GCC 양쪽). 빌드 명령은 저장소의 `build` 스킬을 따른다.
- 네트워크: **kiotty 엔진이 전부 담당한다.** 소켓·accept·IO 다중화(IOCP / epoll / io_uring)·헤더 파싱·부분 송수신·
  클라이언트별 송신 큐·접속 상한·커넥션↔채널 바인딩이 엔진에 있다. **게임 코드는 소켓 API 를 한 줄도 부르지 않는다.**
  - `select()` 루프, 리스너, 송신 큐, 누적 버퍼, `Connection` 파생 클래스를 **새로 만들지 않는다.** `Connection` 은 상속 대상이 아니다.
  - `TCP_NODELAY` 는 엔진이 설정하지 않는다 (§11 엔진 요청 1번). 게임 코드에서 `setsockopt` 를 부르지 않는다.
- 영속화: 엔진의 **`SqliteDataSource`** (엔진 CMake 가 SQLite amalgamation 을 FetchContent 로 받는다. `KIOTTY_WITH_SQLITE=ON`).
  **키-값 저장소다.** 테이블·SQL 을 직접 쓰지 않는다 (§7).
- 비밀번호 해시: 엔진의 **`PasswordHasher`** (Argon2, 엔진 CMake 가 받는다). SHA-256 을 직접 구현하지 않는다.
- 그 외 외부 의존성 없음. C++ 표준 라이브러리 + kiotty 엔진만 사용.

### 1.1 엔진이 제공하는 것 (그대로 쓴다)

| 필요한 것 | 엔진 타입 | 헤더 |
| --- | --- | --- |
| 조립: 리스닝·accept·IO 루프 | `Endpoint`, `IoLoop` | `presentation/endpoint/kiotty_endpoint.h`, `presentation/io_loop/kiotty_io_loop.h` |
| 커넥션 슬롯 + 접속 상한 | `ConnectionTable(capacity, pool, send_queue_limit, binder, codec)` | `presentation/connection/kiotty_connection_table.h` |
| 커넥션 ↔ 채널 바인딩 | `IChannelBinder`, 기본 구현 `ChannelPoolBinder(pool, sessions, request_listener)` | `domain/channel/kiotty_channel_binder.h` |
| 패킷 ↔ 요청/응답/이벤트 | `IPacketCodec`, 기본 구현 `DefaultPacketCodec` | `domain/codec/kiotty_packet_codec.h` |
| 커넥션 1개의 통로 | `GameChannel` → `BusinessGameChannel { channel_id, request, response, event }` | `domain/channel/kiotty_game_channel.h` |
| 채널 찾기 (스레드 안전) | `GameChannelPool::access(ChannelId)` → `ChannelAccess` (풀 락을 쥔 채 채널을 준다) | `domain/channel/kiotty_game_channel_pool.h` |
| 커넥션 식별자 | `ChannelId { index, generation }` — 슬롯 재사용을 세대로 구분 | `domain/entity/kiotty_channel_id.h` |
| 요청 데이터 | `GameRequest { state_sequence, channel_id, correlation_id, command, payload }` | `domain/entity/kiotty_game_request.h` |
| 응답/이벤트 데이터 | `GameResponse { correlation_id, command, payload }`, `GameEvent { command, payload }` | `domain/entity/kiotty_game_response.h`, `kiotty_game_event.h` |
| 메시지 핸들러 | `IUsecase::execute(const GameRequest&, BusinessGameChannel&)`, `IPublicUsecase` (세션 없이 허용) | `domain/usecase/kiotty_usecase.h` |
| command → usecase 표, 세션 게이트 | `UsecaseRegistry(vector<Holder<IUsecase>>)`, `UsecaseDispatcher(registry, channels, sessions)` | `domain/usecase/kiotty_usecase_registry.h`, `kiotty_usecase_dispatcher.h` |
| 세션 (로그인 상태) | `SessionRepository(channels, policy, random, max_sessions)`: `open / find / close / detach`, `ISessionPolicy` | `datalayer/repository/session/kiotty_session_repository.h`, `kiotty_session_policy.h` |
| 세션 핸들 | `Session { account(), channel(), reply(), notify() }` | `datalayer/repository/session/kiotty_session.h` |
| 계정 식별자 | `AccountId` (최대 63자 이름), `tryMakeAccountId` | `domain/entity/kiotty_account_id.h` |
| 비밀번호 해시 | `PasswordHasher(params, random)`: `hashBlocking`, `matchesBlocking`, `PasswordHash` (encoded 문자열 160B) | `datalayer/repository/cryptor/kiotty_password_hasher.h` |
| 난수 | `SecureRandom : IRandomSource` | `datalayer/repository/cryptor/kiotty_secure_random.h` |
| 영속 저장 | `IDataSource { readBlocking(key) → Bytes, writeBlocking(key, value) }`, `SqliteDataSource(path, pool)`, `InMemoryDataSource(pool)` | `datalayer/datasource/*.h` |
| 버퍼 메모리 | `BlockPool`, `Bytes`(move-only), `ByteView`, `ByteSpan` | `core/kiotty_block_pool.h`, `core/kiotty_bytes.h` |
| 헤더 필드 / 리틀엔디안 | `LittleEndian<T>`, `PacketHeader`, `PACKET_FLAG_EVENT` | `core/kiotty_packet_concept.h` |
| 힙 없는 다형 객체 | `Holder<IUsecase>::make<T>(...)` (`sizeof(T) <= 64`) | `core/kiotty_holder.h` |
| 에러 값 반환 | `Result<ErrorCode, T>`, `ok()`, `error()` | `core/kiotty_result.h` |
| 고정 용량 FIFO | `RingBuffer<T>` | `core/kiotty_ring_buffer.h` |

조립 순서의 실물은 `test/scratch/usecase_check.cpp` 의 `main()` 이다. **그 순서를 그대로 베낀다** (§3.5).
설계 원문은 [docs/requirement.md](../docs/requirement.md) 다. 세션 유스케이스 S1~S20 과 설계 가이드라인 2·8번을 읽고 시작한다.

### 1.2 엔진에 없어서 게임 쪽에 만드는 것

| 필요한 것 | 만들 것 | 비고 |
| --- | --- | --- |
| payload 필드 단위 읽기/쓰기 (`u8/u16/u32/i32/f32/str`) | `PayloadReader`, `PayloadWriter` | 경계 검사 필수. `f32` 는 `memcpy` → `uint32_t` → `LittleEndian<uint32_t>`. 쓰기 결과는 `Bytes(pool, n)` |
| 20Hz 틱 | 틱 스레드 1개 | 엔진 `IoLoop` 에는 타이머 훅이 없다 (§3.4) |
| 메시지별 핸들러 | `IUsecase` 구현 10개 (§3.3) | 엔진 `UsecaseRegistry` 에 등록 |
| 게임 상태 | `GameWorld` (플레이어·상자·뮤텍스 1개) | 모든 usecase 와 틱이 이것 하나를 공유 |
| 세션 정책 | `MiniGameSessionPolicy : ISessionPolicy` | 고아 수명 0, 중복 로그인 거절 (§4) |
| 영속 레코드 직렬화 | `AccountRecord`, `InventoryRecord`, `ChestRecord` ↔ `Bytes` | `IDataSource` 는 바이트만 안다 (§7) |
| 플레이어·월드·전투·인벤토리 규칙 | `game/` | 원본 구조 유지 |

### 1.3 파일 구조

게임 프로젝트는 엔진 저장소 안의 **`prj/mini_game/`** 에 둔다 (엔진 `src/` 와 분리. 루트 `CMakeLists.txt` 에 `add_subdirectory(prj/mini_game)` 한 줄만 추가한다 — 엔진 코드 변경이 아니라 빌드 등록이다).
파일 접두어는 엔진의 `kiotty_` 와 겹치지 않게 **`kmg_`**, 네임스페이스는 `kmg` 다.

```
prj/mini_game/
  CMakeLists.txt                   kmg_server 실행 파일. kiotty 에 link. KIOTTY_HAS_SQLITE 필수
  src/
    kmg_main.cpp                   진입점, 포트/DB 경로 파싱, §3.5 조립, 틱 스레드 시작/종료
    net/
      kmg_protocol.h               메시지 ID / 열거형 / 상수 (§2·§3)
      kmg_payload_reader.h         리틀엔디안 역직렬화 + 경계 검사
      kmg_payload_writer.h         리틀엔디안 직렬화 → Bytes
    usecase/
      kmg_hello_usecase.{h,cpp}    IPublicUsecase  HELLO → WELCOME / KICK
      kmg_login_usecase.{h,cpp}    IPublicUsecase  LOGIN → AUTH_RESULT, CHARACTER_LIST
      kmg_ping_usecase.{h,cpp}     IPublicUsecase  PING → PONG
      kmg_select_character_usecase.{h,cpp}
      kmg_move_input_usecase.{h,cpp}
      kmg_attack_usecase.{h,cpp}
      kmg_interact_chest_usecase.{h,cpp}
      kmg_request_chunks_usecase.{h,cpp}
      kmg_respawn_usecase.{h,cpp}
      kmg_use_item_usecase.{h,cpp}
    game/
      kmg_game_world.{h,cpp}       게임 상태 소유자. 뮤텍스 1개, 틱, 브로드캐스트, 퇴장 처리
      kmg_player.{h,cpp}           플레이어 상태(위치/방향/모션/HP/입력/쿨다운)
      kmg_world_map.{h,cpp}        청크/상자 상태, 청크 조회
      kmg_combat.{h,cpp}           공격 판정 (순수 함수)
      kmg_inventory.{h,cpp}        슬롯 인벤토리 로직 (순수 함수)
      kmg_session_policy.h         MiniGameSessionPolicy
    db/
      kmg_records.{h,cpp}          AccountRecord / InventoryRecord / ChestRecord ↔ Bytes, 키 문자열 규칙
      kmg_account_store.{h,cpp}    IDataSource 위의 계정·인벤토리·상자 조회/저장
  test/
    kmg_test_client.cpp            §9.2 자체 검증 클라이언트
    unit/                          PayloadReader/Writer, Inventory, Combat, Records 의 GoogleTest (cpp-tester 에게 위임)
```

---

## 2. 전송 계층 규격

- **TCP**, 기본 포트 **9000** (실행 인자로 변경 가능하게 할 것). `Endpoint("0.0.0.0", port, backlog, table)`.
- **리틀엔디안 고정** (헤더·payload 모두).

### 2.1 프레이밍 — 엔진 헤더 24바이트 고정

```
+-------+----------------+-----------+---------+-------+---------+----------------+-------------------+
| magic | correlation_id | timestamp | command | flags | version | payload_length | payload (N bytes) |
|  u32  |      u32       |    u64    |   u16   |  u16  |   u16   |      u16       |                   |
+-------+----------------+-----------+---------+-------+---------+----------------+-------------------+
   0        4                8          16        18      20         22              24
```

| 필드 | 값 | 누가 정하나 |
| --- | --- | --- |
| `magic` | `0x544F494B` (와이어: `4B 49 4F 54`, "KIOT") | 고정. 틀리면 엔진이 연결을 끊는다 |
| `correlation_id` | 요청: 클라이언트가 발급(커넥션 안에서 유일). 응답: 요청 값 **반사**. 이벤트: 커넥션별 단조 증가 (1부터) | 엔진 (`DefaultPacketCodec`·`Connection::EventListener`) |
| `timestamp` | 서버 단조 시각 µs. 클라이언트→서버는 **0** 으로 채운다 | 서버 송신은 엔진 `writePacket` 이 찍는다 |
| `command` | 메시지 ID (§3). 원본의 `msgId` 그대로 | 게임 |
| `flags` | bit0 `EVENT` = 1 이면 서버 이벤트. 나머지 비트는 0 | 엔진 |
| `version` | **엔진 헤더 버전** `0x0001` (major 1, minor 0). 게임 `protocolVersion` 과 다른 것이다 | 고정 |
| `payload_length` | payload 바이트 수. **command 를 포함하지 않는다.** 최대 65535 | 게임 |

- 프레임 전체 크기는 `24 + payload_length`.
- `magic` 불일치·`version` major 불일치는 **엔진이** 연결을 끊는다. 게임 코드가 검사할 것은 `command` 와 payload 내용뿐이다.
- TCP 는 스트림이지만 **누적 버퍼는 엔진이 한다.** usecase 에는 헤더와 payload 가 완성된 패킷 하나가 `GameRequest` 로 온다.

### 2.2 바이트 예시 (그대로 검증에 사용하라)

클라이언트 → 서버 `HELLO` (correlation_id=1, protocolVersion=3, clientBuild=1):

```
4B 49 4F 54               magic = 0x544F494B
01 00 00 00               correlation_id = 1
00 00 00 00 00 00 00 00   timestamp = 0 (클라이언트는 0)
01 00                     command = 0x0001 HELLO
00 00                     flags = 0
01 00                     version = 1.0
04 00                     payload_length = 4
03 00                     protocolVersion = 3
01 00                     clientBuild = 1
```

서버 → 클라이언트 `WELCOME` (응답, correlation_id=1 반사, protocolVersion=3, tickRate=20, serverTimeMs=1000):

```
4B 49 4F 54               magic
01 00 00 00               correlation_id = 1 (반사)
xx xx xx xx xx xx xx xx   timestamp (서버 단조 시각, 클라이언트는 무시해도 된다)
01 80                     command = 0x8001 WELCOME
00 00                     flags = 0 (응답)
01 00                     version = 1.0
08 00                     payload_length = 8
03 00                     protocolVersion = 3
14 00                     tickRate = 20
E8 03 00 00               serverTimeMs = 1000
```

서버 → 클라이언트 `SNAPSHOT` (이벤트, 커넥션 시퀀스 7, 플레이어 0명):

```
4B 49 4F 54
07 00 00 00               correlation_id = 이벤트 시퀀스 7
xx xx xx xx xx xx xx xx
05 80                     command = 0x8005 SNAPSHOT
01 00                     flags = EVENT
01 00
06 00                     payload_length = 6
2A 00 00 00               tick = 42
00 00                     count = 0
```

클라이언트 → 서버 `LOGIN` (correlation_id=2, accountId="u1", password="p"):

```
4B 49 4F 54  02 00 00 00  00 00 00 00 00 00 00 00  02 00  00 00  01 00  07 00
02 00 75 31               str "u1"  (u16 길이 2 + UTF-8 바이트)
01 00 70                  str "p"
```

### 2.3 기본 타입

| 표기 | 크기 | 설명 |
|---|---|---|
| `u8` `u16` `u32` | 1/2/4 | 부호 없는 정수 (리틀엔디안) |
| `i32` | 4 | 부호 있는 정수 (2의 보수, 리틀엔디안) |
| `f32` | 4 | IEEE-754 단정도 (리틀엔디안) |
| `str` | 2+N | `u16 byteLen` + UTF-8 바이트. 널 종료 없음. 최대 255바이트 |

> **주의**: 구조체를 `memcpy` 로 바로 보내지 마라. C++ 구조체는 패딩이 들어가서 레이아웃이 어긋난다.
> 반드시 필드 단위로 쓰고 읽어라(`PayloadWriter` / `PayloadReader`). 헤더는 엔진이 쓴다.

### 2.4 응답과 이벤트 — 엔진이 정해 둔 구분

`BusinessGameChannel` 의 두 sink 가 곧 두 경로다. **헤더 규칙과 드롭 정책은 엔진 `Connection` 이 정하고 게임은 고를 수 없다.**

| sink | `correlation_id` | `flags` | 송신 큐가 꽉 찼을 때 (`Connection::emit`) |
| --- | --- | --- | --- |
| `channel.response.emit(GameResponse)` | 게임이 넣는 값 — **요청 값을 반사한다** | 0 | `DropPolicy::Never` — 큐의 `Oldest` 항목을 하나 버리고 넣는다. 그래도 자리가 없으면 **연결 종료** |
| `channel.event.emit(GameEvent)` | 엔진이 커넥션별 시퀀스 발급 | `EVENT` | `DropPolicy::Oldest` — 자기보다 오래된 이벤트를 버리고 넣거나, 안 되면 **자기 자신을 버린다** |

**이벤트는 전부 버려질 수 있는 경로다.** `DEATH`·`INVENTORY_DELTA`·`KICK` 도 예외가 아니다. 이 게임의 트래픽(4인, 20Hz 스냅샷 60바이트)에서
큐가 차는 일은 클라이언트가 멈춘 경우뿐이므로 **`send_queue_limit` 을 256 으로 잡고** 받아들인다. 이벤트별 드롭 정책은 §11 엔진 요청 3번.

클라이언트 수신 구조는 `flags.EVENT` 로 갈라 응답은 `correlation_id → 대기 슬롯`, 이벤트는 큐로 받는 것을 전제한다
([docs/requirement.md](../docs/requirement.md) 설계 가이드라인 9번). **요청 하나에 응답을 둘 보내지 마라** — 두 번째부터는 이벤트로 보낸다.

### 2.5 열거형

```
Facing      : 0 = Down, 1 = Up, 2 = Left, 3 = Right
Motion      : 0 = Idle, 1 = Walk, 2 = Attack, 3 = Dead
AuthError   : 0 = None, 1 = BadCredentials, 2 = AlreadyOnline, 3 = ServerFull,
              4 = VersionMismatch, 5 = InternalError
LeaveReason : 0 = Disconnect, 1 = Timeout, 2 = Kick
ChestError  : 0 = None, 1 = TooFar, 2 = AlreadyOpened, 3 = InventoryFull, 4 = UnknownChest
ItemUseError: 0 = None, 1 = EmptySlot, 2 = NotUsable, 3 = ItemMismatch,
              4 = Dead, 5 = AlreadyFullHp
RespawnRule : 0 = SameCharacterAtSpawn, 1 = MustReselectCharacter
```

메시지 ID 규칙: `0x0001~0x7FFF` = Client→Server, `0x8001~0xFFFF` = Server→Client. 헤더 `command` 에 그대로 들어간다.
`IUsecase::command()` 가 돌려주는 값이 곧 Client→Server ID 다.

---

## 3. 메시지 전체 목록 (필드 순서가 곧 바이트 순서다)

### 3.1 Client → Server

| ID | 이름 | 페이로드 | usecase 종류 |
|---|---|---|---|
| `0x0001` | HELLO | `u16 protocolVersion`, `u16 clientBuild` | `IPublicUsecase` |
| `0x0002` | LOGIN | `str accountId`, `str password` | `IPublicUsecase` |
| `0x0003` | SELECT_CHARACTER | `u16 characterId` | `IUsecase` |
| `0x0004` | MOVE_INPUT | `u32 seq`, `f32 dirX`, `f32 dirY`, `u32 clientTimeMs` | `IUsecase` |
| `0x0005` | ATTACK | `u32 seq`, `u8 facing` | `IUsecase` |
| `0x0006` | INTERACT_CHEST | `u32 chestId` | `IUsecase` |
| `0x0007` | REQUEST_CHUNKS | `u16 count`, `count × { i32 chunkX, i32 chunkY }` | `IUsecase` |
| `0x0008` | PING | `u32 clientTimeMs` | `IPublicUsecase` |
| `0x0009` | RESPAWN_REQUEST | (없음) | `IUsecase` |
| `0x000A` | USE_ITEM | `u8 slot`, `u16 itemId` | `IUsecase` |

`IUsecase`(세션 필요)는 **엔진 `UsecaseDispatcher` 가 `SessionRepository::find(channel_id)` 로 거른다.** 로그인 전 게임 메시지는 usecase 에 도달하지 않는다.
게임 코드는 "로그인했는가"를 다시 검사하지 않는다 — 대신 "HELLO 를 거쳤는가"(LOGIN 에서)와 "월드에 들어왔는가"(그 이후)는 `GameWorld` 가 검사한다 (§3.3).

보충 설명:

- **MOVE_INPUT**: `dir` 은 **정규화된 입력 방향**이며 좌표가 아니다(길이 0 또는 1).
  클라이언트는 입력이 바뀔 때 + **0.1초마다 반복 전송**한다(하트비트 성격).
  서버는 마지막 입력을 저장해두고 틱마다 적분한다. `seq`/`clientTimeMs` 는 지금은 로깅/진단용이다.
  **응답이 없다.** 결과는 `SNAPSHOT` 으로 간다.
- **ATTACK**: 방향만 온다. 대상 선택과 명중 판정은 서버가 한다. **응답이 없다** — `COMBAT_EVENT` 가 전원에게 이벤트로 간다.
- **REQUEST_CHUNKS**: 클라이언트가 카메라 주변에서 새로 필요해진 청크를 알려준다.
  서버는 각 청크에 대해 `WORLD_OBJECTS` **이벤트** 로 응답한다(상자가 없으면 `chestCount=0` 으로라도 보낸다).
  응답이 아니라 이벤트인 이유는 §2.4 — 요청 하나에 패킷 N 개가 나가기 때문이다.
- **USE_ITEM**: `itemId` 는 클라이언트가 그 슬롯에 있다고 믿는 값이다.
  서버 인벤토리와 다르면 `ItemMismatch` 로 거절한다(슬롯 변경 경합 방지).

### 3.2 Server → Client

| ID | 이름 | sink | 페이로드 |
|---|---|---|---|
| `0x8001` | WELCOME | response (HELLO) | `u16 protocolVersion`, `u16 tickRate`, `u32 serverTimeMs` |
| `0x8002` | AUTH_RESULT | response (LOGIN) | `u8 ok`, `u8 errorCode`, `u32 accountId`, `str displayName` |
| `0x8003` | CHARACTER_LIST | event | `u8 count`, `count × u16 characterId` |
| `0x8004` | ENTER_WORLD | response (SELECT_CHARACTER) | 아래 상세 |
| `0x8005` | SNAPSHOT | event (틱 스레드) | `u32 tick`, `u16 count`, `count × { u32 playerId, f32 x, f32 y, u8 facing, u8 motion, u16 hp }` |
| `0x8006` | PLAYER_JOIN | event | `u32 playerId`, `u16 characterId`, `str displayName`, `f32 x`, `f32 y`, `u8 facing`, `u16 hp`, `u16 maxHp` |
| `0x8007` | PLAYER_LEAVE | event | `u32 playerId`, `u8 reason` |
| `0x8008` | COMBAT_EVENT | event | `u32 attackerId`, `u8 facing`, `u16 hitCount`, `hitCount × { u32 targetId, u16 damage, u16 targetHp }` |
| `0x8009` | DEATH | event | `u32 playerId`, `u32 killerId`, `u8 respawnRule` |
| `0x800A` | WORLD_OBJECTS | event | `i32 chunkX`, `i32 chunkY`, `u16 chestCount`, `chestCount × { u32 chestId, f32 x, f32 y, u8 opened }` |
| `0x800B` | CHEST_RESULT | response (INTERACT_CHEST) | `u32 chestId`, `u8 ok`, `u8 errorCode`, `u8 grantCount`, `grantCount × { u16 itemId, u16 qty }` |
| `0x800C` | INVENTORY_FULL | event | `u8 slotCount`, `slotCount × { u8 slot, u16 itemId, u16 qty }` |
| `0x800D` | INVENTORY_DELTA | event | `u8 count`, `count × { u8 slot, u16 itemId, u16 qty }` |
| `0x800E` | PONG | response (PING) | `u32 clientTimeMs`, `u32 serverTimeMs` |
| `0x800F` | RESPAWN | response (RESPAWN_REQUEST) | `u32 playerId`, `f32 x`, `f32 y`, `u16 hp`, `u16 maxHp`, `u8 needCharacterSelect` |
| `0x8010` | KICK | event | `u8 code`, `str message` |
| `0x8011` | ITEM_USE_RESULT | response (USE_ITEM) | `u8 slot`, `u16 itemId`, `u8 ok`, `u8 errorCode` |

**ENTER_WORLD 상세** (필드 순서 그대로):

```
u32 playerId          // 이번 세션의 월드 엔티티 id (accountId 와 다른 값)
u16 characterId
f32 x
f32 y
u8  facing
u16 hp
u16 maxHp
u32 worldSeed         // 클라이언트가 지형(초원 타일/장식)을 결정론 생성하는 데 쓴다
u16 chunkSize         // 픽셀 단위 청크 한 변 (256)
f32 moveSpeed         // px/s (90)
u16 attackCooldownMs  // 500
f32 attackRange       // px (24)
f32 interactRange     // px (24)
```

> 클라이언트는 이 값들을 하드코딩하지 않고 **서버가 준 값**으로 동작한다. 이동 속도나 사거리를
> 바꾸고 싶으면 서버에서 이 값을 바꾸면 클라이언트가 따라온다.

중요한 수신 측 특성:

- **SNAPSHOT 은 전체 상태다(델타 아님).** 접속 중인 모든 플레이어를 매번 포함시켜라.
  클라이언트는 같은 상태가 반복돼도 애니메이션을 재시작하지 않으므로, 매 틱 보내도 문제없다.
  이벤트 경로가 `Oldest` 라 큐가 밀리면 오래된 스냅샷부터 버려지는데, 전체 상태이므로 **버려져도 손실이 없다.**
- **INVENTORY_FULL 은 슬롯 전체 교체**, **INVENTORY_DELTA 는 부분 갱신**이다.
  델타에서 `qty = 0` 은 "그 슬롯을 비워라"라는 뜻이다.
- `str` 은 UTF-8 이다. 한글 이름도 그대로 보낼 수 있다.

### 3.3 usecase — 메시지 하나에 클래스 하나

`IUsecase::execute(const GameRequest& request, BusinessGameChannel& channel)` 이 **IO 루프 스레드에서** 불린다.
usecase 가 하는 일은 넷뿐이다 — (1) `PayloadReader` 로 파싱, (2) `GameWorld` 의 해당 메서드 호출, (3) 결과를 `PayloadWriter` 로 직렬화,
(4) `channel.response.emit` / `channel.event.emit`. **게임 규칙은 `GameWorld` 에 두고 usecase 에는 두지 않는다.**

```cpp
class MoveInputUsecase : public kiotty::IUsecase
{
public:
    explicit MoveInputUsecase(GameWorld& world) : _world(world) {}

    uint16_t command() const override { return COMMAND_MOVE_INPUT; }

    void execute(const kiotty::GameRequest& request, kiotty::BusinessGameChannel&) override
    {
        MoveInput input;

        if (!readMoveInput(request.payload.view(), input))
        {
            return;                                   // 짧거나 남는 payload: 버리고 로그
        }
        _world.applyMoveInput(request.channel_id, input);
    }

private:
    GameWorld& _world;                                // Holder 한도 64B 안에 충분히 든다
};
```

- **요청자 식별은 `request.channel_id` 다.** `GameWorld` 는 플레이어를 `ChannelId` 로 찾는다(`index` 로 슬롯, `generation` 으로 stale 검사).
  계정 이름이 필요하면 `sessions.find(request.channel_id).value().account()`.
- usecase 생성자로 의존성을 주입한다 (`GameWorld&`, `SessionRepository&`, `PasswordHasher&`, `AccountStore&`, `BlockPool&`). 전역을 두지 않는다.
- `Holder<IUsecase>::make<T>(...)` 는 `sizeof(T) <= 64` 를 요구한다. 참조 멤버 몇 개면 충분하다. 큰 상태는 `GameWorld` 에 둔다.
- 응답 `correlation_id` 는 **반드시 `request.correlation_id`** 를 넣는다.
- `GameRequest.state_sequence` 는 이 게임에서 쓰지 않는다 (`DefaultPacketCodec` 이 헤더 `timestamp` 를 넣어 주는데 클라이언트가 0 을 보낸다).

**세션 전 단계 (HELLO → LOGIN).** 엔진 세션은 LOGIN 성공 시 `sessions.open(channel_id, account)` 로 생긴다. 그 전의 "HELLO 를 거쳤는가"는
`GameWorld` 의 채널별 `PeerState` 가 기억한다 — `HelloUsecase` 가 기록하고 `LoginUsecase` 가 확인한다. HELLO 없이 온 LOGIN 은 무시한다.

**LOGIN 의 에러 매핑.**

| 원인 | 엔진 결과 | `AuthError` |
| --- | --- | --- |
| 계정 없음 / 비밀번호 불일치 (`PasswordHasher::matchesBlocking` false) | — | `BadCredentials`(1) |
| 같은 계정이 이미 로그인 | `sessions.open` → `SESSION_LOGIN_REJECTED` (정책이 `replacesPreviousLogin=false`) | `AlreadyOnline`(2) |
| 세션 정원 초과 | `SESSION_TOO_MANY` | `ServerFull`(3) |
| 같은 커넥션에서 재 LOGIN | `SESSION_ALREADY_AUTHENTICATED` | 무시 (응답 없음) |
| 난수·DB 실패 | `SESSION_RANDOM_UNAVAILABLE`, `DATASOURCE_*` | `InternalError`(5) |

**연결을 서버가 끊어야 할 때 (KICK).** usecase 는 `Connection` 을 모른다. `GameWorld` 가 `ConnectionTable&` 를 들고
`closeChannel(ChannelId)` 를 제공한다 — `table.at(i)` 를 돌며 `connection->channelId() == id` 인 것을 `close()`. **이 메서드는 IO 루프 스레드(usecase 안)에서만 부른다.**
`ConnectionTable::at()` 과 `reapClosed()` 가 같은 스레드에서 돌아야 안전하기 때문이다 (§11 엔진 요청 2번).

### 3.4 스레드 모델 — IO 루프 스레드 + 틱 스레드, 게임 상태 뮤텍스 1개

엔진 `IoLoop::run()` 은 IO 완료만 돌리고 usecase 를 그 스레드에서 동기로 부른다. 20Hz 틱을 걸 자리가 없으므로 **틱 스레드를 따로 둔다.**

```
IO 루프 스레드 (IoLoop::run)                       틱 스레드 (GameWorld::runTicks)
  Connection → codec → UsecaseDispatcher             50ms 마다:
    → usecase.execute(request, channel)                 lock(_state_mutex)
        lock(_state_mutex)                                 입력 적분, 전투 쿨다운, Attack 모션 복귀
          GameWorld 상태 변경                               SNAPSHOT payload 1회 직렬화
        unlock                                              플레이어마다 channels.access(id) → event.emit
        channel.response/event.emit                       unlock
  ~Connection → binder.onDisconnected
    → sessions.detach, pool.remove                      ※ 끊긴 플레이어는 틱이 access() 실패로 알아챈다
```

- **게임 상태 전체(플레이어 표·월드·상자·PeerState)는 `GameWorld::_state_mutex` 하나가 지킨다.** 4인 20Hz 에서 임계 구역은 수십 µs 다.
  입력 큐·워커·락프리 구조를 **만들지 않는다.** 필요해지면 측정치와 함께 제안한다.
- **다른 플레이어에게 보내는 방법은 하나다** — `GameEvent` 를 스택에 한 번 만들고, 대상마다
  `ChannelAccess access = channels.access(player.channel_id); if (access) access.channel().business().event.emit(event);`.
  `emit` 은 `const&` 라 같은 이벤트를 N 명에게 재사용한다. `Session::notify` 는 `Bytes` 를 소비하므로 1명에게 보낼 때만 쓴다.
- **`ChannelAccess` 는 풀의 락을 쥐고 있다.** `emit` 이 돌아온 뒤 곧바로 놓는다(스코프를 짧게). 락을 쥔 채 DB 를 읽지 않는다.
- **끊긴 플레이어 정리.** 엔진이 `~Connection` 에서 `ChannelPoolBinder::onDisconnected` 를 부르고, 거기서 풀 락 안에 리스너 `clear` → `sessions.detach` → `pool.remove` 를 한다.
  `GameChannelBinder` 가 `ChannelPoolBinder` 에 위임하지 않고 직접 짜면 **같은 락(`pool.access`) 안에서 `clear()` 해야 한다** — 틱 스레드의 `emit` 과 겹치지 않는 유일한 자리다.
  `GameWorld` 는 이것을 직접 듣지 못하므로 **`IChannelBinder` 를 게임이 감싼다** — `GameChannelBinder : IChannelBinder` 가
  `ChannelPoolBinder` 에 위임한 뒤 `world.onChannelClosed(channel_id)` 를 부른다. 거기서 플레이어 제거 + `PLAYER_LEAVE` 브로드캐스트.
  이 호출은 IO 루프 스레드다.
- **틱 스레드의 stale 채널.** 플레이어가 막 끊긴 순간 틱이 `access()` 하면 `CHANNEL_STALE` / `CHANNEL_NOT_FOUND` 로 실패한다. 그냥 건너뛴다 — 정상 경로다.
- **blocking 호출의 위치.** `PasswordHasher::hashBlocking/matchesBlocking`(Argon2) 과 `IDataSource::*Blocking` 은 이름대로 IO 루프를 세운다.
  LOGIN 은 접속당 1회이고 DB 쓰기는 상자·아이템·계정 생성뿐이므로 **이번 범위에서는 IO 루프 스레드에서 그대로 부른다.**
  Argon2 파라미터는 개발용으로 `HashParameters{ time_cost 1, memory_kib 16384, parallelism 1 }` 로 낮춘다.
  **LOGIN 처리 시간을 측정해 보고한다.** 한 틱(50ms)을 넘으면 로그인 전용 워커를 별도 작업으로 제안한다 (요구사항 설계 가이드라인 8번).
- 서버 종료: `IoLoop::stop()` → 틱 스레드 플래그 → `join`.

### 3.5 조립 (`kmg_main.cpp`)

`test/scratch/usecase_check.cpp` 의 `main()` 순서 그대로다. 수명은 선언 순서의 역순으로 끝나므로 **이 순서를 바꾸지 않는다.**

```cpp
kiotty::BlockPool            pool(kiotty::defaultBlockClasses(), kiotty::defaultBlockClassCount());
kiotty::GameChannelPool      channels(CONNECTION_CAPACITY);          // 8
kmg::MiniGameSessionPolicy   policy;                                 // orphan 0ms, replacesPreviousLogin false
kiotty::SecureRandom         random;
kiotty::SessionRepository    sessions(channels, policy, random, PLAYER_CAPACITY);   // 4
kiotty::PasswordHasher       hasher(kmg::DEV_HASH_PARAMETERS, random);
kiotty::SqliteDataSource     datasource(db_path, pool);
kmg::AccountStore            store(datasource, pool);
kmg::GameWorld               world(channels, sessions, store, pool, world_seed);

kiotty::UsecaseRegistry      registry(kmg::makeUsecases(world, sessions, hasher, store, pool));
kiotty::UsecaseDispatcher    dispatcher(registry, channels, sessions);
kiotty::ChannelPoolBinder    pool_binder(channels, sessions, dispatcher);
kmg::GameChannelBinder       binder(pool_binder, world);
kiotty::DefaultPacketCodec   codec;

kiotty::ConnectionTable      connections(CONNECTION_CAPACITY, pool, SEND_QUEUE_LIMIT /*256*/, binder, codec);
kiotty::Endpoint             endpoint("0.0.0.0", port, BACKLOG, connections);
kiotty::IoLoop               loop(endpoint);

world.bindTable(connections);        // closeChannel 용 (§3.3)
std::thread ticks(&kmg::GameWorld::runTicks, &world);
loop.run();
world.stopTicks(); ticks.join();
```

각 객체의 `operator bool()` 을 **전부 검사하고** 실패한 것의 `code()` 를 찍고 종료한다. 반쯤 올라온 서버를 돌리지 않는다.
`pool.fallbackCount()` 를 종료 시 출력한다 — 0 이 아니면 블록 표가 모자란 것이다.

접속 상한은 두 겹이다 — `ConnectionTable`(8)을 넘으면 **엔진이 accept 자체를 거절**하고(패킷 없이 끊김), 그 안이지만
세션 정원(4)을 넘으면 `AUTH_RESULT(ServerFull)`. 테이블과 채널 풀은 세션 정원보다 크게 잡아 로그인 대기 중인 커넥션이 자리를 얻게 한다.

---

## 4. 표준 흐름

```
TCP connect                      (엔진: accept → ConnectionTable 슬롯 → binder.onConnected → GameChannel → submitReceive)
  C→S HELLO
  S→C WELCOME                     response. 버전 불일치면 KICK event 후 world.closeChannel()
  C→S LOGIN
  S→C AUTH_RESULT(ok=1)           response. 실패면 ok=0 + errorCode, 클라이언트는 연결을 끊는다
  S→C CHARACTER_LIST              event (v1 기준 1,2,3,4)
  C→S SELECT_CHARACTER
  S→C ENTER_WORLD                 response
  S→C INVENTORY_FULL              event ← 반드시 보낼 것. 없으면 클라이언트 인벤토리가 빈 상태로 남는다
  S→C PLAYER_JOIN × N             event ← 이미 접속 중인 다른 플레이어들
  (다른 클라이언트들에게) S→C PLAYER_JOIN   event (새로 들어온 이 플레이어)
  C→S REQUEST_CHUNKS              ← 클라이언트가 카메라 주변 청크(초기 5x5)를 요청
  S→C WORLD_OBJECTS × N           event
  --- 이후 루프 ---
  C→S MOVE_INPUT / ATTACK / INTERACT_CHEST / USE_ITEM / REQUEST_CHUNKS / PING
  S→C SNAPSHOT(20Hz, 틱 스레드) / COMBAT_EVENT / CHEST_RESULT / INVENTORY_DELTA / DEATH
      / ITEM_USE_RESULT / WORLD_OBJECTS / PONG
```

### 세션 정책 (`MiniGameSessionPolicy`)

| `ISessionPolicy` | 값 | 이유 |
| --- | --- | --- |
| `orphanLifetimeMs` | **0** | 이 게임에는 재바인딩(토큰)이 없다. 끊기면 세션을 즉시 폐기하고 클라이언트는 재로그인한다. 강제 종료 직후 재접속이 `AlreadyOnline` 에 막히지 않게 하는 값이기도 하다 |
| `replacesPreviousLogin` | **false** | 원본 규칙 — 중복 접속은 `AlreadyOnline` 으로 거절 |

`SessionRepository::sweep()` 은 고아 수명이 0 이라 부를 일이 없다. 부르지 않는다.

### 사망 → 재시작 흐름 (중요)

```
HP 0 → S→C DEATH(playerId, killerId, respawnRule = 1 MustReselectCharacter)  ※ 전체 브로드캐스트 (event)
클라이언트: Dead 화면 → 사용자가 "다시 시작" 클릭
C→S RESPAWN_REQUEST
S→C RESPAWN(playerId, x, y, hp, maxHp, needCharacterSelect = 1)              response
클라이언트: 캐릭터 선택 화면으로 돌아감
C→S SELECT_CHARACTER            ← 다시 선택. 서버는 이 재선택을 허용해야 한다
S→C ENTER_WORLD + INVENTORY_FULL + PLAYER_JOIN × N   ← 첫 진입과 동일하게 다시 보낸다
```

- **사망해도 인벤토리는 지운다/초기화하지 않는다.** 인벤토리는 캐릭터가 아니라 **계정**의 데이터다.
- `needCharacterSelect = 0` 으로 응답하면 클라이언트는 재선택 없이 그 자리에서 부활한다(선택지로 남겨둠).

### 연결 종료 흐름

```
클라이언트 끊김 / closeChannel()
  → 엔진: 떠 있는 IO 가 0 이 되면 Closed 표시 → 다음 wait() 뒤 reapClosed() 에서 ~Connection (IO 루프 스레드)
  → ~Connection → GameChannelBinder.onDisconnected
      → ChannelPoolBinder: 풀 락 안에서 리스너 clear → sessions.detach (수명 0 → 즉시 폐기) → pool.remove
      → world.onChannelClosed(channel_id): Player 제거, 남은 전원에게 PLAYER_LEAVE(reason)
```

`reason` 은 `GameWorld` 의 `PeerState` 가 기억한다 — KICK 뒤 `closeChannel` 이면 `Kick(2)`, 타임아웃 퇴장이면 `Timeout(1)`, 그 외 `Disconnect(0)`.

**타임아웃(10초 무응답)은 월드 퇴장으로 처리하고 소켓은 건드리지 않는다.** 틱 스레드가 `PLAYER_LEAVE(Timeout)` 를 브로드캐스트하고 플레이어를
월드에서 빼되, 세션과 커넥션은 남겨 TCP 가 끊김을 알릴 때까지 둔다. 틱 스레드에서 `closeChannel` 을 부를 수 없기 때문이다 (§3.3, §11 엔진 요청 2번).
퇴장된 플레이어가 다시 패킷을 보내면 "월드 밖" 상태로 취급한다 — `SELECT_CHARACTER` 부터 다시 하면 들어온다.

---

## 5. 게임 규칙과 상수 (클라이언트가 이 값을 전제로 동작한다)

| 항목 | 값 | 비고 |
|---|---|---|
| tickRate | **20Hz** | 스냅샷 브로드캐스트 주기. 시뮬레이션도 이 주기로 적분 |
| 이동 속도 | **90 px/s** | `pos += normalize(dir) * 90 * dt` |
| maxHp | **10** | 클라이언트 UI 는 하트 1개 = HP 2 로 표시(반 칸 하트 있음) → 하트 5개 |
| 기본 데미지 | **2** | 하트 1칸 |
| 공격 쿨다운 | **500ms** | 서버가 반드시 검증(클라이언트 쿨다운은 스팸 방지용일 뿐) |
| 공격 범위 | **24px** | 그리고 바라보는 방향과의 내적 ≥ 0.3 인 대상만 |
| 상호작용 범위 | **24px** | 상자 개방 검증 |
| chunkSize | **256px** | 16px 타일 × 16. 청크 좌표 = `floor(worldPos / 256)` |
| 상자 배치 | **서버 소유** | 예: 청크당 70% 확률로 1개. 위치는 서버가 정한다 |
| 상자 개방 | **1회성 (모든 플레이어 공유)** | 이미 열린 상자는 `AlreadyOpened` 로 거절 |
| 인벤토리 | **6칸**, 스택 상한 **99** | 슬롯 인덱스는 0부터 |
| 인벤토리 소유자 | **계정(accountId)** | 사망·재접속·캐릭터 변경과 무관하게 유지 |
| 스폰 위치 | 자유 (예: `(0,0)` 근처) | 4명이 겹치지 않게 약간 흩어도 좋다 |

### 아이템 표 (클라이언트 정의와 반드시 일치)

```
itemId 1  "우유 반 병"   사용 시 HP +2
itemId 2  "우유 한 병"   사용 시 HP 를 maxHp 까지 회복
```

- 아이템은 **상자에서만** 나온다. 상자 지급 내용/확률은 서버가 정한다(예: 반 병 3 : 한 병 1).
- 아이템 이름·아이콘·설명은 **클라이언트가 소유**한다. 서버는 `itemId` 와 수량만 다룬다.
- 새 아이템을 추가하려면 클라이언트 팀과 `itemId` 를 합의해야 한다(클라이언트에 정의가 없으면
  "알 수 없는 아이템"으로 표시된다).

### 캐릭터

- 서버는 `CHARACTER_LIST` 로 **선택 가능한 id 집합만** 통보한다(현재 `1,2,3,4`).
- 외형/이름/애니메이션은 클라이언트 애셋이 소유한다. 서버는 characterId 를 저장·중계만 한다.
- 캐릭터별 능력치 차이는 없다(전원 동일). 서버에서 굳이 분기하지 마라.

### 지형

- **서버는 지형을 만들지 않는다.** 초원 타일과 풀/꽃 장식은 클라이언트가 `worldSeed` 로
  결정론 생성한다(트래픽 0). 같은 seed → 모든 클라이언트가 같은 지형을 본다.
- 서버가 할 일은 (a) `ENTER_WORLD` 에서 **모든 플레이어에게 동일한 `worldSeed`** 를 주는 것,
  (b) 상태를 가진 오브젝트(상자)를 `WORLD_OBJECTS` 로 내려주는 것.
- **나무/충돌 오브젝트는 이 게임에 없다.** 월드는 막힘 없는 평지다(현재 이동 충돌 판정 없음).

---

## 6. 서버가 반드시 검증해야 하는 것 (보안)

클라이언트가 조작된 패킷을 보낼 수 있다고 가정하고 전부 서버에서 막아라.

| 요청 | 검증 | 누가 |
|---|---|---|
| 헤더 (`magic`·`version`·`payload_length`) | 불일치 시 연결 종료 | **엔진** `Connection` |
| 로그인 전 `IUsecase` 메시지 | 세션 없으면 usecase 에 도달하지 않음 | **엔진** `UsecaseDispatcher` |
| 알 수 없는 `command` | 등록된 usecase 없음 → 버림 (연결 유지) | **엔진** `UsecaseDispatcher` |
| 모든 payload | `PayloadReader` 경계 검사. 짧거나 남으면 **그 메시지를 버리고 로그**. 연결은 유지 | usecase |
| `HELLO` | `protocolVersion == 3`. 아니면 `KICK(VersionMismatch)` event 후 `closeChannel`. 두 번째 HELLO 는 무시 | `HelloUsecase` |
| `LOGIN` | HELLO 를 거쳤는지. 계정 존재/비밀번호 일치. 중복·정원은 §3.3 표 | `LoginUsecase` |
| `SELECT_CHARACTER` | `CHARACTER_LIST` 로 준 id 집합에 속하는지. 아니면 무시. 월드 안에서 살아 있는 플레이어의 재선택은 무시 | `GameWorld` |
| `MOVE_INPUT` | 월드 안인지. `dir` 길이가 1을 넘으면 정규화. NaN/Inf 방어. **좌표는 절대 클라이언트에서 받지 않는다** | `GameWorld` |
| `ATTACK` | 월드 안·생존. 쿨다운 500ms. 대상은 서버가 사거리/방향으로 선택 | `GameWorld`·`Combat` |
| `INTERACT_CHEST` | 월드 안·생존. 상자 존재, 거리 ≤ interactRange, 이미 열렸는지, 인벤토리 여유 | `GameWorld`·`Inventory` |
| `USE_ITEM` | 월드 안. 슬롯 존재, `itemId` 일치, 사용 가능한 아이템인지, 사망 여부, HP 만충 여부 | `GameWorld`·`Inventory` |
| `REQUEST_CHUNKS` | 월드 안. `count` 상한 64. 넘으면 버린다 | usecase |

- 비밀번호는 개발 단계에서 평문으로 온다(TLS 없음). DB 에는 **평문 저장 금지** — 엔진 `PasswordHasher` 의 encoded 문자열(Argon2, 솔트 포함)을 저장한다.
  `matchesBlocking` 이 솔트를 encoded 에서 꺼내므로 솔트를 따로 저장하지 않는다.
  운영 단계에서 TLS 또는 챌린지-응답으로 교체할 예정이며, 그때 `protocolVersion` 을 올린다.
- 계정이 없으면 **자동 생성한다** (개발 편의). `display_name` 은 `accountId` 와 같게 둔다.

---

## 7. 영속화 (`IDataSource` 위의 키-값)

**요구사항**: 클라이언트를 강제 종료(프로세스 킬)해도, 재접속하면 인벤토리가 그대로 남아 있어야 한다.

엔진 `IDataSource` 는 `readBlocking(key) → Bytes` / `writeBlocking(key, value)` 뿐이다. **테이블·SQL·트랜잭션이 없다.**
그래서 **레코드 하나 = 키 하나 = 값 하나** 로 설계하고, 원자성은 "값 하나를 통째로 쓴다"로 얻는다.

| 키 (UTF-8) | 값 (`PayloadWriter` 형식) | 쓰는 시점 |
| --- | --- | --- |
| `acct:<login_id>` | `u32 account_id`, `str display_name`, `str pw_encoded`, `u32 created_at_s` | 계정 자동 생성 시 1회 |
| `acct_seq` | `u32 next_account_id` | 계정 생성 직전 읽고 +1 써서 발급 |
| `inv:<account_id>` | `u8 slot_count(6)`, `6 × { u16 item_id, u16 qty }` — **슬롯 전체** | 지급/소모가 확정될 때마다 전체 쓰기 |
| `chest:<chest_id>` | `i32 chunk_x`, `i32 chunk_y`, `f32 x`, `f32 y`, `u8 opened` | 청크 최초 생성 시, 개방 시 |
| `chunk:<x>:<y>` | `u16 chest_count`, `chest_count × u32 chest_id` | 청크 최초 생성 시 1회 (이후 읽기만) |

- 값 인코딩은 와이어와 같은 `PayloadWriter` 를 쓴다. 레코드 타입은 `kmg_records.h` 에 struct + `encode/decode` 로 둔다.
- **서버에서 지급이 확정된 시점에 즉시 쓴다.** 종료 시점에 몰아서 저장하지 마라(서버가 죽으면 유실된다).
- 쓰기 순서: 메모리 인벤토리 갱신 → `writeBlocking` → 실패하면 메모리도 되돌리고 `InternalError`/`ChestError` 로 거절 → 성공하면 `INVENTORY_DELTA`.
  상자는 `inv:` 쓰기 성공 **뒤에** `chest:` 를 `opened=1` 로 쓴다. 둘 사이에 죽으면 "아이템은 받았는데 상자가 안 열림" 이 되는데, 반대(상자만 열림)보다 플레이어에게 유리한 쪽이라 이 순서다.
- 로그인 성공 → `inv:` 읽기 (없으면 빈 6칸) → 메모리에 올림 → `ENTER_WORLD` 직후 **`INVENTORY_FULL`**.
- 슬롯 규칙: 같은 `itemId` 스택에 먼저 채우고(상한 99), 남으면 빈 슬롯(0~5)에 넣는다.
  6칸이 모두 차서 넣을 수 없으면 `ChestError::InventoryFull` 로 거절하고 **상자를 열지 않은 상태로 둔다.**
- 상자 id 는 `(chunk_x, chunk_y, n)` 에서 결정론적으로 만든다 (예: `hash32(seed, x, y) * 16 + n`). 서버를 재시작해도 같은 청크는 같은 상자를 낸다.
- 유닛 테스트는 `InMemoryDataSource` 로 돌린다. 파일이 필요 없다.

---

## 8. 구현 순서 (이 순서대로 진행하고 각 단계마다 검증하라)

각 단계가 끝나면 §9 의 방법으로 동작을 확인한 뒤 다음 단계로 넘어간다.
**단계마다 g++ `-Wall -Wextra -Wpedantic` / MSVC `/W4` 워닝 0 을 유지한다.** 엔진이 그 기준이다.

1. **조립 + HELLO/PING**: `prj/mini_game/CMakeLists.txt`, `kmg_main.cpp` 에서 §3.5 조립. `PayloadReader/Writer`.
   `HelloUsecase`·`PingUsecase` 두 개만 등록. `HELLO` → `WELCOME`, `PING` → `PONG`(클라이언트가 1초마다 보낸다).
   **§2.2 의 바이트 예시와 대조한다.** 버전 불일치 → `KICK` → `closeChannel` 까지.
2. **로그인**: `AccountStore`(키-값 레코드), `LoginUsecase` — 계정 조회/자동 생성, `PasswordHasher` 검증, `sessions.open`, `AUTH_RESULT`, `CHARACTER_LIST`.
   `SESSION_*` → `AuthError` 매핑(§3.3). **LOGIN 처리 시간을 찍어 보고한다.**
3. **월드 진입**: `SelectCharacterUsecase` → playerId 발급 → `ENTER_WORLD` → `INVENTORY_FULL`(빈 상태여도 보낸다).
   `GameChannelBinder` 와 `onChannelClosed` 도 여기서 — 끊기면 플레이어가 표에서 빠지는 것을 확인.
4. **틱 루프 + 이동**: 틱 스레드(§3.4) 시작, 20Hz 고정 틱, `MOVE_INPUT` 저장 → 틱마다 적분 → 전원에게 `SNAPSHOT`.
   `facing` 은 입력 방향에서 계산(좌우 우선), `motion` 은 Idle/Walk 전환.
5. **다중 접속**: `PLAYER_JOIN` / `PLAYER_LEAVE`, 신규 접속자에게 기존 플레이어 목록 전송.
   여기서 **클라이언트 2개 이상**을 띄워 서로 보이는지 확인하라. Linux 라면 TSan 으로 한 번 돌린다(§9.3) — 틱 스레드와 IO 스레드가 처음 겹치는 단계다.
6. **전투**: `ATTACK` → 쿨다운/사거리/방향 검증 → 데미지 2 → `COMBAT_EVENT`(맞은 사람 목록, 남은 HP).
   HP 0 → `Motion::Dead` + `DEATH` 브로드캐스트. `RESPAWN_REQUEST` → `RESPAWN(needCharacterSelect=1)`.
7. **상자**: 청크별 상자 생성/영속화, `REQUEST_CHUNKS` → `WORLD_OBJECTS`,
   `INTERACT_CHEST` → 검증 → `CHEST_RESULT` + `INVENTORY_DELTA`.
   상자가 열리면 **그 청크를 보고 있는 다른 클라이언트에게도** `WORLD_OBJECTS` 를 다시 보내
   `opened=1` 을 반영하게 하라(클라이언트는 같은 chestId 를 받으면 상태만 갱신한다). "보고 있는 청크"는 플레이어별 최근 `REQUEST_CHUNKS` 집합이다.
8. **아이템 사용**: `USE_ITEM` → 검증 → HP 회복 + 수량 1 감소 → `ITEM_USE_RESULT` + `INVENTORY_DELTA`.
   회복된 HP 는 다음 `SNAPSHOT` 으로 자연히 전달된다.
9. **영속성 검증**: 아이템을 얻은 뒤 **클라이언트 프로세스를 강제 종료**하고 재접속 →
   인벤토리가 복구되는지 확인. 서버를 재시작해도 유지되는지 확인.
10. **안정화**: 4인 동시 접속, 갑작스러운 연결 종료(케이블 뽑힘 수준) 처리, 좌표/HP 이상값 방어,
    로그 정리, 타임아웃(10초 무응답 → 월드 퇴장, §4). `BlockPool::fallbackCount()` 가 0 인지 확인하고 보고한다.

**유닛 테스트는 `cpp-tester` 에게 위임한다.** 단계 1(Reader/Writer), 2(Records·AccountStore, `InMemoryDataSource`), 6(Combat), 7·8(Inventory)이
컴파일되는 시점에 부른다. 엔진 `test/unit` 과 같은 GoogleTest 구성을 `prj/mini_game/test/unit` 에 둔다.

---

## 9. 검증하는 방법

### 9.1 실제 Godot 클라이언트 (문서 머리의 통보 사항 반영 후)

클라이언트는 Godot 4.7 프로젝트이며, 실행 인자로 접속 대상을 지정할 수 있다.
(Windows 기준 경로 예시 — 실제 경로는 클라이언트 팀에 확인)

```bat
:: 창을 띄워 수동 플레이 (실서버 접속)
Godot_v4.7.1-stable_win64_console.exe --path <클라이언트폴더> -- ^
    --real --server=127.0.0.1:9000 --account=u1 --password=p --autologin --debug-overlay

:: 창 없이 자동 흐름 검증. 종료코드 0 = PASS, 1 = FAIL
Godot_v4.7.1-stable_win64_console.exe --headless --path <클라이언트폴더> -- ^
    --smoke --real --server=127.0.0.1:9000 --account=u1 --password=p
```

`--smoke` 자동 검증이 확인하는 항목(= 서버가 통과시켜야 하는 최소 조건):

1. `WELCOME` 수신 → `AUTH_RESULT(ok=1)` → `CHARACTER_LIST` 가 비어 있지 않음
2. 캐릭터 선택 후 `ENTER_WORLD` 수신, `playerId != 0`, `maxHp > 0`
3. 0.6초간 오른쪽 입력 → **서버 좌표가 `moveSpeed * 0.4` px 이상 증가**,
   `facing = Right`, 입력을 떼면 `motion` 이 Idle 로 돌아옴
4. 공격 1회 → `motion` 이 Attack 으로 바뀌고 `COMBAT_EVENT` 가 1건 이상 수신됨
   (맞은 대상이 없어도 `hitCount = 0` 으로 반드시 보내야 한다)
5. 접속한 다른 플레이어가 있다면 그 플레이어의 `displayName` 이 비어 있지 않고 `characterId > 0`

4인 테스트는 같은 명령을 계정만 바꿔 4개 띄우면 된다(`--account=u1 ~ u4`).

수동 플레이 시 조작: 이동 `WASD`/방향키, 공격 `Space`/`J`, 상자 열기 `E`,
아이템 사용 숫자키 `1`~`6`, 디버그 오버레이 `F3`(상태·RTT·좌표·HP·인원 표시).

### 9.2 자체 테스트 클라이언트 (클라이언트 팀 합의 전에도 쓴다)

`prj/mini_game/test/kmg_test_client.cpp` — `test/scratch/usecase_check.cpp` 의 클라이언트 쪽(`sendCommand`·`readPacket`)을 본떠 소켓을 직접 열고
위 `--smoke` 1~5 를 그대로 따라 하는 프로그램. 24바이트 헤더를 직접 쓰고, `flags.EVENT` 로 응답/이벤트를 가른다.
CTest 에 `RUN_SERIAL` 로 등록한다(고정 포트). DB 는 임시 경로에 새로 만든다.

### 9.3 플랫폼별 검증

| 검증 | Windows / MSVC | Linux / GCC |
| --- | --- | --- |
| 워닝 0 | `/W4` | `-Wall -Wextra -Wpedantic` |
| 메모리 오류 | `/fsanitize=address` (런타임이 있으면) | `-fsanitize=address,undefined` |
| 데이터 레이스 (§3.4) | **없음** | `-fsanitize=thread` — epoll·io_uring 양쪽 |
| 백엔드 | iocp | epoll (`-DKIOTTY_USE_IO_URING=OFF`), io_uring |

**돌리지 못한 검증은 돌리지 못했다고 보고한다.**

---

## 10. 자주 발생하는 실수 (미리 피하라)

1. **`payload_length` 에 `command` 를 포함함** → 원본 프로토콜의 `payloadLen` 과 다르다. 엔진 헤더의 `payload_length` 는 **payload 만**이다.
   §2.2 의 바이트 예시로 반드시 대조하라.
2. **구조체 `memcpy` 직렬화** → 패딩 때문에 필드가 밀린다. 필드 단위로 써라(헤더는 엔진이 쓴다).
3. **엔디안 혼동** → 빅엔디안 변환 함수(`htons` 등)를 쓰지 마라. 리틀엔디안 고정이다. `LittleEndian<T>` 를 쓴다.
4. **`SNAPSHOT` 을 변경된 플레이어만 보냄** → 클라이언트는 전체 스냅샷을 기대한다.
5. **`INVENTORY_FULL` 을 안 보냄** → 인벤토리가 있는데도 빈 칸으로 보인다. 월드 진입마다 보내라.
6. **`COMBAT_EVENT` 를 맞았을 때만 보냄** → 헛스윙도 `hitCount=0` 으로 보내야 클라이언트가 연출한다.
7. **응답을 이벤트로, 이벤트를 응답으로 보냄** → 클라이언트의 `correlation_id` 대기 슬롯이 영영 안 깨어나거나 엉뚱한 것이 깨어난다. §3.2 표를 따른다.
8. **`GameResponse.correlation_id` 에 요청 값을 안 넣음** → 7번과 같은 증상. `request.correlation_id` 를 그대로 넣는다.
9. **`ChannelAccess` 를 오래 쥠** (락 안에서 DB·해시) → IO 루프가 풀 락에서 멈춘다. `emit` 하고 바로 놓는다.
10. **`Session::notify` 로 브로드캐스트** → `Bytes` 를 매번 새로 만들어야 한다. N 명에게는 `GameEvent` 하나 + `access().event.emit` 루프.
11. **틱 스레드에서 `ConnectionTable::at()` / `close()`** → `reapClosed()` 와 경합한다. 타임아웃은 월드 퇴장으로만 처리한다 (§4).
12. **재선택(사망 후) 경로 누락** → `RESPAWN(needCharacterSelect=1)` 뒤에 오는
    `SELECT_CHARACTER` 를 처리하지 않으면 사용자가 다시 게임에 들어올 수 없다.
13. **사망 시 인벤토리 삭제** → 요구사항 위반이다. 인벤토리는 계정 데이터다.
14. **엔진을 고쳐서 해결함** → 절대 원칙 5 위반. §11 에 적고 보고한다.

---

## 11. 프로토콜을 바꾸고 싶을 때 / 엔진에 요청할 것

이 프로토콜은 클라이언트가 기준 문서를 관리한다(`docs/PROTOCOL.md`). 이 개정으로 **서버 쪽이 3 을 제안한 상태**이며
클라이언트 팀이 문서를 갱신해야 확정된다. 서버 쪽에서 필드 추가/변경이 필요하면 **클라이언트 팀에 먼저 알리고 문서를 갱신한 뒤**
`protocolVersion` 을 올려라. 버전이 다르면 클라이언트는 `HELLO` 직후 `KICK(VersionMismatch)` 를 받고 접속을 중단한다.

헤더의 `version`(엔진 1.0)은 게임 `protocolVersion` 과 별개다. 엔진 헤더가 바뀌는 일은 엔진 쪽 결정이다.

미확정으로 남아 있는 사항(서버가 정하고 클라이언트 팀에 통보하면 되는 것):

- 상자 재생성 여부(현재는 1회성) / 청크당 상자 확률 / 지급 아이템 확률
- 스폰 위치 규칙, 사망 후 규칙(`RespawnRule` 값 선택)
- 타임아웃 시간 (현재 10초)

**엔진에 요청할 것** — 이 문서의 결정을 바꾸는 것들. 게임 코드에서 우회하지 말고 이 목록에 쌓아 보고한다.

| # | 요청 | 지금의 우회 | 근거 |
| --- | --- | --- | --- |
| 1 | `TCP_NODELAY` 설정 | 없음 (Nagle 켜진 채 20Hz) | `kiotty_windows_socket.cpp`·`kiotty_linux_socket.cpp` 에 `setsockopt(TCP_NODELAY)` 없음 |
| 2 | 채널 id 로 커넥션을 닫는 스레드 안전 API (`ConnectionTable::close(ChannelId)` 등) | KICK 은 IO 스레드에서 `at()` 순회, 타임아웃은 소켓을 안 닫음 | `ConnectionTable::at()`·`reapClosed()` 가 슬롯 `live` 를 락 없이 읽고 쓴다 |
| 3 | 이벤트별 `DropPolicy` 선택 (`GameEvent` 에 정책 필드) | `send_queue_limit` 256 으로 드롭 확률을 낮춤 | `Connection::EventListener::onStream` 이 `Oldest` 고정 |
| 4 | `IoLoop` 타이머/틱 훅 | 틱 스레드 + `_state_mutex` | `IoLoop::run()` 은 `wait(1000ms)` 루프뿐 |
| 5 | 로그인(해시)·DB 용 워커 | IO 스레드에서 blocking 호출, 시간 측정 | `requirement.md` 설계 가이드라인 8번이 Worker 를 전제하지만 `worker/` 가 아직 없다 |
