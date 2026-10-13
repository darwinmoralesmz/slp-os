#!/bin/sh
# Modo disco USB (con ADB apagado). prepare: el R1 se presenta como "HiBy R1" con la unidad vacía. export: entrega la
# partición de música (mmcblk0p1) al PC; exige que el sistema esté en la 2.ª partición. release: la recupera (falla si el
# PC no la expulsó y sigue conectado). adb: quita el modo disco y vuelve a ADB. state: estado del USB y unidad entregada.
G=/sys/kernel/config/usb_gadget/r1disk
L=$G/functions/mass_storage.0/lun.0
P=/dev/mmcblk0p1
M=/data/mnt/sd_0
UDC=$(ls /sys/class/udc | head -1)
case "$1" in
prepare)
	grep -q " /sys/kernel/config " /proc/mounts || mount -t configfs none /sys/kernel/config
	if [ ! -d $G ]; then
		mkdir $G && cd $G || exit 1
		echo 0x32BB > idVendor; echo 0x0004 > idProduct; echo 0x0200 > bcdUSB; echo 0x0100 > bcdDevice
		mkdir strings/0x409
		echo HiBy > strings/0x409/manufacturer; echo R1 > strings/0x409/product; echo R1OS > strings/0x409/serialnumber
		mkdir configs/c.1 configs/c.1/strings/0x409
		echo 500 > configs/c.1/MaxPower; echo storage > configs/c.1/strings/0x409/configuration
		mkdir functions/mass_storage.0
		echo 1 > $L/removable; echo 0 > $L/ro; echo 0 > $L/cdrom
		ln -s functions/mass_storage.0 configs/c.1/
	fi
	grep -q . $G/UDC || echo $UDC > $G/UDC
	;;
export)
	[ -b /dev/mmcblk0p2 ] || exit 2
	sync
	umount $M || exit 1
	echo $P > $L/file || { mount -t vfat -o iocharset=utf8 $P $M; exit 1; }
	;;
release)
	[ -z "$(cat $L/file 2>/dev/null)" ] || echo "" > $L/file 2>/dev/null || exit 1
	grep -q " $M " /proc/mounts || mount -t vfat -o iocharset=utf8 $P $M || mount -t exfat $P $M
	;;
adb)
	if [ -d $G ]; then
		echo "" > $G/UDC
		rm $G/configs/c.1/mass_storage.0
		rmdir $G/configs/c.1/strings/0x409 $G/configs/c.1 $G/functions/mass_storage.0 $G/strings/0x409 $G
	fi
	umount /sys/kernel/config 2>/dev/null   # S440adb start no hace nada si configfs sigue montado
	/etc/init.d/adb/S440adb start
	;;
state)
	echo "$(cat /sys/class/udc/$UDC/state) $(cat $L/file 2>/dev/null)"
	;;
esac
