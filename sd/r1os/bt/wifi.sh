#!/bin/sh
# Wi-Fi con las redes de /usr/data/wpa_supplicant.conf: wifi.sh up|renew|down|cli <orden de wpa_cli>.
# up es idempotente: sale con 0 en cuanto wlan0 tiene IP (espera hasta ~20 s), 1 si no se conectó. renew pide IP de nuevo (otra red).
export PATH=/data/mnt/r1os/bin:$PATH LD_LIBRARY_PATH=/data/mnt/r1os/lib
C=/usr/data/wpa_supplicant.conf
ip4() { ifconfig wlan0 2>/dev/null | grep -q "inet addr"; }
case "$1" in
down)
	killall udhcpc wpa_supplicant 2>/dev/null
	ifconfig wlan0 down 2>/dev/null
	exit 0;;
cli) shift; exec wpa_cli -iwlan0 "$@";;
renew) killall udhcpc 2>/dev/null; ifconfig wlan0 0.0.0.0 2>/dev/null;;
esac
ip4 && exit 0
[ -f $C ] || printf 'ctrl_interface=/var/run/wpa_supplicant\nap_scan=1\n' > $C
grep -q '^update_config=1' $C || { { echo update_config=1; cat $C; } > $C.tmp && mv $C.tmp $C; }   # save_config desde la interfaz
[ -d /sys/class/net/wlan0 ] || /data/mnt/r1os/bt/bt_start.sh   # carga cywdhd (Wi-Fi y Bluetooth comparten el driver)
[ -d /sys/class/net/wlan0 ] || exit 1
ifconfig wlan0 up
pidof wpa_supplicant >/dev/null || wpa_supplicant -B -Dnl80211 -iwlan0 -c$C >/dev/null 2>&1
for i in 1 2 3 4 5 6 7 8 9 10; do
	wpa_cli -iwlan0 status 2>/dev/null | grep -q "wpa_state=COMPLETED" && break
	sleep 1
done
wpa_cli -iwlan0 status 2>/dev/null | grep -q "wpa_state=COMPLETED" || exit 1
udhcpc -i wlan0 -n -q -t 5 >/dev/null 2>&1
ip4
