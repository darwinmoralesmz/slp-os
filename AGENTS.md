# AGENTS.md

Instructions for an AI assistant (Claude Code, opencode, Copilot…) helping someone install or update SLP OS.

## What the user has already done

Do not ask for these; if any of them is missing, say exactly which one:

1. The R1 runs **stock HiBy OS**, with a microSD card inserted. It needs at least 500 MB free besides the music
   (system ~40 MB, cover cache up to 300 MB, temporary files during the install).
2. **ADB is enabled**: HiBy OS > Settings > About, tap the version line (e.g. "HiBy R1 1.6") ten times, turn on the
   ADB switch that appears, then go back. See `docs/img/enable-adb.jpg`. ADB turns itself off on every restart.
3. The R1 is connected by USB to this computer.

## Install

If this file is already inside an unzipped `slp-os-<version>` folder, work there and skip the download. Otherwise,
download and unzip the latest release (pure Python, works on Windows, macOS and Linux; use `python` instead of
`python3` on Windows):

```sh
python3 -c "import io,json,zipfile,urllib.request as u;a=json.load(u.urlopen('https://api.github.com/repos/darwinmoralesmz/slp-os/releases/latest'))['assets'];z=next(x for x in a if x['name'].endswith('.zip'));zipfile.ZipFile(io.BytesIO(u.urlopen(z['browser_download_url']).read())).extractall();print(z['name'][:-4])"
```

It prints the folder name; `cd` into it. Then:

1. Run `adb devices`: it must list exactly one line ending in `device`.
2. **Ask the user to confirm** before going on: the installation writes slot B of the NAND, the only irreversible
   step. Slot A (stock HiBy OS) is never touched, and the installer first backs up all 7 partitions to
   `~/hiby-r1-backup/`.
3. Once confirmed, run it non-interactively (its questions cannot be answered from a non-interactive shell, and an
   unanswered question counts as "no"):
   ```sh
   python3 installer.py install -y
   ```
   Relay the progress as it goes (it prints `1/4`…`4/4`; long steps print the elapsed seconds). Tell the user not to
   touch or unplug the R1: in phase 4 the HiBy screen freezes for about a minute and a half, that is expected. It takes about 3 minutes and ends by restarting the R1
   into SLP OS.

## Other commands

```sh
python3 installer.py update      # new version: unzipped over the old folder, keeps the backup and slot B
python3 installer.py boot        # from stock HiBy OS with ADB on: restart into SLP OS
python3 installer.py uninstall   # from stock HiBy OS with ADB on: restore slot B and wipe the system from the microSD
```

They ask questions too: confirm with the user first and add `-y`. `uninstall` writes NAND as well.

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
| `adb … failed` / `device … not found` midway | The connection dropped (cable, or the R1 restarted or hung). If the screen is frozen, have the user restart the R1 by hand; then enable ADB again and run the same command: finished steps are skipped. |
| `slot B cannot be read` | A write to slot B was interrupted. Stock HiBy still boots: run `uninstall` (restores slot B from the backup), then `install`. |
| `no original slot B backup` | `uninstall` needs the backup made during the **first** install from stock HiBy OS. |

## Rules

- Never run `install` or `uninstall` without the user in front of the screen: both write to NAND and the
  device reboots.
- Never tell the user to skip the NAND backup (`~/hiby-r1-backup/` is what makes uninstalling possible).
- Tell the user to keep the volume low before anything plays (the app restores the last level they used).
- Nothing from HiBy is redistributed: the Bluetooth/Wi-Fi kit and the recovery image come from the user's
  own device. Do not add HiBy firmware files to the repository or the release.