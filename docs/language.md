# HANA 언어 정의

각 항목의 상태 표시:
- **[확정]** 결정된 사항. 바꾸려면 명시적으로 다시 결정한다.
- **[초안]** 현재 기준으로 삼는 안. 언제든 바뀔 수 있다.
- **[미정]** 아직 논의하지 않았다.

## 1. 기반 [확정]
- 네이티브 컴파일: 자체 프론트엔드 -> LLVM IR -> 네이티브 코드
- 메모리: RAII + 이동 기본 + `unsafe`, GC 없음
- GUI/3D는 범용 언어 기능 위에 만든 표준 라이브러리

## 2. 어휘

### 2.1 주석 [확정]
```
// line comment
/* block comment /* may nest */ */
```

### 2.2 문장 구분 [확정]
- 줄바꿈이 문장의 끝이다. 한 줄에 여러 문장을 쓸 때는 `;`로 구분한다.
- 다음 경우에는 줄바꿈이 문장을 끝내지 않고 다음 줄로 이어진다.
  - `(` `)` 또는 `[` `]` 안. 가장 안쪽의 열린 괄호가 기준이므로, 괄호 안의 `{ }` 블록에서는 줄바꿈이 다시 문장 구분자가 된다.
  - 줄이 이항 연산자, 단항 연산자, `,`, `:`, `.` 로 끝날 때.
- 문자열 리터럴은 시작한 줄에서 끝나야 한다.

### 2.3 리터럴 [확정]
```
42  0xff  0b1010  1_000_000  10u8  7i64       // integers
3.0  1e-3  2.5f32                              // floats
true  false
'a'  '\n'                                      // char
"name = {name}, total = {a + b}"               // string with interpolation
[1, 2, 3]  []                                  // list
```
- 정수: 10진, `0x` 16진, `0b` 2진, `_` 자릿수 구분, 타입 접미사(`u8`, `i64` 등).
- 실수: 소수점 또는 지수 표기, 접미사 `f32`/`f64`.
- 문자열 보간: `"{expr}"`. 중괄호 자체는 `\{`, `\}`로 쓴다.
- 리스트 리터럴: `[a, b, c]`.
- [초안] 접미사 없는 정수는 `i32`, 실수는 `f64`가 기본이고, 기대 타입이 있으면 그 타입이 된다.
- [초안] 이스케이프: `\n \t \r \0 \\ \" \' \{ \} \u{XXXX}`
- [미정] 맵/딕셔너리 리터럴

### 2.4 연산자 [확정: C 계열 + 복합 대입, 오버로딩은 trait]
- 산술 `+ - * / %`, 비트 `& | ^ ~ << >>`, 비교 `== != < <= > >=`, 논리 `&& || !`
- 대입 `=`과 복합 대입 `+= -= *= /= %= &= |= ^= <<= >>=`
- 오버로딩은 trait 구현으로 한다: [초안] `Add`, `Sub`, `Mul`, `Div`, `Eq`, `Ord`, `Index` 등.
- [초안] 우선순위 (높은 것부터). C와 달리 비트 연산이 비교보다 먼저 묶인다 (`a & mask == 0`이 의도대로 동작).
  1. 후위: 호출 `f()`, 첨자 `a[i]`, 멤버 `a.b`, `?`
  2. 전위: `-` `!` `~` `&`(inout 표시) `*`(포인터 역참조, unsafe)
  3. `* / %`
  4. `+ -`
  5. `<< >>`
  6. `&`
  7. `^`
  8. `|`
  9. `== != < <= > >=` (연쇄 비교 `a < b < c`는 오류)
  10. `&&`
  11. `||`
  12. 대입 `=`, 복합 대입 (식이 아니라 문장)
- [초안] `++`, `--`는 없다.

## 3. 선언 [확정]
```
fn add(a: i32, b: i32) -> i32 {
    return a + b
}

let x = 1           // immutable
var y = 2           // mutable
let z: f64 = 3.0    // explicit type
```
- 함수는 `fn`, 불변 바인딩은 `let`, 가변 바인딩은 `var`.
- 타입은 이름 뒤에 `: Type`으로 쓰고, 추론 가능하면 생략한다.
- 반환 타입은 `-> Type`. 생략하면 반환값 없음.
- [초안] 정수/실수 타입 이름: `i8 i16 i32 i64 u8 u16 u32 u64 isize usize f32 f64`, 그 외 `bool`, `char`, `str`
- [미정] 마지막 표현식을 반환값으로 쓰는 것을 허용할지

## 4. 타입 확장 [확정: struct + trait + 제네릭, 상속 없음]
```
struct Point {
    x: f32
    y: f32
}

trait Shape {
    fn area(self) -> f32
}

struct Circle {
    center: Point
    radius: f32
}

impl Shape for Circle {
    fn area(self) -> f32 {
        return 3.14159 * self.radius * self.radius
    }
}

fn total_area<T: Shape>(shapes: [T]) -> f32 { ... }
```
- 데이터는 `struct`, 공통 동작은 `trait`, 다형성은 trait과 제네릭으로 표현한다.
- 클래스 상속은 없다.
- [초안] 구현은 `impl Trait for Type`, 타입 고유 메서드는 `impl Type`.
- [초안] 동적 디스패치는 `dyn Trait`로 명시한다.
- [초안] 메서드의 `self`도 일반 인자와 같은 전달 방식을 따른다: `fn area(self)`, `fn grow(inout self)`, `fn into_parts(sink self)`.

### 4.1 enum [확정: 대수적 데이터 타입]
```
enum Shape {
    Circle(radius: f32)
    Rect(w: f32, h: f32)
    Empty
}

enum Option<T> {
    Some(T)
    None
}

enum Result<T, E> {
    Ok(T)
    Err(E)
}

fn area(s: Shape) -> f32 {
    return match s {
        Circle(r) => 3.14159 * r * r
        Rect(w, h) => w * h
        Empty => 0.0
    }
}
```
- 각 경우가 데이터를 가질 수 있는 enum. `Option`과 `Result`도 이것으로 정의된 표준 타입이다.
- `match`는 모든 경우를 다뤄야 한다 (빠지면 컴파일 오류).
- [초안] 패턴: 경우 이름과 바인딩 `Circle(r)`, 와일드카드 `_`, 리터럴, 범위 `1..=9`, 가드 `x if x > 0`
- [초안] 경우 이름은 `match` 안에서는 짧게 (`Circle`), 밖에서는 `Shape.Circle(1.0)`처럼 쓴다.

## 5. 값 전달과 포인터 [확정: 참조 타입 없음, 인자 전달 방식 + unsafe 포인터]
```
fn length(p: Point) -> f32 { ... }            // read-only, never copied
fn scale(p: inout Point, k: f32) { ... }      // mutable access to the caller's value
fn push(list: inout List<Item>, item: sink Item) { ... }   // ownership moves in

scale(&pt, 2.0)                               // '&' marks the inout argument at the call site
push(&items, make_item())
```
- 참조 타입(`&T`)은 없다. 함수 인자에 전달 방식을 붙이고, 실제 전달(포인터로 넘길지 값으로 넘길지)은 컴파일러가 정한다.
  - 표시 없음: 읽기 전용. 복사하지 않는다.
  - `inout`: 호출자의 값을 수정한다. 호출하는 쪽에서 `&`로 표시한다.
  - `sink`: 소유권이 함수로 이동한다. 이동 후 호출자는 그 값을 쓸 수 없다.
- 함수는 인자를 함수 밖에 저장할 수 없으므로 dangling이 생기지 않는다. 대여 검사기와 수명 표기는 없다.
- [초안] 같은 값을 한 호출에 `inout`으로 두 번 넘기거나, `inout`과 읽기로 동시에 넘기는 것은 컴파일 오류다 (배타성 규칙).
- 오래 들고 있거나 공유해야 하는 값은 소유 타입으로 표현한다: [초안] `Box<T>`(단독 소유), `Rc<T>`/`Weak<T>`(공유, 약한 참조).
- 원시 포인터 `*T`는 C 연동, 직접 만드는 자료구조, 시스템 코드용이다. 포인터를 만들고 역참조하고 산술하는 것은 `unsafe` 안에서만 한다.
- [초안] 키워드 이름 `inout`, `sink`, 호출 측 표시 `&`

### 5.1 접근자 [확정: 첨자/프로퍼티 접근자로 내부 원소를 빌려줌]
```
items[i].count = 3          // modifies the element in place, no copy
let n = items[i].count      // reads without copying the element
```
- 컨테이너 안의 값을 빌려주는 방법은 첨자(`x[i]`)와 프로퍼티(`x.name`) 접근자뿐이다.
- 접근자가 내준 값은 그 식 안에서만 유효하고, 저장할 수 없다. 그래서 dangling이 생기지 않는다.
- [초안] 선언 형태:
```
impl<T> List<T> {
    subscript(self, index: usize) -> T {
        read { yield self.slot(index) }
        modify { yield &self.slot(index) }
    }
}
```
  `read`는 읽기용, `modify`는 수정용 접근이다. `yield`로 값을 잠시 내주고, 사용이 끝나면 접근자 뒤쪽 코드가 이어서 실행된다 (정리 작업 가능).

## 6. 제어문 [확정]
```
if x > 0 { ... } else if x < 0 { ... } else { ... }

while running { ... }

loop {
    if done() { break }
}

for item in items { ... }

let sign = if x >= 0 { 1 } else { -1 }

let name = match shape {
    Circle(c) => "circle"
    Rect(r) => "rect"
    _ => "other"
}
```
- `if`/`else`, `while`, `loop`, `for x in xs`, `match`, `break`, `continue`.
- `if`와 `match`는 값을 돌려주는 식으로도 쓸 수 있다.
- [초안] `for`는 이터레이터 trait을 구현한 값을 순회한다.
- [미정] `match`의 패턴 문법 세부 (enum 정의 방식이 정해진 뒤)

## 6.1 클로저 [확정: 캡처는 기본 읽기, 이동은 `sink`로 명시]
```
let limit = 10
let small = items.filter(fn(x: i32) -> bool { return x < limit })   // reads limit

var total = 0
items.each(fn[inout total](x: i32) { total += x })                  // modifies total

let worker = spawn(fn[sink tx]() { tx.send(42) })                    // tx moves into the thread
```
- 캡처 방식은 인자 전달 방식과 같다: 표시 없음(읽기), `inout`, `sink`.
- 읽기 또는 `inout`으로 캡처한 클로저는 인자로 넘길 수만 있고, 저장하거나 반환할 수 없다 (참조를 저장할 수 없다는 규칙과 같다).
- 저장해 두는 콜백(GUI 이벤트 핸들러 등)은 모든 캡처가 `sink`(또는 `Rc` 같은 소유 타입을 `sink`로 가져간 것)여야 한다.
- 다른 스레드로 넘기는 클로저는 `sink` 캡처만 허용하고, 캡처한 값은 모두 `Send`여야 한다.
- [초안] 문법: 익명 함수 `fn[captures](params) -> T { ... }`. 캡처 목록 `[...]`에는 `inout`/`sink` 캡처만 적고, 읽기 캡처는 자동이다.
- [초안] 마지막 인자가 클로저면 괄호 밖에 블록으로 쓸 수 있다: `spawn [sink tx] { tx.send(42) }`
- [미정] 클로저 타입 표기 (`fn(i32) -> bool` 등)

## 7. 모듈 [확정]
```
// file: geometry/shape.hana  ->  module geometry.shape
import geometry.shape
import gui.widget.{Button, Label}

pub struct Circle { ... }     // visible outside the module
fn helper() { ... }           // private to the module
```
- 파일 하나가 모듈 하나다. 모듈 이름은 패키지 루트 기준 경로에서 정해진다.
- 가져오기는 `import`, 외부 공개는 `pub`. 표시가 없으면 모듈 안에서만 보인다.
- 패키지 설정 파일은 `hana.toml`.
- [초안] 여러 이름 가져오기 `import a.b.{X, Y}`, 별칭 `import a.b as c`


## 8. 에러 처리 [확정: Result + `?`, 예외 없음]
```
fn read_config(path: str) -> Result<Config, IoError> {
    let text = read_file(path)?     // returns the error to the caller on failure
    return parse_config(text)
}
```
- 실패할 수 있는 함수는 `Result<T, E>`를 반환한다.
- `?`는 에러면 즉시 호출자에게 그 에러를 반환한다.
- 예외는 없다.
- [초안] 복구 불가능한 오류는 `panic`으로 프로그램을 중단한다.

## 9. 동시성 [확정: 1단계 OS 스레드 + 채널 + Mutex]
```
let (tx, rx) = channel<i32>()

let worker = spawn [sink tx] {
    tx.send(42)
}

let value = rx.receive()
worker.join()

let counter = Arc<Mutex<i32>>(0)
counter.lock().value += 1
```
- 1단계는 OS 스레드, 채널, `Mutex<T>`.
- 스레드 사이에 넘길 수 있는 타입은 컴파일러가 표시하고 검사해 데이터 경합을 막는다.
- [초안] 이름: `spawn`, `channel<T>()`, `Mutex<T>`, 스레드 간 공유 `Arc<T>`, 넘길 수 있는 타입 표시 trait `Send`
- [미정] `async`/`await` (2단계에서 결정)

## 10. C++ 연동 [확정: 1단계 C ABI, 이후 바인딩 생성 도구]
- 1단계: C ABI로 함수를 호출하고 내보낸다.
- 이후: clang 기반 도구로 C++ 헤더에서 HANA 바인딩을 자동 생성한다.
- C++ 템플릿/클래스를 언어에서 직접 쓰는 방식은 하지 않는다.
- [초안] 외부 함수 선언은 `extern "C" fn ...` 형태.

## 11. 미정 항목
- 선언형 UI를 위한 범용 기능 (블록 문법, 반응형 상태)
- 표준 라이브러리 구성
- JIT/핫 리로드
