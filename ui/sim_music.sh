#!/bin/sh
# Genera pistas FLAC de prueba con etiquetas en ui/sim_music (requiere ffmpeg).
cd "$(dirname "$0")" && mkdir -p sim_music && cd sim_music
n=0
ffmpeg -loglevel error -y -f lavfi -i "testsrc2=s=600x600" -frames:v 1 /tmp/sim_cover.png
mk() { n=$((n+1)); ffmpeg -loglevel error -y -f lavfi -i "sine=f=$((200+n*40)):d=$5" -metadata title="$1" -metadata artist="$2" -metadata album="$3" -metadata track="$4" -metadata albumartist="$2" "$(printf '%02d' $n) $1.flac"; }
mkc() { n=$((n+1)); ffmpeg -loglevel error -y -f lavfi -i "sine=f=$((200+n*40)):d=$5" -i /tmp/sim_cover.png -map 0 -map 1 -c:a flac -c:v mjpeg -disposition:v attached_pic -metadata title="$1" -metadata artist="$2" -metadata album="$3" -metadata track="$4" -metadata albumartist="$2" "$(printf '%02d' $n) $1.flac"; }
mkc "Airbag" "Radiohead" "OK Computer" 1 284
mkc "Paranoid Android" "Radiohead" "OK Computer" 2 383
mk "Pyramid Song" "Radiohead" "Amnesiac" 1 290
mk "Reckoner" "Radiohead" "In Rainbows" 1 290
mk "Weird Fishes" "Radiohead" "In Rainbows" 2 318
mk "Idioteque" "Radiohead" "Kid A" 1 309
mk "Roygbiv" "Boards of Canada" "Music Has the Right to Children" 1 151
mk "Aquarius" "Boards of Canada" "Music Has the Right to Children" 2 342
mk "Dayvan Cowboy" "Boards of Canada" "The Campfire Headphase" 1 300
mk "Fun In Hi Skool" "Protomartyr" "Formal Growth In The Desert" 1 210
mk "Rhythm of the Night" "Todd Terje" "It's Album Time" 1 255
mk "Strobe" "deadmau5" "For Lack of a Better Name" 1 600
ffmpeg -loglevel error -y -f lavfi -i "sine=f=900:d=120" -i /tmp/sim_cover.png -map 0 -map 1 -c:a libmp3lame -c:v mjpeg -id3v2_version 3 -metadata title="Ejemplo MP3" -metadata artist="Prueba" -metadata album="Etiquetas ID3" -metadata track=1 "13 Ejemplo MP3.mp3" || true
