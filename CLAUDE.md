# HANA

GUI를 선언하고 기능(로직)을 정의하는 언어. 하나의 소스로 가능한 모든 환경(데스크톱, 웹, 모바일)에서 동작하며 2D UI와 3D를 함께 지원한다.

- 프로젝트/언어 공식 표기: `HANA` (문서, 로고, 제목, 창 제목 등 사람이 읽는 곳은 항상 대문자)
- 코드 식별자는 소문자 유지: 확장자 `.hana`, CLI `hana run` / `hana build` / `hana check`, 네임스페이스 `hana::`, CMake 타깃 `hana`
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

## 아키텍처: 4계층

```
.hana 소스
   |
[1. 언어]     hana::lang      Lexer -> Parser/AST -> Analyzer -> IR builder
   |  IR
[2. 런타임]   hana::runtime   Interpreter, State/Binding, Layout/Input, Scene graph
   |  Draw list
[3. 렌더링]   hana::render    2D renderer, 3D renderer, WGSL shaders, WebGPU API
   |
[4. 플랫폼]   hana::platform  Win32, Web(WASM), Android/iOS, Linux/macOS
   |
D3D12 / Vulkan / Metal / Browser WebGPU
```

의존 규칙:
- 언어 계층은 런타임을 모른다. IR만 만든다.
- 런타임은 렌더러 내부(WebGPU)를 모른다. Draw list만 넘긴다.
- 렌더러는 언어의 존재를 모른다.
- 플랫폼 계층은 가장 아래에서 창, 입력, 파일, 메인 루프, 시간을 인터페이스(IWindow, IInput, IFileSystem 등)로 제공한다. 런타임은 입력/파일을 플랫폼에서 직접 받는다.
- 계층 간 전달물은 IR과 Draw list 두 가지뿐이다.

## 확정된 설계 결정

1. 실행 방식: 인터프리터. 엔진(C++)은 미리 빌드하고 `.hana` 파일은 실행 시 파싱한다.
   - 1단계: IR 트리 순회 인터프리터
   - 이후 필요 시 같은 IR에서 바이트코드 VM 추가
   - 핫 리로드 지원: 파일 변경 감지 -> 재파싱 -> state는 이름으로 매칭해 값 유지
   - 배포 형태: 소스 그대로 / 사전 컴파일 바이트코드 / exe 내장 (모두 같은 런타임)
2. 렌더링: 자체 렌더링 방식 (네이티브 위젯 매핑 안 함). 모든 플랫폼에서 동일한 모양.
3. GPU 계층: WebGPU (`webgpu.h`), 네이티브는 Dawn, 웹은 Emscripten(emdawnwebgpu). 셰이더는 WGSL로 통일.
4. 2D와 3D는 같은 GPU 디바이스/큐를 공유한다.
5. 3D 에셋 표준 포맷: glTF 2.0. 머티리얼 기본은 PBR(metallic-roughness).
6. 무거운 계산은 C++ 함수로 만들고 언어에서 호출(FFI)한다.

## 웹 이식을 위해 처음부터 지킬 것

- 메인 루프를 `while`로 직접 돌리지 않는다. "한 프레임 처리 함수"를 분리해서 네이티브는 루프로, 웹은 requestAnimationFrame 콜백으로 호출한다.
- GPU 어댑터/디바이스 초기화는 콜백 기반 비동기 흐름으로 작성한다 (Dawn도 동일 API 사용).
- 파일/에셋 로딩은 플랫폼 계층 인터페이스를 통해서만 한다 (웹은 HTTP fetch 또는 가상 파일 시스템).

## 언어 정의

전체 정의와 각 항목의 상태([확정]/[초안]/[미정])는 `docs/language.md`가 기준이다. 문법을 바꾸면 이 문서를 먼저 고친다. [초안]이나 [미정] 항목을 확정된 것처럼 다루지 않는다.

확정된 사항 요약:
- 주석: `// 한 줄`, `/* 블록 */` (중첩 가능)
- 문장 구분: 줄바꿈 또는 `;`. `( )`, `[ ]` 안이거나 줄이 연산자, `,`, `:`, `.`로 끝나면 다음 줄로 이어진다. 문자열은 한 줄 안에서 끝난다
- 위젯: 블록 속성형 `kind { prop: value; child { ... } }`
- 위젯 이름: `id: name` 프로퍼티
- 이벤트 핸들러: action 참조(`on_click: reset`)와 인라인 블록(`on_click: { count = 0 }`) 모두 허용
- 조건/반복: 위젯 트리 안의 `if cond { } else { }`, `for item in list key: item.id { }` (`key:`는 선택)
- 바인딩: 기본 단방향, 위젯이 state를 바꾸는 양방향은 `value: bind rot`으로 명시
- 색 리터럴: `#rrggbb`, `#rrggbbaa`
- 타입: 정적 타입 + 추론. 기본 타입 `int float bool string size vec2 vec3 color list<T>`
- 재사용 UI: `component` 키워드

## 디렉터리 구조

```
hana/
  CMakeLists.txt
  src/
    lang/        Lexer, Parser, AST, Analyzer, IR
    runtime/     Interpreter, State, Binding, Layout, Scene
    render/      WebGPU 2D/3D renderer, WGSL shaders
    platform/    file_system.h (IFileSystem, 콜백 기반)
      native/    데스크톱 공용 구현 (NativeFileSystem)
      win32/
      web/
      ...
    cli/         hana executable (run, build, check)
  docs/
    language.md  언어 정의
  examples/
    counter.hana
  tests/
```

## 빌드와 테스트

```
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
./build/hana check --tokens examples/counter.hana
```

## 로드맵

1. [진행 중] 언어 계층: Lexer(완료, `hana check --tokens`) -> Parser/AST(다음 작업) -> Analyzer -> IR. `hana check` 로 토큰/AST/IR 덤프 확인
2. 런타임: 트리 순회 인터프리터, 반응형 바인딩(의존성 그래프), 레이아웃
3. 첫 백엔드: Windows + Dawn(WebGPU) 2D 렌더러
4. WASM 빌드 (Emscripten)
5. 3D: 씬 그래프, glTF 로더, PBR, 피킹
6. 모바일 (Android NDK / iOS)
7. C++ 호스트 연동 FFI, 핫 리로드, 바이트코드 VM

## 아직 정하지 않은 것

- 3D 범위: 시각화/뷰어 수준 / 인터랙티브 콘텐츠 수준(애니메이션, 물리, 파티클) / XR(OpenXR, WebXR) 중 택일
- 문법 세부: `docs/language.md`의 미정 항목 참고 (scene/3D 문법은 3D 범위 결정 후)
- 2D 렌더러를 직접 구현할지, Skia Graphite(Dawn 위)를 쓸지
