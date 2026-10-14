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
`app` `component` `scene` `state` `action` `if` `else` `for` `in` `true` `false`

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
- [미정] 반복 위젯의 항목 식별 키 (리스트가 바뀔 때 위젯 상태를 유지하기 위함)

### 2.6 state와 action [초안]
```
state count: int = 0
state name = "HANA"             // type inferred from initializer

action increment() {
    count = count + 1
}
```
- `app`과 `component` 안에 선언한다.

### 2.7 표현식 바인딩 [초안]
- 프로퍼티 값이 state를 참조하면, 그 state가 바뀔 때 자동으로 다시 계산된다.
  예: `enabled: count > 0`

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
| `color` | 색 | [미정] |
| `list<T>` | T의 리스트 | `[1, 2, 3]` |

- [초안] 튜플 리터럴은 기대하는 타입에 따라 `vec2`/`vec3`가 된다.
- [초안] `int`는 `float` 자리에 쓸 수 있다 (`fov: 60`). 반대 방향은 명시적 변환이 필요하다.
- [미정] `color` 리터럴 표기 (`#ff8800`, `rgb(1, 0.5, 0)` 등)

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

## 5. 미정 항목
- 입력 위젯의 양방향 바인딩 (`slider { value: rot }`이 `rot`을 갱신하는지)
- `color` 리터럴 표기
- 반복 위젯의 항목 식별 키
- `scene` 블록과 3D 노드 문법
