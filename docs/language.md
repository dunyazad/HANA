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
- [미정] `enum`(대수적 데이터 타입), 연산자 오버로딩

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

let worker = spawn {
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
- `enum`과 패턴 매칭 세부
- 클로저와 캡처 규칙 (`spawn { ... }` 블록이 바깥 값을 이동/읽기로 가져가는 방식)
- 연산자 목록과 우선순위, 복합 대입(`+=` 등), 연산자 오버로딩
- 리터럴 (정수 접미사, 문자열 보간, 컬렉션 리터럴)
- 선언형 UI를 위한 범용 기능 (블록 문법, 반응형 상태)
- 표준 라이브러리 구성
- JIT/핫 리로드
