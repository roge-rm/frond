# Frond

Frond runs Palm OS 5 on Linux, with a Palm TX on your desktop. It's built on
[CloudpilotEmu](https://github.com/cloudpilot-emu/cloudpilot-emu) and
[uARM](https://github.com/uARM-Palm/uARM), with a Qt launcher and TX hardware,
sound and input support.

It's early, at version 0.3.0. You'll need your own Palm TX firmware; none is
included. The launcher and Palm display use separate windows. Sound can break
up when the host is busy, and some menus are slow.

<img src="doc/images/tx-sunvox-editor.png" width="240" alt="SunVox running on the emulated Palm TX" /> <img src="doc/images/tx-simcity-zone.png" width="240" alt="Building a city in SimCity on the emulated Palm TX" />

## Build and run

On Debian 13 (Trixie):

```sh
sudo apt install git build-essential python3 cmake qt6-base-dev qt6-wayland \
  libsdl2-dev libsdl2-image-dev libreadline-dev libcurl4-openssl-dev libslirp-dev pkg-config mtools
git clone --recurse-submodules https://github.com/roge-rm/frond.git
cd frond
make -j4
./out/frond-desktop
```

Choose your TX NOR and NAND images and click **Start**. Frond expects a 16 MiB
NOR and a 132 MiB raw NAND dump, including spare bytes. It leaves the original
images unchanged. Complete Palm OS setup, then use **File → Install Application**
to install PRC or PDB files. If taps land in the wrong place, calibrate the
screen in Palm Preferences.

Close either window to save the session. Frond can resume it on the next launch;
**Save and Stop** returns to the launcher, and **Save a Copy** creates a separate
snapshot. Sessions contain firmware and personal data, so keep them private.
There is no periodic autosave; a crash can lose changes since the last save.

The buttons below the Palm screen are clickable. With the Palm infrared keyboard
detached, the arrow keys control the five-way navigator, Enter selects, and F1–F4
operate the application buttons. See **Help → Hardware Keyboard Controls** for
the mappings. You can also attach an emulated Palm infrared keyboard for typing;
it requires a separately supplied Palm driver.

The **SD Card** menu creates and imports card images. **Exchange Files…** copies
files between the host and an ejected card without root access. Close files on
the Palm before ejecting. Virtual Wi-Fi provides Internet access through your
computer without a bridge or physical Wi-Fi adapter. The **Serial port** and
**USB** settings connect the TX's cradle serial port and USB HotSync to a PTY or
a TCP port on your computer, for terminal programs and pilot-link. Bluetooth and
infrared beaming are not supported.

See the [desktop guide](doc/desktop.md) for sessions and controls, the
[peripheral guide](doc/peripherals.md) for SD cards, networking, USB HotSync and
keyboard setup, and the [serial guide](doc/tx-serial.md) for the serial port.

## Debian package

To build and install a package locally (`dpkg-dev` and `binutils` are also needed):

```sh
make deb
sudo apt install ./out/frond_0.3.0-1_amd64.deb
```

The package adds Frond to Plasma's application menu. It does not include firmware
or Palm applications.

## Development

```sh
make test
QT_QPA_PLATFORM=offscreen make test-desktop
make test-build-tools check-reconstruction
```

The tests above need no Palm firmware. With your own firmware, the
[developer runner](doc/developer-run.md) supports application installation,
launch, RAM database export and log retrieval, including during qualified TX
native UI waits. The [compatibility tests](doc/compatibility.md) include fresh-token
SDK probes with real form interaction and a repeatable private application matrix
that separates launch, interaction, persistence and restore observations.

The [ARM and PACE debugger](doc/debugger.md) provides ARM/Thumb stepping and
breakpoints, PACE inspection and explicit 68k stepping validated with real GDB
and a TX session. Foreground-app attribution is restricted to the qualified TX
UI-root wait; offline symbol reports require matching executable, PRC and live
resource bytes. General code ownership and watchpoints remain unsupported.
Tools for [measuring input and audio timing](doc/input-latency.md) are also available.

[Lifecycle tests](doc/session-testing.md) cover audio, SD transfers and local
network/UART ownership across sleep, reset and restore. Physical-device timing
remains unvalidated.

The upstream core is pinned in `vendor/cloudpilot`; ordered patches in `patches/`
are applied to `build/source`. See [build validation](doc/build-validation.md)
for reconstruction and package checks.

## Credits and licence

Frond uses CloudpilotEmu's optimized execution core, PACE integration and SDL
frontend, and Dmitry Grinberg's uARM TX board model. Thanks to their authors for
the work this builds on.

GPL-3.0-or-later, see [LICENSE](LICENSE). Palm firmware and applications have
their own licences and must be supplied separately.

Enjoy!<br>
Dan
