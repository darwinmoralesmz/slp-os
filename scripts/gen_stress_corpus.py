#!/usr/bin/env python3
"""Genera una biblioteca de prueba grande con casos raros: python3 scripts/gen_stress_corpus.py DESTINO [N]
N pistas FLAC de 1 s con etiquetas variadas (artistas, álbumes, discos, nombres largos y unicode) más archivos corruptos,
formatos no soportados y variantes de frecuencia/canales."""
import os, random, shutil, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

dest = sys.argv[1]
n = int(sys.argv[2]) if len(sys.argv) > 2 else 10000
random.seed(1)
os.makedirs(dest, exist_ok=True)
tmpl = os.path.join(dest, "_tmpl.flac")
def ff(args): subprocess.run(["ffmpeg", "-loglevel", "error", "-y"] + args, check=True)
ff(["-f", "lavfi", "-i", "sine=f=330:d=1", "-ac", "2", "-ar", "44100", tmpl])
words = "Luz Mar Noche Fuego Río Sol Viento Tierra Ciudad Sueño Ángel Niño Canción Última Primera Azul Rojo Verde Oscuro Frío".split()
def name(k): return " ".join(random.choice(words) for _ in range(k))
artists = [name(2) + f" {i}" for i in range(n // 20)]
jobs = []
for i in range(n):
    ar = artists[i % len(artists)]
    al = f"{name(2)} {i // (len(artists) * 5)}"
    disc, trk = (i // 9) % 2 + 1, i % 9 + 1
    title = name(random.randint(1, 4)) + (" " + "x" * 150 if i % 997 == 0 else "")
    d = os.path.join(dest, ar, al)
    jobs.append((d, f"{trk:02d} - {title}.flac", ar, al, title, disc, trk))
def make(j):
    d, fn, ar, al, title, disc, trk = j
    os.makedirs(d, exist_ok=True)
    p = os.path.join(d, fn[:200])
    shutil.copyfile(tmpl, p)
    subprocess.run(["metaflac", f"--set-tag=TITLE={title}", f"--set-tag=ARTIST={ar}", f"--set-tag=ALBUM={al}",
                    f"--set-tag=TRACKNUMBER={trk}", f"--set-tag=DISCNUMBER={disc}", p], check=True)
with ThreadPoolExecutor(8) as ex: list(ex.map(make, jobs))
bad = os.path.join(dest, "_Casos raros"); os.makedirs(bad, exist_ok=True)
def w(name, data): open(os.path.join(bad, name), "wb").write(data)
good = open(tmpl, "rb").read()
w("vacio.flac", b""); w("vacio.mp3", b""); w("vacio.m4a", b"")
w("truncado.flac", good[:300]); w("basura.flac", os.urandom(5000)); w("basura.mp3", os.urandom(5000)); w("basura.m4a", os.urandom(5000))
w("medio_roto.flac", good[:400] + os.urandom(200) + good[600:])
w("id3_basura.mp3", b"ID3\x04\x00\x00\x7f\x7f\x7f\x7f" + os.urandom(3000))
w("nombre_largo_" + "n" * 200 + ".flac", good)
w("日本語 – 🎵 emoji.flac", good)
for cfg, ext, extra in [("6ch", "flac", ["-ac", "6", "-ar", "44100"]), ("192k", "flac", ["-ar", "192000"]), ("384k", "flac", ["-ar", "384000"]),
                        ("8k_mono", "flac", ["-ac", "1", "-ar", "8000"]), ("22k", "mp3", ["-ar", "22050"]), ("alac", "m4a", ["-c:a", "alac"]),
                        ("aac_lc", "m4a", ["-c:a", "aac"]), ("wav", "wav", []), ("ogg", "ogg", []), ("opus", "opus", []), ("adts", "aac", [])]:
    try: ff(["-f", "lavfi", "-i", "sine=f=440:d=2", "-ac", "2", "-ar", "44100"] + extra + [os.path.join(bad, f"{cfg}.{ext}")])
    except subprocess.CalledProcessError: print("sin codificador para", cfg)
os.remove(tmpl)
print("listo:", dest)
