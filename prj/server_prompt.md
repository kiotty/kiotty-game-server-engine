# C++ 게임 서버 개발 지시서 (kiotty-mini-game) — kiotty 엔진판

> **이 문서는 서버 개발자의 AI 에이전트(Claude)에게 그대로 전달하는 프롬프트다.**
> 이 문서 하나와 이 저장소의 `src/`(kiotty 엔진)만으로 서버를 완성할 수 있도록 필요한 정보를 모두 담았다.
> 클라이언트 저장소에 접근할 필요는 없다.
>
> 원본: 클라이언트(Godot 4.7) 팀 / 프로토콜 버전 **2** / 2026-08-15
> 개정: 2026-08-22 — 자체 소켓 계층 대신 **kiotty 엔진(`src/`) 위에 올리도록** 전송 계층·구조·구현 순서를 바꿨다.
> 게임 규칙(§5·§6·§9·§10)은 원본 그대로다.

> **클라이언트 팀에 통보가 필요한 변경 (이 개정으로 생김)**
>
> 1. 프레이밍이 6바이트 헤더(`u32 payloadLen + u16 msgId`)에서 **엔진의 24바이트 고정 헤더**(§2)로 바뀐다.
>    엔진의 헤더 파서는 엔진 인터페이스이므로 서버 쪽에서 바꿀 수 없다.
> 2. 서버→클라이언트 메시지가 **응답(Response)** 과 **이벤트(Event)** 로 나뉘고 헤더 `flags`로 구분된다(§2.4).
> 3. 그래서 `protocolVersion`(HELLO 페이로드)을 **2 → 3** 으로 올린다. 클라이언트가 보내는 값이 3이 아니면 `KICK(VersionMismatch)`.
>
> 이 세 가지가 합의되기 전까지 실제 클라이언트 검증(§9)은 통과하지 않는다. 그 전까지는 §9.2의 자체 테스트 클라이언트로 검증한다.

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
3. **인원수를 하드코딩하지 않는다.** 지금 목표는 4명이지만, 자료구조는 `unordered_map<playerId, Player>`
   처럼 임의 인원으로 확장 가능해야 한다. 접속 상한은 `ConnectionTable` 용량 하나로 정한다.
4. **영속 데이터는 서버 DB 가 기준이다.** 클라이언트 로컬 저장은 인벤토리 근거로 쓰지 않는다.
5. **엔진(`src/`)의 인터페이스는 바꾸지 않는다.** 엔진이 제공하는 것은 그대로 쓰고, 없는 것은 게임 쪽 코드에 만든다.
   엔진에 손대야만 풀리는 문제가 나오면 **고치지 말고 보고한다.**

---

## 1. 개발 환경 (확정 사항)

- 언어/표준: **C++11** (엔진의 `CMAKE_CXX_STANDARD 11`, 확장 OFF 를 그대로 따른다).
  `std::optional`·`string_view`·구조적 바인딩·`if constexpr` 은 쓸 수 없다. `Result<E,T>`(엔진 `core/kiotty_result.h`)와 `bool tryGet(T&)` 로 대신한다.
- 빌드: **CMake** (Windows MSVC / Linux GCC 양쪽). 빌드 명령은 저장소의 `build` 스킬을 따른다.
- 네트워크: **kiotty 엔진이 전부 담당한다.** 소켓·accept·논블로킹·IO 다중화(IOCP / epoll / io_uring)·
  헤더 파싱·부분 송수신·클라이언트별 송신 큐·접속 상한은 엔진에 있다. **게임 코드는 소켓 API 를 한 줄도 부르지 않는다.**
  - `select()` 루프, `Listener`, `Connection` 송신 큐, 누적 버퍼를 **새로 만들지 않는다.**
  - `TCP_NODELAY` 는 엔진이 설정하지 않는다. 필요하면 엔진 이슈로 보고하고, 게임 코드에서 `setsockopt` 를 부르지 않는다.
- 영속화: **SQLite** (amalgamation `sqlite3.c` / `sqlite3.h` 를 프로젝트에 포함. 별도 DB 서버 없음)
- 그 외 외부 의존성 없음. C++ 표준 라이브러리 + kiotty 엔진만 사용.

### 1.1 엔진이 제공하는 것 (그대로 쓴다)

| 필요한 것 | 엔진 타입 | 헤더 |
| --- | --- | --- |
| 리스닝·accept·IO 루프 | `Endpoint`, `IoLoop` | `presentation/endpoint/kiotty_endpoint.h`, `presentation/io_loop/kiotty_io_loop.h` |
| 클라이언트 1명 (소켓 + 수신 상태기계 + 송신 큐) | `Connection` (추상, `onPacket` 을 채운다) | `presentation/connection/kiotty_connection.h` |
| 커넥션 슬롯 + 접속 상한 | `ConnectionTable<T>` | `presentation/connection/kiotty_connection_table.h` |
| 수신 패킷 | `ReceivedPacket { PacketHeader header; Bytes payload; }` | `core/kiotty_connection_buffer.h` |
| 송신 패킷 직렬화 (헤더 + payload 한 블록) | `writePacket(pool, command, flags, correlation_id, payload)` | `core/kiotty_packet_writer.h` |
| 헤더 필드 / 리틀엔디안 | `PacketHeader`, `LittleEndian<T>`, `PACKET_FLAG_EVENT` | `core/kiotty_packet_concept.h` |
| 버퍼 메모리 | `BlockPool`, `Bytes`, `ByteView`, `ByteSpan` | `core/kiotty_block_pool.h`, `core/kiotty_bytes.h` |
| 에러 값 반환 | `Result<ErrorCode, T>`, `ok()`, `error()` | `core/kiotty_result.h` |
| 고정 용량 FIFO | `RingBuffer<T>` | `core/kiotty_ring_buffer.h` |

엔진 계약의 원문은 [docs/progress/HANDOVER.md](../docs/progress/HANDOVER.md) 다. 특히 §3.2(끊김 시점), §4(Connection), §5(조립)를 읽고 시작한다.

**쓰지 않는 엔진 기능.** `UsecaseRegistry` / `UsecaseDispatcher` / `Request` / `ResponseSink` / `EventSink` 는 이번에 쓰지 않는다.
`Request` 에 **보낸 커넥션의 식별자가 없어서** usecase 가 어느 플레이어의 요청인지 알 수 없기 때문이다
([code_refactoring_plan.md](../docs/progress/code_refactoring_plan.md) §3.2 의 `channel_id` 가 들어오면 그때 옮긴다).
대신 `GameConnection::onPacket` 에서 `header.command` 로 직접 분기한다(§3.3).

### 1.2 엔진에 없어서 게임 쪽에 만드는 것

| 필요한 것 | 만들 것 | 비고 |
| --- | --- | --- |
| payload 필드 단위 읽기/쓰기 (`u8/u16/u32/i32/f32/str`) | `PayloadReader`, `PayloadWriter` | 경계 검사 필수. `f32` 는 `memcpy` → `uint32_t` → `LittleEndian<uint32_t>` |
| 20Hz 틱 | 틱 스레드 1개 | 엔진 `IoLoop` 에는 타이머 훅이 없다 (§3.4) |
| 응답/이벤트 송신 헬퍼 | `GameConnection::sendResponse / sendEvent / sendSnapshot` | `writePacket` + `Connection::emit` 을 감싼다 (§3.3) |
| 플레이어·월드·전투·인벤토리·DB | `game/`, `db/` | 원본 구조 유지 |

### 1.3 파일 구조

게임 프로젝트는 엔진 저장소 안의 **`prj/mini_game/`** 에 둔다 (엔진 `src/` 와 분리. 루트 `CMakeLists.txt` 에서 `add_subdirectory(prj/mini_game)` 한 줄만 추가한다).
파일 접두어는 엔진의 `kiotty_` 와 겹치지 않게 **`kmg_`**, 네임스페이스는 `kmg` 다.

```
prj/mini_game/
  CMakeLists.txt              kmg_server 실행 파일. kiotty 에 link
  third_party/sqlite/{sqlite3.c,sqlite3.h}
  src/
    kmg_main.cpp              진입점, 포트 파싱, BlockPool·ConnectionTable·Endpoint·IoLoop 조립, 틱 스레드 시작
    net/
      kmg_protocol.h          메시지 ID / 열거형 / 상수 (§2·§3)
      kmg_payload_reader.h    리틀엔디안 역직렬화 + 경계 검사
      kmg_payload_writer.h    리틀엔디안 직렬화
      kmg_game_connection.{h,cpp}  kiotty::Connection 파생. onPacket 분기, 송신 헬퍼, 세션 단계
    game/
      kmg_game_server.{h,cpp} 게임 상태 소유자. 뮤텍스 1개, 메시지 핸들러, 틱, 브로드캐스트
      kmg_player.{h,cpp}      플레이어 상태(위치/방향/모션/HP/입력)
      kmg_world.{h,cpp}       청크/상자 상태, 청크 조회
      kmg_combat.{h,cpp}      공격 판정
      kmg_inventory.{h,cpp}   슬롯 인벤토리 로직
    db/
      kmg_database.{h,cpp}    SQLite 열기/스키마/계정·인벤토리·상자 조회·저장
  test/
    kmg_test_client.cpp       §9.2 자체 검증 클라이언트 (엔진 test/integration 의 스모크를 본뜬다)
    unit/                     PayloadReader/Writer, Inventory, Combat 의 GoogleTest (cpp-tester 에게 위임)
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
| `correlation_id` | 요청: 클라이언트가 발급(커넥션 안에서 유일). 응답: 요청 값 **반사**. 이벤트: 서버 세션별 단조 증가 | 방향별 규칙 (§2.4) |
| `timestamp` | 서버 단조 시각 µs. 클라이언트→서버는 **0** 으로 채운다 | 서버 송신은 엔진 `writePacket` 이 찍는다 |
| `command` | 메시지 ID (§3). 원본의 `msgId` 그대로 | 게임 |
| `flags` | bit0 `EVENT` = 1 이면 서버 이벤트. 나머지 비트는 0 | 서버 송신 헬퍼가 찍는다 |
| `version` | **엔진 헤더 버전** `0x0001` (major 1, minor 0). 게임 `protocolVersion` 과 다른 것이다 | 고정 |
| `payload_length` | payload 바이트 수. **command 를 포함하지 않는다.** 최대 65535 | 게임 |

- 프레임 전체 크기는 `24 + payload_length`.
- `magic` 불일치·`version` major 불일치는 **엔진이** 연결을 끊는다. 게임 코드가 검사할 것은 `command` 와 payload 내용뿐이다.
- TCP 는 스트림이지만 **누적 버퍼는 엔진이 한다.** `onPacket` 에는 헤더와 payload 가 완성된 패킷 하나가 온다.
  부분 수신·붙어 온 프레임을 게임 코드가 다시 처리하지 않는다.

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

서버 → 클라이언트 `SNAPSHOT` (이벤트, 세션 시퀀스 7, 플레이어 0명):

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
> 반드시 필드 단위로 쓰고 읽어라(`PayloadWriter` / `PayloadReader`). 헤더만은 예외다 — 엔진 `PacketHeader` 는
> 모든 필드가 `LittleEndian<T>` 라 정렬 1, 패딩 0 이고 `writePacket` 이 알아서 쓴다.

### 2.4 응답과 이벤트 — 엔진이 요구하는 구분

엔진의 송신 경로는 둘이고 헤더 규칙과 드롭 정책이 다르다. **서버→클라이언트 메시지마다 어느 쪽인지 정해져 있다(§3.2 표).**

| | `correlation_id` | `flags` | 송신 큐가 꽉 찼을 때 | 용도 |
| --- | --- | --- | --- | --- |
| **응답** | 요청 값 반사 | 0 | `DropPolicy::Never` — 버리지 않고 **연결 종료** | 요청 1개에 정확히 1개 |
| **이벤트** | 세션별 시퀀스 (1부터 증가) | `EVENT` | `DropPolicy::Never` (기본) | 요청과 무관하게 서버가 보내는 것 |
| **스냅샷** | 세션별 시퀀스 (이벤트와 같은 카운터) | `EVENT` | `DropPolicy::Oldest` — 오래된 것부터 버린다 | `SNAPSHOT` 하나뿐 |

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

---

## 3. 메시지 전체 목록 (필드 순서가 곧 바이트 순서다)

### 3.1 Client → Server

| ID | 이름 | 페이로드 | 로그인 전 허용 |
|---|---|---|---|
| `0x0001` | HELLO | `u16 protocolVersion`, `u16 clientBuild` | ○ |
| `0x0002` | LOGIN | `str accountId`, `str password` | ○ (HELLO 이후) |
| `0x0003` | SELECT_CHARACTER | `u16 characterId` | |
| `0x0004` | MOVE_INPUT | `u32 seq`, `f32 dirX`, `f32 dirY`, `u32 clientTimeMs` | |
| `0x0005` | ATTACK | `u32 seq`, `u8 facing` | |
| `0x0006` | INTERACT_CHEST | `u32 chestId` | |
| `0x0007` | REQUEST_CHUNKS | `u16 count`, `count × { i32 chunkX, i32 chunkY }` | |
| `0x0008` | PING | `u32 clientTimeMs` | ○ |
| `0x0009` | RESPAWN_REQUEST | (없음) | |
| `0x000A` | USE_ITEM | `u8 slot`, `u16 itemId` | |

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

| ID | 이름 | 종류 | 페이로드 |
|---|---|---|---|
| `0x8001` | WELCOME | 응답 (HELLO) | `u16 protocolVersion`, `u16 tickRate`, `u32 serverTimeMs` |
| `0x8002` | AUTH_RESULT | 응답 (LOGIN) | `u8 ok`, `u8 errorCode`, `u32 accountId`, `str displayName` |
| `0x8003` | CHARACTER_LIST | 이벤트 | `u8 count`, `count × u16 characterId` |
| `0x8004` | ENTER_WORLD | 응답 (SELECT_CHARACTER) | 아래 상세 |
| `0x8005` | SNAPSHOT | **스냅샷** (`Oldest`) | `u32 tick`, `u16 count`, `count × { u32 playerId, f32 x, f32 y, u8 facing, u8 motion, u16 hp }` |
| `0x8006` | PLAYER_JOIN | 이벤트 | `u32 playerId`, `u16 characterId`, `str displayName`, `f32 x`, `f32 y`, `u8 facing`, `u16 hp`, `u16 maxHp` |
| `0x8007` | PLAYER_LEAVE | 이벤트 | `u32 playerId`, `u8 reason` |
| `0x8008` | COMBAT_EVENT | 이벤트 | `u32 attackerId`, `u8 facing`, `u16 hitCount`, `hitCount × { u32 targetId, u16 damage, u16 targetHp }` |
| `0x8009` | DEATH | 이벤트 | `u32 playerId`, `u32 killerId`, `u8 respawnRule` |
| `0x800A` | WORLD_OBJECTS | 이벤트 | `i32 chunkX`, `i32 chunkY`, `u16 chestCount`, `chestCount × { u32 chestId, f32 x, f32 y, u8 opened }` |
| `0x800B` | CHEST_RESULT | 응답 (INTERACT_CHEST) | `u32 chestId`, `u8 ok`, `u8 errorCode`, `u8 grantCount`, `grantCount × { u16 itemId, u16 qty }` |
| `0x800C` | INVENTORY_FULL | 이벤트 | `u8 slotCount`, `slotCount × { u8 slot, u16 itemId, u16 qty }` |
| `0x800D` | INVENTORY_DELTA | 이벤트 | `u8 count`, `count × { u8 slot, u16 itemId, u16 qty }` |
| `0x800E` | PONG | 응답 (PING) | `u32 clientTimeMs`, `u32 serverTimeMs` |
| `0x800F` | RESPAWN | 응답 (RESPAWN_REQUEST) | `u32 playerId`, `f32 x`, `f32 y`, `u16 hp`, `u16 maxHp`, `u8 needCharacterSelect` |
| `0x8010` | KICK | 이벤트 | `u8 code`, `str message` |
| `0x8011` | ITEM_USE_RESULT | 응답 (USE_ITEM) | `u8 slot`, `u16 itemId`, `u8 ok`, `u8 errorCode` |

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
  `Oldest` 정책이라 큐가 밀리면 오래된 스냅샷부터 버려지는데, 전체 상태이므로 **버려져도 손실이 없다.** 이것이 스냅샷만 `Oldest` 인 이유다.
- **INVENTORY_FULL 은 슬롯 전체 교체**, **INVENTORY_DELTA 는 부분 갱신**이다.
  델타에서 `qty = 0` 은 "그 슬롯을 비워라"라는 뜻이다.
- `str` 은 UTF-8 이다. 한글 이름도 그대로 보낼 수 있다.

### 3.3 `GameConnection` — 엔진 `Connection` 과 게임의 경계

```cpp
class GameConnection : public kiotty::Connection
{
public:
    GameConnection(kiotty::SocketHandle accepted, kiotty::IOMultiEventListener& listener,
                   kiotty::BlockPool& pool, size_t send_queue_limit);   // ConnectionTable 이 요구하는 시그니처
    ~GameConnection();                                                   // GameServer 에서 자신을 등록 해제 (§3.4)

    void sendResponse(uint16_t command, uint32_t correlation_id, kiotty::ByteView payload);  // Never
    void sendEvent(uint16_t command, kiotty::ByteView payload);                               // EVENT, 시퀀스, Never
    void sendSnapshot(kiotty::ByteView payload);                                              // EVENT, 시퀀스, Oldest

    SessionStage stage() const;      // Connected → Greeted → Authenticated → InWorld → Dead

private:
    void onOpened() override;                         // 아무것도 안 한다 (HELLO 를 기다린다)
    void onPacket(kiotty::ReceivedPacket& packet) override;   // command 로 분기 → GameServer 호출
    void onClosed() override;                         // 로그만. 정리는 소멸자

    uint32_t nextEventSequence();
};
```

- `GameServer` 는 전역이 아니라 **`kmg_main.cpp` 가 만든 하나**이며, `ConnectionTable<GameConnection>` 이 4-인자 생성자로만 짓기 때문에
  `GameConnection` 은 생성자에서 `GameServer` 를 받을 수 없다. `GameServer::bind(GameConnection&)` 를 `onOpened` 에서 부르는 대신
  **`GameConnection` 이 정적 포인터 하나(`GameServer* GameConnection::s_server`)로 서버를 찾는다.** `kmg_main` 이 `IoLoop::run()` 전에 설정한다.
  이것이 [HANDOVER.md](../docs/progress/HANDOVER.md) §7-3 이 "하네스는 전역으로 넘겼다"고 적은 바로 그 제약이다. 다른 방법이 필요하면 보고한다.
- `onPacket` 에서 하는 일은 넷뿐이다 — (1) `stage()` 로 로그인 전 게임 메시지 거부, (2) `command` 분기, (3) `PayloadReader` 로 파싱,
  (4) `GameServer` 의 해당 메서드 호출. **게임 규칙은 여기 두지 않는다.**
- `sendEvent` 와 `sendSnapshot` 은 같은 시퀀스 카운터를 쓴다(커넥션당 1개). 응답은 카운터를 건드리지 않는다.
- 모든 송신 헬퍼는 `kiotty::writePacket(pool, command, flags, correlation_id, payload)` → `Connection::emit(packet, policy)` 두 줄이다.
  `emit` 은 **아무 스레드나 부를 수 있고 블록하지 않는다.** 틱 스레드가 브로드캐스트할 때 그대로 쓴다.
- `payload.size() > 65535` 면 `writePacket` 이 빈 `Bytes` 를 준다. 이 게임의 최대 패킷은 SNAPSHOT(4인 기준 60바이트 남짓)이라 일어나지 않지만 검사한다.

### 3.4 스레드 모델 — IO 루프 스레드 + 틱 스레드, 게임 상태 뮤텍스 1개

엔진 `IoLoop::run()` 은 IO 완료만 돌린다. 20Hz 틱을 걸 자리가 없으므로 **틱 스레드를 따로 둔다.**

```
IO 루프 스레드 (IoLoop::run)                 틱 스레드 (GameServer::runTicks)
  onPacket → GameServer::onXxx(conn, ...)      50ms 마다:
     lock(_state_mutex)                           lock(_state_mutex)
       상태 변경, 응답 emit                          입력 적분, 전투 쿨다운, 타임아웃
     unlock                                         SNAPSHOT 을 전원에게 emit
  ~GameConnection                                unlock
     lock(_state_mutex)
       플레이어 표에서 제거, PLAYER_LEAVE 브로드캐스트
     unlock
```

- **게임 상태 전체(플레이어 표·월드·상자)는 `GameServer::_state_mutex` 하나가 지킨다.** 4인 20Hz 에서 임계 구역은 수십 µs 이고 경합이 측정되지 않는다.
  입력 큐·워커·락프리 구조를 **만들지 않는다.** 필요해지면 측정치와 함께 제안한다.
- **커넥션 포인터의 수명.** 엔진은 끊긴 `Connection` 을 IO 루프 스레드에서 `reapClosed()` 로 파괴한다 (HANDOVER §5.4).
  틱 스레드가 `Player::connection` 으로 `emit` 하는 도중에 파괴되면 안 되므로, **`~GameConnection` 이 `_state_mutex` 를 잡고 플레이어 표에서 자신을 빼는 것**이
  유일한 보호 장치다. 틱 스레드는 락 안에서만 커넥션 포인터를 역참조한다. 락 밖으로 포인터를 들고 나가지 않는다.
- **DB 쓰기는 틱 스레드·IO 스레드 어느 쪽에서든 락을 잡은 채 한다.** SQLite 쓰기 1회는 WAL 모드에서 밀리초 단위이고, 쓰기가 일어나는 메시지는
  상자 개방·아이템 사용·계정 생성뿐이다. 이것이 IO 루프를 그만큼 세우는 것은 알고 있다 — 4인 규모에서 허용하고, **`fallbackCount()` 나 틱 지연이
  측정되면** 그때 워커로 뺀다.
- `IoLoop::run()` 은 `wait(1000ms)` 하트비트를 돈다. 서버 종료는 `IoLoop::stop()` 후 틱 스레드 `join`.

---

## 4. 표준 흐름

```
TCP connect                      (엔진: accept → ConnectionTable 슬롯 → GameConnection 생성 → submitReceive)
  C→S HELLO
  S→C WELCOME                     응답. 버전 불일치면 KICK 이벤트 후 Connection::close()
  C→S LOGIN
  S→C AUTH_RESULT(ok=1)           응답. 실패면 ok=0 + errorCode, 클라이언트는 연결을 끊는다
  S→C CHARACTER_LIST              이벤트 (v1 기준 1,2,3,4)
  C→S SELECT_CHARACTER
  S→C ENTER_WORLD                 응답
  S→C INVENTORY_FULL              이벤트 ← 반드시 보낼 것. 없으면 클라이언트 인벤토리가 빈 상태로 남는다
  S→C PLAYER_JOIN × N             이벤트 ← 이미 접속 중인 다른 플레이어들
  (다른 클라이언트들에게) S→C PLAYER_JOIN   이벤트 (새로 들어온 이 플레이어)
  C→S REQUEST_CHUNKS              ← 클라이언트가 카메라 주변 청크(초기 5x5)를 요청
  S→C WORLD_OBJECTS × N           이벤트
  --- 이후 루프 ---
  C→S MOVE_INPUT / ATTACK / INTERACT_CHEST / USE_ITEM / REQUEST_CHUNKS / PING
  S→C SNAPSHOT(20Hz, 틱 스레드) / COMBAT_EVENT / CHEST_RESULT / INVENTORY_DELTA / DEATH
      / ITEM_USE_RESULT / WORLD_OBJECTS / PONG
```

접속 상한(`ServerFull`)은 두 겹이다 — `ConnectionTable` 용량을 넘으면 **엔진이 accept 자체를 거절**하고(패킷 없이 끊김),
용량 안이지만 게임 정원을 넘으면 `AUTH_RESULT(ServerFull)` 로 거절한다. 테이블 용량은 게임 정원보다 크게 잡는다(예: 정원 4, 테이블 8).

### 사망 → 재시작 흐름 (중요)

```
HP 0 → S→C DEATH(playerId, killerId, respawnRule = 1 MustReselectCharacter)  ※ 전체 브로드캐스트 (이벤트)
클라이언트: Dead 화면 → 사용자가 "다시 시작" 클릭
C→S RESPAWN_REQUEST
S→C RESPAWN(playerId, x, y, hp, maxHp, needCharacterSelect = 1)              응답
클라이언트: 캐릭터 선택 화면으로 돌아감
C→S SELECT_CHARACTER            ← 다시 선택. 서버는 이 재선택을 허용해야 한다
S→C ENTER_WORLD + INVENTORY_FULL + PLAYER_JOIN × N   ← 첫 진입과 동일하게 다시 보낸다
```

- **사망해도 인벤토리는 지운다/초기화하지 않는다.** 인벤토리는 캐릭터가 아니라 **계정**의 데이터다.
- `needCharacterSelect = 0` 으로 응답하면 클라이언트는 재선택 없이 그 자리에서 부활한다(선택지로 남겨둠).

### 연결 종료 흐름

```
클라이언트 끊김 / close() / 타임아웃
  → 엔진: 떠 있는 IO 가 0 이 되면 onClosed() (IO 루프 스레드)
  → 엔진: 다음 wait() 뒤 reapClosed() 에서 ~GameConnection
      → _state_mutex 잡고 Player 제거, 남은 전원에게 PLAYER_LEAVE(reason) 이벤트
```

reason 은 `GameConnection` 이 기억한다 — 타임아웃으로 서버가 `close()` 했으면 `Timeout(1)`, KICK 뒤 `close()` 면 `Kick(2)`, 그 외 `Disconnect(0)`.

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

| 요청 | 검증 |
|---|---|
| 헤더 (`magic`·`version`·`payload_length`) | **엔진이 한다.** 게임 코드는 검사하지 않는다 |
| 모든 payload | `PayloadReader` 경계 검사. 짧거나 남으면 **그 메시지를 버리고 로그**. 연결은 유지 |
| `HELLO` | `protocolVersion == 3`. 아니면 `KICK(VersionMismatch)` 이벤트 후 `close()`. HELLO 전에 다른 메시지가 오면 무시 |
| `LOGIN` | HELLO 를 거쳤는지. 계정 존재/비밀번호 일치. 동일 계정 중복 접속은 `AlreadyOnline`(2) 로 거절하고, 필요하면 이전 세션을 정리. 이미 로그인한 커넥션의 재 LOGIN 은 무시 |
| `SELECT_CHARACTER` | `CHARACTER_LIST` 로 준 id 집합에 속하는지. 아니면 KICK 또는 무시 |
| `MOVE_INPUT` | `dir` 길이가 1을 넘으면 정규화. NaN/Inf 방어. **좌표는 절대 클라이언트에서 받지 않는다** |
| `ATTACK` | 쿨다운 500ms 경과 여부, 사망 상태 여부. 대상은 서버가 사거리/방향으로 선택 |
| `INTERACT_CHEST` | 상자 존재, 거리 ≤ interactRange, 이미 열렸는지, 인벤토리 여유 |
| `USE_ITEM` | 슬롯 존재, `itemId` 일치, 사용 가능한 아이템인지, 사망 여부, HP 만충 여부 |
| `REQUEST_CHUNKS` | `count` 상한(예: 64). 넘으면 버린다 |
| 공통 | 로그인 전 게임 메시지 무시, 알 수 없는 `command` 는 로그만 남기고 무시(연결 유지) |

- 비밀번호는 개발 단계에서 평문으로 온다(TLS 없음). DB 에는 **평문 저장 금지** —
  최소 `SHA-256 + per-account salt` 로 저장하라. SHA-256 은 외부 의존 없이 게임 코드에 구현한다(엔진 `repository/cryptor` 는 아직 없다).
  운영 단계에서 TLS 또는 챌린지-응답으로 교체할 예정이며, 그때 `protocolVersion` 을 올린다.
- 계정이 없으면 자동 생성할지(개발 편의) 거절할지는 서버 판단. 자동 생성 시에도 위 해시 규칙을 따르라.

---

## 7. 영속화 (SQLite)

**요구사항**: 클라이언트를 강제 종료(프로세스 킬)해도, 재접속하면 인벤토리가 그대로 남아 있어야 한다.

- **서버에서 지급이 확정된 시점에 즉시 DB 에 쓴다.** 종료 시점에 몰아서 저장하지 마라
  (서버가 죽으면 유실된다).
- WAL 모드(`PRAGMA journal_mode=WAL`)를 켠다. 쓰기 주체는 서버 프로세스 하나다.
- 스키마 예시:

```sql
CREATE TABLE IF NOT EXISTS accounts (
  account_id   INTEGER PRIMARY KEY AUTOINCREMENT,
  login_id     TEXT UNIQUE NOT NULL,
  pw_hash      TEXT NOT NULL,
  pw_salt      TEXT NOT NULL,
  display_name TEXT NOT NULL,
  created_at   INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS inventories (
  account_id INTEGER NOT NULL,
  slot       INTEGER NOT NULL,
  item_id    INTEGER NOT NULL,
  qty        INTEGER NOT NULL,
  PRIMARY KEY (account_id, slot)
);

-- 상자 상태도 영속화하면 서버 재시작 후에도 "이미 열린 상자"가 유지된다(권장).
CREATE TABLE IF NOT EXISTS chests (
  chest_id INTEGER PRIMARY KEY,
  chunk_x  INTEGER NOT NULL,
  chunk_y  INTEGER NOT NULL,
  x        REAL NOT NULL,
  y        REAL NOT NULL,
  opened   INTEGER NOT NULL
);
```

- 로그인 성공 → `inventories` 조회 → `ENTER_WORLD` 직후 **`INVENTORY_FULL`** 로 전송.
- 아이템 지급/소모 → 슬롯 갱신 → DB 쓰기(트랜잭션) → **`INVENTORY_DELTA`** 전송.
- 슬롯 규칙: 같은 `itemId` 스택에 먼저 채우고(상한 99), 남으면 빈 슬롯(0~5)에 넣는다.
  6칸이 모두 차서 넣을 수 없으면 `ChestError::InventoryFull` 로 거절하고 **상자를 열지 않은 상태로 둔다.**
- `sqlite3*` 와 `sqlite3_stmt*` 는 RAII 래퍼로 감싼다. 원시 `sqlite3_finalize` 호출을 코드에 남기지 않는다.

---

## 8. 구현 순서 (이 순서대로 진행하고 각 단계마다 검증하라)

각 단계가 끝나면 §9 의 방법으로 동작을 확인한 뒤 다음 단계로 넘어간다.
**단계마다 g++ `-Wall -Wextra -Wpedantic` / MSVC `/W4` 워닝 0 을 유지한다.** 엔진이 그 기준이다.

1. **조립 + HELLO/PING**: `prj/mini_game/CMakeLists.txt`, `kmg_main.cpp` 에서
   `BlockPool` → `ConnectionTable<GameConnection>` → `Endpoint` → `IoLoop` 를 HANDOVER §5 순서로 조립.
   `PayloadReader/Writer`, `GameConnection` 송신 헬퍼. `HELLO` → `WELCOME`, `PING` → `PONG`(클라이언트가 1초마다 보낸다).
   **§2.2 의 바이트 예시와 대조한다.**
2. **로그인**: SQLite 스키마 생성, 계정 조회/생성, 해시 검증, `AUTH_RESULT`, `CHARACTER_LIST`.
3. **월드 진입**: `SELECT_CHARACTER` → playerId 발급 → `ENTER_WORLD` → `INVENTORY_FULL`(빈 상태여도 보낸다).
4. **틱 루프 + 이동**: 틱 스레드(§3.4) 시작, 20Hz 고정 틱, `MOVE_INPUT` 저장 → 틱마다 적분 → 전원에게 `SNAPSHOT`(`sendSnapshot`).
   `facing` 은 입력 방향에서 계산(좌우 우선), `motion` 은 Idle/Walk 전환.
   **여기서 `~GameConnection` 의 등록 해제 경로를 같이 만든다.** 없으면 다음 단계에서 dangling 이 난다.
5. **다중 접속**: `PLAYER_JOIN` / `PLAYER_LEAVE`, 신규 접속자에게 기존 플레이어 목록 전송.
   여기서 **클라이언트 2개 이상**을 띄워 서로 보이는지 확인하라. Linux 라면 TSan 으로 한 번 돌린다(§9.3).
6. **전투**: `ATTACK` → 쿨다운/사거리/방향 검증 → 데미지 2 → `COMBAT_EVENT`(맞은 사람 목록, 남은 HP).
   HP 0 → `Motion::Dead` + `DEATH` 브로드캐스트. `RESPAWN_REQUEST` → `RESPAWN(needCharacterSelect=1)`.
7. **상자**: 청크별 상자 생성/영속화, `REQUEST_CHUNKS` → `WORLD_OBJECTS`,
   `INTERACT_CHEST` → 검증 → `CHEST_RESULT` + `INVENTORY_DELTA`.
   상자가 열리면 **그 청크를 보고 있는 다른 클라이언트에게도** `WORLD_OBJECTS` 를 다시 보내
   `opened=1` 을 반영하게 하라(클라이언트는 같은 chestId 를 받으면 상태만 갱신한다).
8. **아이템 사용**: `USE_ITEM` → 검증 → HP 회복 + 수량 1 감소 → `ITEM_USE_RESULT` + `INVENTORY_DELTA`.
   회복된 HP 는 다음 `SNAPSHOT` 으로 자연히 전달된다.
9. **영속성 검증**: 아이템을 얻은 뒤 **클라이언트 프로세스를 강제 종료**하고 재접속 →
   인벤토리가 복구되는지 확인. 서버를 재시작해도 유지되는지 확인.
10. **안정화**: 4인 동시 접속, 갑작스러운 연결 종료(케이블 뽑힘 수준) 처리, 좌표/HP 이상값 방어,
    로그 정리, 타임아웃(10초간 아무 패킷도 없는 연결을 틱 스레드가 `close()` → `PLAYER_LEAVE(reason=1 Timeout)`).
    `BlockPool::fallbackCount()` 가 0 인지 확인하고 보고한다.

**유닛 테스트는 `cpp-tester` 에게 위임한다.** 단계 1(Reader/Writer), 6(Combat), 7·8(Inventory)이 컴파일되는 시점에 부른다.
엔진 `test/unit` 과 같은 GoogleTest 구성을 `prj/mini_game/test/unit` 에 둔다.

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

`prj/mini_game/test/kmg_test_client.cpp` — 엔진 `test/integration/kiotty_io_event_listener_smoke.cpp` 처럼 소켓을 직접 열어
위 `--smoke` 1~5 를 그대로 따라 하는 프로그램. 24바이트 헤더를 직접 쓰고, `flags.EVENT` 로 응답/이벤트를 가른다.
CTest 에 `RUN_SERIAL` 로 등록한다(고정 포트).

### 9.3 플랫폼별 검증

| 검증 | Windows / MSVC | Linux / GCC |
| --- | --- | --- |
| 워닝 0 | `/W4` | `-Wall -Wextra -Wpedantic` |
| 메모리 오류 | `/fsanitize=address` (런타임이 있으면) | `-fsanitize=address,undefined` |
| 데이터 레이스 (§3.4 뮤텍스 경로) | **없음** | `-fsanitize=thread` — epoll·io_uring 양쪽 |
| 백엔드 | iocp | epoll (`-DKIOTTY_USE_IO_URING=OFF`), io_uring |

**돌리지 못한 검증은 돌리지 못했다고 보고한다.**

---

## 10. 자주 발생하는 실수 (미리 피하라)

1. **`payload_length` 에 `command` 를 포함함** → 원본 프로토콜의 `payloadLen` 과 다르다. 엔진 헤더의 `payload_length` 는 **payload 만**이다.
   §2.2 의 바이트 예시로 반드시 대조하라.
2. **구조체 `memcpy` 직렬화** → 패딩 때문에 필드가 밀린다. 필드 단위로 써라(헤더는 엔진 `writePacket` 에 맡긴다).
3. **엔디안 혼동** → 빅엔디안 변환 함수(`htons` 등)를 쓰지 마라. 리틀엔디안 고정이다. `LittleEndian<T>` 를 쓴다.
4. **`SNAPSHOT` 을 변경된 플레이어만 보냄** → 클라이언트는 전체 스냅샷을 기대한다.
5. **`INVENTORY_FULL` 을 안 보냄** → 인벤토리가 있는데도 빈 칸으로 보인다. 월드 진입마다 보내라.
6. **`COMBAT_EVENT` 를 맞았을 때만 보냄** → 헛스윙도 `hitCount=0` 으로 보내야 클라이언트가 연출한다.
7. **응답을 이벤트로, 이벤트를 응답으로 보냄** → 클라이언트의 `correlation_id` 대기 슬롯이 영영 안 깨어나거나 엉뚱한 것이 깨어난다. §3.2 표를 따른다.
8. **`SNAPSHOT` 이외를 `sendSnapshot`(`Oldest`) 으로 보냄** → 큐가 밀리면 `DEATH`·`INVENTORY_DELTA` 가 조용히 사라진다.
9. **락 밖으로 `GameConnection*` 를 들고 나감** → `reapClosed()` 가 그 사이에 파괴한다. §3.4.
10. **재선택(사망 후) 경로 누락** → `RESPAWN(needCharacterSelect=1)` 뒤에 오는
    `SELECT_CHARACTER` 를 처리하지 않으면 사용자가 다시 게임에 들어올 수 없다.
11. **사망 시 인벤토리 삭제** → 요구사항 위반이다. 인벤토리는 계정 데이터다.
12. **엔진을 고쳐서 해결함** → 절대 원칙 5 위반. 보고한다.

---

## 11. 프로토콜을 바꾸고 싶을 때

이 프로토콜은 클라이언트가 기준 문서를 관리한다(`docs/PROTOCOL.md`). 이 개정으로 **서버 쪽이 3 을 제안한 상태**이며
클라이언트 팀이 문서를 갱신해야 확정된다. 서버 쪽에서 필드 추가/변경이 필요하면 **클라이언트 팀에 먼저 알리고 문서를 갱신한 뒤**
`protocolVersion` 을 올려라. 버전이 다르면 클라이언트는 `HELLO` 직후 `KICK(VersionMismatch)` 를 받고 접속을 중단한다.

헤더의 `version`(엔진 1.0)은 게임 `protocolVersion` 과 별개다. 엔진 헤더가 바뀌는 일은 엔진 쪽 결정이다.

미확정으로 남아 있는 사항(서버가 정하고 클라이언트 팀에 통보하면 되는 것):

- 상자 재생성 여부(현재는 1회성) / 청크당 상자 확률 / 지급 아이템 확률
- 스폰 위치 규칙, 사망 후 규칙(`RespawnRule` 값 선택)
- 계정 자동 생성 허용 여부, 타임아웃 시간
- 서버 재시작 시 상자 상태 유지 여부

엔진 쪽에 요청할 수 있는 것(이 문서의 결정을 바꾸는 것들):

- `TCP_NODELAY` 설정 (§1)
- `Request` 에 커넥션 식별자 추가 → usecase 계층으로 이전 (§1.1)
- `IoLoop` 타이머/틱 훅 → 틱 스레드와 뮤텍스 제거 (§3.4)
