# AGENTS.md

Instructions for an AI assistant (Claude Code, opencode, Copilot…) helping someone install or update SLP OS.

## What the user has already done

Do not ask for these; if any of them is missing, say exactly which one:

1. The R1 runs **stock HiBy OS**, with a microSD card inserted. It needs at least 500 MB free besides the music
   (system ~40 MB, cover cache up to 300 MB, temporary files during the install).
2. **ADB is enabled**: HiBy OS > Settings > About, tap "HiBy R1 1.6" ten times, turn on the ADB switch that appears, then go back.
   See `docs/img/enable-adb.jpg`. ADB turns itself off on every restart.
3. The R1 is connected by USB to this computer.

## Install

```sh
python3 -c "import json,urllib.request as u;a=json.load(u.urlopen('https://api.github.com/repos/darwinmoralesmz/slp-os/releases/latest'))['assets'];print(next(x['browser_download_url'] for x in a if x['name'].endswith('.zip')))" | xargs curl -LO
unzip -oq slp-os-*.zip && cd slp-os-*/
adb devices          # must list exactly one line ending in "device"
python3 installer.py install
```

Relay the progress as it goes (the installer prints `1/4`…`4/4`). It takes about 3 minutes.

**Stop at the prompt that asks to write slot B and ask the user for confirmation** (the installer will
refuse without a `y`). Writing to NAND is the only irreversible step; slot A, stock HiBy OS, is never
touched, and the installer has already backed up all 7 partitions to `~/hiby-r1-backup/`.

Then it offers to restart into SLP OS. Accepting is fine.

## Other commands

```sh
python3 installer.py update      # new version: unzipped over the old folder, keeps the backup and slot B
python3 installer.py boot        # from stock HiBy OS with ADB on: restart into SLP OS
python3 installer.py uninstall   # from stock HiBy OS with ADB on: restore slot B and wipe the system from the microSD
```

## When it stops, this is what it means

| Message | What to tell the user |
| --- | --- |
| `adb not found in PATH` | Install Android platform-tools. |
| `expected one device in ADB, found 0` | ADB is off (it is after every restart): repeat step 2, or check the USB cable. |
| `found 0` + `unauthorized` in `adb devices` | Tap "OK" on the authorization prompt on the R1 screen. |
| `found 2` | Two devices: pass `-s SERIAL`. |
| `this does not look like a HiBy R1` | Wrong device or wrong firmware. Stop. |
| `microSD not mounted` | No card, or not FAT32/exFAT. |
| `device …, running slot B (custom system)` | Already installed: use `update` or `boot`, and `uninstall` needs a restart into stock first. |
| `no original slot B backup` | `uninstall` needs the backup made during the **first** install from stock HiBy OS. |

## Rules

- Never run `install` or `uninstall` without the user in front of the screen: both write to NAND and the
  device reboots.
- Never tell the user to skip the NAND backup (`~/hiby-r1-backup/` is what makes uninstalling possible).
- Volume is an attenuation on this device: 255 is mute, lower is louder. Say this before anything plays.
- Nothing from HiBy is redistributed: the Bluetooth/Wi-Fi kit and the recovery image come from the user's
  own device. Do not add HiBy firmware files to the repository or the release.