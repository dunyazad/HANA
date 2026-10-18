# HANA

- 프로젝트/언어 공식 표기: `HANA` (문서, 로고, 제목, 창 제목 등 사람이 읽는 곳은 항상 대문자)
- 코드 식별자는 소문자 유지: 확장자 `.hana`, CLI `hana`, 네임스페이스 `hana::`
- 매크로/상수 접두사는 대문자: `HANA_VERSION`, `HANA_API` 등
- 저장소 이름, 패키지 이름 등 식별용 이름: `hana` (`hana-lang`, `hanalang` 같은 별칭은 쓰지 않는다)
- 구현 언어: C++ (C++20)
- 빌드: CMake

## 작업 규칙 (반드시 지킬 것)

- 답변은 한국어 존댓말로 한다.
- 코드 주석에는 유니코드(한글, 특수기호 포함 비 ASCII 문자)를 쓰지 않는다. 주석은 ASCII 영어로만 작성한다.
- 코드는 함수 단위로 전달한다. 항상 그대로 복사해 컴파일 가능한 완전한 함수 본문을 준다. 조각 코드, "N번째 줄 뒤에 추가" 같은 위치 지시는 쓰지 않는다.
- 긴 함수는 이름 있는 작은 헬퍼 함수로 나누고, 각 헬퍼도 완전한 함수로 제공한다.
- 문제 수정은 근본 원인을 고친다. 임시방편(workaround)은 제안하지 않는다.
- 사용자가 "터졌다"고 하면 빌드 에러가 아니라 런타임 크래시를 의미한다.
- 사용자가 확정하지 않은 설계를 확정된 것처럼 다루지 않는다. 제안은 [초안]으로 표시한다.

## 언어 정의

- `docs/language.md`: 언어 정의. 문법과 의미의 기준. 문법을 바꾸면 이 문서를 먼저 고친다.
- `docs/decisions.md`: 사용자가 확정한 설계 결정 목록 (번호 순).
- 두 문서의 [초안], [미정] 항목을 확정된 것처럼 다루지 않는다.

요약:
- 프로그램의 뼈대는 JSON 확장 문법이다. 표준 JSON은 그대로 올바른 HANA 소스다.
- 모든 선언은 `이름: 종류 { 키: 값 }`. 함수는 `fn<반환타입(인자)>`(C++ `std::function` 모양) 또는 객체형 `fn { in, out, do }`.
- 실행 코드는 한 줄이면 값 자리에 그대로, 여러 줄이면 `'''` 코드 블록에 Python식 들여쓰기로 쓴다.
- 범위는 GUI부터 시스템 프로그래밍까지. 네이티브 컴파일(LLVM), 정적 타입 + 추론, GC 없는 RAII + 이동, 참조 대신 인자 전달 방식(`inout`, `sink`).
- 3D 수학 타입(`v2` `v3` `v4` `m33` `m44`)을 기본 제공. 사용자 타입은 표준 타입과 같은 지위.
- C 헤더 직접 가져오기, C++ 접착 코드 자동 생성, 동적 라이브러리 세 방식 지원.

## 디렉터리 구조

```
hana/
  CMakeLists.txt
  docs/
    language.md   언어 정의 (14절: 현재 컴파일러 구현 범위)
    decisions.md  설계 결정 기록
  src/
    support/   SourceLocation, 진단 메시지(DiagnosticEngine), UTF-8
    lang/      OuterParser(JSON 층), CodeLexer/CodeParser(''' 코드), AST, Type, Symbol, Sema(+ sema_match/option/tuple/list/generic/coverage), ast_clone(제네릭 인스턴스용 AST 복사), MoveChecker(이동), AccessChecker(쓰는 중인 자리), Formatter (hana::lang)
    codegen/   LLVM IR 생성(generator.h, codegen: 식/문장, codegen_match: match, codegen_memory: 소유/해제/clone/str, codegen_list: List), 최적화, AddressSanitizer 계측, 오브젝트 파일 출력 (hana::codegen)
    driver/    check / fmt / build / run 파이프라인, 링커 호출 (hana::driver)
    cli/       hana 실행 파일
  runtime/     컴파일된 프로그램에 링크되는 C 런타임 (출력, panic)
  examples/
  tests/
    unit/        OuterParser/CodeLexer/CodeParser/Sema 단위 테스트
    e2e/run/     <이름>.hana 를 실행해 .out(표준 출력), .status(종료 코드), .stderr(포함 문구), .flags(hana run 옵션) 확인
    e2e/errors/  <이름>.hana 의 진단 메시지가 <이름>.err 의 각 줄을 포함하는지 확인
    (memory.*)   e2e/run 파일을 --sanitize=address 로 다시 실행 (해제 후 사용, 두 번 해제, 누수 검사)
    (format.*)   e2e/run 파일을 hana fmt 한 결과가 다시 포맷해도 같고, 실행 결과가 같은지 확인
    (formatted.example.*) examples/ 가 표준 배치인지 확인
```

## 빌드와 테스트

필요: CMake 3.20+, C++20 컴파일러, LLVM 18 개발 패키지 (Ubuntu: `llvm-18-dev`), 링크용 `cc`.

```
cmake -S . -B build -G Ninja -DLLVM_DIR=/usr/lib/llvm-18/lib/cmake/llvm
cmake --build build
ctest --test-dir build
./build/hana run examples/fib.hana
./build/hana run -O examples/fib.hana
./build/hana run --sanitize=address examples/fib.hana
./build/hana check --tokens --ast examples/fib.hana
./build/hana check --format json examples/fib.hana
./build/hana fmt --check examples/fib.hana
./build/hana build --emit-ir examples/fib.hana
```
