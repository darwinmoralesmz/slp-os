#!/bin/sh
# Reinicia bluealsa cuando deja de responder (se cuelga si el enlace cae mientras suena) y reconecta el último dispositivo para que
# BlueZ vuelva a crear el transporte A2DP. Matar los "bluealsa-cli open" libera a la aplicación si estaba bloqueada escribiendo.
. /data/mnt/r1os/bt/bt_env.sh
T=/data/mnt/r1os/bt/to.sh
echo "$(date +%T) bt_heal" >> /tmp/bt_heal.log
killall -9 bluealsa-cli bluealsa 2>/dev/null
sleep 1
pidof bt-agent >/dev/null || bt-agent -c NoInputNoOutput >/dev/null 2>&1 &
bluealsa -p a2dp-source $BA_OPTS > /tmp/bluealsa.log 2>&1 &
sleep 2
M=$(cat /usr/data/bt_last 2>/dev/null) || exit 0
grep -q '^bton=0' /usr/data/r1_state.txt 2>/dev/null && exit 0
$T 8 bluetoothctl disconnect "$M" >/dev/null 2>&1
sleep 2
$T 15 bluetoothctl connect "$M" >/dev/null 2>&1
exit 0
