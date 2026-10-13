#!/bin/sh
# Compila mksquashfs estático para el R1 (mipsel, musl, solo LZO): el instalador arma la imagen del slot B en el propio equipo.
# Resultado: tools/mksquashfs-mipsel. Requiere la toolchain de scripts/setup.sh.
set -e
cd "$(dirname "$0")/.."
ROOT=$PWD T=mipsel-linux-musl
TC=$PWD/tools/musl/mipsel-linux-musl-cross
mkdir -p tools/src && cd tools/src
[ -f lzo-2.10.tar.gz ] || curl -fsSL -o lzo-2.10.tar.gz https://www.oberhumer.com/opensource/lzo/download/lzo-2.10.tar.gz
[ -f squashfs-tools-4.7.5.tar.gz ] || curl -fsSL -o squashfs-tools-4.7.5.tar.gz https://github.com/plougher/squashfs-tools/archive/refs/tags/4.7.5.tar.gz
sha256sum -c - <<EOF
c0f892943208266f9b6543b3ae308fab6284c5c90e627931446fb49b4221a072  lzo-2.10.tar.gz
547b7b7f4d2e44bf91b6fc554664850c69563701deab9fd9cd7e21f694c88ea6  squashfs-tools-4.7.5.tar.gz
EOF
B=$(mktemp -d) && trap 'rm -rf "$B"' EXIT   # sin espacios en la ruta: configure y make no los admiten
tar xzf lzo-2.10.tar.gz -C "$B" && tar xzf squashfs-tools-4.7.5.tar.gz -C "$B" && ln -s "$TC" "$B/tc" && cd "$B"
export PATH="$B/tc/bin:$PATH"
(cd lzo-2.10 && ./configure --host=mipsel-linux-musl CC=$T-gcc LD=$T-ld --prefix="$B/lzo" --disable-shared >/dev/null && make -j"$(nproc)" install >/dev/null)
make -C squashfs-tools-4.7.5/squashfs-tools -j"$(nproc)" mksquashfs CC=$T-gcc \
	GZIP_SUPPORT=0 XZ_SUPPORT=0 LZ4_SUPPORT=0 ZSTD_SUPPORT=0 LZO_SUPPORT=1 COMP_DEFAULT=lzo XATTR_SUPPORT=0 \
	EXTRA_CFLAGS="-I$B/lzo/include" EXTRA_LDFLAGS="-static -L$B/lzo/lib" >/dev/null 2>&1
$T-strip -o "$ROOT/tools/mksquashfs-mipsel" squashfs-tools-4.7.5/squashfs-tools/mksquashfs
ls -l "$ROOT/tools/mksquashfs-mipsel"
