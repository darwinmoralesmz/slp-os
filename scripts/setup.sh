#!/bin/sh
# Descarga lo que no se guarda en el repositorio: toolchain MIPS (musl), LVGL, FAAD2, Opus y Noto Emoji.
# Ejecutar desde la raíz del proyecto. Versiones usadas: ver third_party/VERSIONS.md.
set -e
cd "$(dirname "$0")/.."

mkdir -p tools/musl tools/pkg

# Toolchain mipsel-linux-musl (estático, GCC 11.2.1). musl.cc no publica checksum oficial.
[ -x tools/musl/mipsel-linux-musl-cross/bin/mipsel-linux-musl-gcc ] || {
	curl -fsSL -o tools/musl/mipsel-linux-musl-cross.tgz https://musl.cc/mipsel-linux-musl-cross.tgz
	tar xzf tools/musl/mipsel-linux-musl-cross.tgz -C tools/musl
}

# LVGL 9.2.2 y FAAD2 (GPLv2) en el commit usado.
[ -d lvgl ] || git clone --depth 1 --branch v9.2.2 https://github.com/lvgl/lvgl.git lvgl
[ -d third_party/opus ] || git clone --depth 1 --branch v1.5.2 https://github.com/xiph/opus.git third_party/opus   # decodificador Opus (BSD)
[ -d third_party/faad2 ] || {
	git clone https://github.com/knik0/faad2.git third_party/faad2
	git -C third_party/faad2 checkout 521d105979795b887ea11e1f36565e97a8fbfc53
}
# Noto Emoji monocromo (OFL): respaldo para emoji en etiquetas; el color (CBDT) no lo dibuja tiny_ttf.
[ -f tools/fonts/NotoEmoji.ttf ] || { mkdir -p tools/fonts; curl -fsSL -o tools/fonts/NotoEmoji.ttf "https://github.com/google/fonts/raw/main/ofl/notoemoji/NotoEmoji%5Bwght%5D.ttf"; }
[ -x tools/mksquashfs-mipsel ] || scripts/build_mksquashfs.sh   # tools/mksquashfs-mipsel, lo usa installer.py
echo "listo"
