# HANA

GUI를 선언하고 기능(로직)을 정의하는 언어. 하나의 소스로 가능한 모든 환경(데스크톱, 웹, 모바일)에서 동작하며 2D UI와 3D를 함께 지원한다.

- 프로젝트/언어 공식 표기: `HANA` (문서, 로고, 제목, 창 제목 등 사람이 읽는 곳은 항상 대문자)
- 코드 식별자는 소문자 유지: 확장자 `.hana`, CLI `hana run` / `hana build` / `hana check`, 네임스페이스 `hana::`, CMake 타깃 `hana`
- 매크로/상수 접두사는 대문자: `HANA_VERSION`, `HANA_API` 등
- 저장소 이름: `hana-lang` (C++ 생태계의 Boost.Hana와 검색 혼동을 피하기 위함)
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

## 문법 초안 (확정 전, 이 기준으로 Lexer부터 시작)

```
app Counter {
    state count: int = 0

    action increment() {
        count = count + 1
    }

    action reset() {
        count = 0
    }

    window title: "Counter" size: 300x200 {
        column spacing: 8 {
            label text: "Count: {count}"
            row {
                button text: "+1" on_click: increment
                button text: "Reset" on_click: reset enabled: count > 0
            }
        }
    }
}
```

3D 통합 예시:

```
window title: "Viewer" size: 1280x720 {
    row {
        viewport3d id: view camera: main_cam {
            scene: model_scene
        }
        column width: 300 {
            slider label: "Rotation" value: rot min: 0 max: 360
            button text: "Reset" on_click: reset_view
        }
    }
}

scene model_scene {
    camera main_cam position: (0, 2, 5) target: (0, 0, 0) fov: 60
    light sun type: directional direction: (-1, -1, -1) intensity: 3.0

    mesh part source: "part.gltf" rotation: (0, rot, 0) {
        material metallic: 0.8 roughness: 0.3
        on_click: select_part
    }

    panel3d position: (1.5, 1, 0) size: 1x0.6 {
        label text: "Selected: {selected_name}"
    }
}
```

문법 요소:
- 주석: `// 한 줄`, `/* 블록 */` (블록 주석은 중첩 가능) - 확정
- 최상위: `app`, `scene`
- 선언: `state name: type = init`, `action name(params) { ... }`
- 위젯: `kind prop: value ... { children }`
- 문자열 보간: `"Count: {expr}"`
- 표현식 바인딩: `enabled: count > 0` (참조하는 state가 바뀌면 자동 재평가)
- 리터럴: int, float, string, bool, 크기 `300x200`, 튜플 `(x, y, z)`

현재 Lexer 구현 (초안 기준의 잠정 동작, 문법이 바뀌면 함께 바뀜):
- 키워드: `app`, `scene`, `state`, `action`, `true`, `false`. 위젯 종류와 프로퍼티 이름은 일반 식별자
- 줄바꿈은 Newline 토큰으로 전달 (연속 줄바꿈은 하나로 합침)

## 디렉터리 구조

```
hana-lang/
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
- 문법 세부: 타입 목록, 리스트/반복 위젯(for), 조건부 위젯(if), 컴포넌트 재사용 문법
- 상태 모델 세부: 프로퍼티 바인딩 방식 기본, 단방향 흐름 도입 여부
- 2D 렌더러를 직접 구현할지, Skia Graphite(Dawn 위)를 쓸지
