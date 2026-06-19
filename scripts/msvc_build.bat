@echo off
setlocal EnableExtensions
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 (
  echo vcvars64 failed
  exit /b 1
)
set "SRC=%~dp0.."
if defined BUILD_DIR (
  set "BUILD=%BUILD_DIR%"
) else (
  set "BUILD=%SRC%\cmake-build-msvc"
)
cmake --build "%BUILD%" -j 4
exit /b %ERRORLEVEL%