#!/bin/sh
# Lanzador fijo en la raíz de la partición de música (el slot B lo busca ahí). El sistema vive en la 2.ª partición, montada en
# /data/mnt/r1os; si la tarjeta tiene una sola, en la carpeta r1os/ de esta.
R=/data/mnt/r1os
if [ -b /dev/mmcblk0p2 ]; then
	[ -L $R ] && rm $R
	mkdir -p $R
	grep -q " $R " /proc/mounts || mount -t vfat -o iocharset=utf8 /dev/mmcblk0p2 $R || mount -t exfat /dev/mmcblk0p2 $R
else
	[ -d $R ] && [ ! -L $R ] && rmdir $R
	ln -sfn /data/mnt/sd_0/r1os $R
fi
[ "$1" = montar ] && exit 0   # deploy.sh: solo preparar /data/mnt/r1os
exec $R/boot.sh
