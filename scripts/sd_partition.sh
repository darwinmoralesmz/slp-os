#!/bin/bash
# Reparte la SD del R1 en dos particiones FAT32, desde un lector de tarjetas del PC:
#   1 MUSICA (todo menos 2 GiB): music/, el lanzador boot.sh y los archivos del HiBy original (lo que ven el slot A, USB y el lector)
#   2 R1OS   (2 GiB): el sistema (contenido de r1os/) y backup/ (nandbak, slotB, r1.upt); se monta en /data/mnt/r1os
# Respalda todo en ~/r1_sd_backup/<fecha> (verificado por contenido), particiona, formatea, restaura y vuelve a verificar. Borra la tarjeta: pide confirmar.
# Uso: sudo scripts/sd_partition.sh /dev/sdX
# Retomar tras un corte (ya particionada, sin formatear): sudo scripts/sd_partition.sh /dev/sdX ~/r1_sd_backup/<fecha> (formatea, restaura y verifica).
set -euo pipefail
DEV=${1:-} RESUME=${2:-}
[ "$(id -u)" = 0 ] || { echo "Ejecutar con sudo" >&2; exit 1; }
[ -b "$DEV" ] && [ "$(lsblk -dno TYPE "$DEV")" = disk ] || { echo "Uso: sudo $0 /dev/sdX (disco, no partición)" >&2; exit 1; }
[ "$(lsblk -dno RM "$DEV" | tr -d ' ')" = 1 ] || { echo "$DEV no es extraíble: no se toca" >&2; exit 1; }
SIZE=$(lsblk -bdno SIZE "$DEV")
[ "$SIZE" -gt $((8 << 30)) ] && [ "$SIZE" -lt $((1 << 41)) ] || { echo "Tamaño inesperado ($SIZE B)" >&2; exit 1; }
P1=${DEV}1 P2=${DEV}2
case "$DEV" in *[0-9]) P1=${DEV}p1 P2=${DEV}p2 ;; esac
[ -b "$P1" ] || { echo "No hay $P1: ¿es la tarjeta del R1?" >&2; exit 1; }
unmount_all() { for p in $(lsblk -lno PATH "$DEV" | tail -n +2); do umount "$p" 2>/dev/null || true; udisksctl unmount -b "$p" >/dev/null 2>&1 || true; done; }
if [ -n "$RESUME" ]; then
	[ -b "$P2" ] && [ -d "$RESUME/music" ] && [ -d "$RESUME/r1os" ] && [ -f "$RESUME/boot.sh" ] || { echo "Para retomar: tarjeta con 2 particiones y un respaldo con music/, r1os/ y boot.sh" >&2; exit 1; }
	BK=$RESUME M1=$(mktemp -d) M2=$(mktemp -d) SRC=$(mktemp -d)
	trap 'umount "$M1" "$M2" 2>/dev/null || true; rmdir "$M1" "$M2" "$SRC" 2>/dev/null || true' EXIT
	lsblk -o NAME,SIZE,RM,TRAN,MODEL,LABEL "$DEV"
	read -r -p "Se formatearán $P1 y $P2 y se restaurará $BK. Escribe el nombre del dispositivo ($DEV): " OK
	[ "$OK" = "$DEV" ] || { echo "Cancelado"; exit 1; }
	diffs() { rsync -rt --dry-run --itemize-changes --checksum "$@" | grep -E '^(>f|<f|cd)' || true; }
fi
[ -z "$RESUME" ] && [ -b "$P2" ] && { echo "$DEV ya tiene una 2.ª partición: parece ya repartida (para retomar, pasa la carpeta del respaldo)" >&2; exit 1; }
if [ -z "$RESUME" ]; then

USR=${SUDO_USER:-root}
HOMEDIR=$(getent passwd "$USR" | cut -d: -f6)
BK=$HOMEDIR/r1_sd_backup/$(date +%Y%m%d-%H%M)
SRC=$(mktemp -d) M1=$(mktemp -d) M2=$(mktemp -d)
cleanup() { umount "$SRC" "$M1" "$M2" 2>/dev/null || true; rmdir "$SRC" "$M1" "$M2" 2>/dev/null || true; }
trap cleanup EXIT

for p in $(lsblk -lno PATH "$DEV" | tail -n +2); do umount "$p" 2>/dev/null || true; done
mount -o ro "$P1" "$SRC"
[ -d "$SRC/music" ] && [ -f "$SRC/boot.sh" ] && [ -d "$SRC/r1os" ] || { echo "No tiene music/, boot.sh y r1os/: no parece la SD del R1" >&2; exit 1; }
USED=$(du -sb "$SRC" | cut -f1)
FREE=$(df -B1 --output=avail "$HOMEDIR" | tail -1)
[ "$FREE" -gt $((USED + USED / 10)) ] || { echo "Falta espacio en $HOMEDIR para el respaldo ($((USED >> 20)) MB)" >&2; exit 1; }
[ "$(du -sb "$SRC/r1os" | cut -f1)" -lt $((1800 << 20)) ] || { echo "r1os/ no cabe en 2 GiB" >&2; exit 1; }

lsblk -o NAME,SIZE,RM,TRAN,MODEL,LABEL "$DEV"
echo
echo "Se BORRARÁ $DEV ($((SIZE >> 30)) GiB). Respaldo: $BK ($((USED >> 20)) MB)."
echo "Quedará: 1 MUSICA FAT32 ($(((SIZE >> 30) - 2)) GiB) · 2 R1OS FAT32 (2 GiB)."
read -r -p "Escribe el nombre del dispositivo ($DEV) para continuar: " OK
[ "$OK" = "$DEV" ] || { echo "Cancelado"; exit 1; }

echo "1/5 Respaldo"
mkdir -p "$BK"
rsync -a --info=progress2 "$SRC"/ "$BK"/
diffs() { rsync -rt --dry-run --itemize-changes --checksum "$@" | grep -E '^(>f|<f|cd)' || true; }   # compara el contenido de cada archivo (lento, seguro)
[ -z "$(diffs "$SRC"/ "$BK"/)" ] || { echo "El respaldo no coincide; no se toca la tarjeta" >&2; exit 1; }
chown -R "$USR" "$HOMEDIR/r1_sd_backup"
umount "$SRC"

echo "2/5 Particiones"
P2_START=$(( (SIZE / 512) - (2 << 21) ))   # 2 GiB al final, en sectores
P2_START=$(( P2_START / 2048 * 2048 ))
sfdisk --wipe always "$DEV" <<EOF
label: dos
start=2048, size=$((P2_START - 2048)), type=c
start=$P2_START, type=c
EOF
partprobe "$DEV" 2>/dev/null || true; udevadm settle; sleep 2
fi

echo "3/5 Formato"
unmount_all; sleep 1; unmount_all   # el escritorio vuelve a montar las particiones al detectarlas
mkfs.vfat -F 32 -n MUSICA "$P1"
mkfs.vfat -F 32 -n R1OS "$P2"

echo "4/5 Restauración"
mount "$P1" "$M1"; mount "$P2" "$M2"
rsync -rt --modify-window=2 --info=progress2 --exclude=/r1os/ --exclude=/nandbak/ --exclude=/slotB/ --exclude=/r1.upt --exclude=/.Trash-1000/ "$BK"/ "$M1"/
rsync -rt --modify-window=2 --info=progress2 "$BK"/r1os/ "$M2"/
mkdir -p "$M2/backup"
for b in nandbak slotB r1.upt; do [ -e "$BK/$b" ] && rsync -rt --modify-window=2 "$BK/$b" "$M2/backup/"; done
sync

echo "5/5 Verificación (relee de la tarjeta, no de la memoria del PC)"
umount "$M1" "$M2"; echo 3 > /proc/sys/vm/drop_caches
mount -o ro "$P1" "$M1"; mount -o ro "$P2" "$M2"
D1=$(diffs --exclude=/r1os/ --exclude=/nandbak/ --exclude=/slotB/ --exclude=/r1.upt --exclude=/.Trash-1000/ "$BK"/ "$M1"/)
D2=$(diffs "$BK"/r1os/ "$M2"/)
[ -z "$D1$D2" ] || { printf '%s\n%s\n' "$D1" "$D2" | head -20; echo "Hay diferencias (arriba). El respaldo sigue en $BK" >&2; exit 1; }
df -h "$M1" "$M2"
echo "Listo. Respaldo en $BK (borrarlo cuando el R1 arranque bien)."
