#!/usr/bin/env python3
"""Copia una carpeta de música a la SD del R1 por ADB, archivo a archivo y reanudable.

Uso: scripts/push_music.py ORIGEN [DESTINO]   (DESTINO por defecto /data/mnt/sd_0/music/<nombre de ORIGEN>)
Omite lo ya copiado (mismo nombre y tamaño) y reemplaza por "_" los caracteres fuera del plano básico de Unicode
(vfat los rechaza y `adb push` de la carpeta entera se detiene en el primer error).
"""
import os, re, subprocess, sys

SERIAL = os.environ.get("R1_SERIAL", "ingenic_")
adb = lambda *a: subprocess.run(["adb", "-s", SERIAL, *a], capture_output=True, text=True, errors="replace")

src = os.path.abspath(sys.argv[1])
dst = sys.argv[2] if len(sys.argv) > 2 else "/data/mnt/sd_0/music/" + os.path.basename(src)
adb("shell", f"mkdir -p '{dst}'")
# El ls de busybox pinta colores y cambia los caracteres no ASCII por "?": se compara por tamaño y por los caracteres ASCII del nombre.
norm = lambda name: "".join(c for c in re.sub(r"\x1b\[[0-9;]*m", "", name) if c.isascii() and c.isalnum())
have = set()
for l in adb("shell", f"find '{dst}' -type f -exec ls -l {{}} +").stdout.splitlines():
    p = l.split(None, 8)
    if len(p) == 9: have.add((norm(os.path.basename(p[8])), int(p[4])))
todo = []
for root, _, files in os.walk(src):
    for f in sorted(files):
        full = os.path.join(root, f)
        rel = os.path.relpath(full, src)
        safe = "".join("_" if ord(c) > 0xFFFF else c for c in rel)
        if (norm(os.path.basename(safe)), os.path.getsize(full)) not in have: todo.append((full, safe))
print(f"{len(todo)} por copiar")
ok = bad = 0
for full, safe in todo:
    d = os.path.join(dst, safe)
    adb("shell", f"mkdir -p '{os.path.dirname(d)}'")
    r = adb("push", full, d)
    if r.returncode: bad += 1; print("FALLA", full, r.stderr.strip()[-100:], flush=True)
    else: ok += 1
    if (ok + bad) % 25 == 0: print(f"{ok + bad}/{len(todo)}", flush=True)
print(f"copiados {ok}, fallidos {bad}")
sys.exit(1 if bad else 0)
