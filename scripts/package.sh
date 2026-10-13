#!/bin/sh
# Arma dist/slp-os-<versión>.zip: lo que descarga el usuario (installer.py + archivos con las rutas que espera).
# Requiere ui/r1 y tools/mksquashfs-mipsel compilados (scripts/setup.sh, make -C ui r1) y las fuentes Noto instaladas o en tools/fonts.
set -e
cd "$(dirname "$0")/.."
V=${1:?uso: scripts/package.sh <versión>}
[ -z "$(git status --porcelain --untracked-files=no)" ] || { echo "hay cambios sin commit: el zip debe salir de un commit" >&2; exit 1; }
make -C ui r1 >/dev/null
P=slp-os-$V T=$(mktemp -d)
D=$T/$P
mkdir -p $D/ui $D/tools $D/sd/r1os/fonts/noto $D/sd/r1os/fonts/noto-cjk
# Solo archivos versionados: en el disco hay binarios de HiBy ignorados por git (bt/bin, bt/lib) que no se distribuyen.
git archive HEAD installer.py AGENTS.md README.md LICENSE docs/img/enable-adb.jpg sd/boot.sh sd/r1os/boot.sh sd/r1os/usb_disk.sh sd/r1os/bt sd/r1os/web slotB/overlay | tar -x -C $D
cp ui/r1 $D/ui/
cp tools/mksquashfs-mipsel $D/tools/
for f in noto/NotoSans-Medium.ttf noto-cjk/NotoSansCJK-Medium.ttc noto/NotoSansSymbols-Medium.ttf noto/NotoSansSymbols2-Regular.ttf noto/NotoSansMath-Regular.ttf noto/NotoEmoji.ttf; do
	src=/usr/share/fonts/$f; [ -f "$src" ] || src=tools/fonts/${f##*/}
	cp "$src" $D/sd/r1os/fonts/$f
done
cp /usr/share/licenses/noto-fonts-cjk/LICENSE $D/sd/r1os/fonts/OFL.txt
cat > $D/SOURCE.txt <<EOF
SLP OS $V
Source: https://github.com/darwinmoralesmz/slp-os/tree/$(git rev-parse HEAD)
ui/r1 is built from that commit (GPL-2.0, see LICENSE).
tools/mksquashfs-mipsel: squashfs-tools 4.7.5 (GPL-2.0) + LZO 2.10 (GPL-2.0), built by scripts/build_mksquashfs.sh.
sd/r1os/fonts: Noto fonts, SIL Open Font License 1.1 (fonts/OFL.txt).
EOF
mkdir -p dist
rm -f dist/$P.zip
(cd $T && zip -qr9 - $P) > dist/$P.zip
rm -rf $T
(cd dist && sha256sum $P.zip > $P.zip.sha256 && ls -l $P.zip && cat $P.zip.sha256)
