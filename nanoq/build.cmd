@echo off
rem Configures (if needed) and builds Soundshed Nano Q with the MSVC environment loaded.
rem   nanoq\build.cmd [Debug|Release|RelWithDebInfo] [target]
rem To force a fresh configure, delete nanoq\build\<config>\build.ninja.
setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release
set TARGET=%2
set VCVARS=
for %%E in (Community Professional Enterprise Insiders BuildTools) do (
  for %%V in (18 2022) do (
    if exist "%ProgramFiles%\Microsoft Visual Studio\%%V\%%E\VC\Auxiliary\Build\vcvars64.bat" if "%VCVARS%"=="" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\%%V\%%E\VC\Auxiliary\Build\vcvars64.bat"
  )
)
if "%VCVARS%"=="" (echo Visual Studio with the C++ workload not found & exit /b 1)
call "%VCVARS%" >nul
set "SRC=%~dp0."
set "BLD=%~dp0build\%CONFIG%"
rem Configure only a new tree: Ninja re-runs CMake itself when a CMakeLists or the source list
rem changes. Configuring every time cost ~40 s and recompiled miniz and the core's archive code.
if not exist "%BLD%\build.ninja" (
  cmake -S "%SRC%" -B "%BLD%" -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% || exit /b 1
)
if "%TARGET%"=="" (cmake --build "%BLD%") else (cmake --build "%BLD%" --target %TARGET%)
