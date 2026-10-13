#!/bin/sh
# Reintenta conectar el último dispositivo; si la pila está caída la reinicia primero.
. /data/mnt/r1os/bt/bt_env.sh
grep -q '^bton=0' /usr/data/r1_state.txt 2>/dev/null && exit 0
if ! pidof bluetoothd >/dev/null || ! hciconfig hci0 2>/dev/null | grep -q "UP RUNNING"; then
	/data/mnt/r1os/bt/bt_start.sh force
fi
$T 4 bluealsa-cli list-pcms >/dev/null 2>&1; [ $? = 137 ] && { /data/mnt/r1os/bt/bt_heal.sh; exit 0; }   # bluealsa colgado: reiniciarlo ya reconecta
M=$(cat /usr/data/bt_last 2>/dev/null) || exit 1
$T 15 bluetoothctl connect "$M" >/dev/null 2>&1
