# SLP OS

An alternative music player system for the HiBy R1 (Ingenic X1600, MIPS32r2, Cirrus Logic CS43131 DAC).

Personal hobby project. The name is a nod to AI slop: the code was written with heavy use of AI coding assistants.
The system itself has no name; on the device it is just "the custom system" next to stock HiBy OS.

## Warnings

- Experimental. Use at your own risk. Not affiliated with HiBy Music.
- Volume is an **attenuation**: 255 = mute, lower = louder. Keep it low before pressing play.
- Install with at least 50 % battery. The installer writes slot B (`mtd4`) of the NAND; stock HiBy OS is never touched.

## How it works

- **Two systems.** Stock HiBy OS stays in slot A. The custom system boots from slot B (HiBy's recovery slot): HiBy's own
  recovery rootfs plus one init script (`slotB/overlay`). The image is rebuilt **on your device** from its own recovery,
  so nothing from HiBy is distributed. The app and everything else live on the microSD.
- **Boot flag.** Slot B clears the boot flag when it starts and the app sets it again after 30 s. A hang before 30 s
  falls back to stock on the next power-on; after that (or after "Power off"), the next power-on starts the custom system again.
- **microSD.** One partition: the system goes in `r1os/` and your music in `music/`. Two partitions (optional,
  `scripts/sd_partition.sh`): partition 1 for music, partition 2 (`R1OS`, FAT32, 2 GiB) for the system and caches.
- **Vendor files.** The Bluetooth, Wi-Fi and ALSA files are copied on the device from its own stock rootfs.

## Features

- Formats: FLAC, MP3, AAC and ALAC (M4A), Vorbis, Opus, WAV. Output is 16-bit.
- Outputs: headphone jack and Bluetooth (SBC and AAC tested; LDAC/aptX untested), AVRCP controls.
- Library by albums, artists and songs, cover art, search with on-screen keyboard (up to 5 artists, 8 albums, 100 songs).
- Playlists as `.m3u8` files in `music/Listas` (fixed folder name), favorites, play queue.
- Spanish and English interface, themes with automatic accent color from the cover.
- Sleep timers (10 and 30 min without playback by default; can be set to "Never").
- Untested on the device: Wi-Fi settings and "Music over Wi-Fi" (simulator only), USB disk mode.

## Using it

- **Quick panel:** drag down from the top-left corner (150×154 px; 150×58 px on the player and with the keyboard open).
  It shows time, weather, battery, output and brightness; fully open: volume, Bluetooth card (tap = reconnect/disconnect,
  hold = Settings > Bluetooth), Settings, stand-by and power.
- **Power menu:** Power off, Restart, Restart into HiBy, Screen off, Cancel.
- **Player:** taps are counted: 1 = play/pause, 2 = next, 3 = previous. The same works on the right 110 px of the
  mini-player. Hold 600 ms for the technical sheet (codec, average bitrate, sample rate, path).
- **Rows:** swipe a song more than 160 px: right = add to the end of the queue, left = remove (queue, playlists, favorites).
- **Selection:** long-press to select. Action bar: Now, Next, Playlist, Favorite, More. In playlists and favorites:
  Remove, Now, Playlist, More. In the queue: Remove, More. In Playlists: Delete.
- **Search:** any vertical drag hides the keyboard.
- **Buttons:** there is no physical play/pause; the key reported as `KEY_PLAYPAUSE` acts as "previous".

## Install

Requirements: HiBy R1 on stock firmware, a FAT32/exFAT microSD, Python 3 and `adb` (Windows, macOS or Linux).
Building `ui/r1` and `tools/mksquashfs-mipsel` needs Linux: `scripts/setup.sh`, then `make -C ui r1`.

1. In stock HiBy OS: Settings > About, tap 10 times, then enable ADB in that menu. It does not persist after a restart.
2. Connect the R1 by USB and run, **from stock HiBy OS the first time**:
   ```sh
   python3 installer.py install
   ```
   It backs up the whole NAND to `backup/<serial>/` (verified by SHA-256), copies the vendor kit and the system to the
   microSD, builds the slot B image on the device, asks before writing it, verifies it and offers to restart.
   Keep `backup/`: it is the only way to uninstall.

Other commands (`-s SERIAL` to pick a device, `-y` to skip questions):

```sh
python3 installer.py update      # new version: copies the system files again, keeps the backup and slot B
python3 installer.py boot        # from stock HiBy OS (ADB on): restart into the custom system
python3 installer.py uninstall   # from stock HiBy OS: restores the original slot B and boots stock
```

To go back to stock, use "Restart into HiBy" in the power menu. Uninstall needs a backup taken from stock before the
first install; it refuses to run from slot B.

## Development

`make -C ui sim` builds an SDL simulator for the PC; `scripts/deploy.sh` builds and pushes the app to the microSD.
Architecture notes are in the source comments; `third_party/VERSIONS.md` lists the bundled libraries.

## Credits

- Reference: `bahusoid/rockbox` (HiBy R1 port), `bidhata/Hiby-R1-Mod`, `Meowby-R1`, `hiby_os_crack`, `open_hiby_player`.
- Libraries: LVGL, FAAD2, libopus, minimp3, dr_flac, dr_wav, stb_vorbis, stb_image, minimp4, ALAC decoder (David Hammerton,
  via Rockbox), musl.
- Fonts: Montserrat, Font Awesome, Noto.
- Tool shipped in releases: squashfs-tools 4.7.5 and LZO 2.10 (`scripts/build_mksquashfs.sh`).

## License

GPL-2.0 (see `LICENSE`); the app links FAAD2, which is GPL-2.0. Third-party files keep their own licenses.
