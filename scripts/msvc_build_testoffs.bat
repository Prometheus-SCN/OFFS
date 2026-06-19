@echo off
setlocal EnableExtensions
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 (
  echo vcvars64 failed
  exit /b 1
)
set "SRC=%~dp0.."
set "BUILD=%SRC%\cmake-build-msvc"
set "TEST_EXE=%BUILD%\test\testoffs.exe"

REM Configure (picks up submodule / source changes).
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  "-DCMAKE_TOOLCHAIN_FILE=C:\Users\victor morrow\vcpkg\scripts\buildsystems\vcpkg.cmake" ^
  "-DVCPKG_TARGET_TRIPLET=x64-windows-static-md" ^
  -S "%SRC%" -B "%BUILD%"
if errorlevel 1 exit /b %ERRORLEVEL%

REM Force-rebuild poll-dancer if its source files were edited (liboffs builds it
REM via ExternalProject whose CONFIGURE only re-runs when liboffs' CMakeLists
REM changes; source edits inside deps/poll-dancer are not always picked up).
pushd "%BUILD%\deps\poll-dancer" || exit /b 1
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cmake --build . --target poll_dancer --config Release
set "PD_RC=%ERRORLEVEL%"
popd
if not "%PD_RC%"=="0" exit /b %PD_RC%

REM Build the test executable against the freshly-rebuilt poll_dancer.lib.
cmake --build "%BUILD%" --target testoffs -j 4
if errorlevel 1 exit /b %ERRORLEVEL%

if not exist "%TEST_EXE%" (
  echo testoffs.exe not found at %TEST_EXE%
  exit /b 1
)

echo.
echo === Running testoffs ===
"%TEST_EXE%"
set "RC=%ERRORLEVEL%"

if not "%RC%"=="0" (
  echo.
  echo *** testoffs FAILED with code %RC%
  exit /b %RC%
)

echo.
echo === testoffs PASSED ===
exit /b 0