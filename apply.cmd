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
%ADB% push "%SRC%\chgd.c" %MOD%/chgd.c
%ADB% push "%SRC%\chgd.h" %MOD%/chgd.h
%ADB% push "%SRC%\util.c" %MOD%/util.c
%ADB% push "%SRC%\tele.c" %MOD%/tele.c
%ADB% push "%SRC%\notify.c" %MOD%/notify.c
%ADB% push "%SRC%\charge.c" %MOD%/charge.c
%ADB% push "%SRC%\mods" %MOD%/mods
%ADB% push "%SRC%\led.conf" %MOD%/led.conf
%ADB% push "%SRC%\service.sh" %MOD%/service.sh
%ADB% push "%SRC%\keepalive.sh" %MOD%/keepalive.sh
%ADB% push "%SRC%\module.prop" %MOD%/module.prop

echo Restarting stack...
%ADB% shell "kill -9 $(pidof chgd) $(pidof keepalive.sh) 2>/dev/null; sleep 1; rm -f /data/local/tmp/ledd.log; chmod 755 %MOD%/chgd %MOD%/service.sh %MOD%/keepalive.sh; setsid sh %MOD%/service.sh"
echo Done. Check: %ADB% shell tail -n 5 /data/local/tmp/ledd.log
exit /b 0

:fail
echo BUILD OR PUSH FAILED - is the device connected?
exit /b 1