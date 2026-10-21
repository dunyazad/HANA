@echo off
rem Builds HANA in Debug and Release, then runs the Release hana with the
rem given arguments (for example: run.bat run examples\fib.hana).
rem LLVM_DIR may point at another LLVM 18 installation's lib\cmake\llvm.
setlocal
set "ROOT=%~dp0"
if not defined LLVM_DIR set "LLVM_DIR=D:\Tools\clang+llvm-18.1.8-x86_64-pc-windows-msvc\lib\cmake\llvm"

if not exist "%ROOT%build\CMakeCache.txt" (
    cmake -S "%ROOT%." -B "%ROOT%build" -G "Visual Studio 18 2026" -A x64 -DLLVM_DIR="%LLVM_DIR%"
    if errorlevel 1 exit /b 1
)
cmake --build "%ROOT%build" --config Debug --parallel
if errorlevel 1 exit /b 1
cmake --build "%ROOT%build" --config Release --parallel
if errorlevel 1 exit /b 1

"%ROOT%build\Release\hana.exe" %*
exit /b %errorlevel%
