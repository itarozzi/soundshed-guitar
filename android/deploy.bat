@echo off
setlocal EnableExtensions
rem ---------------------------------------------------------------------------
rem Installs the APK on the first connected phone or tablet and launches it,
rem optionally building it first. Emulators are skipped: they have no
rem low-latency audio path, so nothing about tone or latency can be judged on
rem one (see docs/android-build.md).
rem
rem   deploy.bat                 install the release APK - the one to use for
rem                              anything audio
rem   deploy.bat debug           install the debug APK
rem   deploy.bat build           build the release APK, then install it
rem   deploy.bat debug build     build the debug APK, then install it
rem   deploy.bat build nolaunch  build and install, but do not start the app
rem   deploy.bat build "abi=arm64-v8a,x86_64"   quote a list - cmd splits on commas
rem
rem Arguments may appear in any order. Without "build", build it yourself first:
rem   gradlew assembleRelease -Pssg.abis=arm64-v8a
rem ---------------------------------------------------------------------------

set "HERE=%~dp0"
rem Captured before the parse loop: shift moves %0 along with the rest.
set "SELF=%~nx0"
set "VARIANT="
set "REBUILD="
set "NOLAUNCH="
set "ABIS="

:parse
if "%~1"=="" goto parsed
if /i "%~1"=="release" (
    set "VARIANT=release"
    shift
    goto parse
)
if /i "%~1"=="debug" (
    set "VARIANT=debug"
    shift
    goto parse
)
if /i "%~1"=="build" (
    set "REBUILD=1"
    shift
    goto parse
)
if /i "%~1"=="rebuild" (
    set "REBUILD=1"
    shift
    goto parse
)
if /i "%~1"=="nolaunch" (
    set "NOLAUNCH=1"
    shift
    goto parse
)
if /i "%~1"=="abi" (
    set "ABIS=%~2"
    shift
    shift
    goto parse
)
set "ARG=%~1"
if /i "%ARG:~0,4%"=="abi=" (
    set "ABIS=%ARG:~4%"
    shift
    goto parse
)
echo Unrecognised argument: %~1
echo Usage: %SELF% [release^|debug] [build] [nolaunch] [abi=^<abi^>[,^<abi^>...]]
exit /b 2

:parsed
if not defined VARIANT set "VARIANT=release"
if /i "%VARIANT%"=="release" (set "TASK=assembleRelease") else (set "TASK=assembleDebug")
set "APK=%HERE%app\build\outputs\apk\%VARIANT%\app-%VARIANT%.apk"

rem --- adb: sdk.dir from local.properties, then ANDROID_HOME / ANDROID_SDK_ROOT,
rem --- then the default SDK location.
set "SDK="
if exist "%HERE%local.properties" (
    for /f "usebackq eol=# tokens=1,* delims==" %%a in ("%HERE%local.properties") do (
        if /i "%%a"=="sdk.dir" set "SDK=%%b"
    )
)
if not defined SDK if defined ANDROID_HOME set "SDK=%ANDROID_HOME%"
if not defined SDK if defined ANDROID_SDK_ROOT set "SDK=%ANDROID_SDK_ROOT%"
if not defined SDK set "SDK=%LOCALAPPDATA%\Android\Sdk"
rem Android Studio writes the path Java-properties style: C\:\\Users\\...
set "SDK=%SDK:\:=:%"
set "SDK=%SDK:\\=\%"
set "SDK=%SDK:/=\%"
set "ADB=%SDK%\platform-tools\adb.exe"
if not exist "%ADB%" (
    echo adb not found at %ADB%
    echo Set sdk.dir in android\local.properties, or ANDROID_HOME.
    exit /b 1
)

rem --- first device in the "device" state whose serial is not an emulator's
set "SERIAL="
for /f "skip=1 tokens=1,2" %%a in ('call "%ADB%" devices') do call :consider "%%a" "%%b"
if not defined SERIAL (
    echo No connected device in the "device" state ^(emulators are skipped^).
    echo Plug the phone in, unlock it, and accept the USB debugging prompt.
    "%ADB%" devices -l
    exit /b 1
)

if defined REBUILD call :build || exit /b 1

if not exist "%APK%" (
    echo No %VARIANT% APK at:
    echo   %APK%
    echo Build it here:  %SELF% %VARIANT% build
    echo Or by hand:     gradlew %TASK% -Pssg.abis=arm64-v8a
    exit /b 1
)

echo Installing %VARIANT% APK on %SERIAL% ...
"%ADB%" -s %SERIAL% install -r "%APK%" || exit /b 1

if defined NOLAUNCH exit /b 0

"%ADB%" -s %SERIAL% shell am start -n com.soundshed.guitar/.MainActivity >nul || exit /b 1
echo Launched. Follow the log with:
echo   "%ADB%" -s %SERIAL% logcat -s SoundshedGuitar JUCE
exit /b 0

rem --- Builds the APK for the ABI the connected device actually runs, so the
rem --- packaged libjuce_jni.so matches it.
:build
if not defined ABIS (
    rem exec-out, not shell: shell runs the command through a pty, and the CR it
    rem adds to the line would end up inside %ABIS% and reach gradle.
    for /f "tokens=1" %%a in ('call "%ADB%" -s %SERIAL% exec-out getprop ro.product.cpu.abi 2^>nul') do set "ABIS=%%a"
)
if not defined ABIS set "ABIS=arm64-v8a"
echo Building %VARIANT% APK for %ABIS% ...
pushd "%HERE%" || exit /b 1
rem By full path: the current directory is not always searched for commands.
call "%HERE%gradlew.bat" %TASK% -Pssg.abis=%ABIS%
if errorlevel 1 (
    popd
    exit /b 1
)
popd
exit /b 0

:consider
if defined SERIAL goto :eof
if not "%~2"=="device" goto :eof
set "CANDIDATE=%~1"
if /i "%CANDIDATE:~0,9%"=="emulator-" goto :eof
set "SERIAL=%CANDIDATE%"
goto :eof
