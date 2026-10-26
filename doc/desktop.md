# Frond 0.1.0 desktop

Frond's Qt 6 launcher provides desktop controls alongside the SDL Palm display.
The Palm display remains a separate window in this release. Firmware is supplied
by the user. Palm TX support is experimental. Virtual SD cards are supported; Wi-Fi and
Bluetooth remain unavailable. See [peripherals](peripherals.md). Use the normal audio path while evaluating desktop behaviour.

## Debian Trixie / KDE Plasma

Install the locally built package:

```sh
sudo apt install ./frond_0.1.0-2_amd64.deb
```

Launch **Frond** from Plasma's application menu. On first use, choose an existing
Frond session, or select a 16 MiB TX NOR image and a 132 MiB raw NAND image, then
click **Start**. Firmware paths are remembered. Complete Palm OS setup in the
Palm window when starting from raw firmware.

Close Frond or its Palm window to save the running device automatically. The next
launch resumes that working session when **Resume last session on startup** is
checked. Uncheck it to start at the launcher and choose a session yourself.
**Save and Stop** saves the Palm and returns to the launcher.

Working copies are stored under `$XDG_DATA_HOME/frond/sessions`, normally
`~/.local/share/frond/sessions`. Imported session files and firmware images are
not overwritten. Each import gets its own working copy on save. **File → Save
Session** updates the working copy; **Save a Copy** writes a separate snapshot.
Session files contain firmware, installed applications and Palm user data.

The last-session setting is updated only after a successful working-copy save.
If saving fails, Frond stays open and offers Retry, Cancel or Discard. Session
writes use a temporary file beside the destination and replace it atomically.
Normal close is covered; forced termination, a crash or power loss can lose work
since the last save. There is no periodic autosave in this release.

The **SD Card** menu creates/imports cards and provides Eject, Reinsert and Export
Image. Card contents and insertion state are saved inside the working session,
including on normal close. Close Palm files before ejecting; export an image
before replacing a card you want to keep. Supported sizes are 32–128 MiB FAT16.

The **Network** selector offers an experimental simulated NetLib echo server and
connection error profiles. It has no Internet access. Mode and pending replies
persist in the session. See [supported APIs and limits](peripherals.md#simulated-network-experimental).

**File → Install Application** accepts PRC/PDB files after Palm OS has started.
**Launch Application** starts an installed app by its Palm database name.
**Export RAM Databases** writes a ZIP of the Palm's RAM databases, including
unrelated installed apps and user data. **Device** provides normal reset and
rotation. Sound can be enabled or disabled from the launcher. Choose normal or double-size
display before starting; the default fits the desktop height. A working session
is locked while open to prevent two Frond windows overwriting it. Errors appear in
the GUI; **View → Emulator Log** exposes diagnostic details.

`.frond` files can be opened through the desktop file manager. Existing `.session`
files also work through Open Session. A GUI-controlled session currently supports
Palm TX only.

## Build and package

```sh
sudo apt install git build-essential python3 cmake qt6-base-dev qt6-wayland \
  libsdl2-dev libsdl2-image-dev libreadline-dev libcurl4-openssl-dev
make -j4
make test
make test-desktop
make deb
```

`out/frond-desktop` finds `out/frond` beside it. The installed launcher uses
`/usr/lib/frond/frond-core`. For development, `--core /absolute/path/to/frond`
selects another binary. `frond-desktop /path/to/file.frond` opens that session;
`--version` reports 0.1.0. `make out/frond` builds only the SDL emulator.

The Debian package derives shared-library dependencies from the binaries and
includes Qt's platform and Wayland plugins. It contains no firmware, PRC,
session or test capture. Audio and display use the user's normal desktop session;
no global environment overrides are installed.

## Control and persistence tests

`make test-desktop` tests the private socket client, partial frames, Unicode file
names, command serialization, process restart and keeping the GUI open when
automatic saving fails and the user cancels closing. With your own TX fixture:

```sh
SDL_AUDIODRIVER=pulseaudio env \
  FROND_TEST_CORE="$PWD/out/frond" FROND_TEST_SESSION=/path/to/tx.session \
  ./build/desktop/test-desktop-workflow persistence
python3 tests/desktop-control.py --core out/frond --session /path/to/tx.session
```

The GUI workflow test isolates settings and working copies, checks automatic save
on close and resume on a new launch, checks that the imported file stays unchanged
and verifies that disabling resume leaves the launcher idle. It does not touch the
user's Frond settings. The core integration test uses a private copy of guest
state in memory and writes only to its own temporary directory.
