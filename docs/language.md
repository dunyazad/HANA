# HANA 언어 정의

각 항목의 상태 표시:
- **[확정]** 결정된 사항. 바꾸려면 명시적으로 다시 결정한다.
- **[초안]** 현재 기준으로 삼는 안. 언제든 바뀔 수 있다.
- **[미정]** 아직 논의하지 않았다.

## 1. 어휘

### 1.1 소스 [확정]
- UTF-8. 파일 앞의 BOM은 무시한다.
- 확장자 `.hana`.

### 1.2 주석 [확정]
```
// 한 줄 주석
/* 블록 주석 /* 중첩 가능 */ */
```

### 1.3 문장 구분 [확정]
- 줄바꿈이 문장의 끝이다. 한 줄에 여러 문장을 쓸 때는 `;`로 구분한다.
- 다음 경우에는 줄바꿈이 문장을 끝내지 않고 다음 줄로 이어진다.
  - `(` `)` 또는 `[` `]` 안. 가장 안쪽의 열린 괄호가 기준이므로, 괄호 안의 `{ }` 블록에서는 줄바꿈이 다시 문장 구분자가 된다.
  - 줄이 이항 연산자, `!`, `,`, `:`, `.` 로 끝날 때.
- 문자열 리터럴은 시작한 줄에서 끝나야 한다. 여러 줄 문자열은 없고, 줄바꿈은 `\n`으로 쓴다.

```
total = price * count +
        shipping                // continues after '+'

point = (1,
         2)                     // continues inside '('

a = 1; b = 2                    // two statements on one line
```

### 1.4 키워드 [초안]
`app` `component` `scene` `state` `action` `if` `else` `for` `in` `bind` `true` `false`

위젯 종류(`window`, `button` 등), 프로퍼티 이름, 타입 이름(`int` 등)은 키워드가 아니라 일반 식별자다.

### 1.5 리터럴 [초안]
| 종류 | 예 |
|---|---|
| int | `0`, `42` |
| float | `3.0`, `0.5` |
| bool | `true`, `false` |
| string | `"text"`, 이스케이프 `\n \t \r \0 \\ \" \{ \}` |
| 보간 문자열 | `"Count: {count}"` |
| 크기 | `300x200`, `1x0.6` |
| 튜플 | `(x, y, z)` |
| 리스트 | `[1, 2, 3]`, `[]` |
| 색 | `#ff8800`, `#ff8800cc` (16진 6자리 또는 알파 포함 8자리) [확정] |

### 1.6 연산자 [초안]
`+ - * / %`, `== != < <= > >=`, `&& || !`, `=`, `.`

## 2. 구조

### 2.1 최상위 선언 [초안]
`app`, `component`, `scene`

### 2.2 위젯: 블록 속성형 [확정]
```
kind {
    prop: value
    child_kind { ... }
}
```
- 위젯 블록 안에는 프로퍼티(`name: expr`)와 자식 위젯이 섞여 올 수 있다.
- 한 줄로 쓸 때는 `;`로 구분한다: `button { text: "+1"; on_click: increment }`

[초안] 프로퍼티가 없는 위젯도 `{}`를 쓴다: `spacer {}`

### 2.3 위젯 이름 (id) [확정: 일반 프로퍼티]
```
camera {
    id: main_cam
    position: (0, 2, 5)
}

viewport3d { camera: main_cam }
```
- 이름은 문법이 아니라 `id` 프로퍼티로 붙인다.
- [초안] `id` 값은 식별자이고, 같은 `app`/`component`/`scene` 안에서 유일해야 한다. 다른 위젯은 이 이름으로 참조한다.

### 2.4 이벤트 핸들러 [확정: action 참조와 인라인 블록 모두 허용]
```
button { text: "+1"; on_click: increment }

button {
    text: "Reset"
    on_click: {
        count = 0
    }
}
```
- [초안] 인라인 블록 안에는 action 본문과 같은 문장을 쓴다.

### 2.5 조건부 위젯과 반복 위젯 [확정: 위젯 트리 안의 if / for]
```
column {
    if count > 0 {
        label { text: "Positive" }
    } else {
        label { text: "Zero" }
    }

    for item in items {
        label { text: "{item}" }
    }
}
```
- [초안] `else if`를 허용한다.
- [초안] 조건이나 리스트가 참조하는 state가 바뀌면 해당 부분의 위젯이 다시 만들어진다.

반복 위젯의 키 [확정: 선택 사항인 `key:`]
```
for item in items key: item.id {
    row { label { text: item.name } }
}
```
- 키가 있으면 리스트가 바뀔 때 같은 키의 위젯은 상태를 유지한 채 재사용된다. 없으면 위치(인덱스)로 맞춘다.
- [초안] `key`는 키워드가 아니라 `for` 헤더에서만 의미를 갖는 이름이다.

### 2.6 state와 action [초안]
```
state count: int = 0
state name = "HANA"             // type inferred from initializer

action increment() {
    count = count + 1
}
```
- `app`과 `component` 안에 선언한다.

### 2.7 바인딩 [확정: 기본 단방향, 양방향은 `bind`로 명시]
```
slider { value: rot }           // one-way: reads rot, never changes it
slider { value: bind rot }      // two-way: dragging the slider updates rot
```
- 프로퍼티 값이 state를 참조하면, 그 state가 바뀔 때 자동으로 다시 계산된다 (단방향).
  예: `enabled: count > 0`
- 위젯이 값을 바꾸려면 `bind`를 붙인다. state가 바뀌는 곳이 코드에서 항상 드러난다.
- [초안] `bind` 뒤에는 대입 가능한 대상(state 또는 그 필드)만 올 수 있다. `bind count + 1`은 오류다.
- [초안] `bind`를 받을 수 있는 프로퍼티는 위젯마다 정해져 있다 (`slider.value`, `text_input.text`, `checkbox.checked` 등).

### 2.8 컴포넌트 [확정: `component` 키워드 사용]
```
component CounterRow(label: string, value: int) {
    row {
        label { text: "{label}: {value}" }
    }
}
```
- [초안] 컴포넌트 안에서도 자체 `state`/`action`을 선언할 수 있다.
- [초안] 사용할 때는 내장 위젯과 같은 블록 속성형으로 쓰고, 매개변수는 프로퍼티로 넘긴다:
  `CounterRow { label: "A"; value: count }`

## 3. 타입 [확정: 정적 타입 + 추론]
- 실행 전에 Analyzer가 타입 오류, 없는 프로퍼티, 잘못된 참조를 검사한다.
- 초기값이 있으면 타입 표기를 생략할 수 있다.

### 3.1 기본 타입 [확정]
| 타입 | 뜻 | 값의 예 |
|---|---|---|
| `int` | 정수 | `42` |
| `float` | 실수 | `3.0` |
| `bool` | 참/거짓 | `true` |
| `string` | 문자열 | `"text"` |
| `size` | 너비 x 높이 | `300x200` |
| `vec2` | 2차원 벡터 | `(1, 2)` |
| `vec3` | 3차원 벡터 | `(0, 2, 5)` |
| `color` | 색 | `#ff8800` |
| `list<T>` | T의 리스트 | `[1, 2, 3]` |

- [초안] 튜플 리터럴은 기대하는 타입에 따라 `vec2`/`vec3`가 된다.
- [초안] `int`는 `float` 자리에 쓸 수 있다 (`fov: 60`). 반대 방향은 명시적 변환이 필요하다.

## 4. 예시 [초안]

```
app Counter {
    state count: int = 0

    action increment() {
        count = count + 1
    }

    action reset() {
        count = 0
    }

    window {
        title: "Counter"
        size: 300x200

        column {
            spacing: 8

            label { text: "Count: {count}" }

            row {
                button { text: "+1"; on_click: increment }
                button {
                    text: "Reset"
                    on_click: reset
                    enabled: count > 0
                }
            }
        }
    }
}
```

## 5. 3D

### 5.1 범위 [확정: 인터랙티브 콘텐츠]
- 포함: glTF 모델과 PBR 렌더링, 카메라, 조명, 피킹, glTF 애니메이션 재생, 트윈, 프레임 갱신, 물리, 파티클, 3D 공간 안의 2D UI(`panel3d`)
- 제외: XR (OpenXR, WebXR)

### 5.2 씬 선언과 참조 [확정: 별도 `scene` 선언 + 참조]
```
app Viewer {
    state rot: float = 0

    scene model_scene {
        camera { id: main_cam; transform { position: (0, 2, 5) }; target: (0, 0, 0) }
        mesh { source: "part.gltf"; transform { rotation: (0, rot, 0) } }
    }

    window {
        viewport3d { scene: model_scene; camera: main_cam }
    }
}
```
- 씬은 `scene name { ... }`으로 따로 선언하고, `viewport3d`가 이름으로 참조한다. 하나의 씬을 여러 뷰포트에서 볼 수 있다.
- [초안] `scene`은 `app`/`component` 안에 선언하고, 그 안의 state와 action을 그대로 참조한다.
- [초안] 씬 안에서도 `if` / `for`로 노드를 조건부로 두거나 반복해 만들 수 있다.
- 전체 예시: `examples/viewer.hana`

### 5.3 노드 변환과 계층 [확정: `transform` 하위 블록]
```
node {
    id: arm
    transform {
        position: (0, 1, 0)
        rotation: (0, 45, 0)
        scale: (1, 1, 1)
    }

    mesh { source: "hand.gltf"; transform { position: (0, 0.5, 0) } }
}
```
- 변환은 노드 안의 `transform { }` 블록에 쓴다.
- 노드 안에 노드를 쓰면 자식이 되고, 자식의 변환은 부모 좌표계 기준이다.
- [초안] `transform`, `material`처럼 노드의 성질을 묶는 블록은 자식 노드가 아니다. 어떤 이름이 성질 블록인지는 노드 종류마다 정해져 있다.
- [초안] `rotation`은 오일러 각(도 단위, XYZ 순서). 쿼터니언 표기는 필요해지면 추가한다.
- [초안] 좌표계는 glTF와 같은 오른손 좌표계, Y가 위, 단위는 미터.

### 5.4 노드 종류 [초안]
| 노드 | 용도 | 주요 프로퍼티 / 블록 |
|---|---|---|
| `node` | 빈 그룹 | `transform` |
| `camera` | 카메라 | `transform`, `target`, `fov`, `near`, `far` |
| `light` | 조명 | `type: directional / point / spot`, `color`, `intensity`, `direction` |
| `mesh` | glTF 모델 | `source`, `transform`, `material`, `animation`, `body` |
| `panel3d` | 3D 공간 안의 2D UI | `transform`, `size`, 자식 2D 위젯 |
| `particles` | 파티클 방출기 | `transform`, `rate`, `lifetime`, `velocity`, `color` |

### 5.5 3D 안의 2D UI [확정: `panel3d` 1차 범위 포함]
```
panel3d {
    transform { position: (1.5, 1, 0) }
    size: 1x0.6

    label { text: "Selected: {selected_name}" }
    button { text: "Close"; on_click: { selected_name = "" } }
}
```
- [초안] 자식은 일반 2D 위젯이고, 2D 렌더러 결과를 텍스처로 씬에 그린다. 3D 공간에서 클릭하면 패널 좌표로 바꿔 2D 위젯에 전달한다.
- [초안] `size`는 미터 단위다.

### 5.6 이벤트와 피킹 [초안]
```
mesh {
    source: "part.gltf"
    on_click: select_part
    on_hover: { hovered = true }
}
```
- 3D 노드도 2D 위젯과 같은 방식의 이벤트 핸들러를 갖는다.

### 5.7 애니메이션 [초안]
```
mesh {
    source: "robot.gltf"
    animation { clip: "Walk"; playing: walking; loop: true; speed: 1.0 }
}

action open_door() {
    animate door_angle to 90 over 0.5 ease: ease_out
}
```
- glTF 애니메이션은 `animation` 블록으로 재생한다.
- 트윈은 action 안의 `animate <state> to <값> over <초>` 문으로 state 값을 시간에 따라 바꾼다. 바인딩된 프로퍼티는 자동으로 따라 움직인다.
- [미정] `animate`, `to`, `over`를 키워드로 만들지, 내장 함수 `animate(door_angle, to: 90, duration: 0.5)`로 할지

### 5.8 프레임 갱신 [초안]
```
scene world {
    on_frame: tick
}

action tick(dt: float) {
    rot = rot + 30 * dt
}
```
- `on_frame`에 연결한 action은 매 프레임 경과 시간 `dt`(초)를 받는다.

### 5.9 물리 [초안]
```
mesh {
    source: "ball.gltf"
    transform { position: (0, 5, 0) }
    body { type: dynamic; mass: 1.0; shape: sphere; restitution: 0.6 }
    on_collide: bounce
}
```
- `body` 블록이 있는 노드만 물리 시뮬레이션에 참여한다. `type: static / dynamic / kinematic`.
- 물리가 움직인 노드의 변환은 시뮬레이션이 소유한다. `transform`은 초기값으로만 쓴다.
- [미정] 물리 엔진 (직접 구현 / Jolt Physics 등 외부 라이브러리)

### 5.10 파티클 [초안]
```
particles {
    transform { position: (0, 0, 0) }
    rate: 200
    lifetime: 1.5
    velocity: (0, 2, 0)
    color: #ffaa00
}
```

## 6. 미정 항목
- 트윈 문법: 전용 문(`animate x to v over t`)과 내장 함수 중 선택
- 물리 엔진 선택
