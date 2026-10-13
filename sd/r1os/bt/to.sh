#!/bin/sh
# to.sh N orden...: ejecuta la orden y la mata a los N s (busybox no trae timeout). Si se agotó el tiempo sale con 137.
n=$1; shift
exec 3<&0   # en segundo plano la entrada pasaría a /dev/null; las búsquedas le escriben órdenes a bluetoothctl por ella
"$@" 0<&3 & p=$!
( trap 'kill $s 2>/dev/null; exit 0' TERM; sleep "$n" & s=$!; wait $s && kill -9 $p 2>/dev/null ) & w=$!
wait $p; r=$?
kill $w 2>/dev/null
exit $r
