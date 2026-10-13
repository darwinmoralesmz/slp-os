#!/bin/sh
# Simulador: empareja y conecta MAC.
D=/tmp/r1sim
sleep 1
sed -i "s/^$1|0|/$1|1|/" $D/bt_devices
echo "$1" > $D/bt_link
echo "$1" > $D/bt_last
