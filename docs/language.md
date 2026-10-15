# HANA 언어 정의

이 문서가 HANA 문법과 의미의 기준이다. 문법을 바꾸면 이 문서를 먼저 고친다.

항목의 상태 표시:
- 표시 없음: 확정. 바꾸려면 명시적으로 다시 결정한다.
- **[초안]**: 현재 기준으로 삼는 안. 바뀔 수 있다.
- **[미정]**: 아직 정하지 않았다.

괄호 안의 `(결정 N)`은 `docs/decisions.md`의 결정 번호다.

---

## 1. 개요

HANA는 GUI, 3D, 일반 로직, 시스템 프로그래밍까지 하나의 문법으로 다루는 언어다 (결정 2).
- 프로그램의 뼈대는 JSON을 확장한 문법이다. 표준 JSON 파일은 그대로 올바른 HANA 소스다 (결정 1).
- 실행할 코드는 JSON 값 자리에 쓴다. 한 줄이면 그대로, 여러 줄이면 `'''` 코드 블록에 Python식 들여쓰기로 쓴다 (결정 4, 5).
- 네이티브 코드로 컴파일한다. GC가 없고, 정적 타입을 쓴다 (결정 8, 9, 10).

한눈에 보는 예제:

```
{
  use: ["gui"],

  Shape: enum {
    Circle: { radius: f32 },
    Rect: { w: f32, h: f32 },
  },

  pub area: fn<f32(s: Shape)> '''
    match s:
      Circle(r): return 3.14159 * r * r
      Rect(w, h): return w * h
  ''',

  scale: fn<(inout p: v3, k: f32)> p = p * k,

  Counter: component {
    state: { count: 0 },
    body: column {
      children: [
        label { text: f"Count: {count}" },
        button { text: "+1", on_click: count += 1 },
        button { text: "Reset", on_click: count = 0, enabled: count > 0 },
      ],
    },
  },

  main: fn '''
    var pos = v3(1, 2, 3)
    scale(&pos, 2.0)
    print(f"pos = {pos}, area = {area(Circle { radius: 2.0 })}")
  ''',
}
```

---

## 2. 파일 구조: JSON 확장 문법

### 2.1 JSON 상위 집합 (결정 1)
모든 표준 JSON 파일은 그대로 올바른 HANA 소스다. HANA는 여기에 다음을 더한다.

| 확장 | 예 | 상태 |
|---|---|---|
| 따옴표 없는 키 | `title: "Counter"` | 확정 |
| 따옴표 있는 키 (표준 JSON) | `"title": "Counter"` | 확정 |
| 주석 `//`, `/* */` | `// window size` | 확정 (결정 11) |
| 끝 쉼표 | `[1, 2, 3,]` | [초안] |
| 값 자리의 식 | `enabled: count > 0` | [초안] |
| 종류 이름이 붙은 객체 | `button { text: "+1" }` | [초안] |

- [미정] HANA 소스를 순수 JSON으로 변환하는 기능. 변환하면 코드 블록은 문자열 하나가 된다 (결정 4).

### 2.2 모듈 = 하나의 객체 (결정 3, 14)
파일 하나가 모듈 하나이고, 파일 전체가 하나의 객체다. 객체의 각 키가 선언이다.

```
{
  use: ["std.io", "gui", "geometry.shape"],

  pub area: fn<f32(s: Shape)> ...,    // visible to other modules
  helper: fn ...,                      // private to this module
}
```
- `use` 키로 다른 모듈을 가져온다. `use`는 일반 JSON 키이므로 표준 JSON 도구로도 의존성을 읽을 수 있다.
- 다른 모듈에 공개할 이름 앞에 `pub`을 붙인다.

### 2.3 선언의 모양 (결정 3)
모든 선언은 `이름: 종류 { 키: 값 }` 형태다. 함수, 타입, 화면, 하드웨어 레지스터가 모두 같은 모양이다.

```
{
  Point: struct { x: f32, y: f32 },
  Counter: component { state: { count: 0 } },
  Register: struct { layout: "c", address: usize },
}
```

---

## 3. 코드 값

### 3.1 한 줄 코드 (결정 4)
식이나 문장이 한 줄이면 따옴표 없이 값 자리에 쓴다.

```
enabled: count > 0,                         // value: an expression
on_click: count = 0,                        // action: a statement
double: fn<i32(x: i32)> x * 2,              // one-line function body
```
- 한 줄 본문은 `,`, `}`, `]` 또는 줄바꿈에서 끝난다. 괄호 안의 쉼표(`print(a, b)`)는 본문의 일부다.
- 값 자리와 동작 자리는 모양이 같다. 컴파일러가 그 키가 기대하는 타입으로 구분하고, 값 자리에 대입문을 쓰면 오류를 낸다.
- 반환 타입이 있는 함수의 한 줄 본문은 그 식의 값이 반환값이다.

### 3.2 `'''` 코드 블록 (결정 4, 29)
여러 줄 코드는 `'''`로 감싼다.

```
fib: fn<i64(n: i64)> '''
  if n < 2: return n
  return fib(n - 1) + fib(n - 2)
''',
```
- `'''...'''`는 문자열이 아니라 "코드 값"이다. 컴파일러가 파싱하고 검사한다. 일반 문자열은 `"..."`다.
- 닫는 `'''`의 들여쓰기만큼 각 줄 앞의 공백을 제거한다. 그래서 JSON 안에 들여 써도 된다.
- `'''` 블록은 중첩하지 않는다. 블록 안의 클로저 본문은 들여쓰기 블록으로 쓴다 (6.6절).
- 편집기 지원(언어 서버)은 `'''` 안을 코드로 인식해야 한다.

### 3.3 들여쓰기 블록 (결정 5)
`'''` 안의 블록은 Python처럼 들여쓰기로 나눈다.

```
'''
var i = 0
while i < 3:
  print(f"i = {i}")
  i += 1
'''
```
- [초안] 블록을 여는 줄은 `:`로 끝난다 (`if n < 2:`, `while i < 3:`).
- [초안] 본문이 한 문장이면 같은 줄에 쓸 수 있다 (`if n < 2: return n`).
- 공백만 허용하고 탭은 컴파일 오류다. 한 블록 안에서 같은 깊이를 지키면 칸 수는 자유다. 포매터(`hana fmt`)는 2칸으로 맞춘다 (결정 39).

### 3.4 주석 (결정 11)
- `'''` 코드 블록 안: `#`
- 바깥 JSON 부분: `//`, `/* */` (JSON5와 같음)

---

## 4. 변수와 기본 타입

### 4.1 변수 (결정 20)
```
let n = 10        # immutable
var i = 0         # mutable
let x: f32 = 1    # explicit type
```
- `let`은 불변, `var`는 가변이다. 선언 없이 대입하면 오류다 (오타가 새 변수를 만드는 것을 막는다).

### 4.2 타입 검사 (결정 9)
정적 타입에 추론을 더한다. 추론할 수 있으면 타입 표기를 생략한다.
- [초안] 기본 타입: `i8` `i16` `i32` `i64` `u8` `u16` `u32` `u64` `f32` `f64` `bool` `str`
- `usize` / `isize`: 포인터와 같은 폭의 정수. 64비트 대상에서는 64비트, WASM32에서는 32비트. 길이, 인덱스, 주소에 쓴다 (결정 40).
- `char`: 유니코드 문자 하나(32비트). 리터럴은 `'a'`, `'한'`, `'\n'`. 작은따옴표 하나는 `char`, 세 개(`'''`)는 코드 블록이다. 문자열은 큰따옴표만 쓴다 (결정 40).

### 4.2.1 튜플 (결정 36)
```
divmod: fn<(i32, i32)(a: i32, b: i32)> (a / b, a % b),

main: fn '''
  let (q, r) = divmod(7, 2)          # destructuring
  let pair = divmod(9, 4)
  print(pair.0)                      # access by position
'''
```
- 타입 `(i32, i32)`, 값 `(a, b)`, 분해 `let (q, r) = ...`, 위치 접근 `.0`, `.1`.
- 원소가 모두 `Copy`면 튜플도 `Copy`다.
- 순수 JSON으로 바꾸면 배열이 된다.

### 4.3 숫자 리터럴 (결정 31)
```
42   -7   3.14   1e-3   2.5E+8          // standard JSON numbers
0xff   0b1010   1_000_000                // HANA additions
10u8   7i64   2.5f32                     // type suffixes
```
- 접미사가 없는 정수는 `i32`, 소수점이나 지수가 있으면 `f64`가 기본이다.
- 들어갈 자리의 타입이 정해져 있으면 그 타입이 된다 (`let a: u8 = 200`, `hp: u16` 필드에 JSON `100`).

### 4.4 정수 오버플로 (결정 32)
- 디버그 빌드: panic으로 멈춘다.
- 배포 빌드(`-O`): 감싼다(wrap).
- 의도적인 경우는 함수로 쓴다: `wrapping_add`, `saturating_add`, `checked_add`(`Option` 반환).
- 0으로 나누기는 두 빌드 모두 panic이다.

### 4.5 문자열 (결정 18, 33)
```
let name = "HANA"                         # plain string, braces are literal: "{x}"
let msg = f"hello {name}, {1 + 2}"        # interpolation
```
- 보간은 `f"..."`로만 한다. 일반 문자열 `"..."` 안의 `{ }`는 글자 그대로다. 표준 JSON 문자열의 의미가 바뀌지 않게 하기 위함이다.
- 문자열 타입은 `str` 하나다. 소유하는 UTF-8 문자열이고, 읽기 인자로 넘기면 복사되지 않는다 (8.2절).

### 4.6 `null`과 `Option` (결정 19)
```
nickname: Option<str> = null,

greet: fn<(name: Option<str>)> '''
  match name:
    Some(n): print(f"hi {n}")
    null: print("hi stranger")
''',
```
- `null`은 `Option<T>`의 "값 없음"이다. 아무 타입에나 들어갈 수 있는 값이 아니다.
- [초안] `match`의 갈래는 `Some(n)` / `null`이다.

### 4.7 컬렉션 (결정 33)
| 타입 | 뜻 | JSON과의 관계 |
|---|---|---|
| `str` | 소유하는 UTF-8 문자열 | JSON 문자열 |
| `List<T>` | 길이가 바뀌는 배열 | JSON 배열 `[1, 2, 3]` |
| `Array<T, N>` | 고정 길이 배열 | 기대 타입이 `Array`면 JSON 배열 |
| `Map<K, V>` | 키-값 사전 | 기대 타입이 `Map`이면 JSON 객체 |
| `Set<T>` | 집합 | |

### 4.8 3D 수학 타입 (결정 6)
처음 제공하는 타입: `v2` `v3` `v4` `m33` `m44`. 나머지(`quat`, `v3i`, `v3d`, `m43` 등)는 필요할 때 추가한다.

```
let a = v3(1, 2, 3)
let b: v3 = [4, 5, 6]          # JSON array where a v3 is expected
let c = a + b                  # component-wise
let n = dot(a, b)
let side = a cross b           # named infix, see 6.7
let xy = a.xy                  # swizzle
let m = m44.identity()
let p = m * v4(a, 1)
```
- 키워드가 아니라 표준 라이브러리의 기본 가져오기(prelude)에 있다. 사용자가 같은 이름을 정의하면 사용자 것이 우선하므로, 타입을 추가해도 기존 코드가 깨지지 않는다.
- [초안] 원소는 `f32`, 행렬은 열 우선(column-major)
- [초안] 벡터 `+ - * /`는 성분별, `m * v`와 `m * m`은 행렬 곱, 내적/외적은 `dot`/`cross`
- [초안] swizzle `.xyz`, `.rgba`
- [초안] 기대 타입이 벡터인 자리의 JSON 배열은 그 벡터가 된다

### 4.9 복사와 이동 (결정 10, 24)
- 값은 기본적으로 이동된다.
- 숫자, `bool`, 수학 타입은 `Copy`라서 자동으로 복사된다.
- 사용자 타입은 `Point: struct copy { ... }`처럼 표시해야 `Copy`가 된다.
- 그 밖의 값을 복제하려면 `.clone()`을 직접 부른다.

---

## 5. 함수

### 5.1 짧은 머리 표기 `fn<반환타입(인자)>` (결정 3)
C++ `std::function<int(int)>`과 같은 모양이다.

```
double: fn<i32(x: i32)> x * 2,
scale: fn<(inout p: v3, k: f32)> p = p * k,              // no return: void omitted
divmod: fn<(i32, i32)(a: i32, b: i32)> (a / b, a % b),   // several return values
fib: fn<i64(n: i64)> '''
  if n < 2: return n
  return fib(n - 1) + fib(n - 2)
''',
```
- 반환값이 없으면 `void`를 생략한다.
- [초안] 제네릭 함수의 타입 매개변수는 이름 쪽에 붙인다: `max<T>: fn<T(a: T, b: T)> ...`
- [초안] 인자도 반환값도 없는 함수는 `fn`만 쓴다: `main: fn '''...'''`

### 5.2 객체형 표기 (결정 3)
```
fib: fn {
  in: { n: i64 },
  out: i64,
  do: '''
    if n < 2: return n
    return fib(n - 1) + fib(n - 2)
  ''',
},
```
- 짧은 표기와 같은 함수다. 속성이 많을 때 쓴다 (예: `unsafe`, 이후 `inline`, `export`).

### 5.3 함수 타입 (결정 3)
콜백 같은 함수 값의 타입도 같은 표기를 쓴다.
```
on_damage: fn<(amount: i32)>,
compare: fn<bool(a: T, b: T)>,
```

### 5.4 인자 전달 방식 (결정 16)
참조 타입은 없다. 인자에 전달 방식을 붙이고, 실제 전달(포인터 또는 값)은 컴파일러가 정한다.

| 표시 | 뜻 | 호출 |
|---|---|---|
| 없음 | 읽기 전용, 복사하지 않는다 | `length(p)` |
| `inout` | 호출자의 값을 수정한다 | `scale(&p, 2.0)` |
| `sink` | 소유권이 함수로 이동한다 | `push(&items, item)` |

- 함수는 인자를 함수 밖에 저장할 수 없다. 그래서 해제된 메모리를 가리키는 일이 생기지 않고, 대여 검사기나 수명 표기도 필요 없다.
- 호출하는 쪽은 `inout` 인자에 `&`를 붙인다.

### 5.5 클로저 (결정 22, 29)
```
let alive = players.filter(fn<bool(p: Player)> p.alive())

var total = 0
items.each([inout total] fn<(x: i32)> total += x)
```
- 이름 없는 함수도 `fn<...>`로 쓴다.
- 캡처는 기본 읽기다. 수정은 `[inout x]`, 소유권 이동은 `[sink x]`를 앞에 붙인다.
- 함수 밖에 저장하는 클로저(이벤트 핸들러, 다른 스레드로 넘기는 클로저)는 모든 캡처가 `sink`여야 한다.
- 코드 블록 안에서 여러 줄 클로저는 들여쓰기로 쓴다 (`'''`는 중첩하지 않는다).
  - [초안] 표기:
    ```
    let worker = spawn([sink tx] fn:
      tx.send(42)
    )
    ```
    괄호 안에서 들여쓰기 블록을 여는 형태라 Python에는 없는 규칙이다. 구현하면서 다듬는다.

---

## 6. 사용자 정의 타입

사용자 정의 타입은 표준 라이브러리 타입과 같은 지위를 갖는다 (결정 7).

### 6.1 struct와 메서드 (결정 21)
```
Player: struct {
  name: str,
  hp: i32 = 100,

  alive: fn<bool(self)> self.hp > 0,
  damage: fn<(inout self, amount: i32)> self.hp -= amount,
  new: fn<Player(name: str)> Player { name: name },
},
```
- 메서드는 struct 객체 안에 함수 키로 둔다.
- `self`도 인자 전달 방식을 따른다: `self`(읽기), `inout self`(수정), `sink self`(소유권 이동).
- `self`가 없는 함수는 `Player.new("A")`처럼 타입 이름으로 부른다.
- [초안] 필드 기본값 `hp: i32 = 100`
- [초안] 값 만들기 `Player { name: "A" }` (위 `new` 안에서 쓰는 표기)

### 6.2 enum (결정 13, 23)
```
Shape: enum {
  Circle: { radius: f32 },
  Rect: { w: f32, h: f32 },
},
```
- 경우마다 데이터를 가질 수 있다. `Option`과 `Result`도 enum이다.

### 6.3 trait (결정 17)
```
Shape: trait {
  area: fn<f32(self)>,
},
```
- 공통 동작을 정의한다. 클래스 상속은 없다.
- [미정] 타입이 trait을 구현한다고 표시하는 방법

### 6.4 제네릭 (결정 17)
`List<T>`, `Map<K, V>`처럼 타입 매개변수를 갖는 타입과 함수를 만든다.
- [미정] 제네릭 struct의 선언 표기, 타입 매개변수의 trait 제약 표기

### 6.5 연산자 오버로딩 (결정 34)
```
Money: struct copy {
  cents: i64,
  "+": fn<Money(self, other: Money)> Money { cents: self.cents + other.cents },
  "==": fn<bool(self, other: Money)> self.cents == other.cents,
},
```
- 따옴표 키에 기호를 써서 정의한다.
- 대상: 산술, 비교(`==`, `<`를 정의하면 나머지는 자동 생성), 첨자 `[]`, 복합 대입(정의하지 않으면 이항 연산자에서 자동 생성).
- `and`, `or`, `not`은 오버로딩할 수 없다. `and`/`or`의 짧은 평가가 모든 타입에서 같게 동작하도록 하기 위함이다. 식을 조립하는 DSL은 `&`, `|`를 오버로딩한다.

### 6.6 기존 타입 확장
- [초안] 기존 타입에 메서드나 연산자를 더하는 `v3: extend { ... }`

### 6.7 이름으로 된 중위 연산자 (결정 30)
```
v3: extend {
  dot: infix fn<f32(self, o: v3)> self.x * o.x + self.y * o.y + self.z * o.z,
},
# use: let n = a dot b
```
- 우선순위는 하나로 고정이다. 함수 호출보다 약하고 비교 연산자보다 강하다.
- 기호로 새 연산자를 만드는 기능은 두지 않는다.

---

## 7. 제어문 (결정 12, 23)

```
if hp <= 0:
  die()
elif hp < 20:
  warn()
else:
  heal(1)

for enemy in enemies:
  if not enemy.alive(): continue
  attack(enemy)

while running:
  step()

let label = match hp:
  0: "dead"
  1..=20: "critical"
  _: "ok"
```
- `if`/`elif`/`else`, `while`, `for x in xs`, `break`, `continue`, `return`, `match`.
- 논리 연산자는 `and`, `or`, `not`이다.
- `match`는 모든 경우를 다뤄야 하고, 빠지면 컴파일 오류다.
- 패턴: 경우와 바인딩 `Circle(r)`, 나머지 `_`, 리터럴, 범위 `1..=9`, 조건 `n if n > 100`.
- `match`와 `if`는 값을 돌려주는 식으로도 쓴다.
- [초안] `match` 갈래는 `패턴: 본문`
- 값을 돌려주는 `if`는 Python식 삼항식이다: `let bigger = a if a > b else b`. 여러 줄에 걸친 값 선택은 `match`를 쓴다 (결정 38).

---

## 8. 메모리

### 8.1 RAII와 이동 (결정 10)
GC는 없다. 값이 범위를 벗어나면 소멸자가 자원을 해제한다. 값은 기본적으로 이동되고, 공유는 명시적인 참조 카운트 타입으로 표현한다.
- [미정] 소멸자 정의 표기, 참조 카운트 타입 이름 (`Rc<T>` 등)

### 8.2 참조 없는 값 전달
5.4절을 따른다. 컨테이너 안의 원소를 빌려주는 방법은 미정이다.
- [미정] 컨테이너 원소를 복사 없이 수정하는 방법 (`items[i].hp -= 1`)

---

## 9. 에러 처리 (결정 13)

```
load: fn<Result<Config, IoError>(path: str)> '''
  let text = read_file(path)?
  return parse_config(text)
''',
```
- 실패할 수 있는 함수는 `Result<T, E>`를 반환한다.
- `?`는 에러면 그 에러를 즉시 호출자에게 반환한다.
- 예외는 없다.
- [미정] 복구할 수 없는 오류(`panic`)를 직접 일으키는 방법

---

## 10. 반응형 상태와 UI (결정 15)

```
Counter: component {
  state: { count: 0 },
  body: column {
    children: [
      label { text: f"Count: {count}" },
      button { text: "+1", on_click: count += 1 },
      button { text: "Reset", on_click: count = 0, enabled: count > 0 },
    ],
  },
},
```
- 어떤 객체에서든 `state` 키에 넣은 값은 반응형이다.
- 그 값을 읽는 식(`text`, `enabled`)은 값이 바뀌면 자동으로 다시 계산된다.
- `app` 전용이 아니라 사용자 `component`에도 똑같이 쓰인다.
- [미정] `component`, `app`, `column` 같은 종류가 언어 기능인지 표준 라이브러리 타입인지
- [미정] 양방향 바인딩 (슬라이더가 state를 바꾸는 경우)

---

## 11. 시스템 프로그래밍

### 11.1 `unsafe`와 포인터 (결정 25)
```
poke: fn<(address: usize, value: u32)> unsafe: true '''
  let p = address as *var u32
  *p = value
''',

copy_bytes: fn<(dst: usize, src: usize, n: usize)> '''
  unsafe:
    memcpy(dst as *var u8, src as *u8, n)
''',
```
- 원시 포인터 `*T`(읽기), `*var T`(쓰기)는 `unsafe` 안에서만 만들고, 역참조하고, 산술한다.
- `unsafe`는 함수 전체 또는 코드 블록 일부(`unsafe:` 들여쓰기 블록)에 붙인다.
- 숫자 변환과 주소에서 포인터로의 변환은 `as`.
- [초안] 함수 전체 표시 위치 `fn<...> unsafe: true '''...'''`

### 11.2 C 연동 (결정 26)
```
{
  use: ["c:stdio.h"],

  puts: extern "c" fn<i32(s: *u8)>,
  hana_add: export "c" fn<i32(a: i32, b: i32)> a + b,
  Vertex: struct { layout: "c", pos: v3, uv: v2 },

  main: fn '''
    unsafe: c.printf("hello %d\n", 42)
  ''',
}
```
- C 함수 선언은 `extern "c"`, HANA 함수 공개는 `export "c"`, C와 같은 메모리 배치는 `layout: "c"`.
- C 헤더는 컴파일러가 libclang으로 직접 읽는다 (`use: ["c:stdio.h"]`, 사용 `c.printf(...)`).

### 11.3 C++ 연동 (결정 26)
도구가 C 인터페이스 접착 코드를 자동 생성해, 시스템 C++ 컴파일러로 빌드하고 함께 링크한다.

| C++ 기능 | 연동 |
|---|---|
| 일반 함수, 네임스페이스 | 자동 바인딩 |
| 템플릿이 아닌 클래스 | HANA struct로 감싼다. 생성자/메서드/소멸자 연결, C++ 소멸자는 HANA RAII로 자동 호출 |
| enum, POD 구조체 | 그대로 대응 |
| 템플릿 | 설정에 적은 인스턴스(`std::vector<float>` 등)만 바인딩 생성 |
| 예외 | 경계에서 잡아 `Result`의 에러로 바꿈 |
| C++ 클래스 상속 | 지원하지 않음 |

```
{
  use: ["cpp:physics.hpp"],
  main: fn '''
    var world = cpp.physics.World.new()
    world.add_body(v3(0, 10, 0))
    let r = world.step(1.0 / 60.0)?
  ''',
}
```

### 11.4 동적 라이브러리 (결정 26)
세 방식을 모두 지원한다.

1. **빌드할 때 연결**: `hana.json`의 `link`에 적으면 실행할 때 OS가 불러온다.
   ```json
   { "link": [ { "name": "physics", "kind": "dynamic" } ] }
   ```
2. **실행 중 불러오기, 헤더 있음**: 처음 호출할 때 불러오고 `available()`로 확인한다.
   ```
   physics: library { name: "physics", load: "lazy" },
   ```
3. **실행 중 불러오기, 헤더 없음**: 호출은 `unsafe`, 실패는 `Result`, 해제는 RAII.
   ```
   let lib = Library.open(f"plugins/{name}")?
   let add = lib.symbol<fn<i32(a: i32, b: i32)>>("add")?
   ```
- [초안] 위 표기들
- C++ 클래스를 내보낸 DLL은 헤더로 만든 접착 코드가 필요하다(1, 2번). 접착 코드는 DLL과 같은 종류의 컴파일러(MSVC 또는 GCC/Clang)로 빌드한다. 3번은 `extern "C"` 함수만 가능하다.

---

## 12. 동시성 (결정 27)

```
main: fn '''
  let (tx, rx) = channel<i32>()
  let worker = spawn([sink tx] fn:
    tx.send(42)
  )
  print(f"got {rx.receive()}")
  worker.join()
''',
```
- 1단계는 OS 스레드, 채널, `Mutex<T>`.
- 스레드에 넘기는 값은 `sink`로 캡처한다. 스레드 사이에 넘길 수 있는 타입인지 컴파일러가 검사해 데이터 경합을 막는다.
- `async`/`await`는 나중에 정한다.

---

## 13. 실행 방식과 표준 라이브러리

### 13.1 실행 방식 (결정 8)
네이티브 컴파일(LLVM)이다. 개발 중 빠른 실행은 JIT로 보완한다.

### 13.2 표준 라이브러리 (결정 28)
| 모듈 | 내용 |
|---|---|
| `core` | OS 없이 동작. 수학 타입, `Option`/`Result`, 컬렉션 기본 |
| `std` | 파일, 스레드, 네트워크, 시간 |
| `platform` | 창, 입력 |
| `gui` | 위젯, 레이아웃 |
| `gfx` | 3D |

GPU 계층은 WebGPU다. 네이티브는 Dawn, 웹은 Emscripten.

### 13.3 출력 (결정 37)
```
print("hello")              # with newline
print(f"x = {x}")
write("no newline, ")       # without newline
print(v3(1, 2, 3))
```
- `print`(줄바꿈 붙임), `write`(줄바꿈 없음). 둘 다 prelude에 있어 `use` 없이 쓴다.
- 숫자, `bool`, `str`, 수학 타입, 출력용 trait을 구현한 사용자 타입을 출력한다.
- 실수는 값을 되살릴 수 있는 가장 짧은 표기로 출력한다 (`0.1 + 0.2`는 `0.30000000000000004`, `2.0`은 `2.0`).

### 13.4 패키지 (결정 35)
패키지 설정 파일은 순수 JSON인 `hana.json`이다.
```json
{
  "name": "my_game",
  "version": "0.1.0",
  "entry": "src/main.hana",
  "dependencies": { "physics": "1.2" },
  "link": [ { "name": "physics", "kind": "dynamic" } ]
}
```
- [초안] 키 이름들

---

## 14. 컴파일러 1단계 구현 범위

현재 컴파일러(`hana`)가 실제로 처리하는 범위다. 아래 "잠정 동작" 중 이 문서의 앞 절에서 정하지 않은 것은 모두 [초안]이다.

**지원**
- 모듈 객체, 따옴표 있는/없는 키, `//` `/* */` 주석, 끝 쉼표, `pub`, 빈 `use: []`
- 함수: `fn<R(인자)>`, `fn<(인자)>`, `fn<void(...)>`, `fn`, 객체형 `fn { in, out, do }`, 인자 전달 방식 `inout` / `sink`
- 본문: 여러 줄 `'''` 블록, 한 줄 `''' ... '''`, 한 줄 값
- 문장: `let`, `var`, 대입과 복합 대입, `if`/`elif`/`else`, `while`, `break`, `continue`, `return`, 함수 호출
- 식: 산술, 비트, 시프트, 비교, `and`/`or`/`not`(짧은 평가), 삼항식 `a if c else b`, `as` 변환, 함수 호출
- 타입: `i8`~`i64`, `u8`~`u64`, `isize`, `usize`, `f32`, `f64`, `bool`, `char`, `str`
- 리터럴: 정수(16진, 2진, `_`, 접미사), 실수(지수, 접미사), `true`/`false`, 문자, 문자열(JSON 이스케이프와 `\u{...}`), `f"..."`
- `print`, `write`. `main: fn` 또는 `main: fn<i32()>` (반환값이 종료 코드)
- 정수 오버플로: 디버그는 panic, `-O`는 감싸기. 0으로 나누기는 panic

**아직 지원하지 않음** (사용하면 "not supported yet" 오류): 최상위 데이터 값, `struct`, `enum`, `trait`, 제네릭, 튜플, `for`, `match`, 클로저, 리스트와 객체 값, `null`/`Option`, 포인터와 `unsafe`, C 연동, 모듈 가져오기, 멤버 접근, 첨자, `?`

**잠정 동작 [초안]**
1. (확정, 결정 41) 여러 줄 `'''` 블록에서는 첫 코드 줄의 들여쓰기가 블록의 기준(0단계)이다. 그래서 코드를 닫는 `'''`보다 더 들여 써도 된다. 닫는 `'''`보다 덜 들여 쓴 줄과 첫 코드 줄보다 덜 들여 쓴 줄은 오류다.
2. 빈 블록에는 Python처럼 `pass`를 쓴다.
3. `!`는 연산자가 아니다. 부정은 `not`, 다름은 `!=`이다.
4. 정수 `/`는 0 쪽으로 버리고, `%`의 부호는 왼쪽 피연산자를 따른다 (C와 같음). `-7 / 3`은 `-2`, `-7 % 3`은 `-1`.
5. 시프트 양은 비트 폭으로 나눈 나머지를 쓴다 (`1u32 << 33`은 `2`).
6. 보간 문자열 `f"..."`은 `print`/`write`의 인자에서만 쓸 수 있다. 다른 곳에서 쓰려면 `str` 값을 만드는 메모리 관리가 필요하다.
7. `as`: 숫자끼리, `bool`과 `char`에서 정수로 변환할 수 있다. 실수를 정수로 바꿀 때 범위를 넘으면 최댓값/최솟값으로 고정하고 NaN은 0이 된다. 정수를 `char`로 바꾸는 것은 막는다.
8. 숫자 타입 사이의 암묵적 변환은 없다 (`var i = 0`은 `i32`이므로 `i64` 인자에 넘기려면 `i as i64`).
9. 같은 범위에서 같은 이름을 다시 선언하면 오류다. 안쪽 블록에서 바깥 이름을 가리는 것은 허용한다.
10. 식만 있는 문장은 함수 호출만 허용한다 (`a + 1`만 쓰면 오류).
11. `while true:` 안에 `break`가 없으면 끝나지 않는 반복으로 보고, 그 뒤에 `return`이 없어도 된다.
12. 함수 본문(`'''` 또는 한 줄 코드)은 `fn` 머리와 같은 줄에서 시작한다.
13. 실행 환경은 Linux다. 런타임 라이브러리를 `/proc/self/exe` 기준으로 찾고, 링크는 시스템 `cc`(`HANA_CC`로 변경)로 한다. macOS는 `HANA_RUNTIME`을 지정하면 동작할 것으로 보이지만 확인하지 않았다. Windows는 이후 단계에서 지원한다.

## 15. 미정 항목 모음

이 문서를 쓰면서 드러난 것들이다. 예제에는 쓰였지만 아직 정하지 않았다.

1. **trait 구현 표시**: 타입이 어떤 trait을 구현하는지 쓰는 방법.
2. **제네릭 struct 선언**과 타입 매개변수의 trait 제약 표기.
3. **소멸자**와 참조 카운트 타입.
4. **컨테이너 원소를 복사 없이 수정하는 방법** (`items[i].hp -= 1`).
5. **`panic`** 직접 호출 방법.
6. **`component`, `app`, `column`의 정체**: 언어 기능인지 표준 라이브러리 타입인지.
7. **양방향 바인딩**.
8. **순수 JSON 변환 기능**.
9. **한 줄 코드 끝의 주석**: 한 줄 코드는 JSON 부분에 있으므로 `//`를 쓴다고 보는 것이 자연스럽다. 명시적으로 정하지는 않았다.
