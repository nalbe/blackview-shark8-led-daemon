@echo off
rem ============================================================
rem  build.cmd - compile the modular chgd daemon.
rem
rem  Compiles every .c in this directory (core) plus every .c in
rem  mods\ (extensions) into a single static binary. Adding a
rem  feature = dropping a file into mods\ and running this.
rem
rem  Override the compiler with:   set NDK_CC=path\to\clang.cmd
rem ============================================================
setlocal enabledelayedexpansion
if not defined NDK_CC set "NDK_CC=D:\System\Apps\Android NDK\android-ndk-r27d\toolchains\llvm\prebuilt\windows-x86_64\bin\aarch64-linux-android29-clang.cmd"

set SRC=
for %%f in ("%~dp0*.c") do set "SRC=!SRC! "%%f""
for %%f in ("%~dp0mods\*.c") do set "SRC=!SRC! "%%f""
if not defined SRC (
    echo ERROR: no C sources found in %~dp0 or %~dp0mods
    exit /b 1
)

echo Compiling: %SRC%
call "%NDK_CC%" -O2 -s -Wall -Wno-comment -o "%~dp0chgd" %SRC%
if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)
echo BUILT: %~dp0chgd