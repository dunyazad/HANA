# HANA 진행 상황과 이어서 하는 법

이 문서는 다른 작업 환경(로컬 Claude Code 등)에서 작업을 그대로 이어받기 위한 인수인계 문서다.
작업을 시작하기 전에 `CLAUDE.md`, 이 문서, `docs/decisions.md`, `docs/language.md` 14절을 읽는다.

## 1. 사용자와 일하는 방식

- 답변은 한국어 존댓말. 코드 주석은 ASCII 영어만. 코드는 완전한 함수 단위로 전달한다 (`CLAUDE.md`).
- 사용자가 **"추천대로 가자" / "쭉 추천대로 진행해"**라고 하면, 묻지 않고 추천안으로 끝까지 구현한다.
  - 사용자가 직접 고른 설계만 `docs/decisions.md`에 번호를 붙여 기록한다 (현재 1~62번).
  - 내가 고른 세부 설계는 `docs/language.md` 14절 "잠정 동작 [초안]"에 번호를 붙여 적는다 (현재 1~84번). 작업이 끝나면 확인할 [초안] 목록을 한 번에 보고한다.
  - 큰 갈림길(문법 표기 등)은 선택지 2~4개와 추천안을 함께 묻는다. 사용자가 "추천대로"라고 하면 그대로 결정으로 기록한다.
- "터졌다" = 런타임 크래시. 문제는 근본 원인을 고친다. 임시방편은 쓰지 않는다.
- 단계를 마칠 때마다 커밋하고, 보고에는 무엇을 했는지, 찾은 버그, 정해 둔 [초안]을 쓴다.

## 2. 지금까지 한 것 (브랜치 `claude/new-session-0ldehv`)

| 커밋 | 내용 |
|---|---|
| `d153cd9` ~ `6fddc38` | 1단계: JSON 확장 문법, 함수, 정수/실수/bool/char/str, if/while, 리터럴 변수 추론, JSON 진단, `hana fmt` |
| `5851ce3` ~ `8026196` | 2단계: struct/메서드/이동 검사, enum/match(빠진 경우 검사), 정수 범위 for, 튜플, Option |
| `203d310` | 소멸자 `drop`, `.clone()`, 소유하는 `str`(연결, 비교, len, f-string 값), `--sanitize=address` |
| `7d9542e` | AccessChecker: 쓰는 중인 자리를 바꾸거나 이동하면 오류 (해제 후 사용 버그 두 개를 근본 수정) |
| `a38e771` | `List<T>`: 값, 첨자 자리, 메서드, `for` / `for inout` / `for sink` |
| `54229d6` | 제네릭(`Pair<T>`, `max<T: Ord>`, 제네릭 메서드)과 trait(`is: [...]`, 기본 trait Copy/Clone/Eq/Ord), 단형화 |
| `b5543ee` | 프렐류드의 `Result<T, E>`, `Ok`/`Err`, `?`(Result와 Option), 일찍 반환할 때 만들다 만 값까지 해제 |
| `4aff61b` | struct 분해 `let Pair { first, second } = p` |

테스트: `ctest --test-dir build` 기준 135개가 GCC, ASan/UBSan 빌드, Clang 빌드에서 모두 통과한다.

## 3. 다음에 할 일 (추천 순서)

1. **`panic` 직접 호출** (language.md 9절 [미정]). 추천안: prelude 함수 `panic(message: str)`, 반환 타입은 "돌아오지 않음"으로 보고, 이후 코드가 도달 불가임을 sema가 안다. `main`이 `Result`를 반환하는 것(잠정 동작 80번)도 같이 검토한다.
2. **`match`의 struct 패턴** (잠정 동작 84번): `Point { x: 0, y }`. sema_match와 sema_coverage에 Struct 패턴을 넣는다.
3. **사용자 타입의 `Eq`/`Ord`와 연산자 오버로딩** (결정 34, 잠정 동작 64번).
4. **에러 타입 변환**(`?`의 From, 잠정 동작 77번), `main: fn<Result<...>>`.
5. 이후 큰 항목: 클로저(결정 22), `Map`/`Set`/`Array`(결정 33), 모듈과 `use`(결정 14), 3D 수학 타입(결정 6), C 연동(결정 26).

## 4. 구현의 큰 구조 (새로 손댈 때 알아야 할 것)

- **파이프라인**: OuterParser(JSON 층) → CodeLexer/CodeParser(`'''` 코드) → Sema → MoveChecker → AccessChecker → CodeGenerator(LLVM). Sema에 오류가 있으면 두 검사기는 돌지 않는다.
- **프렐류드** (`src/lang/prelude.cpp`): 모든 프로그램 앞에 붙는 HANA 소스다. 위치에는 `in_prelude` 표시가 붙고, 진단은 `<prelude>:줄`로 나온다. 표준 타입을 HANA로 추가할 때는 여기에 쓴다.
- **소유권 codegen** (`codegen_memory.cpp`): 변수는 i1 drop flag를 갖고, 임시 값은 full expression마다 해제한다. `Use::Read`/`Use::Consume` 구분이 핵심이다. 만들다 만 값은 `hold_pending`/`release_pending`으로 잡아 두어 `?`의 일찍 반환에도 해제된다.
- **AccessChecker** (`access_checker.cpp`): 포인터로 넘긴 인자, `+`의 왼쪽, 첨자 대상, `match` 바인딩, 반복 중인 리스트를 "쓰는 중"으로 잡고, 겹치는 변경과 이동을 막는다. 새 문법이 값을 포인터나 얕은 복사로 오래 쥐면 여기에 규칙을 추가해야 한다.
- **제네릭** (`sema_generic.cpp`): 제네릭 본문은 Param 타입으로 한 번 검사한다. 인스턴스는 `ast_clone`으로 함수를 복사해 구체 타입으로 다시 검사하고, `Program::instances`에 모은다. codegen은 `has_code()`인 함수만 만든다.
- **리스트** (`codegen_list.cpp`): `{ptr, len, cap}`이다. `for sink`는 `OwnedSlot::start`로, 아직 꺼내지 않은 원소만 해제한다.
- 파일 구성은 `CLAUDE.md`의 디렉터리 구조를 따른다.

## 5. 검증하는 법

```
cmake -S . -B build -G Ninja -DLLVM_DIR=/usr/lib/llvm-18/lib/cmake/llvm
cmake --build build && ctest --test-dir build -j8

# compiler itself under ASan/UBSan
cmake -S . -B build-asan -G Ninja -DLLVM_DIR=/usr/lib/llvm-18/lib/cmake/llvm \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=undefined"
cmake --build build-asan && ctest --test-dir build-asan -j8

# fuzzers (run from the repository root)
python3 tools/fuzz/mutate_frontend.py build-asan/hana /tmp/fz1 1      # front-end crashes
python3 tools/fuzz/gen_ownership.py build/hana /tmp/fz2 1 200          # leaks / double frees
python3 tools/fuzz/gen_try.py build/hana /tmp/fz3 1 200                # '?' early-return cleanup
```

- 새 기능에는 `tests/e2e/run`(출력), `tests/e2e/errors`(진단), `tests/unit`을 함께 추가한다. `e2e/run` 프로그램은 자동으로 `memory.*`(ASan)와 `format.*` 테스트도 거친다.
- 메모리를 다루는 기능은 `hana run --sanitize=address`로 직접 돌려 본다. 퍼저가 실제로 버그를 잡는지는, 일부러 해제 코드를 빼서 한 번 확인해 두면 좋다.

## 6. 알아 둘 것

- 클라우드 세션에서는 GitHub 연결 문제로 푸시가 막혀, git 번들로 옮겼다. 로컬에서는 평소처럼 `git push`를 하면 된다.
- 사용자의 Windows 작업 폴더는 `D:\Workspace\HANA-push`다 (GitHub 브랜치와 연결된 clone).
