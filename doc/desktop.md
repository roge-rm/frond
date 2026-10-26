# Frond 0.2.0 desktop

Frond's Qt 6 launcher provides desktop controls alongside the SDL Palm display.
The Palm display remains a separate window in this release. Firmware is supplied
by the user. Palm TX support is experimental. Virtual SD cards and experimental virtual Wi-Fi are supported;
Bluetooth remains unavailable. See [peripherals](peripherals.md). Use the normal audio path while evaluating desktop behaviour.

## Debian Trixie / KDE Plasma

Install the locally built package:

```sh
sudo apt install ./frond_0.2.0-1_amd64.deb
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
including on normal close. **Exchange Files…** saves a recovery session, then
copies files through a private ejected image using `mtools`. Choose **Apply and
Reinsert** to use the edits, or Cancel to keep the original ejected card. Existing
host and card filenames are refused. See the [file exchange guide](peripherals.md#hostpalm-file-exchange).
Close Palm files before ejecting; export an image
before replacing a card you want to keep. Supported sizes are 32–128 MiB FAT16.

For Internet access, select **Network: Virtual Wi-Fi (Frond)** before turning on
Wi-Fi in Palm OS. Scan and connect to the open **Frond** network. Its virtual
router supplies an address, DNS and IPv4 Internet access through the computer's
connection. No root access, bridge setup or host Wi-Fi adapter is needed.
Blazer supports HTTP pages through this path; modern TLS/browser compatibility
is still limited. The mode, radio and saved Palm network profile persist, while
live host connections close on resume; reload or reconnect in the Palm app.

**File → Export Database…** exports one named Palm database into a ZIP, useful
for retrieving application logs when an open system database prevents a full
RAM export.

**Internet via host** remains available for a subset of 68k NetLib apps, along
with local echo and error fixtures. See [network setup and limits](peripherals.md#virtual-wi-fi-experimental).

**Attach Palm infrared keyboard** sends computer-keyboard input through an
emulated Palm keyboard accessory. Install and enable its Palm OS driver first,
then type in the Palm window. The attachment persists in the session. See
[keyboard setup](peripherals.md#computer-keyboard).

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
Palm TX only. See the [device profile inventory](device-profiles.md) for the
inherited models and staged support limits.

## Physical buttons

The TX window includes a button strip beneath its display: Home, Calendar,
five-way navigation with centre select, Contacts and Web. Click and hold
for a held button; releasing the mouse releases it. Moving off the button or
leaving, hiding or closing the window releases the pointer hold. Losing focus
also releases keyboard-held buttons. A keyboard key and mouse press can hold
the same button independently, and different buttons can form a chord.

The strip sends hardware keypad events, so Palm OS button assignments and app
handling apply. It remains usable when the infrared keyboard accessory is
attached. With that accessory detached, F1–F4 operate the four application
buttons, arrow keys navigate and Enter selects. The existing Escape power-key
shortcut remains available, but TX sleep/resume is incomplete; the strip does
not expose a Power button.
The same shortcuts are listed in **Help → Hardware Keyboard Controls**.

The strip stays beneath the display when the view rotates. Its clicks are
separate from touchscreen calibration. Normal and double-size displays include
the controls; startup uses normal size if the complete double-size window would
exceed the available screen height. Screenshots exported through the core's
framebuffer commands still contain only the Palm display.

## Audio scheduling

On a PipeWire desktop, check the sound server's scheduling as well as Frond's
log when investigating gaps:

```sh
systemctl status rtkit-daemon
ps -eLo pid,tid,cls,rtprio,comm | rg 'pipewire|data-loop|frond'
journalctl --user -u pipewire -u pipewire-pulse -u xdg-desktop-portal
```

PipeWire's active `data-loop` workers should receive real-time priority when
RTKit is available. A running RTKit service alone does not prove that requests
from the audio server succeed.

If the desktop portal reports `Could not get pidns` during priority requests,
PipeWire supports using RTKit directly. Put this override in
`~/.config/pipewire/pipewire.conf.d/90-direct-rtkit.conf` and
`~/.config/pipewire/pipewire-pulse.conf.d/90-direct-rtkit.conf`:

```conf
module.rt.args = {
    rtportal.enabled = false
}
```

The same override in `~/.config/pipewire/client.conf.d/90-direct-rtkit.conf`
applies to PipeWire clients. Restart the user audio services, which briefly
interrupts audio in other apps, then check the actual worker priorities:

```sh
systemctl --user restart pipewire pipewire-pulse wireplumber
```

This uses the existing broker permissions and CPU limits. Remove the override
files and restart those services to restore portal selection. See PipeWire's
[RT module options](https://docs.pipewire.org/page_module_rt.html).

## Build and package

```sh
sudo apt install git build-essential python3 cmake qt6-base-dev qt6-wayland \
  libsdl2-dev libsdl2-image-dev libreadline-dev libcurl4-openssl-dev mtools
make -j4
make test
make test-desktop
make deb
```

`out/frond-desktop` finds `out/frond` beside it. The installed launcher uses
`/usr/lib/frond/frond-core`. For development, `--core /absolute/path/to/frond`
selects another binary. `frond-desktop /path/to/file.frond` opens that session;
`--version` reports 0.2.0. `make out/frond` builds only the SDL emulator.

The Debian package derives shared-library dependencies from the binaries and
includes Qt's platform and Wayland plugins. It contains no firmware, PRC,
session or test capture. Audio and display use the user's normal desktop session;
no global environment overrides are installed.

## Control and persistence tests

`make test-desktop` requires `mtools`. Its firmware-free exchange tests cover
32/64/128 MiB FAT16 copies, binary bytes, long filenames, case and alias
collisions, existing host destinations and symlinks, full cards, subprocess
failure rollback, recovery-save/export/insertion failures and cancellation.

`make test-desktop` also tests the private socket client, partial frames, Unicode file
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

## Adding device control profiles

Physical controls use the native core's `DeviceControls.h` profiles. Each profile
owns its layout, labels, icon choices and hit regions; drawing and pointer handling share that
geometry. A new model should supply its own control arrangement and map presses
to its actual keypad/GPIO inputs. The TX profile is the initial implementation.
Models without a profile retain their existing display-only window.

Keep the same approach as more devices are added: original vector artwork with
rounded, shaded controls and a device-like bezel; device-specific button counts,
navigators and keyboards; visible pressed states; and reliable release on focus
loss, hiding, closing and rotation. Preserve the touchscreen's coordinate space,
normal/double-size rendering and framebuffer-only exports. Test shared holds,
button chords and the supported view rotations for each profile.

Use independently created artwork and Frond styling. Do not copy another
emulator's skin or bundle manufacturer photographs or branding as UI assets.
Original artwork does not by itself settle trademark or registered-design
questions for an exact product replica.
