@echo off
rem ============================================================
rem  build.cmd - compile the modular chgd daemon.
rem
rem  Compiles every .c in this directory (core) plus every .c in
rem  mods\ (extensions) into a single static binary. Adding a
rem  feature = dropping a file into mods\ and running this.
rem
rem  Override the compiler with:
rem     set NDK_CC=path\to\aarch64-linux-android29-clang.cmd
rem  Or set NDK to the Android NDK root (auto-resolves to the clang.cmd).
rem ============================================================
setlocal enabledelayedexpansion
if not defined NDK_CC (
    if defined NDK (
        set "NDK_CC=%NDK%\toolchains\llvm\prebuilt\windows-x86_64\bin\aarch64-linux-android29-clang.cmd"
    )
)
if not defined NDK_CC (
    echo ERROR: compiler not found. Set NDK_CC or NDK to your Android NDK.
    exit /b 1
)

set SRC=
for %%f in ("%~dp0*.c") do set "SRC=!SRC! "%%f""
for %%f in ("%~dp0mods\*.c") do set "SRC=!SRC! "%%f""
if not defined SRC (
    echo ERROR: no C sources found in %~dp0 or %~dp0mods
    exit /b 1
)

echo Compiling: %SRC%
"%NDK_CC%" -O2 -s -Wall -Wno-comment -o "%~dp0chgd" %SRC%
if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)
echo BUILT: %~dp0chgd