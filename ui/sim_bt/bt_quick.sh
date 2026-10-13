#!/bin/sh
# Simulador: reconecta el último dispositivo.
M=$(cat /tmp/r1sim/bt_last 2>/dev/null) || exit 1
echo "$M" > /tmp/r1sim/bt_link
