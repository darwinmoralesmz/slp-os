#!/bin/sh
# Compila y despliega a la SD del R1 (slot B con ADB activo) y reinicia el equipo.
# SD: partición 1 = música (music/) y el lanzador boot.sh; sistema (r1, boot.sh, bt, fuentes, caché) en la partición 2 montada en
# /data/mnt/r1os, o en sd_0/r1os si la tarjeta tiene una sola partición (boot.sh crea el enlace).
set -e
cd "$(dirname "$0")/.."
make -C ui r1
D=/usr/data/mnt/sd_0 O=/usr/data/mnt/r1os
adb push sd/boot.sh $D/
adb shell "sh $D/boot.sh montar; mkdir -p $O/bt $O/web"
adb push ui/r1 sd/r1os/boot.sh sd/r1os/usb_disk.sh $O/
adb push sd/r1os/bt/*.sh $O/bt/
adb push sd/r1os/web/index.html $O/web/
# Fuentes de respaldo (cirílico, griego, CJK, símbolos, emoji): solo la primera vez. El emoji monocromo lo baja scripts/setup.sh.
for f in noto/NotoSans-Medium.ttf noto-cjk/NotoSansCJK-Medium.ttc noto/NotoSansSymbols-Medium.ttf noto/NotoSansSymbols2-Regular.ttf noto/NotoSansMath-Regular.ttf noto/NotoEmoji.ttf; do
	src=/usr/share/fonts/$f; [ -f "$src" ] || src=tools/fonts/${f##*/}
	[ "$(adb shell "[ -f $O/fonts/$f ] && echo si" | tr -d "\r")" = si ] || { adb shell "mkdir -p $O/fonts/${f%/*}"; adb push "$src" "$O/fonts/$f"; }
done
[ "$(md5sum < ui/r1 | cut -d' ' -f1)" = "$(adb shell md5sum $O/r1 | cut -d' ' -f1)" ] || { echo "r1 en la SD no coincide" >&2; exit 1; }
# Reinicia: un proceso lanzado por ADB muere al cerrar la sesión (PROBLEMAS #21); boot.sh lo lanza al arrancar.
adb shell "chmod +x $D/boot.sh $O/boot.sh $O/usb_disk.sh $O/r1 $O/bt/*.sh; sync; reboot"
echo "desplegado; reiniciando"
