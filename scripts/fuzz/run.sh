#!/bin/sh
# Fuzzing con ASAN/UBSAN de etiquetas (src/meta.h) y decodificadores (FLAC, MP3, M4A/AAC/ALAC, Ogg Vorbis, Opus, WAV) sobre archivos mutados.
# Uso: scripts/fuzz/run.sh [iteraciones por archivo, 300]. Los casos que fallan quedan en /tmp/r1fuzz/crash_*.
set -e
cd "$(dirname "$0")/../.."
make -C ui -j"$(nproc)" fz
W=/tmp/r1fuzz; mkdir -p $W/seeds
cd $W/seeds
ff() { ffmpeg -loglevel error -y -f lavfi -i "sine=f=440:d=3" "$@"; }
[ -f s.flac ] || ff -ac 2 -c:a flac -metadata title=T -metadata artist=A -metadata album=B s.flac
[ -f s.mp3 ] || ff -c:a libmp3lame -b:a 128k -id3v2_version 3 -metadata title=T s.mp3
[ -f s_end.m4a ] || ff -c:a aac -b:a 96k -metadata title=T s_end.m4a
[ -f s_fast.m4a ] || ff -c:a aac -b:a 96k -movflags +faststart -metadata title=T s_fast.m4a
[ -f s_mono.mp3 ] || ff -ac 1 -ar 22050 -c:a libmp3lame -q:a 6 s_mono.mp3
[ -f s_alac.m4a ] || ff -ac 2 -c:a alac -metadata title=T s_alac.m4a
[ -f s_alac24.m4a ] || ff -ac 2 -sample_fmt s32p -c:a alac -metadata title=T s_alac24.m4a
[ -f s_vorbis.ogg ] || ff -ac 2 -c:a libvorbis -metadata title=T -metadata artist=A s_vorbis.ogg
[ -f s_opus.opus ] || ff -ac 2 -ar 48000 -c:a libopus -b:a 96k -metadata title=T -metadata artist=A s_opus.opus
[ -f s_opus_mono.opus ] || ff -ac 1 -ar 48000 -c:a libopus -b:a 24k s_opus_mono.opus
[ -f s.wav ] || ff -ac 2 -c:a pcm_s16le -metadata title=T s.wav
[ -f s24.wav ] || ff -ac 1 -c:a pcm_s24le s24.wav
cd - >/dev/null
python3 scripts/fuzz/fuzz.py "${1:-300}" $W/seeds/*
