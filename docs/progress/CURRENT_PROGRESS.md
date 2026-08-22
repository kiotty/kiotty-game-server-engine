# 현재 진행상황 — 2026-08-22

## 한 줄 요약
presentation 과 domain 이 `GameChannel` 위의 푸시 스트림으로만 데이터를 주고받는 경계까지
구현·검증이 끝났고, 다음은 Session 실물화와 Worker 다.

## 완료
- `core/` — `ISink` · `StreamListener` · `IStream` · `MutableStream` (고정 배열, 힙 0),
  `Result<E, T&>` 참조 특수화 + `okRef()`
- `domain/entity/` — `ChannelId{index, generation}` · `ChannelCode` · `ConnectionInfo`(IPv4 값 복사) ·
  `GameRequest` · `GameResponse` · `GameEvent`
- `domain/channel/` — `GameChannel`(`IoGameChannel` / `BusinessGameChannel` 참조 뷰),
  `GameChannelPool` + RAII `ChannelAccess`(`recursive_mutex`), `IChannelBinder` + `ChannelPoolBinder`
- `domain/codec/` — `IPacketCodec` + `DefaultPacketCodec`
- `UsecaseDispatcher` 가 `StreamListener<GameRequest>` 로, `IUsecase::execute(const GameRequest&, BusinessGameChannel&)`
- `Connection` 은 구체 클래스(상속 없음), `ConnectionTable` 은 비템플릿으로 binder·codec 주입.
  `open(SocketHandle&)` 이 핸들 소유권을 명시적으로 가져가 이중 close 방지
- scratch 하네스 3종 · 통합 스모크 · 유닛 테스트(신규 177개) 이전
- 문서: `docs/progress/code_refactoring_plan.md`(결정·결과), `HANDOVER.md` §1·§4·§5·§6·§7 갱신

## 진행 중 / 다음 할 일
1. Session 실물화 — `GameRequest.authenticated` 플래그를 세션 조회로, 세션은 `ChannelId`(generation 포함) 기억
2. Worker — 채널 접근은 작업 마지막에, 소켓 없는 더블로 `access`/`remove` 교차를 TSan 검증
3. `ConnectionTable::open` 포인터 반환 → `Result<SocketCode, Connection&>`
4. `SendBuffer` → `SendQueue` 개명
5. 기존 실패 테스트 `RingBuffer.TryPushOnFullBufferLeavesAMovedValueWithTheCaller` 처리
   (가득 찬 버퍼에 `tryPush` 하면 인자를 소비함)

## 핵심 결정·메모
- nullable API 를 두지 않는다. 실패는 `Result`, 성공은 참조. 채널은 `access()` 로만 접근
- 스트림은 동기 푸시 — `emit` 이 돌아오면 아이템은 더 이상 참조되지 않는다. 이벤트 팬아웃에
  refcount 없음(커넥션마다 인코딩)
- 풀 락은 `create`·`remove`·`access` 공통 `std::recursive_mutex`. 같은 스레드 재진입 허용
- `Result::operator bool` 은 본체·특수화 모두 **실패일 때 true**. `ChannelAccess::operator bool`
  은 성공일 때 true — 극성 통일 여부 미결, `isOk()` 사용 권장
- `MutableStream` 리스너가 `onStream` 안에서 자신을 제거하면 그 `emit` 에서 다음 리스너 하나를
  건너뜀. 현재 코드는 닿지 않는 경로, 테스트로 동작만 고정
- 구현 소스(`src/implementation`)에는 주석을 두지 않는다

## 검증 상태
- g++ `-Wall -Wextra -Wpedantic` epoll·io_uring / MSVC `/W4` iocp — 워닝 0
- scratch `connection_check` · `io_loop_check` · `usecase_check` + 스모크 — 3 백엔드 PASS
- ASan+UBSan · TSan (epoll·io_uring 하네스 3종) — 리포트 0
- 유닛 테스트 — Linux 334/335, Windows 333/334 (실패 1건은 위 `RingBuffer` 기존 실패)
