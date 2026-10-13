#!/usr/bin/env python3
"""Installer for the HiBy R1 custom system (slot B). Needs only Python 3 and adb (Windows, macOS, Linux).

  python3 installer.py install     back up the NAND, copy the system to the microSD, build and flash slot B
  python3 installer.py update      same as install: refreshes the system files, keeps the backup and slot B
  python3 installer.py boot        restart into the custom system (from stock HiBy OS with ADB enabled)
  python3 installer.py uninstall   restore the original slot B from the backup and boot stock HiBy OS

Nothing from HiBy is shipped: the Bluetooth/Wi-Fi kit is copied on the device from its own stock rootfs, and the
slot B image is rebuilt on the device from its own recovery rootfs plus one init script (slotB/overlay)."""
import argparse, hashlib, os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
SD = '/usr/data/mnt/sd_0'        # microSD partition 1 (music and the boot.sh launcher)
R = '/usr/data/mnt/r1os'         # system: partition 2 if present, otherwise SD/r1os (sd/boot.sh decides)
INST = R + '/inst'
MTD4_SIZE = 0x1800000
SERIAL = []

# Files taken from the device's own stock rootfs (mtd2): destination under r1os/ <- source path.
KIT = {
    'bt/bin/bluetoothd': 'usr/libexec/bluetooth/bluetoothd', 'bt/bin/bt-adapter': 'usr/bin/bt-adapter',
    'bt/bin/bt-agent': 'usr/bin/bt-agent', 'bt/bin/hciconfig': 'usr/bin/hciconfig', 'bt/bin/dbus-daemon': 'usr/bin/dbus-daemon',
    'lib/libasound.so.2': 'usr/lib/libasound.so.2', 'cywdhd.ko': 'module_driver/cywdhd.ko',
    'bin/wpa_supplicant': 'usr/sbin/wpa_supplicant', 'bin/wpa_cli': 'usr/sbin/wpa_cli',
}
KIT_LIBS = ['libasound.so.2', 'libbluetooth.so.3', 'libdbus-1.so.3', 'libfdk-aac.so.2', 'libgio-2.0.so.0',
            'libglib-2.0.so.0', 'libgobject-2.0.so.0']


def die(msg):
    sys.exit('\nERROR: ' + msg)


def adb(*args, check=True):
    p = subprocess.run(['adb', *SERIAL, *args], capture_output=True, text=True)
    if check and p.returncode:
        die('adb %s failed: %s' % (args[0], (p.stderr or p.stdout).strip()))
    return p.stdout


def sh(cmd, check=True):
    """Runs cmd on the device. The old adbd does not return exit codes, so a marker carries it."""
    out = adb('shell', '%s; echo "__rc=$?"' % cmd).replace('\r', '')
    body, _, rc = out.rpartition('__rc=')
    if check and rc.strip() != '0':
        die('on the device: %s\n%s' % (cmd, body.strip()))
    return body.strip()


def push(src, dst):
    adb('push', src, dst)


def ask(q, yes):
    if yes:
        return True
    try:
        return input(q + ' [y/N] ').strip().lower() in ('y', 'yes')
    except EOFError:
        return False


def connect():
    if subprocess.run(['adb', 'version'], capture_output=True).returncode:
        die('adb not found in PATH (install Android platform-tools).')
    devs = [l.split()[0] for l in adb('devices').splitlines()[1:] if l.endswith('\tdevice')]
    if SERIAL:
        devs = [d for d in devs if d == SERIAL[1]]
    if len(devs) != 1:
        die('expected one device in ADB, found %d. Enable ADB on the R1 (see README) or use -s SERIAL.' % len(devs))
    SERIAL[:] = ['-s', devs[0]]
    mtd = sh('cat /proc/mtd')
    if '"rootfs2"' not in mtd or '"ota"' not in mtd or not sh('[ -x /usr/bin/bootmode.sh ] && echo ok', False):
        die('this does not look like a HiBy R1 (no rootfs2/ota partitions or bootmode.sh).')
    slot = 'B' if 'mtdblock_bbt_ro4' in sh('cat /proc/cmdline') else 'A'
    print('device %s, running slot %s (%s)' % (devs[0], slot, 'custom system' if slot == 'B' else 'stock HiBy OS'))
    return devs[0], slot


def mount_sd():
    if 'mmcblk0p1' not in sh('grep " %s " /proc/mounts || true' % SD):
        die('microSD not mounted at %s. Insert a FAT32/exFAT card.' % SD)
    push(os.path.join(HERE, 'sd/boot.sh'), SD + '/boot.sh')
    sh('chmod +x %s/boot.sh; sh %s/boot.sh montar; mkdir -p %s' % (SD, SD, INST))


def slotb_is_ours():
    sh('mkdir -p /tmp/rec; umount /tmp/rec 2>/dev/null; mount -t squashfs -o ro /dev/mtdblock_bbt_ro4 /tmp/rec')
    ours = sh('[ -e /tmp/rec/etc/init.d/S92_01_dev ] && echo yes || true') == 'yes'
    sh('umount /tmp/rec')
    return ours


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def backup(serial):
    """Full NAND dump (7 partitions, ~125 MB) to backup/<serial>/, verified by SHA-256 against the device."""
    d = os.path.join(HERE, 'backup', serial.replace(':', '_'))
    if os.path.exists(os.path.join(d, 'SHA256SUMS')):
        print('backup already present: %s' % d)
        return d
    os.makedirs(d, exist_ok=True)
    original = not slotb_is_ours()
    sums = []
    for n in range(7):
        name = 'mtd%d.bin' % n
        print('  backing up %s ...' % name, flush=True)
        tmp = '%s/%s' % (INST, name)
        sh('dd if=/dev/mtd%d of=%s bs=128k 2>/dev/null' % (n, tmp))
        remote = sh('sha256sum %s' % tmp).split()[0]
        adb('pull', tmp, os.path.join(d, name))
        sh('rm %s' % tmp)
        if sha256(os.path.join(d, name)) != remote:
            die('%s: checksum mismatch after transfer.' % name)
        sums.append('%s  %s\n' % (remote, name))
    with open(os.path.join(d, 'SHA256SUMS'), 'w') as f:
        f.writelines(sums)
    with open(os.path.join(d, 'INFO'), 'w') as f:
        f.write('date=%s\nmtd4_original=%s\n' % (time.strftime('%Y-%m-%d %H:%M'), 'yes' if original else 'no'))
    print('backup saved in %s' % d)
    if not original:
        print('  WARNING: slot B was already modified, so this backup cannot restore it (uninstall needs a backup\n'
              '  made from stock HiBy OS before the first install).')
    return d


def copy_kit():
    """Copies the Bluetooth/Wi-Fi/ALSA kit from the device's stock rootfs to the microSD."""
    sh('mkdir -p /tmp/stock; umount /tmp/stock 2>/dev/null; mount -t squashfs -o ro /dev/mtdblock_bbt_ro2 /tmp/stock')
    cmds = ['mkdir -p %s/bt/bin %s/bt/lib %s/lib %s/bin' % (R, R, R, R)]
    cmds += ['cp -L /tmp/stock/%s %s/%s' % (src, R, dst) for dst, src in KIT.items()]
    cmds += ['cp -L /tmp/stock/usr/lib/%s %s/bt/lib/' % (l, R) for l in KIT_LIBS]
    cmds += ['rm -rf %s/alsa && cp -rL /tmp/stock/usr/share/alsa %s/alsa' % (R, R)]
    try:
        sh(' && '.join(cmds))
    finally:
        sh('umount /tmp/stock', False)


def copy_system():
    """Copies the system files (app, scripts, web page and fonts/ if packaged) to the microSD."""
    r1 = os.path.join(HERE, 'ui/r1')
    if not os.path.exists(r1):
        die('ui/r1 is missing: use a release package or build it with "make -C ui r1".')
    sd = os.path.join(HERE, 'sd/r1os')
    files = ['boot.sh', 'usb_disk.sh', 'web/index.html'] + ['bt/' + f for f in os.listdir(os.path.join(sd, 'bt')) if f.endswith('.sh')]
    for root, _, names in os.walk(os.path.join(sd, 'fonts')):
        files += [os.path.relpath(os.path.join(root, n), sd).replace(os.sep, '/') for n in names]
    for d in sorted({os.path.dirname(f) for f in files} - {''}):
        sh('mkdir -p "%s/%s"' % (R, d))
    for f in files:
        push(os.path.join(sd, f), '%s/%s' % (R, f))
    push(r1, R + '/r1')
    sh('chmod +x %s/r1 %s/boot.sh %s/usb_disk.sh %s/bt/*.sh' % (R, R, R, R))
    if sh('md5sum %s/r1' % R).split()[0] != hashlib.md5(open(r1, 'rb').read()).hexdigest():
        die('r1 on the microSD does not match the local file.')


FLASH = '''set -e; I=%(img)s; S=$(wc -c < $I); M=$(md5sum $I | cut -d' ' -f1)
[ $S -gt 1048576 ] || exit 1; [ $S -le %(max)d ] || exit 1
flash_erase /dev/mtd4 0 0 >/dev/null; nandwrite -p /dev/mtd4 $I >/dev/null
[ "$(dd if=/dev/mtd4 bs=128k count=$(( (S + 131071) / 131072 )) 2>/dev/null | head -c $S | md5sum | cut -d' ' -f1)" = $M ]'''


def flash_mtd4(img):
    """Writes img (path on the device) to slot B and verifies it by reading back in 128 KB blocks."""
    sh((FLASH % {'img': img, 'max': MTD4_SIZE}).replace('\n', '; '))


def build_slotb():
    """Builds the slot B image on the device: its own recovery rootfs without recoveryd, plus S92_01_dev."""
    push(os.path.join(HERE, 'tools/mksquashfs-mipsel'), INST + '/mksquashfs')
    push(os.path.join(HERE, 'slotB/overlay/etc/init.d/S92_01_dev'), INST + '/S92_01_dev')
    sh('mkdir -p /tmp/rec; umount /tmp/rec 2>/dev/null; mount -t squashfs -o ro /dev/mtdblock_bbt_ro4 /tmp/rec')
    print('  building the slot B image on the device (about a minute) ...', flush=True)
    try:
        # -e must be the last option: mksquashfs takes every following argument as an exclusion.
        sh('chmod +x {i}/mksquashfs && {i}/mksquashfs /tmp/rec {i}/slotb.img -comp lzo -b 131072 -all-root -noappend '
           '-no-progress -quiet -no-xattrs -processors 1 -mem 16M '
           "-p 'etc/init.d/S92_01_dev f 755 0 0 cat {i}/S92_01_dev' -e etc/init.d/S92_03_start_recovery".format(i=INST))
    finally:
        sh('umount /tmp/rec', False)
    return INST + '/slotb.img'


def reboot_r1os():
    sh('sync; /usr/bin/bootmode.sh Recovery >/dev/null 2>&1')
    adb('shell', 'reboot', check=False)


def cmd_install(a):
    serial, slot = connect()
    mount_sd()
    print('1/4 NAND backup')
    backup(serial)
    print('2/4 Bluetooth/Wi-Fi kit from the device')
    copy_kit()
    print('3/4 system files to the microSD')
    copy_system()
    print('4/4 slot B')
    if slotb_is_ours():
        print('  slot B already has the custom system: not touched')
    elif slot == 'B':
        die('running from slot B but it is not the custom system: unexpected state, stopping.')
    else:
        img = build_slotb()
        if not ask('Write the image to slot B (mtd4)? Stock HiBy OS (slot A) is not touched.', a.yes):
            die('cancelled before writing.')
        flash_mtd4(img)
        sh('rm -f %s/slotb.img' % INST)
        print('  slot B written and verified')
    print('\nInstalled. To start it later: python3 installer.py boot')
    if ask('Restart into the custom system now?', a.yes):
        reboot_r1os()


def cmd_boot(a):
    connect()
    reboot_r1os()
    print('restarting into the custom system')


def cmd_uninstall(a):
    serial, slot = connect()
    if slot == 'B':
        die('running from slot B. Use "Restart into HiBy" in the power menu, enable ADB in HiBy OS and run again.')
    d = a.backup or os.path.join(HERE, 'backup', serial.replace(':', '_'))
    img = os.path.join(d, 'mtd4.bin')
    if not os.path.exists(img) or 'mtd4_original=yes' not in open(os.path.join(d, 'INFO')).read():
        die('no original slot B backup in %s (it must come from before the first install).' % d)
    want = [l.split()[0] for l in open(os.path.join(d, 'SHA256SUMS')) if l.split()[1] == 'mtd4.bin'][0]
    if sha256(img) != want:
        die('%s does not match its SHA-256.' % img)
    if not slotb_is_ours():
        print('slot B is already the original recovery')
    else:
        if not ask('Restore the original slot B (mtd4) from %s?' % img, a.yes):
            die('cancelled.')
        mount_sd()
        push(img, INST + '/mtd4.bin')
        flash_mtd4(INST + '/mtd4.bin')
        print('slot B restored and verified')
    sh('/usr/bin/bootmode.sh >/dev/null 2>&1')
    if ask('Delete the system from the microSD (r1os/ and boot.sh; music/ is kept)?', a.yes):
        mount_sd()
        sh('umount %s 2>/dev/null; rm -rf %s/r1os %s/boot.sh /usr/data/mnt/r1os' % (R, SD, SD), False)
        print('removed. If the card has a second partition (R1OS), it can be deleted from a PC.')
    print('done: the R1 is back to stock HiBy OS')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('command', choices=['install', 'update', 'boot', 'uninstall'])
    p.add_argument('-s', metavar='SERIAL', help='adb serial when several devices are connected')
    p.add_argument('-y', '--yes', action='store_true', help='do not ask for confirmation')
    p.add_argument('--backup', help='backup folder for uninstall (default backup/<serial>)')
    a = p.parse_args()
    if a.s:
        SERIAL[:] = ['-s', a.s]
    {'install': cmd_install, 'update': cmd_install, 'boot': cmd_boot, 'uninstall': cmd_uninstall}[a.command](a)


if __name__ == '__main__':
    main()
