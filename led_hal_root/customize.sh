#!/system/bin/sh
# led_hal_root v2.10 installer hook (KernelSU / Magisk compatible)

ui_print "- ==============================="
ui_print "- Notification LED daemon v2.10"
ui_print "- AW2033 breathing LED, Shark8"
ui_print "- ==============================="

ui_print "- Setting permissions..."
set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/service.sh"   0 0 0755
set_perm "$MODPATH/chgd"         0 0 0755
set_perm "$MODPATH/keepalive.sh" 0 0 0755

# clean stale runtime state from previous installs (no-op, harmless)
rm -f /data/local/tmp/led_chg /data/local/tmp/led_chg.tmp
rm -f /data/local/tmp/led_status /data/local/tmp/led_status.tmp
rm -f /data/local/tmp/led_chgd.lock

ui_print "- Colors: RGB-configurable in led.conf"
ui_print "- Defaults: WhatsApp=green, Telegram=magenta,"
ui_print "-          other=default_color in led.conf"
ui_print "- Charge defaults: lower=breath red <90%,"
ui_print "-         middle=breath lime 90-94%,"
ui_print "-         upper=static green >=95%"
ui_print "- Per-range colors/types editable in led.conf"
ui_print "- Modular: drop a .c file into mods/"
ui_print "- and run build.cmd to add a color/handler."
ui_print "- Done! Reboot to activate."