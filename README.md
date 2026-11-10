# Frond

Frond is a Palm OS emulator for Linux and Android, for running and writing Palm apps and games.

Currently it runs the Palm T|X, Tungsten E2 and Zire 72, the Palm IIIx, and the Sony CLIÉ TG50, with sound, memory cards, infrared, Bluetooth, HotSync, networking and saved sessions. Some peripherals are still in progress and more devices are on the way - check the peripheral table below to see what is currently supported.

It's built on [CloudpilotEmu](https://github.com/cloudpilot-emu/cloudpilot-emu) and [uARM](https://github.com/uARM-Palm/uARM). You need your own firmware for each device, none is included.

How it differs is by focussing on a couple of goals that are important to me - simulating peripherals to emulate the experience of actually using these devices and emulating them fast enough that they can be enjoyed on less than stellar hardware. These goals are achieved by simulating each offered connection and peripheral (work in progress) and finding ways to increase the speed of emulation - like the [dynarmic](https://github.com/lioncash/dynarmic) based JIT compiler that was added in 0.11.0. 

You can run multiple devices on one computer (Linux only for now) - or multiple phones - and link them together over virtual infrared or Bluetooth, connect to the internet with a virtual modem or simulated WIFI connection, install an IR keyboard driver and work on your screenplay, etc. This project is fueled by nostalgia and I want to play with the things I remember, as I remember them, while also trying things I knew of back then but never saw in person.

Let me know what works, what doesn't, and what you'd like in #frond [on my discord](https://discord.gg/9Wun47jGC6) - or open an issue here.

Enjoy,<br>
Dan

---

## Screenshots

<table>
  <tr>
    <td align="center"><img src="doc/images/android-home.png" width="200" alt="The Frond home screen listing the T|X, E2, Zire 72 and TG50" /><br>Devices</td>
    <td align="center"><img src="doc/images/android-tx.png" width="200" alt="The Palm T|X favourites launcher with its button bar" /><br>Palm T|X</td>
    <td align="center"><img src="doc/images/android-e2.png" width="200" alt="Calculator on the Tungsten E2, with its silkscreen and buttons" /><br>Tungsten E2</td>
  </tr>
  <tr>
    <td align="center"><img src="doc/images/android-zire72.png" width="200" alt="The Zire 72 agenda view over its photo wallpaper, with silkscreen and buttons" /><br>Zire 72</td>
    <td align="center"><img src="doc/images/android-tg50.png" width="200" alt="The CLIÉ TG50 launcher with its jog bar and keyboard" /><br>CLIÉ TG50</td>
    <td align="center"><img src="doc/images/android-peripherals.png" width="200" alt="The peripherals screen with the SD card, infrared, Bluetooth, serial, USB and network" /><br>Peripherals</td>
  </tr>
</table>

All six are from Android.

---

## What's in it

### Sessions

Each Palm you run is a session on the home screen, where you can rename, duplicate, export, import and delete them. There's no autosave, so a crash loses anything since the last save. On Linux closing the window saves it, Save and Stop goes back to the list and Save a copy… keeps a separate snapshot. Sessions have your firmware and personal data in them, so keep them to yourself.

Hard reset erases the Palm, and Save first saves it before you do. The TX and E2 ask first (press up to erase), the Zire erases straight away.

Each machine has its own display, smoothing and sound settings. Set them when you add it, or with Edit settings while it's stopped. On Android they're in the menu while it runs too.

### Screen

On Linux pick a scale from 1× to 2× in 0.25 steps, it starts at the largest that fits. On Android the screen fits the phone, in whole steps or filling it, and follows the phone or stays in portrait or landscape. Pixels stay sharp, or turn on Smooth pixels to soften them.

### Buttons

The buttons under the screen and the E2's and Zire's silkscreens can be clicked or tapped. On a keyboard the arrow keys are the five-way, Enter selects and F1 to F4 are the app buttons. Help → Hardware Keyboard Controls lists the rest. On Android game controllers press the Palm's buttons, and you can change which does what in Settings.

The TG50 has its keyboard, jog wheel, Home, Back and Hold. Hold locks the controls and turns off the screen, and audio keeps playing.

You can also type with an emulated Palm infrared keyboard, if Palm's keyboard driver is installed.

### Apps and cards

Install PRC and PDB files with Install…, in the Applications panel on Linux or sheet on Android. Launch, Export and Delete are on each app's row.

Frond makes and imports SD card images up to 2 GB (1 GB on the E2), and copies files between your computer or phone and an ejected card with Files, under SD card. Close any files on the Palm before ejecting.

The TG50 takes a Memory Stick from 4 to 128 MB, or a Memory Stick Pro from 256 MB to 2 GB. Files copies files and folders to and from it, up to 8 MiB each, and Sony Launcher runs apps from its Card category. See [Storage cards](doc/using-frond.md#storage-cards).

### Peripherals

| | T\|X | E2 | Zire 72 | TG50 | IIIx |
|---|---|---|---|---|---|
| Virtual Wi-Fi, onto the internet through your computer or phone | yes | | | | |
| Cradle serial port, to a PTY or TCP port for terminal programs and [HotSync](doc/using-frond.md#palm-iiix-serial-and-ppp) | yes | yes | | yes | yes |
| HotSync button in the Connections panel, like the cradle's | yes | yes | | | yes |
| Microphone | | | | yes | |
| USB HotSync over a PTY or TCP port | yes | yes | yes | yes | |
| [Infrared beaming](doc/using-frond.md#infrared) between Frond machines | yes | yes | yes | yes | yes |
| [Virtual Bluetooth](doc/using-frond.md#bluetooth), with a local provider you start yourself | yes | yes | yes | yes | |

Anything else stays greyed out until it's done, like the E2's and Zire's networking and the Zire's camera and microphone. The Zire 72 has no serial port, as on the real one.

The Palm IIIx (Palm OS 3.1, 68k DragonBall EZ) runs in its own runner, `frond-68k`. It has sound, backlight, serial with the HotSync button, infrared and the screen contrast, and no SD, USB, Bluetooth or Wi-Fi, as on the real one. It only has the exact engine (no Fast), at its real 16.58 MHz or Uncapped. Networking is PPP over the serial port, see [Palm IIIx serial and PPP](doc/using-frond.md#palm-iiix-serial-and-ppp). Android has it too, and the debugger works with prc-tools' gdb, see [Palm IIIx](doc/developer-reference.md#palm-iiix-frond-68k).

The infrared keyboard and beaming share the port, so take the keyboard off before turning beaming on. On Android infrared, Bluetooth, serial, USB HotSync and the gdb server go over the network, to another Frond or to `tools/link-irda.py`, `tools/link-bluetooth.py` and `tools/link-pty.py` (for pilot-link) on a computer.

The TG50 plays beeps, alarms and Sony Voice Recorder audio. Its microphone is off each time it starts, and saving, stopping or going to the background turns it off again. Android asks for permission the first time. Bluetooth is tested with serial (SPP) and object exchange. Its early Palm OS 5 has no SndStream, as on the real one, so apps like SunVox need Modern Clie Audio 2 installed first. The remote control, MagicGate and Memory Stick audio aren't there yet.

### Low-latency audio

Off by default. On the T|X and E2 it plays one 44.1 kHz stereo stream with less delay, for music apps like SunVox, and drops back to the normal path when your computer or phone can't keep up. On the TG50, and a T|X or E2 with other firmware, it keeps a shorter audio queue while sound plays, about 35 ms less delay. See [Low-latency audio](doc/using-frond.md#low-latency-audio).

### For developers

- The [developer runner](doc/developer-reference.md#developer-runner) installs and launches apps, exports RAM databases and fetches logs from the command line.
- The [debugger](doc/developer-reference.md#gdb-and-pace-monitor) works with GDB: ARM and Thumb stepping, breakpoints, PACE inspection and 68k stepping, no watchpoints yet. On Android turn on the gdb server in Settings.
- [Compatibility tests](doc/testing.md#real-firmware-checks) check apps launch, take input, keep their data and restore.
- [Input and audio timing](doc/performance.md#input-and-menu-latency) tools measure latency.

### Documentation

- [Using Frond](doc/using-frond.md) covers machines, settings, keys, cards and connections.
- [Developer reference](doc/developer-reference.md) has the command line, console, control socket, replay files, GDB and environment variables.
- [Testing](doc/testing.md) lists the test levels and what each one needs.
- [Performance](doc/performance.md) covers the engines, benchmarks and latency tools.
- [Adding a device](doc/adding-a-device.md) explains profiles, bring-up and button bar art.

---

## Installing

### Android

Android 8 or newer. Add [my F-Droid repo](https://hunke.ws/fdroid/repo?fingerprint=80438B253C257BCCE05CDCB9E3AC9B6174C2250659962B14FCBE7F32FD42D53E), then search for Frond. F-Droid offers new versions as updates.

Or get the APK from [Releases](https://github.com/roge-rm/frond/releases).

### Linux

On Debian 13 (Trixie) and the 64-bit Raspberry Pi OS based on it, the easiest way is my apt repo, so it updates with the rest of the system. Add it once:

```
sudo curl -fsSLo /etc/apt/keyrings/hunke.asc https://hunke.ws/apt/hunke.asc
echo "deb [signed-by=/etc/apt/keyrings/hunke.asc] https://hunke.ws/apt stable main" | sudo tee /etc/apt/sources.list.d/hunke.list
sudo apt update
```

Then `sudo apt install frond`. It shows up in the application menu.

Or get the `.deb` for your machine, amd64 or arm64, from [Releases](https://github.com/roge-rm/frond/releases) and install it with `sudo apt install ./frond_*.deb`.

### Firmware

| Device | NOR | NAND |
|---|---|---|
| Palm TX | 16 MiB | 132 MiB raw dump, with spare bytes |
| Tungsten E2 | 8 MiB | 34,603,008 bytes raw dump, optional |
| Zire 72 | 14,942,208 bytes | none |
| Sony CLIÉ TG50 | 13,041,664 bytes | none |
| Palm IIIx | one ROM file, 1.2 to 1.7 MB | none |

The IIIx takes the OS 3.1 ROM (retail or debug), a generic OS 3.5 EZ ROM (retail or debug), or the IIIx/V OS 4.1 ROM.

Frond works out the device from the NOR and never changes your files. On Linux use File → Firmware… or Add device, on Android import them when you add a device. Go through Palm OS setup the first time, and if taps land in the wrong spot, calibrate the screen in Prefs.

## Building

On Debian 13 (Trixie):

```sh
sudo apt install git build-essential python3 cmake ninja-build libboost-dev qt6-base-dev qt6-wayland \
  libsdl2-dev libsdl2-image-dev libreadline-dev libcurl4-openssl-dev libslirp-dev pkg-config mtools
git clone --recurse-submodules https://github.com/roge-rm/frond.git
cd frond
make -j4
./out/frond-desktop
```

`make deb` builds a Debian package in `out/` instead, which also needs `dpkg-dev` and `binutils`. `make appimage` and `make appimage-arm64` build AppImages in Docker that run on most distributions from 2023 on (glibc 2.36). Neither the packages nor the APK have firmware or Palm apps in them.

The Android app is in `android/` and builds with Gradle (`./gradlew assembleDebug`).

The upstream core is pinned in `vendor/cloudpilot`, and the patches in `patches/` are applied to it in order in `build/source`. See [Testing](doc/testing.md).

## Testing

These need no firmware:

```sh
make test
QT_QPA_PLATFORM=offscreen make test-desktop
make test-build-tools check-reconstruction
```

---

## Licence

Frond is free software under the GNU General Public License, version 3 or later. See [LICENSE](LICENSE).

It's built on [CloudpilotEmu](https://github.com/cloudpilot-emu/cloudpilot-emu) (its execution core, PACE support and SDL frontend) and Dmitry Grinberg's [uARM](https://github.com/uARM-Palm/uARM) TX board model. Thanks to their authors.

Palm firmware and apps have their own licences and you need to supply them yourself.
