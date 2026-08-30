@echo off
rem led_hal_root apply script: build, push module files, restart the stack.
rem Requires: adb reachable (edit ADB below if needed), adbd root, KernelSU.
setlocal

set ADB=D:\System\Apps\adb\adb.exe
set MOD=/data/adb/modules/led_hal_root
set SRC=%~dp0led_hal_root

echo Rebuilding chgd from sources...
call "%SRC%\build.cmd"
if errorlevel 1 goto fail

echo Pushing module files...
%ADB% push "%SRC%\chgd" %MOD%/chgd
if errorlevel 1 goto fail
rem Core sources live in the module root; the mods\*.c sources live in mods\
rem (and are pushed together with the mods\ directory). No .c is expected in
rem the module root - the old references to tele.c/notify.c/charge.c up here
rem were wrong and just relied on stale copies left on the device.
%ADB% push "%SRC%\chgd.h" %MOD%/chgd.h
%ADB% push "%SRC%\core.c" %MOD%/core.c
%ADB% push "%SRC%\led.c" %MOD%/led.c
%ADB% push "%SRC%\config.c" %MOD%/config.c
%ADB% push "%SRC%\util.c" %MOD%/util.c
rem Wipe the old mods\ tree first so `push mods` cannot layer stale files
rem (past runs left conf.c and a nested mods\ inside it) - then push fresh.
%ADB% shell "rm -rf %MOD%/mods"
%ADB% push "%SRC%\mods" %MOD%/mods
%ADB% push "%SRC%\led.conf" %MOD%/led.conf
%ADB% push "%SRC%\service.sh" %MOD%/service.sh
%ADB% push "%SRC%\keepalive.sh" %MOD%/keepalive.sh
%ADB% push "%SRC%\module.prop" %MOD%/module.prop

rem Drop stale per-file copies that were wrongly pushed to the module root in
rem past releases; nothing at runtime reads them (chgd is deployed as the
rem prebuilt binary; only "mods\" is the live source tree on device).
%ADB% shell "rm -f %MOD%/tele.c %MOD%/notify.c %MOD%/charge.c %MOD%/ring.c %MOD%/dialer.c"

echo Restarting stack...
%ADB% shell "kill -9 $(pidof chgd) $(pidof keepalive.sh) 2>/dev/null; sleep 1; rm -f /data/local/tmp/ledd.log; chmod 755 %MOD%/chgd %MOD%/service.sh %MOD%/keepalive.sh; setsid sh %MOD%/service.sh"
echo Done. Check: %ADB% shell tail -n 5 /data/local/tmp/ledd.log
exit /b 0

:fail
echo BUILD OR PUSH FAILED - is the device connected?
exit /b 1