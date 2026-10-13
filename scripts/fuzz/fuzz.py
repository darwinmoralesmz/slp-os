"""Fuzzing de decodificadores y etiquetas: python3 fuzz.py ITERACIONES ARCHIVO... (usa run.sh)."""
import os, random, subprocess, sys, shutil
random.seed(1)
iters = int(sys.argv[1])
seeds = sys.argv[2:]
BIN = 'ui/build/fz/fz'
W = '/tmp/r1fuzz'
os.makedirs(W + '/f', exist_ok=True)
crashes = {}
env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:abort_on_error=0", UBSAN_OPTIONS="print_stacktrace=1")
def mutate(b):
    b = bytearray(b)
    k = random.choice(["flip", "trunc", "zero", "dup", "insert", "head"])
    if k == "flip":
        for _ in range(random.randint(1, 60)): b[random.randrange(len(b))] = random.randrange(256)
    elif k == "trunc": b = b[:random.randrange(1, len(b))]
    elif k == "zero":
        o = random.randrange(len(b)); b[o:o + random.randint(1, 4000)] = bytes(random.randint(1, 4000))
    elif k == "dup":
        o = random.randrange(len(b)); n = random.randint(1, 3000); b[o:o] = b[o:o + n]
    elif k == "insert": 
        o = random.randrange(len(b)); b[o:o] = os.urandom(random.randint(1, 2000))
    else:
        for i in range(min(64, len(b))):
            if random.random() < .3: b[i] = random.randrange(256)
    return bytes(b)
tot = 0
for seed in seeds:
    data = open(seed, 'rb').read()
    ext = os.path.splitext(seed)[1]
    for i in range(iters):
        p = f'{W}/f/m{i % 8}{ext}'
        open(p, 'wb').write(mutate(data))
        try:
            r = subprocess.run([BIN, p], capture_output=True, timeout=20, env=env)
            rc, out = r.returncode, (r.stderr or b'').decode('latin1')
        except subprocess.TimeoutExpired:
            rc, out = 'TIMEOUT', ''
        tot += 1
        if rc != 0:
            line = next((l for l in out.splitlines() if 'ERROR' in l or 'runtime error' in l), str(rc))[:150]
            key = (os.path.basename(seed), line)
            if key not in crashes:
                crashes[key] = f'{W}/crash_{len(crashes)}{ext}'
                shutil.copy(p, crashes[key])
                frames = [l.strip()[:110] for l in out.splitlines() if l.strip().startswith('#')][:4]
                print('FALLO', key, crashes[key]); [print('   ', f) for f in frames]
print('ejecuciones', tot, 'fallos distintos', len(crashes))
