@echo off
rem Runs the last Release build of hana with the given arguments, without
rem building.
setlocal
set "HANA=%~dp0build\Release\hana.exe"
if not exist "%HANA%" (
    echo error: "%HANA%" does not exist. Run run.bat first to build it. 1>&2
    exit /b 1
)
"%HANA%" %*
exit /b %errorlevel%
