#!/bin/sh
# Enciende o apaga la radio Bluetooth: bt_power.sh on|off. Encendida queda conectable y no visible; apagada no se reconecta ni se ve.
. /data/mnt/r1os/bt/bt_env.sh
if [ "$1" = off ]; then
	$T 6 bluetoothctl disconnect >/dev/null 2>&1
	$T 6 bluetoothctl power off >/dev/null 2>&1
else
	$T 6 bluetoothctl power on >/dev/null 2>&1
	$T 6 bluetoothctl pairable on >/dev/null 2>&1
	$T 6 bluetoothctl discoverable off >/dev/null 2>&1
	hciconfig hci0 pscan
fi
exit 0
