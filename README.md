# Frond

Frond is a Palm OS emulator for Linux and Android, made for running and developing Palm apps and games. 
Currently it emulates the Palm T|X and the Tungsten E2, with their buttons, sound, SD
card, sleep and wake, and saved sessions. More devices are planned.

To run this you will need to provide a Palm T|X or E2 device firmware as none are included.
This is an early release - 0.5.0 - but so far results are promising. 

Dan

<img src="doc/images/tx-sunvox-editor.png" width="240" alt="SunVox running on the emulated Palm TX" /> <img src="doc/images/tx-simcity-zone.png" width="240" alt="Building a city in SimCity on the emulated Palm TX" />

## Build

On Debian 13 (Trixie):

```sh
sudo apt install git build-essential python3 cmake qt6-base-dev qt6-wayland \
  libsdl2-dev libsdl2-image-dev libreadline-dev libcurl4-openssl-dev libslirp-dev pkg-config mtools
git clone --recurse-submodules https://github.com/roge-rm/frond.git
cd frond
make -j4
./out/frond-desktop
```

To build and install a Debian package instead (also needs `dpkg-dev` and
`binutils`):

```sh
make deb
sudo apt install ./out/frond_0.5.0-1_amd64.deb
```

The package adds Frond to the application menu. It doesn't include firmware or
Palm apps.

Or add my apt repo as shown on https://hunke.ws/apt/ and run `sudo apt install frond`.

## Firmware

| Device | NOR | NAND |
|---|---|---|
| Palm TX | 16 MiB | 132 MiB raw dump, with spare bytes |
| Tungsten E2 | 8 MiB | 34,603,008 bytes raw dump, optional |

Pick the device, or let Frond detect it from the NOR, choose the files and click
**Start**. Frond reads the files and never changes them. Go through Palm OS setup,
then use **File → Install Application** to install PRC and PDB files. If taps land
in the wrong place, calibrate the screen in Palm Preferences.

## Using it

The launcher and the Palm screen are separate windows. Closing either one saves
the session, and Frond offers to resume it next time. **Save and Stop** goes back
to the launcher and **Save a Copy** keeps a separate snapshot. There's no
autosave, so a crash loses what changed since the last save. Sessions hold your
firmware and personal data, so keep them private.

Choose a display scale from 1× to 2× in 0.25 steps before starting. Frond defaults
to the largest that fits your screen. LCD pixels stay sharp; **Smooth LCD scaling**
softens them if you prefer.
Enjoy!<br>
Dan

The buttons under the screen and the E2's silkscreen are clickable. On your
keyboard the arrow keys work the five-way, Enter selects and F1 to F4 are the app
buttons; **Help → Hardware Keyboard Controls** lists them all. On either device you can
also attach an emulated Palm infrared keyboard for typing, which needs Palm's
keyboard driver.

The **SD Card** menu makes and imports card images, and **Exchange Files…** copies
files between your computer and an ejected card. Close files on the Palm before
ejecting.

On the TX:

- Virtual Wi-Fi gets the Palm onto the internet through your computer, with no
  bridge or Wi-Fi adapter needed.
- **Serial port** connects the cradle serial port to a PTY or a TCP port, for
  terminal programs and pilot-link.

Both the TX and E2 support **USB HotSync** over a PTY or TCP port and
[infrared beaming](doc/infrared.md) between Frond instances. Detach the infrared
keyboard before enabling beaming. Both also support an opt-in
[virtual Bluetooth controller](doc/bluetooth-provider.md), using a separately
started local provider. Bluetooth defaults to off. The E2's networking and serial
controls stay greyed out.

Both devices also offer opt-in **Low-latency audio** for one 44.1 kHz
stereo stream on qualified firmware. It is off by default. Qualified basic ARM
callbacks can preview at the render-to-writer handoff; other supported streams use HAL interception.
Playback falls back to the normal path when the host cannot keep up. See the
[audio limits](doc/fast-audio.md).

More in the [desktop guide](doc/desktop.md), the
[peripheral guide](doc/peripherals.md) (SD cards, networking, USB HotSync,
keyboard), the [serial guide](doc/tx-serial.md) and
[E2 notes](doc/e2-qualification.md).

## Android

Frond also runs on Android 8 and up, on phones and tablets. It does the T|X and the E2, with saved sessions you can export and import, SD cards with file exchange, installing and launching Palm apps and the low-latency audio option. As with the desktop version, you bring your own firmware.

Virtual Wi-Fi and Internet via host work on the T|X. Infrared beaming, Bluetooth, the serial port, USB HotSync and the gdb server go over the network, to another Frond or to the desktop tools: `tools/link-irda.py`, `tools/link-bluetooth.py` and `tools/link-pty.py` for pilot-link.

To get it, add my F-Droid repo https://hunke.ws/fdroid/repo in your F-Droid client, and updates come with it.

## For developers

- The [developer runner](doc/developer-run.md) installs and launches apps, exports
  RAM databases and fetches logs from the command line.
- The [debugger](doc/debugger.md) works with GDB: ARM and Thumb stepping,
  breakpoints, PACE inspection and 68k stepping. Watchpoints aren't supported yet.
- [Compatibility tests](doc/compatibility.md) check apps for launch, interaction,
  saved data and restore.
- [Input and audio timing](doc/input-latency.md) tools measure latency.

Tests that need no firmware:

```sh
make test
QT_QPA_PLATFORM=offscreen make test-desktop
make test-build-tools check-reconstruction
```

The upstream core is pinned in `vendor/cloudpilot`, and the ordered patches in
`patches/` are applied to it in `build/source`. See
[build validation](doc/build-validation.md) and
[session testing](doc/session-testing.md).

## Credits and licence

Frond is built on [CloudpilotEmu](https://github.com/cloudpilot-emu/cloudpilot-emu)
(its execution core, PACE support and SDL frontend) and Dmitry Grinberg's
[uARM](https://github.com/uARM-Palm/uARM) TX board model. Thanks to their authors.

GPL-3.0-or-later, see [LICENSE](LICENSE). Palm firmware and apps have their own
licences and must be supplied separately.
