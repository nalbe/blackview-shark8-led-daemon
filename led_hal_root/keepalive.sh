#!/system/bin/sh
# led_hal_root v7: keepalive - supervise chgd, single instance guaranteed
MODDIR=/data/adb/modules/led_hal_root

# reject if ANY other keepalive instance lives (pidfiles go stale)
for d in /proc/[0-9]*; do
    [ "$d" = "/proc/$$" ] && continue
    if grep -q "led_hal_root/keepalive.sh" "$d/cmdline" 2>/dev/null; then
        exit 0
    fi
done
echo $$ > /data/local/tmp/led_keepalive.pid

while true; do
    sleep 30
    pidof chgd >/dev/null 2>&1 || \
        /data/adb/ksu/bin/busybox setsid $MODDIR/chgd >/data/local/tmp/chgd.err 2>&1 &
done
