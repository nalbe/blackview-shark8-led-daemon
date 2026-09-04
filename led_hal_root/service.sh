#!/system/bin/sh
# led_hal_root v5: launch single LED daemon (chgd v2)
MODDIR=${0%/*}
LOG=/data/local/tmp/ledfix.log

echo "$(date) module service.sh started" >> $LOG

# give logd a moment at boot before we subscribe to its socket
sleep 8

/data/adb/ksu/bin/busybox setsid $MODDIR/chgd >/data/local/tmp/chgd.err 2>&1 &
setsid sh $MODDIR/keepalive.sh >/dev/null 2>&1 &
echo "$(date) service.sh: chgd v2 + keepalive launched" >> $LOG
