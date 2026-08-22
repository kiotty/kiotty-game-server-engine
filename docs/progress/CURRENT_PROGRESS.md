# 현재 진행상황 — 2026-08-22

## 한 줄 요약
datalayer(datasource · repository)가 구현·검증되어 Usecase 가 세션·저장소·암호화를 주입받을 수
있고, 엔진이 `SessionRepository` 로 인증을 판정한다. 다음은 Worker 와 Server 조립이다.

## 완료
- `domain/entity/` — `SessionCode` · `DataSourceCode` · `CryptoCode`, `AccountId`(char[64],
  `tryMakeAccountId` 는 64자 이상·null 거부), `SessionToken`(32B, 상수 시간 비교), `CipherKey`(32B),
  `isNull(ChannelId)` — 풀 generation 이 1부터 시작하므로 `{0,0}` 이 빈 id
- `core/` — `constantTimeEquals`, `IRandomSource`
- `datalayer/datasource/` — `IDataSource{readBlocking, writeBlocking}`, `InMemoryDataSource`,
  `SqliteDataSource`(WAL, amalgamation FetchContent, `KIOTTY_WITH_SQLITE`)
- `datalayer/repository/cryptor/` — `PasswordHasher`(Argon2id, phc-winner-argon2 FetchContent),
  `Cryptor`(직접 구현 AES-256-GCM: `Aes256` 블록 + CTR + GHASH, 출력 nonce12|ct|tag16),
  `SecureRandom`(BCryptGenRandom / getrandom)
- `datalayer/repository/session/` — `Session`(계정·토큰만 보유, emit 마다 Repository 에서 현재 채널
  조회), `ISessionPolicy`, `SessionRepository`(open/rebind/find/close/detach/sweep, S1~S19)
- 엔진 연결 — `UsecaseDispatcher` 가 `SessionRepository::find(channel_id)` 로 인증 판정
  (`GameRequest.authenticated` 제거), `ChannelPoolBinder` 가 끊김 시 리스너 정리 → `detach` → `remove`
- 학습 문서 `docs/learning/aes_gcm.html`, 개념 대장 `current_architecture_reference.json`, `user_profile.md`
- 유닛 테스트 약 250개 추가 (datasource · hasher · cryptor(NIST 벡터) · session · binder · dispatcher)

## 진행 중 / 다음 할 일
1. Worker — `IWorker` / `WorkerRegistry`, 채널 접근은 작업 마지막에, TSan 검증
2. Server 조립 — Usecase 에 Repository·Worker 주입, `sweep(now_ms)` 호출 주기, 그리고 rebind 가
   옛 커넥션을 닫게 하는 콜백(S13 완성, 현재는 옛 채널이 살아 있어도 emit 만 막음)
3. `history/` — 설계 없음. 요구사항 한 줄뿐이라 설계부터
4. `ConnectionTable::open` 포인터 반환 → `Result<SocketCode, Connection&>`, `SendBuffer` → `SendQueue` 개명

## 핵심 결정·메모
- AES-256 은 라이브러리 없이 직접 구현, 모드는 GCM. 사용자는 GCM 학습 문서를 아직 읽지 않았고
  구현을 먼저 지시함 — 문서는 읽기 대기
- 세션 토큰은 CSPRNG 32B 불투명 난수(OWASP 표준). JWT/HMAC 은 상태 없는 서버용이라 쓰지 않음
- 같은 계정 교체 로그인은 정원이 가득 차도 성공하고 옛 토큰은 무효화 — 의도된 동작
- `SessionRepository` 는 `IDataSource` 를 받지 않는다(세션은 메모리에만, S20)
- `detach(ChannelId)` 는 시각을 받지 않고 다음 `sweep(now_ms)` 가 첫 도장을 찍는다 — 고아 수명은
  최대 sweep 간격만큼만 길어지고 짧아지지 않음
- 정책 콜백은 세션 락 **밖**에서 부른다 — 콜백 안에서 `Session::channel()/reply()` 가 같은 락을
  재진입해 데드락 났던 것을 고침. 적용 전 토큰으로 재조회해 콜백 중 rebind 되면 무시
- 락 순서: 풀 → 세션(디스패처·바인더). 워커 경로(`Session::reply`)는 세션 락을 풀고 나서 풀 락을
  잡으므로 겹치지 않음
- 직접 구현 AES 는 S-box 조회가 캐시 타이밍에 노출된다 — 저장 데이터·토큰 용도로만 쓰고 네트워크
  스트림 암호화에는 쓰지 않는다
- `Result` 의 `ok()` 헬퍼는 move-only `Bytes` 를 못 받는다 — `Result(Success(), std::move(x))` 로 직접 생성
- 구현 소스(`src/implementation`)에는 주석을 두지 않는다

## 검증 상태
- MSVC `/W4` iocp / g++ `-Wall -Wextra -Wpedantic` io_uring — 워닝 0
- ASan+UBSan (Linux 유닛 테스트 + 스모크) — 리포트 0. TSan 은 이번에 돌리지 않음(새 스레드 없음)
- 유닛 테스트 — Windows 585/585 전부 통과 (`RingBuffer::tryPush` lvalue/rvalue 오버로드로 기존 실패 해소). Linux 는 그 수정 전 585/586
- 통합 스모크 PASS, scratch `usecase_check` · `connection_check` · `io_loop_check` PASS (Linux)
- clang-tidy(bugprone/cert/performance/misc) — 새 코드에 bugprone·cert 지적 0
