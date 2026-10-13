#!/bin/sh
# Se ejecuta desde el slot B tras montar la SD: respeta la opción de ADB, fija los LED, levanta Wi-Fi/Bluetooth en segundo plano y lanza el reproductor.
# Si el reproductor termina por una señal fatal (código >= 128) lo relanza hasta 5 veces; el archivo culpable queda en cache/bad.txt.
[ "$(cat /usr/data/r1_adb 2>/dev/null)" = "0" ] && { /etc/init.d/adb/S440adb stop; /data/mnt/r1os/usb_disk.sh prepare; }   # sin ADB: modo disco listo
# LED: rojo mientras carga, azul con la batería llena (los maneja el kernel)
echo axp_battery-charging > /sys/class/leds/red/trigger
echo axp_battery-full > /sys/class/leds/blue/trigger
setsid /data/mnt/r1os/bt/bt_start.sh > /tmp/bt_start.log 2>&1 < /dev/null &
for i in 1 2 3 4 5; do
	/data/mnt/r1os/r1 >> /tmp/r1.log 2>&1
	rc=$?
	[ $rc -lt 128 ] && break
	echo "r1 terminó con código $rc, se relanza" >> /tmp/r1.log
	sleep 1
done
