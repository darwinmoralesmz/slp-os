#!/bin/sh
# Busca dispositivos durante 10 s.
. /data/mnt/r1os/bt/bt_env.sh
( echo "scan on"; sleep 10; echo "scan off"; echo quit ) | $T 16 bluetoothctl >/dev/null 2>&1
