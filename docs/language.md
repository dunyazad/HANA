# HANA 언어 정의

각 항목의 상태 표시:
- **[확정]** 결정된 사항. 바꾸려면 명시적으로 다시 결정한다.
- **[초안]** 현재 기준으로 삼는 안. 언제든 바뀔 수 있다.
- **[미정]** 아직 논의하지 않았다.

## 1. 기반 [확정]
- 네이티브 컴파일: 자체 프론트엔드 -> LLVM IR -> 네이티브 코드
- 메모리: RAII + 이동 기본 + `unsafe`, GC 없음
- GUI/3D는 범용 언어 기능 위에 만든 표준 라이브러리

## 2. 선언 [확정]
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
- [미정] 마지막 표현식을 반환값으로 쓰는 것을 허용할지, 문장 구분(줄바꿈/세미콜론), 주석 문법

## 3. 타입 확장 [확정: struct + trait + 제네릭, 상속 없음]
```
struct Point {
    x: f32
    y: f32
}

trait Shape {
    fn area(self: &Self) -> f32
}

struct Circle {
    center: Point
    radius: f32
}

impl Shape for Circle {
    fn area(self: &Self) -> f32 {
        return 3.14159 * self.radius * self.radius
    }
}

fn total_area<T: Shape>(shapes: &[T]) -> f32 { ... }
```
- 데이터는 `struct`, 공통 동작은 `trait`, 다형성은 trait과 제네릭으로 표현한다.
- 클래스 상속은 없다.
- [초안] 구현은 `impl Trait for Type`, 타입 고유 메서드는 `impl Type`.
- [초안] 동적 디스패치는 `dyn Trait`로 명시한다.
- [미정] `enum`(대수적 데이터 타입), 연산자 오버로딩, `self` 표기와 참조(`&`) 문법의 최종 형태

## 4. 에러 처리 [확정: Result + `?`, 예외 없음]
```
fn read_config(path: &str) -> Result<Config, IoError> {
    let text = read_file(path)?     // returns the error to the caller on failure
    return parse_config(text)
}
```
- 실패할 수 있는 함수는 `Result<T, E>`를 반환한다.
- `?`는 에러면 즉시 호출자에게 그 에러를 반환한다.
- 예외는 없다.
- [초안] 복구 불가능한 오류는 `panic`으로 프로그램을 중단한다.

## 5. C++ 연동 [확정: 1단계 C ABI, 이후 바인딩 생성 도구]
- 1단계: C ABI로 함수를 호출하고 내보낸다.
- 이후: clang 기반 도구로 C++ 헤더에서 HANA 바인딩을 자동 생성한다.
- C++ 템플릿/클래스를 언어에서 직접 쓰는 방식은 하지 않는다.
- [초안] 외부 함수 선언은 `extern "C" fn ...` 형태.

## 6. 미정 항목
- 참조와 대여 규칙, `unsafe` 범위
- 동시성 모델
- 모듈과 패키지
- 선언형 UI를 위한 범용 기능 (블록 문법, 반응형 상태)
- 표준 라이브러리 구성
- JIT/핫 리로드
