#!/bin/sh
. /data/mnt/r1os/bt/bt_env.sh
$T 8 bluetoothctl disconnect "$1" >/dev/null 2>&1
