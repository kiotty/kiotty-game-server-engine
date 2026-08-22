---
name: api-naming
description: 이 저장소의 공개 라이브러리 API, 특히 C ABI(platform_gl_overlay.h 등 L2 계층)의 함수·타입·enum·필드 이름을 짓거나 리뷰할 때 사용한다. pgo 접두어 기반의 명명규칙을 적용한다.
---

# 라이브러리 API 명명규칙 (PlatformGLOverlay C ABI)

공개 C API를 새로 만들거나 고칠 때 아래 규칙을 따른다. 라이브러리 접두어는 **`pgo`**
(PlatformGLOverlay)다.

## 요약 표

| 대상 | 규칙 | 예 |
| --- | --- | --- |
| 함수 | `pgo_` + **camelCase** | `pgo_createContext`, `pgo_drawLine` |
| 타입(핸들·구조체·enum 타입) | `Pgo` + **PascalCase** | `PgoContext`, `PgoRect`, `PgoRenderMode` |
| enum 값·상수·매크로 | `PGO_` + **UPPER_SNAKE** | `PGO_RENDER_MODE_CONTINUOUS` |
| 구조체 필드·함수 파라미터 | **snake_case** | `origin_x`, `stroke_width` |

## 1. 함수 — `pgo_` + camelCase

함수명은 접두어 `pgo_` 뒤에 **camelCase** 동사구를 붙인다. 접두어와 이름은 `_`
하나로만 잇고, camelCase 부분 안에서는 `_`를 쓰지 않는다.

```c
PgoContext  pgo_createContext(const PgoContextDesc* desc);
void        pgo_destroyContext(PgoContext ctx);
void        pgo_drawLine(PgoDrawTool tool, const PgoLine* line);
int         pgo_addLayer(PgoRenderer renderer, const PgoLayerDesc* desc);
```

- 생성/해제 쌍은 `create*` / `destroy*`로 맞춘다.
- 동사로 시작한다(`get*`, `set*`, `draw*`, `add*`, `remove*`).

## 2. 타입 — `Pgo` + PascalCase

opaque 핸들, POD 구조체, enum 타입 모두 접두어를 **PascalCase로 붙인** `PgoXxx`.
C++ 코어의 타입명과 자연스럽게 대응된다.

```c
typedef struct PgoContext_* PgoContext;   // opaque handle
typedef struct PgoRenderer_* PgoRenderer;

typedef struct {                          // POD 구조체
    float origin_x, origin_y;
    float width, height;
} PgoRect;
```

## 3. enum — 타입은 `PgoXxx`, 값은 `PGO_UPPER_SNAKE`

enum **타입 이름**은 §2 규칙(`PgoRenderMode`)을 따르고, **값**은 `PGO_` 접두어의
UPPER_SNAKE로 짓는다. 값 이름에는 타입 의미를 담아 전역에서 충돌하지 않게 한다.

```c
typedef enum {
    PGO_RENDER_MODE_CONTINUOUS,
    PGO_RENDER_MODE_ON_DEMAND,
} PgoRenderMode;
```

상수·매크로도 같은 `PGO_` UPPER_SNAKE를 쓴다. 예: `PGO_MAX_LAYERS`.

## 4. 필드·파라미터 — snake_case

구조체 필드와 함수 파라미터 이름은 **snake_case**로 쓴다. (함수명 camelCase와
다르니 주의 — 의도된 구분이다: 호출부 식별자는 camelCase, 데이터/인자는 snake_case.)

```c
typedef struct {
    float    stroke_width;
    uint32_t color_rgba;
    int      layer_index;
} PgoStroke;

void pgo_setViewport(PgoRenderer renderer, int width, int height, float device_scale);
```

## 적용 범위

- 이 규칙은 **공개 C ABI**(L2, `platform_gl_overlay.h` 계열)에 적용한다.
- C++ 코어 내부(L0) 타입·함수는 이 규칙을 강제하지 않는다. 다만 C ABI로 노출되는
  경계 타입은 위 규칙에 맞춰 래핑한다.
