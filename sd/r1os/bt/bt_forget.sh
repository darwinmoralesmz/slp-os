#!/bin/sh
# Olvida un dispositivo (borra el emparejamiento); si era el último usado, deja de reconectarlo.
. /data/mnt/r1os/bt/bt_env.sh
M=$1
$T 8 bluetoothctl remove "$M" >/dev/null 2>&1
[ "$(cat /usr/data/bt_last 2>/dev/null)" = "$M" ] && rm -f /usr/data/bt_last
exit 0
