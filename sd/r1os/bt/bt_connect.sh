#!/bin/sh
# Empareja (si hace falta), confía y conecta MAC; espera a que bluealsa exponga el PCM.
# Salida: 0 conectado, 1 falló la conexión, 2 no se encontró el dispositivo (no está en modo emparejamiento ni al alcance).
. /data/mnt/r1os/bt/bt_env.sh
M=$1
U=$(echo "$M" | tr ':' '_')
paired() { $T 4 bluetoothctl info "$M" 2>/dev/null | grep -q "Paired: yes"; }
# Busca hasta ver el dispositivo (la búsqueda borra al empezar los que no estén a la vista; reaparecen a los pocos segundos).
find_dev() {
	( echo "scan on"; sleep 20; echo "scan off"; echo quit ) | $T 25 bluetoothctl >/dev/null 2>&1 &
	S=$!
	sleep 2
	i=0
	while [ $i -lt 18 ]; do
		$T 4 bluetoothctl info "$M" 2>/dev/null | grep -q "RSSI:" && break
		sleep 1; i=$((i + 1))
	done
	$T 4 bluetoothctl scan off >/dev/null 2>&1
	kill $S 2>/dev/null
	[ $i -lt 18 ]
}
pair_dev() {
	find_dev || return 2
	$T 20 bluetoothctl pair "$M" >/dev/null 2>&1
	sleep 1
	paired || return 1
}
connect_dev() {
	for t in 1 2 3; do
		$T 15 bluetoothctl connect "$M" 2>/dev/null | grep -q "Connection successful" && return 0
		sleep 2
	done
	return 1
}
if ! paired; then pair_dev || exit $?; fi
$T 4 bluetoothctl trust "$M" >/dev/null 2>&1
# Emparejado pero rechaza la conexión: la clave pudo cambiar (p. ej. se emparejó desde el HiBy original). Se rehace si está en modo emparejamiento.
if ! connect_dev; then
	find_dev || exit 1
	$T 6 bluetoothctl remove "$M" >/dev/null 2>&1
	pair_dev || exit $?
	$T 4 bluetoothctl trust "$M" >/dev/null 2>&1
	connect_dev || exit 1
fi
for i in 1 2 3 4 5 6 7 8 9 10; do
	L=$($T 4 bluealsa-cli list-pcms 2>/dev/null) || { [ $? = 137 ] && /data/mnt/r1os/bt/bt_heal.sh; }
	if echo "$L" | grep -q "$U"; then
		echo "$M" > /usr/data/bt_last
		# Un solo dispositivo de audio a la vez: dos enlaces A2DP se reparten la radio y el audio se corta.
		for o in $(hcitool con | sed -n 's/.*ACL \([0-9A-F:]*\).*/\1/p'); do [ "$o" != "$M" ] && $T 8 bluetoothctl disconnect "$o" >/dev/null 2>&1; done
		exit 0
	fi
	sleep 1
done
exit 1
