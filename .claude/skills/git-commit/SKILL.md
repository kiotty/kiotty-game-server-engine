---
name: git-commit
description: 이 저장소에 변경사항을 git commit 할 때 사용한다. GitHub commit 규칙을 지키고, 커밋 메시지는 무조건 영어로 1~3문장 이내로 작성하며, 커밋 후 자동으로 git push 한다.
---

# git commit 규칙 (PlatformGLOverlay)

이 저장소에 커밋할 때 아래 절차와 규칙을 그대로 따른다. **커밋 메시지는 반드시 영어**,
**1~3문장 이내**, GitHub 관례를 지킨다. 커밋 후 **자동으로 push** 한다.

## 절차

1. **상태 확인** — `git status`와 `git diff --stat`로 무엇이 바뀌었는지 파악한다.
   `.gitignore` 대상(예: `/data/`, `/out/`, `script/build.sh`, 네이티브 DLL)은 커밋하지 않는다.
2. **스테이징** — 관련 변경을 모두 담는다: `git add -A`.
   (실수로 개인 파일·빌드 산출물이 잡히면 `git status`로 확인 후 제외한다.)
3. **커밋** — 아래 메시지 규칙으로 커밋한다.
4. **푸시** — 현재 브랜치를 원격에 올린다: `git push` (upstream이 없으면
   `git push -u origin <현재브랜치>`).

## 커밋 메시지 규칙 (GitHub 관례 + 영어 1~3문장)

- **영어로만** 작성한다.
- **제목 줄(첫 줄)**: 명령형(imperative), 대문자로 시작, 마침표 없음, 약 50자 이내.
  예: `Add C ABI wrapping with headless render smoke test`
- **본문(선택)**: 제목과 빈 줄로 구분하고, 왜/무엇을 1~2문장으로 덧붙인다.
  제목 포함 **전체 1~3문장**을 넘기지 않는다. 불릿을 길게 나열하지 않는다.
- 마지막에 아래 trailer를 반드시 붙인다(빈 줄 뒤):

  ```
  Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
  ```

- 훅을 건너뛰지 않는다(`--no-verify` 금지). 서명 비활성화 금지. 훅이 실패하면 원인을 고친다.

### 메시지 예시

```
Add C ABI wrapping over the core with a headless render smoke test

Wire pgo_* extern "C" functions to Context/RenderTarget/Renderer/Scene/DrawTool
and delegate GL work to the render thread; verified end-to-end via ANGLE offscreen.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
```

## 주의

- 여러 줄 메시지는 heredoc으로 전달한다(PowerShell here-string 금지). 예:

  ```bash
  git commit -m "$(cat <<'EOF'
  <제목 줄>

  <본문 1~2문장>

  Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
  EOF
  )"
  ```

- push는 현재 브랜치 대상이다. 기본 브랜치(main)에 바로 커밋·푸시하는 것이 이 저장소의
  흐름이다. push가 인증/업스트림 문제로 실패하면 사용자에게 사유를 보고한다(임의로 force push 금지).
