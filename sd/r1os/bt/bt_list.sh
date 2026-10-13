#!/bin/sh
# Lista dispositivos de audio conocidos: mac|emparejado|conectado|nombre
. /data/mnt/r1os/bt/bt_env.sh
$T 4 bluetoothctl devices 2>/dev/null | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g' | grep '^Device ' | while read -r _ mac name; do
	info=$($T 4 bluetoothctl info "$mac" 2>/dev/null | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g')
	p=0; c=0
	echo "$info" | grep -q "Paired: yes" && p=1
	echo "$info" | grep -q "Connected: yes" && c=1
	# solo dispositivos de audio (o ya emparejados)
	echo "$info" | grep -q -E "Icon: audio|Audio Sink|Handsfree|Headset" || [ "$p" = 1 ] || continue
	echo "$mac|$p|$c|$name"
done
