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
- [초안] 원소는 두 개 이상이다. `(x)`는 괄호로 묶은 값이고, `(x,)`와 `()`는 없다.
- [초안] 분해는 중첩과 `_`를 쓸 수 있다: `let ((a, _), b) = nested`. `match`에서도 튜플 패턴을 쓴다: `(0, _): ...`

### 4.3 숫자 리터럴 (결정 31)
```
42   -7   3.14   1e-3   2.5E+8          // standard JSON numbers
0xff   0b1010   1_000_000                // HANA additions
10u8   7i64   2.5f32                     // type suffixes
```
- 접미사가 없는 정수는 `i32`, 소수점이나 지수가 있으면 `f64`가 기본이다.
- 들어갈 자리의 타입이 정해져 있으면 그 타입이 된다 (`let a: u8 = 200`, `hp: u16` 필드에 JSON `100`).
- 타입 표기 없이 리터럴로 초기화한 변수는 그 함수 안에서 처음 타입이 정해지는 곳(인자, 타입 있는 값과의 연산, 대입, 반환)의 타입을 따른다. 정수 리터럴 변수는 어떤 숫자 타입도, 실수 리터럴 변수는 실수 타입만 될 수 있다. 정해지지 않으면 `i32`/`f64`가 기본이다 (결정 42).
  ```
  var i = 0              # i64, because of fib(i) below
  while i < 10:
    print(fib(i))        # fib takes i64
    i += 1
  ```

### 4.4 정수 오버플로 (결정 32)
- 디버그 빌드: panic으로 멈춘다.
- 배포 빌드(`-O`): 감싼다(wrap).
- 의도적인 경우는 함수로 쓴다: `wrapping_add`, `saturating_add`, `checked_add`(`Option` 반환).
- 0으로 나누기는 두 빌드 모두 panic이다.

### 4.5 문자열 (결정 18, 33, 53)
```
let name = "HANA"                         # plain string, braces are literal: "{x}"
let msg = f"hello {name}, {1 + 2}"        # interpolation
var s = name + "!"                        # concatenation makes a new str
s += msg
print(s.len())                            # byte count, usize
print("abc" < "abd")                      # byte order
```
- 보간은 `f"..."`로만 한다. 일반 문자열 `"..."` 안의 `{ }`는 글자 그대로다. 표준 JSON 문자열의 의미가 바뀌지 않게 하기 위함이다.
- 문자열 타입은 `str` 하나다. 소유하는 UTF-8 문자열이고, 읽기 인자로 넘기면 복사되지 않는다 (8.2절).
- 기본 기능(결정 53): 연결 `a + b`와 `+=`, 비교 `==` `!=` `<` `<=` `>` `>=`(바이트 순서), `.len()`(바이트 수, `usize`), 복제 `.clone()`. `f"..."`는 `str` 값이라 어디서나 쓴다.

### 4.6 `null`과 `Option` (결정 19, 48, 49, 50)
```
nickname: str? = null,                  // same as Option<str>

find: fn<i32?(n: i32)> '''
  if n > 0: return n                     # becomes Some(n)
  return null
''',

greet: fn<(name: str?)> '''
  match name:
    Some(n): print(f"hi {n}")
    null: print("hi stranger")
''',
```
- `null`은 `Option<T>`의 "값 없음"이다. 아무 타입에나 들어갈 수 있는 값이 아니다.
- `T?`는 `Option<T>`의 짧은 표기다 (결정 49). 타입 자리의 `?`는 식의 `?` 연산자와 다르다.
- `Option<T>`가 기대되는 자리에 `T` 값을 쓰면 자동으로 `Some`이 된다 (결정 48). 직접 쓸 때는 `Some(5)`.
- `x == null`, `x != null`로 값이 있는지 비교한다. 값을 꺼낼 때는 `match`의 `Some(n)` / `null` 갈래를 쓴다 (결정 50).
- 기본값 연산자나 `unwrap_or` 같은 메서드는 제네릭, 표준 라이브러리와 함께 정한다.

### 4.7 컬렉션 (결정 33)
| 타입 | 뜻 | JSON과의 관계 |
|---|---|---|
| `str` | 소유하는 UTF-8 문자열 | JSON 문자열 |
| `List<T>` | 길이가 바뀌는 배열 | JSON 배열 `[1, 2, 3]` |
| `Array<T, N>` | 고정 길이 배열 | 기대 타입이 `Array`면 JSON 배열 |
| `Map<K, V>` | 키-값 사전 | 기대 타입이 `Map`이면 JSON 객체 |
| `Set<T>` | 집합 | |

#### 4.7.1 `List<T>` (결정 55, 56, 57, 58)
```
var xs: List<i32> = []
xs.push(3)
let ys = [1, 2, 3]                  # List<i32>
var players = [Player { name: "a" }, Player { name: "b" }]
players[0].hp -= 1                   # modify in place
heal(&players[1])                    # pass an element as inout
print(players[0].name)               # read
let p = players.remove(0)            # take an element out
let last: Player? = players.pop()
```
- 값은 JSON 배열 모양 `[a, b, c]`이다. 빈 값 `[]`는 기대 타입이 있어야 한다 (`let xs: List<i32> = []`).
- 첨자 `xs[i]`는 필드처럼 자리다 (결정 55). 읽고, 대입하고, 그 자리에서 고치고, `&xs[i]`로 inout 전달한다. 범위 밖이면 panic이다.
- `Copy`가 아닌 원소는 첨자로 꺼내 이동할 수 없다 (`let p = xs[0]`은 오류). `.clone()`, `pop()`, `remove(i)`를 쓴다.
- 첨자와 길이는 `usize`다 (결정 57). 타입 없는 리터럴 변수는 첨자로 쓰이면 `usize`가 된다.
- 기본 기능 (결정 58):
  - `len()`, `is_empty()`
  - `push(sink v)`, `pop() -> T?`
  - `insert(i, sink v)`, `remove(i) -> T`
  - `clear()`
  - `.clone()`: 원소가 clone 가능할 때
- [초안] 리스트가 해제되면 원소도 앞에서부터 해제된다.

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
- 그 밖의 값을 복제하려면 `.clone()`을 직접 부른다 (결정 52).
  - `str`은 기본으로 `.clone()`을 갖는다.
  - struct, enum, 튜플, `Option`은 모든 부분이 clone 가능하면 자동으로 갖는다.
  - 사용자가 `clone: fn<Self(self)>`를 정의하면 자동 clone 대신 그것을 쓴다. 이 struct를 담은 값의 자동 clone도 그것을 부른다.
  - `drop`이 있는 타입은 자동으로 만들지 않는다. 필요하면 직접 정의한다.

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
- 제네릭 함수의 타입 매개변수는 이름 쪽에 붙인다: `max<T: Ord>: fn<T(sink a: T, sink b: T)> ...` (결정 59, 6.4절)
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
- 같은 호출에서 inout으로 넘긴 자리는 다른 인자로 겹쳐 넘길 수 없다. 다만 `Copy` 값은 호출 전에 복사되므로 겹쳐도 된다: `f(&x, x)` (결정 43).

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
  new: fn<Player(sink name: str)> Player { name: name },
},
```
- 메서드는 struct 객체 안에 함수 키로 둔다.
- `self`도 인자 전달 방식을 따른다: `self`(읽기), `inout self`(수정), `sink self`(소유권 이동).
- `self`가 없는 함수는 `Player.new("A")`처럼 타입 이름으로 부른다.
- [초안] 필드 기본값 `hp: i32 = 100`
- [초안] 값 만들기 `Player { name: "A" }` (위 `new` 안에서 쓰는 표기)
- `new`의 `name`이 `sink`인 이유: 읽기 인자는 복사하지 않고(결정 16) `str`은 `Copy`가 아니므로(결정 24), 받은 값을 struct 안에 저장하려면 소유권을 넘겨받아야 한다.

### 6.2 enum (결정 13, 23, 44, 45)
```
Shape: enum {
  Circle: { radius: f32 },
  Rect: { w: f32, h: f32 },
  Empty: {},

  area: fn<f32(self)> '''
    return match self:
      Circle(r): 3.14159 * r * r
      Rect(w, h): w * h
      Empty: 0.0
  ''',
},
Color: enum copy { Red: {}, Green: {}, Blue: {} },
```
```
let s = Shape.Circle { radius: 1.0 }
let c = Color.Red
```
- 경우마다 데이터를 가질 수 있다. `Option`과 `Result`도 enum이다.
- 데이터가 없는 경우는 `Empty: {}`로 쓴다 (결정 44).
- 값은 `Shape.Circle { radius: 1.0 }`처럼 이름 있는 필드로 만들고, 데이터가 없는 경우는 `Color.Red`로 쓴다 (결정 45).
- struct처럼 메서드를 둘 수 있고, `enum copy`로 `Copy`가 된다 (결정 24).
- [초안] 경우의 필드에도 기본값을 줄 수 있다: `Rect: { w: f32, h: f32 = 1.0 }`

### 6.3 trait (결정 17, 61)
```
Shape: trait {
  area: fn<f32(self)>,
  scale: fn<(inout self, by: f32)>,
},
Circle: struct {
  is: [Shape],
  radius: f32,
  area: fn<f32(self)> 3.14 * self.radius * self.radius,
  scale: fn<(inout self, by: f32)> self.radius *= by,
},
```
- 공통 동작을 정의한다. 클래스 상속은 없다.
- trait은 메서드의 시그니처만 쓴다. 시그니처의 `Self`는 그 trait을 구현하는 타입이다.
- 타입은 `is: [Shape]`로 구현할 trait을 표시하고, 메서드는 타입 안에 그대로 정의한다 (결정 61). 빠진 메서드와 모양이 다른 메서드는 오류다.
- 기본 trait `Copy`, `Clone`, `Eq`, `Ord`는 언어가 정한 타입이 저절로 갖는다. `is`에는 쓰지 않는다 (잠정 동작 64번).

### 6.4 제네릭 (결정 17, 59, 60, 62)
```
Pair<T>: struct {
  first: T,
  second: T,
},
Maybe<T>: enum {
  Some: { value: T },
  None: {},
},
max<T: Ord>: fn<T(sink a: T, sink b: T)> a if a > b else b,
describe<S: Shape + Clone>: fn<str(s: S)> f"area {s.area()}",

let p = Pair { first: 1, second: 2 }       # Pair<i32>
let m = max(3, 7)                          # max<i32>
let e: Maybe<str> = Maybe.None
```
- 타입 매개변수는 이름 키에 쓴다: `Pair<T>: struct`, `max<T>: fn<...>`, 메서드도 `map<U>: fn<...>` (결정 59).
- 제약은 매개변수 옆에 `T: Ord`, 여럿이면 `T: Ord + Clone` (결정 60).
- 쓸 때는 `Pair<i32>`이고, 타입 인자는 값과 인자, 기대 타입에서 추론한다.
- 제네릭 본문은 정의할 때 한 번 검사한다. 본문은 제약이 허락하는 것만 쓸 수 있다. 예를 들어 `a < b`는 `T: Ord`가 있어야 한다 (결정 62).
- 코드는 쓰인 타입마다 따로 만든다 (단형화). 동적 디스패치(trait 객체)는 이후에 정한다.

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
- `for`의 원소 받기는 인자 전달 방식을 따른다 (결정 56).
  - `for x in xs`: 읽기.
  - `for inout x in xs`: 그 자리에서 수정한다. `xs`는 `var`여야 한다.
  - `for sink x in xs`: 원소를 소유한다. `xs`는 이동된다.
  - 반복하는 동안 `xs`는 바꿀 수 없다 (`push`, 대입, 이동 모두 오류).
- [초안] 정수 범위 반복: `for i in 0..n`(끝 제외), `for i in 1..=n`(끝 포함). 범위 표기는 `match`의 범위 패턴과 같다.
- 논리 연산자는 `and`, `or`, `not`이다.
- `match`는 모든 경우를 다뤄야 하고, 빠지면 컴파일 오류다.
- 패턴 (결정 46): 경우와 바인딩 `Circle(r)`(필드 선언 순서대로), 나머지 `_`, 정수/문자/bool 리터럴, 범위 `1..=9`와 `1..10`, 조건 `n if n > 100`, 이름 바인딩 `n`.
- `match`와 `if`는 값을 돌려주는 식으로도 쓴다.
- [초안] `match` 갈래는 `패턴: 본문`
- 값을 돌려주는 `if`는 Python식 삼항식이다: `let bigger = a if a > b else b`. 여러 줄에 걸친 값 선택은 `match`를 쓴다 (결정 38).

---

## 8. 메모리

### 8.1 RAII와 이동 (결정 10, 51, 54)
GC는 없다. 값이 범위를 벗어나면 소멸자가 자원을 해제한다. 값은 기본적으로 이동되고, 공유는 명시적인 참조 카운트 타입으로 표현한다.
```
File: struct {
  path: str,
  handle: i32,

  drop: fn<(inout self)> close(self.handle),
},
```
- 소멸자는 struct/enum 안의 `drop: fn<(inout self)>` 키다. 다른 인자와 반환값은 없다.
- 값이 범위를 벗어날 때 자동으로 불린다. 직접 부를 수 없다 (`f.drop()`은 오류).
- `drop` 본문이 끝난 뒤 필드가 선언의 역순으로 자동 해제된다 (결정 54).
- 변수는 선언의 역순으로 해제된다. 이동된 변수는 해제하지 않는다.
- `copy` 타입은 `drop`을 가질 수 없다.
- `drop`이 있는 값에서는 일부를 꺼내 갈 수 없다. `drop`은 값 전체가 있어야 하기 때문이다 (`let n = file.path`는 오류).
- [미정] 참조 카운트 타입 이름 (`Rc<T>` 등)

### 8.2 참조 없는 값 전달
5.4절을 따른다. 컨테이너 원소는 첨자 자리로 그 자리에서 고치고(`items[i].hp -= 1`), `&items[i]`로 inout 전달한다 (결정 55).

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

[초안] 현재 구현 (잠정 동작 73~80번):
```
parse_digit: fn<Result<i32, str>(c: char)> '''
  if c >= '0' and c <= '9':
    return Ok((c as i32) - 48)
  return Err(f"not a digit: {c}")
''',
add: fn<Result<i32, str>(a: char, b: char)> Ok(parse_digit(a)? + parse_digit(b)?),
half: fn<i32?(x: i32?)> x? / 2,           # '?' on an Option returns null early

match add('3', 'x'):
  Ok(v): print(v)
  Err(e): print(e)
```
- `Result<T, E>`는 프렐류드의 제네릭 enum이다: `Ok: { value: T }`, `Err: { error: E }`.
- 값은 `Ok(v)`, `Err(e)`로 만든다. `match`에서는 `Ok(v)`, `Err(e)` 패턴으로 꺼낸다.
- 메서드: `is_ok()`, `is_err()`, `value_or(fallback)`.

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

## 14. 현재 컴파일러 구현 범위

현재 컴파일러(`hana`)가 실제로 처리하는 범위다. 아래 "잠정 동작" 중 이 문서의 앞 절에서 정하지 않은 것은 모두 [초안]이다.

**지원**
- 모듈 객체, 따옴표 있는/없는 키, `//` `/* */` 주석, 끝 쉼표, `pub`, 빈 `use: []`
- 함수: `fn<R(인자)>`, `fn<(인자)>`, `fn<void(...)>`, `fn`, 객체형 `fn { in, out, do }`, 인자 전달 방식 `inout` / `sink`
- 본문: 여러 줄 `'''` 블록, 한 줄 `''' ... '''`, 한 줄 값
- `struct`, `struct copy`: 필드, 필드 기본값, 메서드(`self` / `inout self` / `sink self`), `self` 없는 함수(`Point.new(...)`), 값 만들기 `Point { x: 1.0 }`, 필드 읽기와 대입, `&p.x` 같은 필드 inout 전달
- `enum`, `enum copy`: 데이터 있는/없는 경우, 경우 필드의 기본값, 메서드, 값 만들기 `Shape.Circle { radius: 1.0 }`, `Color.Red`
- 튜플: 타입 `(i32, f64)`, 값 `(a, b)`, `.0` 접근과 대입, 분해 `let (q, r) = ...`, 튜플 반환 `fn<(i32, i32)(...)>`, `match`의 튜플 패턴
- `Option`: `T?`, `Option<T>`, `null`, `Some(...)`, Option 자리의 값 자동 감싸기, `== null` / `!= null`, `Some(p)` / `null` 패턴
- 메모리: 소멸자 `drop: fn<(inout self)>`, 자동 해제(변수, 임시 값, 이동된 나머지), `.clone()`(자동과 사용자 정의 `clone`)
- `str`: `+`, `+=`, 비교, `.len()`, `.clone()`, 값으로 쓰는 `f"..."`
- `List<T>`: 값 `[a, b]`, 빈 값 `[]`, 첨자 `xs[i]`(읽기, 대입, 그 자리 수정, `&xs[i]`), `len`, `is_empty`, `push`, `pop`, `insert`, `remove`, `clear`, `.clone()`, `for x in xs` / `for inout x in xs` / `for sink x in xs`
- 쓰는 중인 자리 검사: 쓰는 중인 자리를 바꾸거나 이동하면 오류 (잠정 동작 54번)
- `Result<T, E>`(프렐류드), `Ok(v)` / `Err(e)`, `is_ok` / `is_err` / `value_or`, `?`(Result와 Option)
- 제네릭과 trait: 제네릭 struct/enum/함수/메서드 `Pair<T>`, `max<T: Ord>`, `map<U>`, 제약 `T: Ord + Clone`, 타입 인자 추론, `trait` 선언과 `is: [...]`, 기본 trait `Copy`/`Clone`/`Eq`/`Ord`, 단형화
- `match` 문과 식: 경우 패턴 `Circle(r)`, `_`, 이름, 정수/문자/bool 리터럴, 범위 `a..b` / `a..=b`, 조건 `if`, 빠진 경우 검사
- 값 이동 검사: 이동된 값을 쓰면 오류
- 문장: `let`, `var`, 튜플/struct 분해 `let (a, b) = t` / `let Pair { first, second } = p`, 대입과 복합 대입, `if`/`elif`/`else`, `while`, 정수 범위 `for i in a..b` / `a..=b`, 리스트 `for`, `break`, `continue`, `return`, 함수 호출
- 식: 산술, 비트, 시프트, 비교, `and`/`or`/`not`(짧은 평가), 삼항식 `a if c else b`, `as` 변환, 함수 호출
- 타입: `i8`~`i64`, `u8`~`u64`, `isize`, `usize`, `f32`, `f64`, `bool`, `char`, `str`
- 리터럴: 정수(16진, 2진, `_`, 접미사), 실수(지수, 접미사), `true`/`false`, 문자, 문자열(JSON 이스케이프와 `\u{...}`), `f"..."`
- `print`, `write`. `main: fn` 또는 `main: fn<i32()>` (반환값이 종료 코드)
- 정수 오버플로: 디버그는 panic, `-O`는 감싸기. 0으로 나누기는 panic

**아직 지원하지 않음** (사용하면 "not supported yet" 오류): 최상위 데이터 값, `extend`, 제네릭 trait과 trait 객체, 연산자 오버로딩, 에러 타입 변환(`?`의 `From`), `List` 외의 컬렉션(`Array`, `Map`, `Set`), 클로저, 객체 값, 포인터와 `unsafe`, C 연동, 모듈 가져오기, `str` 첨자, `?`

**잠정 동작 [초안]**
1. (확정, 결정 41) 여러 줄 `'''` 블록에서는 첫 코드 줄의 들여쓰기가 블록의 기준(0단계)이다. 그래서 코드를 닫는 `'''`보다 더 들여 써도 된다. 닫는 `'''`보다 덜 들여 쓴 줄과 첫 코드 줄보다 덜 들여 쓴 줄은 오류다.
2. 빈 블록에는 Python처럼 `pass`를 쓴다.
3. `!`는 연산자가 아니다. 부정은 `not`, 다름은 `!=`이다.
4. 정수 `/`는 0 쪽으로 버리고, `%`의 부호는 왼쪽 피연산자를 따른다 (C와 같음). `-7 / 3`은 `-2`, `-7 % 3`은 `-1`.
5. 시프트 양은 비트 폭으로 나눈 나머지를 쓴다 (`1u32 << 33`은 `2`).
6. (확정, 결정 53으로 대체) 보간 문자열 `f"..."`은 `str` 값이라 어디서나 쓸 수 있다. 값은 `print`와 같은 모양으로 쓴다 (실수 `2.0`, `true`, 문자).
7. `as`: 숫자끼리, `bool`과 `char`에서 정수로 변환할 수 있다. 실수를 정수로 바꿀 때 범위를 넘으면 최댓값/최솟값으로 고정하고 NaN은 0이 된다. 정수를 `char`로 바꾸는 것은 막는다.
8. 숫자 타입 사이의 암묵적 변환은 없다. 다만 타입 표기 없이 리터럴로 초기화한 변수(`var i = 0`)는 그 함수 안에서 처음 타입이 정해지는 곳의 타입을 따른다 (확정, 결정 42). 끝까지 정해지지 않으면 `i32`/`f64`, 서로 다른 타입으로 쓰이면 오류다. 결정된 타입은 `hana check --ast`에 `(inferred)`로 나온다.
9. 같은 범위에서 같은 이름을 다시 선언하면 오류다. 안쪽 블록에서 바깥 이름을 가리는 것은 허용한다.
10. 식만 있는 문장은 함수 호출과 `?`만 허용한다 (`a + 1`만 쓰면 오류). `check()?` 문장은 `Ok`의 값을 버린다.
11. `while true:` 안에 `break`가 없으면 끝나지 않는 반복으로 보고, 그 뒤에 `return`이 없어도 된다.
12. 함수 본문(`'''` 또는 한 줄 코드)은 `fn` 머리와 같은 줄에서 시작한다.
13. 실행 환경은 Linux다. 런타임 라이브러리를 `/proc/self/exe` 기준으로 찾고, 링크는 시스템 `cc`(`HANA_CC`로 변경)로 한다. macOS는 `HANA_RUNTIME`을 지정하면 동작할 것으로 보이지만 확인하지 않았다. Windows는 이후 단계에서 지원한다.
14. struct 필드의 타입은 파일에서 뒤에 선언된 struct여도 된다. struct는 자기 자신을 값으로 담을 수 없다 (`A`가 `B`를, `B`가 `A`를 담는 것도 오류).
15. 필드 기본값(`hp: i32 = 100`)은 변수를 쓸 수 없는 한 줄 식이다. 그 필드를 생략한 값을 만들 때마다 다시 계산한다.
16. 값 만들기 `Point { x: 1.0, y: 2.0 }`: 기본값 없는 필드는 모두 써야 하고, 순서는 자유이며, 쓴 순서대로 계산한다. 여러 줄에 걸쳐 써도 된다.
17. `inout self` 메서드는 `&` 없이 `p.damage(10)`으로 부른다. 받는 값은 `var` 변수나 그 필드여야 한다.
18. 값 이동: `Copy`가 아닌 값은 `let` 초기화, 대입, 필드 값, `sink` 인자, `sink self`, `return`에서 이동한다. 이동된 변수를 쓰면 오류이고, 새 값을 대입하면 다시 쓸 수 있다. `if`의 한쪽 가지에서만 이동해도 그 뒤로는 이동된 것으로 본다. 반복문 안에서 바깥 변수를 이동하면 다음 반복에서 오류다.
19. (확정, 결정 47) 필드를 이동하면(`let n = p.name`) 변수 전체가 이동된 것으로 본다. 부분 이동은 없고, 나눠 가지려면 분해나 `match`를 쓴다. 읽기 인자, `inout` 인자, 읽기/`inout` `self`에서는 이동할 수 없다.
20. (확정, 결정 43) inout 배타성: 같은 호출에서 겹치는 자리(`p`와 `p.x`)를 inout으로 두 번 넘길 수 없다. `Copy`가 아닌 값은 inout으로 넘기면서 같은 자리를 읽기 인자, `sink` 인자, 받는 값으로도 넘길 수 없다. `Copy` 값은 호출 전에 복사되므로 겹쳐도 된다 (`t.hit(t.hp)`, `f(&x, x)`). `p.x`와 `p.y`는 겹치지 않는다.
21. struct 값은 아직 `print`할 수 없고 `==` 같은 연산자도 쓸 수 없다 (연산자 오버로딩 이후).
22. 함수와 struct/enum은 같은 이름을 쓸 수 없다. 내장 타입 이름(`i32` 등)으로 struct나 enum을 만들 수 없다.
23. 패턴에 쓴 이름이 match하는 enum의 경우 이름이면 그 경우(`Empty`)이고, 아니면 값 전체를 받는 이름 바인딩이다. 경우 이름은 enum 이름 없이 쓴다 (`Shape.Circle(r)`가 아니라 `Circle(r)`).
24. 데이터가 있는 경우는 필드마다 패턴을 하나씩 써야 한다 (`Rect(w, _)`). 이름만 쓰면(`Rect`) 오류다.
25. `match` 식은 줄 끝에서 갈래를 들여 쓰는 모양이라 `let` 값, 대입 값, `return` 값으로만 쓸 수 있다. 식 갈래의 값은 한 줄 식이다.
26. 빠진 경우 검사는 중첩된 패턴까지 본다. enum, `Option`, `bool`, 튜플은 경우마다 따져서, `Some(Some(v))`, `Some(null)`, `null`이면 `Option<Option<T>>`를 모두 다룬 것이다. 빠진 값은 `'Some(null)'`처럼 예를 들어 알린다. 조건(`if`)이 붙은 갈래는 아무 값도 다룬 것으로 세지 않는다. 정수와 문자는 리터럴과 범위로 모두 덮어도 마지막에 `_`나 이름 갈래가 있어야 한다. 앞의 갈래들이 이미 모든 값을 받는 갈래(같은 경우를 두 번 쓴 갈래 포함)는 오류다.
27. 패턴의 이름은 match한 값의 일부다. 읽기만 하면 읽기이고, 이동하면 그 값의 변수 전체가 이동된다 (19번과 같은 규칙). 그래서 읽기 인자를 match해서 필드를 읽을 수는 있지만 꺼내 갈 수는 없다. 이름은 갈래가 선택된 때의 값을 가진다.
28. 갈래는 위에서부터 차례로 시험한다. 패턴이 맞으면 이름을 묶고, 그다음 조건을 계산한다.
29. enum 값은 아직 `print`할 수 없고 `==`로 비교할 수 없다. `match`를 쓴다.
30. `for`는 지금 정수 범위만 돈다: `for i in a..b`(끝 제외), `for i in a..=b`(끝 포함). 범위는 `for` 머리에만 쓸 수 있고 값으로 저장할 수 없다. 양 끝은 반복 전에 한 번 계산한다 (시작 먼저). 양 끝은 산술식이어도 된다 (`0..n + 1`).
31. 루프 변수는 `let`처럼 바꿀 수 없다. 양 끝의 타입이 같아야 하고, 타입 없는 리터럴은 다른 쪽 타입을 따른다. 양 끝이 모두 타입 없는 리터럴이면 루프 변수는 `var i = 0`처럼 처음 쓰이는 곳의 타입을 따른다 (결정 42와 같은 규칙).
32. 빈 범위(`5..5`, `5..=4`)는 한 번도 돌지 않는다. 타입의 끝까지 가는 범위(`0u8..=255`)도 넘치지 않는다. `continue`는 다음 값으로 넘어간다.
33. 문자 범위를 도는 `for`(`'a'..='z'`)는 아직 없다.
34. 위치 접근은 정수 리터럴로만 쓴다 (`t.0`, `nested.0.1`). 필드처럼 대입하고 inout으로 넘길 수 있다 (`t.0 += 1`, `f(&t.1)`).
35. 분해 `let (a, b) = ...`에는 이름, `_`, 튜플, struct 패턴(81번)만 쓴다. 값을 시험하는 패턴(`let (x, 0) = ...`)은 오류이고 `match`를 쓴다. `var (a, b) = ...`는 모든 이름을 `var`로 만든다. 분해하면 값 전체가 이동하고, 각 이름이 자기 부분을 소유한다.
36. `match`의 튜플 패턴은 원소마다 패턴을 하나씩 쓴다. 모든 원소가 `_`나 이름인 튜플 패턴은 모든 값을 받는다. 그렇지 않으면 마지막 `_` 갈래가 필요하다.
37. 타입 없는 리터럴로 만든 튜플은 원소마다 기본 타입(`i32`/`f64`)을 갖는다 (`let t = (0, 1)`). 기대 타입이 있으면 원소마다 그 타입을 따르고, 리터럴 변수는 튜플 안에서도 추론된다 (`let t: (i64, i32) = (i, 1)`이면 `i`는 `i64`).
38. 튜플은 아직 `print`할 수 없고 `==`로 비교할 수 없다. 대입으로 분해하는 `(a, b) = (b, a)`는 없다.
39. `null`은 기대 타입이 `Option`일 때만 쓸 수 있다. `let a = null`은 타입을 알 수 없어 오류이고 `let a: i32? = null`로 쓴다.
40. 자동 감싸기는 `Option`이 아닌 값에만 일어난다. `i32??`가 기대되는 자리의 `i32?` 값은 `Some(...)`으로 직접 감싼다. 기대 타입이 `Option`이면 리터럴과 연산은 값 타입으로 계산한 뒤 감싼다 (`let a: u8? = 300`은 `u8` 범위 오류). 조건식과 `match`는 가지마다 따로 감싼다 (`n if n > 0 else null`).
41. 감싼 값은 새 `Option`이 갖는다. 그래서 `Copy`가 아닌 값은 읽기 인자 자리에서 감싸져도 이동한다 (`keep(q)`에서 `keep`의 인자가 `P?`이면 `q`는 이동). `Some(x)`도 `x`를 이동한다. `inout` 인자는 감싸지 않는다.
42. `Option` 패턴은 `Some(p)`와 `null`이다. `Some`만 쓰면 오류다. `Option` 값은 `== null` / `!= null` 말고는 연산할 수 없고 `print`할 수 없다.
43. `Some`은 prelude의 이름이라 같은 이름의 함수를 만들 수 없다.
44. (확정, 결정 54) 필드는 `drop` 본문 뒤에 선언의 역순으로 해제된다 (C++과 같음, 변수와 같은 규칙). enum은 현재 경우의 필드를, `Option`은 값이 있을 때 그 값을, 튜플은 원소를 역순으로 해제한다.
45. 임시 값(함수가 돌려준 값, 연산 결과, 값 만들기)은 그것이 나온 문장이 끝날 때 해제된다. 조건식의 가지, `and`/`or`의 오른쪽, `match` 갈래의 값과 조건, `while` 조건은 각각 따로 끝나서, 그 안의 임시 값은 그 부분이 끝날 때 해제된다.
46. 대입 `x = v`는 새 값을 다 계산한 뒤 옛 값을 해제하고 새 값을 넣는다. `x`가 이동된 상태였으면 해제하지 않는다.
47. 한쪽 가지에서만 이동된 변수는 실행 중에 이동 여부를 기억해서, 범위가 끝날 때 남아 있는 경우에만 해제한다.
48. 변수의 필드를 이동하면(19번) 나머지 필드는 그 자리에서 해제된다. 임시 값에서 필드를 꺼내면(`make().name`) 나머지는 그 문장이 끝날 때 해제된다. 둘 다 그 값이나 바깥 값에 `drop`이 있으면 오류다.
49. 임시 값을 `match`하면 갈래의 이름이 자기 부분을 소유하고, 나머지는 갈래에 들어갈 때 해제된다. 값에 `drop`이 있으면 `Copy`가 아닌 부분을 이름으로 묶을 수 없다 (변수에 먼저 저장한다). 변수를 `match`하면 이름은 그 변수의 일부다 (27번).
50. `return`, `break`, `continue`는 빠져나가는 범위의 변수를 안쪽부터 해제한다.
51. 사용자 `clone`은 `clone: fn<T(self)>` 모양이어야 한다 (`T`는 그 타입). `drop`이 있는 타입도 `clone`을 정의하면 `.clone()`을 쓸 수 있다.
52. `str`의 `.len()`은 바이트 수다 (`"\u{D55C}".len()`은 3). 비교는 바이트를 차례로 비교하고, 앞부분이 같으면 짧은 쪽이 작다.
53. 리터럴 문자열은 프로그램에 들어 있는 바이트를 가리키고 복사하지 않는다. 처음 바꿀 때(`+=`) 복사한다.
54. 쓰는 중인 자리는 바꾸거나 이동할 수 없다 (결정 43을 중첩된 호출과 연산까지 넓힌 것). 자리는 계산된 때부터 그것을 쓰는 연산이 실행될 때까지 쓰는 중이다:
    - 포인터로 넘기는 인자(inout, `Copy`가 아닌 읽기 인자)와 받는 값은 호출이 실행될 때까지. 그래서 `g(p.name, h(&p))`는 오류다.
    - `Copy`가 아닌 `+`/비교의 왼쪽 값은 오른쪽을 계산하는 동안.
    - 첨자를 매기는 리스트는 첨자를 계산하는 동안.
    - `match`한 변수는, 그 일부를 `Copy`가 아닌 이름으로 묶은 갈래의 조건과 본문 동안 (이름이 변수의 데이터를 함께 쓴다).
    - `for x in xs`, `for inout x in xs`의 `xs`는 본문 동안.
    `Copy` 값은 계산할 때 읽으므로 나중까지 쓰는 중이 아니다 (`show(p.hp, bump(&p))`는 된다). 대입은 값을 먼저 계산하므로 `xs[0] += grow(&xs)`는 된다.
55. 타입 표기 없는 리스트 값의 원소 타입은 타입 없는 리터럴이 아닌 첫 원소의 타입이다 (`[1, x]`는 `x`의 타입). 모두 리터럴이면 첫 원소의 기본 타입(`i32`/`f64`)이다. 기대 타입이 있으면 그것을 따른다.
56. 리스트 원소는 앞에서부터 해제된다. 대입 `xs[i] = v`는 새 값을 계산한 뒤 옛 원소를 해제한다.
57. `pop()`은 빈 리스트에서 `null`이다. `remove(i)`와 첨자는 `i < len()`, `insert(i, v)`는 `i <= len()`이어야 하고, 아니면 "index N is out of bounds for a list of length L" panic이다.
58. `for x in xs`는 반복 전에 리스트를 한 번 계산한다. 리스트가 임시 값이면 `for` 문이 끝날 때 해제된다. 본문 동안 리스트는 쓰는 중이라(54번) 길이가 바뀌지 않는다.
59. `for sink x in xs`에서 `break`나 `return`으로 끝나면, 아직 꺼내지 않은 원소는 그때 앞에서부터 해제된다. 반복 변수는 반복마다 새 원소를 소유한다.
60. 읽기/`inout` 반복 변수와, 리스트 원소를 `match`해서 묶은 이름은 원소의 일부라 이동할 수 없다. `.clone()`하거나 `for sink`, `pop()`, `remove(i)`를 쓴다.
61. `for inout`은 변수나 그 필드인 리스트에만 쓴다 (임시 리스트는 오류).
62. struct는 자기 자신의 리스트를 필드로 가질 수 있다 (`children: List<Node>`). 리스트는 원소를 따로 저장하기 때문이다.
63. 리스트는 아직 `print`할 수 없고 `==`로 비교할 수 없다. 첨자는 리스트에만 쓸 수 있다 (`str` 첨자는 없음).
64. 기본 trait은 `Copy`, `Clone`, `Eq`, `Ord`다.
    - `Copy`는 Copy 타입, `Clone`은 clone 가능한 타입(결정 52)이 갖는다.
    - `Eq`는 숫자, `bool`, `char`, `str`이 갖는다. `Ord`는 숫자, `char`, `str`이 갖는다.
    - `Ord`는 `Eq`를, `Copy`는 `Clone`을 포함한다.
    - 사용자 타입은 `Eq`/`Ord`를 아직 가질 수 없다 (연산자 오버로딩과 함께 정한다).
    - 기본 trait은 `is`에 쓰지 않는다.
65. trait은 `self`를 받는 메서드의 시그니처만 갖는다. 기본 구현, `self` 없는 함수, 제네릭 메서드, trait 상속, 제네릭 trait은 아직 없다.
66. 타입 인자를 직접 쓰는 호출(`max<i64>(1, 2)`)은 없다. 타입 인자는 인자와 결과의 기대 타입에서 추론하고, 정해지지 않으면 오류다 (`let p: Pair<i64> = Pair { ... }`처럼 기대 타입을 준다).
67. 추론 순서: 자기 타입이 있는 인자를 먼저 보고, 그다음 기대 결과 타입, 마지막으로 리터럴, `null`, `[]`를 본다. 리터럴은 그때까지 정해진 타입을 따르고, 아무것도 없으면 기본 타입(`i32`/`f64`)이다. `T?` 자리의 값 `x`는 `T`를 `x`의 타입으로 정한다.
68. 타입 자리의 제네릭 타입에는 항상 타입 인자를 쓴다 (`Pair`만 쓰면 오류). 데이터 없는 경우 `Maybe.None`은 기대 타입이 있어야 한다.
69. 타입 매개변수 값으로 할 수 있는 일은 제약이 정한다.
    - `Copy`가 없으면 값은 이동한다.
    - 출력과 산술은 할 수 없다.
    - `==`/`!=`는 `Eq`, 크기 비교는 `Ord`, `.clone()`은 `Clone`이 있어야 한다.
    - 사용자 trait의 메서드는 그 trait이 제약에 있으면 부를 수 있다.
70. 제네릭 타입의 메서드는 그 타입의 타입 매개변수를 쓰고, 메서드 자신의 타입 매개변수도 가질 수 있다 (`map<U>`). `Stack.new()`처럼 타입 이름으로 부르는 함수의 타입 인자도 인자와 기대 타입에서 추론한다.
71. 제네릭 함수가 자기 타입 매개변수로 만든 더 큰 타입으로 자기를 부르면(`f<T>` 안에서 `f([x])`) 단형화가 끝나지 않으므로 오류다.
72. 인스턴스는 `Pair<i32>`, `max<i32>`처럼 타입 인자를 붙인 이름을 갖는다 (오류 메시지와 IR).
73. 프렐류드: 컴파일러가 모든 프로그램 앞에 붙이는 HANA 선언(`src/lang/prelude.cpp`). 지금은 `Result<T, E>`만 있다. 같은 이름을 다시 정의하면 오류이고, 프렐류드 위치는 `<prelude>:줄`로 표시한다.
74. `Ok(v)`/`Err(e)`는 `Some(v)`처럼 prelude 함수 이름이다. 두 타입 중 하나만 주므로 기대 타입(`let r: Result<i32, str> = ...`, 반환값, 인자)이 있어야 한다. 기대 타입 없는 `let r = Ok(1)`은 오류다.
75. `Result`에는 `Option`처럼 값을 자동으로 감싸는 규칙이 없다 (`return 5`가 아니라 `return Ok(5)`). `T`와 `E`가 같은 타입이면 어느 쪽인지 모호하기 때문이다.
76. `x?`는 `x`를 이동한다. `Result`면 `Err(e)`일 때 함수가 `Err(e)`를 반환하고, 아니면 `Ok`의 값이 식의 값이 된다. `Option`이면 `null`일 때 함수가 `null`을 반환한다.
77. `Result`에 쓴 `?`는 함수가 같은 에러 타입의 `Result`를 반환해야 한다. 에러 타입 변환(Rust의 `From`)은 아직 없다. `Option`에 쓴 `?`는 함수가 `Option`을 반환해야 한다 (Result와 섞을 수 없음).
78. `?`가 함수를 빠져나가면 `return`처럼 모든 변수와 임시 값을 해제한다. 그 식에서 이미 만들었지만 아직 다 만들어지지 않은 값의 부분도 해제한다. 예: `(make(), check()?)`의 `make()`, 앞쪽 `sink` 인자, 리스트의 앞 원소, f-string의 앞부분, 가드에서 빠져나갈 때의 `match` 대상.
79. `x?`의 기대 타입 `T`가 있으면 `x`는 `Result<T, E>`(E는 함수의 에러 타입) 또는 `T?`로 검사된다. 그래서 `let n: i64 = parse()?`처럼 제네릭 함수의 타입이 정해진다.
80. `main`은 아직 `Result`를 반환할 수 없다 (`void` 또는 `i32`).
81. struct 분해 `let Pair { first, second } = p`: 값 전체가 이동하고 각 이름이 자기 필드를 소유한다 (결정 47의 부분 이동 없이 값을 나눠 갖는 방법).
    - 필드만 쓰면 같은 이름의 변수가 생긴다. `field: name`은 다른 이름, `field: _`는 버리기다.
    - `field: (a, b)`나 `field: Q { ... }`로 중첩해 분해한다.
    - 쓰지 않은 필드는 그 자리에서 해제된다 (`..` 표기는 없다). 해제 순서는 선언의 역순이다.
    - `var Pair { ... } = p`는 모든 이름을 `var`로 만든다.
82. 패턴의 타입 이름은 타입 인자 없이 쓴다 (`Pair<i32>` 값도 `let Pair { ... }`). 값의 타입과 이름이 다르면 오류다.
83. `drop`이 있는 struct는 분해할 수 없다. `drop`은 값 전체가 있어야 하기 때문이다. enum은 `match`로 꺼낸다.
84. struct 패턴은 지금 `let`에서만 쓴다. `match`의 struct 패턴(`Point { x: 0, y }`)은 아직 없다.

### 14.1 도구

**`hana check --format json`**: 진단을 JSON 하나로 표준 출력에 낸다. 종료 코드는 텍스트 출력과 같다 (오류가 있으면 1).
```json
{"file": "a.hana", "error_count": 1, "diagnostics": [
    {"severity": "error", "line": 4, "column": 5, "message": "cannot assign to 'x' ...",
     "notes": [{"line": 3, "column": 9, "message": "'x' is declared here"}]}
]}
```
- `note`는 바로 앞 오류의 `notes`에 들어간다.
- [초안] 자동 수정안(`fixes`)은 이후에 추가한다.

**`hana run/build --sanitize=address`**: 생성 코드에 AddressSanitizer 계측을 넣고 `-fsanitize=address`로 링크한다. 해제한 메모리 사용, 두 번 해제, 누수를 실행 중에 찾는다. 컴파일러 개발용이고, 링크하는 `cc`가 AddressSanitizer를 지원해야 한다. 테스트(`memory.*`)는 `e2e/run`의 모든 프로그램을 이 옵션으로 다시 실행한다.

**`hana fmt`**: 표준 배치로 다시 들여 쓴다.
- JSON 층은 열린 `{`, `[`, `(` 하나당 2칸.
- `'''` 블록의 코드는 블록을 여는 줄보다 2칸 오른쪽에서 시작하고, 블록 단계마다 2칸. 닫는 `'''`는 여는 줄과 같은 위치.
- 괄호 안에서 이어지는 줄은 그 논리 줄의 시작과의 거리를 유지한다.
- 주석만 있는 줄은 다음 코드 줄의 들여쓰기를 따른다. 여러 줄 `/* */` 주석의 안쪽 줄은 그대로 둔다.
- 줄 끝 공백을 지우고, 줄바꿈은 `\n`, 파일 끝은 줄바꿈 하나로 맞춘다.
- 바꾸는 것은 줄 앞뒤의 공백뿐이다. 그래서 의미와 주석이 그대로 남는다. 문법 오류가 있는 파일은 바꾸지 않는다.
- `hana fmt <파일>`은 파일을 고친다. `--check`는 바꿀 것이 있으면 실패하고, `--stdout`은 결과를 출력만 한다.
- [초안] 한 줄 안의 공백(연산자 주변 등)은 아직 정리하지 않는다.

## 15. 미정 항목 모음

이 문서를 쓰면서 드러난 것들이다. 예제에는 쓰였지만 아직 정하지 않았다.

1. (결정 61로 정함) trait 구현 표시.
2. (결정 59, 60으로 정함) 제네릭 선언과 제약 표기.
3. **참조 카운트 타입** (소멸자는 결정 51).
4. (결정 55로 정함) 컨테이너 원소를 복사 없이 수정하는 방법.
5. **`panic`** 직접 호출 방법.
6. **`component`, `app`, `column`의 정체**: 언어 기능인지 표준 라이브러리 타입인지.
7. **양방향 바인딩**.
8. **순수 JSON 변환 기능**.
9. **한 줄 코드 끝의 주석**: 한 줄 코드는 JSON 부분에 있으므로 `//`를 쓴다고 보는 것이 자연스럽다. 명시적으로 정하지는 않았다.
10. (잠정 동작 81~84번으로 [초안] 구현) struct 분해. `match`의 struct 패턴은 아직 미정이다.
