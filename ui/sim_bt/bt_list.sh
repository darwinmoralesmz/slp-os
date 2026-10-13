#!/bin/sh
# Simulador: lista los dispositivos de /tmp/r1sim/bt_devices (mac|emparejado|nombre) con su estado de conexión.
D=/tmp/r1sim
[ -f $D/bt_devices ] || printf 'AA:BB:CC:DD:EE:01|1|OnePlus Buds 3 (sim)\n' > $D/bt_devices
L=$(cat $D/bt_link 2>/dev/null)
while IFS='|' read -r mac p name; do
	c=0; [ "$mac" = "$L" ] && c=1
	echo "$mac|$p|$c|$name"
done < $D/bt_devices
