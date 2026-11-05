# Using Frond

How to run Palm machines in Frond on Linux and Android. The two apps have the same features, so this covers both and says where they differ. Install and firmware sizes are in the [README](../README.md). For command line options, console commands and the control socket see [developer-reference.md](developer-reference.md), for tests [testing.md](testing.md) and for speed and audio diagnostics [performance.md](performance.md).

## Firmware and machines

Each Palm you run is a machine on the Devices list (Home on Android). Frond works out the device from the NOR image and never changes your files.

- **Firmware…** (File menu on Linux, the menu on Android) imports a NOR image and, where the device has one, a NAND image. **Add device** does the same and then starts a new machine. Device can detect the model from the NOR image, or you can choose it. A NOR image that belongs to another device than the one chosen is refused. The images are copied into `~/.local/share/frond/firmware/<device>/` on Linux, so you can move the originals.
- The first start runs Palm OS setup. If taps land in the wrong place, calibrate the screen in Prefs.
- **Edit settings…** changes a machine's display and sound settings while it's stopped. Cancel keeps the old ones. Two machines of the same model can have different settings.
- **Rename…**, **Duplicate**, **Export…** and **Delete** are in the row's menu (right-click or the … button on Linux). **Import session…** in the File menu (Import session in the Android menu) brings an exported machine back. A row shows the device, when it was saved and its size. "Saved by an older Frond" rows can only be deleted.
- **Hard reset** erases everything on the Palm. **Save first** in its prompt saves the machine before erasing. The T|X and E2 then ask "Erase all data?", press up to erase. The Zire 72 erases at once and starts at calibration.
- **Reset** (a soft reset) is next to Rotate.

On Linux each machine is a folder, `~/.local/share/frond/sessions/<name>/`, with the session (`<name>.frond`), its settings (`<name>.frond.settings.json`), a lock file and the card files (`sd.frondsd`, `ms.frondms` or `ms.frondmsp`). A machine that was never saved has a `<name>.frond.new` marker instead of the session. Export writes the same folder, so keep the settings with it when you move it. Android keeps the same folders in the app's storage. A machine is locked while it's open, so two windows can't overwrite it.

A session has your firmware, installed apps and personal data in it. Keep it to yourself.

## Running a machine

On Linux the window shows the running machine's controls instead of the list, and the title is its name. Double-click a row, press Enter or click **Start** to start it.

- **Save** (Ctrl+S) saves the machine. **Save a copy…** writes a separate snapshot and leaves the machine as it is.
- **Save and Stop** saves and goes back to the list. Closing the window saves too, and the next launch resumes the last machine if **Resume last session on startup** is on (File → Settings…). On Android, leaving the machine saves it.
- If saving fails Frond stays open and offers Retry, Cancel or Discard. There is no autosave, so a crash or power loss loses everything since the last save.
- **Emulator log** shows the core's messages. On Android it has a Copy button.
- **Force Stop…** shows up if the emulator stops answering (Linux only). It stops without saving.
- **Rotate** turns the view, and the button bar stays under the screen.

### Apps

Under Applications (Linux) or the Peripherals screen (Android):

- **Install application** takes PRC and PDB files, once Palm OS has started.
- **Launch application** starts an installed app by its Palm database name.
- **Export RAM databases** writes a ZIP of everything in RAM, other apps and your data included.
- **Export database** writes one named database. Use it when an open system database, like `Shim Logs`, stops the full export.

Close Palm dialogs first if an install or export can't get through.

## Settings

Set these when you add a machine, or in Edit settings while it's stopped. On Android they're in the machine's menu while it runs too.

| Setting | What it does |
|---|---|
| Size (Linux) | 1× to 2× in quarter steps. It starts at the largest that fits the screen. |
| Size (Android) | Fit, Whole steps or Fill. |
| Smooth pixels | Softens the LCD. Pixels are sharp by default. |
| Show keyboard, Jog controls | TG50 only. Jog controls are Hidden, Left of screen or Left of keyboard. |
| Sound | Turns Palm sound on or off. It can also be switched on the running page. |
| Low-latency audio | Less delay between a tap and the sound. See below. |
| Sony audio preview | The TG50's version of the above. |
| Engine | Exact or Fast. It's hidden: on Linux set `FROND_ENGINE_SETTING=1`, on Android it shows in debug builds. See [performance.md](performance.md). |

Android also has Settings in the home menu: Orientation (follow the phone, portrait or landscape), the gdb server and its port, and the controller keys.

The button bar and the silkscreens of the E2, Zire 72 and TG50 are drawn at the chosen size.

### Low-latency audio

Off by default. It plays one open 44.1 kHz stereo stream with less delay, for music apps like SunVox, and goes back to normal audio when your computer or phone can't keep up, which can pause the sound for a moment. Set it before starting the machine. It's greyed out where the device or firmware isn't supported. Anything else, like several streams or other formats, uses normal audio. Diagnostics are in [performance.md](performance.md).

## Buttons, keys and controllers

Click or tap the buttons under the screen and the silkscreen areas. Click and hold to hold a button, and chords of different buttons work. Holds are released when the window loses focus.

Keys, with the infrared keyboard detached:

| Key | T\|X | E2 | Zire 72 | TG50 |
|---|---|---|---|---|
| Arrows, Enter | five-way, select | five-way, select | five-way, select | scroll rocker |
| F1 | Home | Calendar | Calendar | Date Book |
| F2 | Calendar | Contacts | Contacts | Address |
| F3 | Contacts | Tasks | Music | To Do |
| F4 | Web | Note Pad | Camera | Memo Pad |
| F5 | | | Voice button (hold to record) | Voice button |
| F6 | | | | Home |
| Escape | Power | Power | Power | Power |

On the TG50 type with the computer keyboard, left Alt is Blue and right Alt is Red. Its jog wheel, jog press and Back are on the side tab. Help → Hardware Keyboard Controls lists the same keys.

**Hold** (TG50) latches the device's Hold switch, which turns off the screen and controls, and audio keeps playing. It's saved with the machine.

On Android a game controller presses the Palm's buttons. The d-pad and left stick are the five-way, A is select, L1, X, Y and R1 are the four app buttons and Select is Menu. To change them go to Settings → Controller → Set controls, tap a Palm button, then press the controller key. Reset to defaults puts them back.

## Storage cards

The T|X, E2 and Zire 72 take an SD card, the TG50 a Memory Stick. One card is kept per machine, saved with it.

| | T\|X | E2 | Zire 72 | TG50 |
|---|---|---|---|---|
| Card | SD | SD | SD | Memory Stick, Memory Stick Pro |
| New card sizes | 32 MB to 2 GB | 32 MB to 1 GB | 32 MB to 2 GB | 4 to 128 MB, Pro 256 MB to 2 GB |
| Images it takes | up to 2 GB | up to 1 GB | up to 2 GB | `.frondms`, Pro as raw `.img` |

SDHC isn't supported, so 2 GB is the top. A new 2 GB card is 1886 MB, like a real one, and Palm OS formats it. Cards over 128 MB are made in the machine's folder and need room for the whole card, with progress and Cancel. New cards are blank, so let Palm OS format them. On the TG50 a blank stick gets Palm OS's Unrecognized Format prompt, and a Pro stick needs a saved machine.

Controls (SD card or Memory Stick section, only the ones that fit the state show):

- **Eject** and **Insert** take the card out and put it back. Close files on the Palm before ejecting, as Frond can't flush them.
- **Export image** (once ejected) writes `.img`, or `.frondms` for a classic stick. **Insert image** and **New card** replace the card and discard an ejected one, so export first.
- **Write protected** (Memory Stick) is set while ejected.
- **Files** copies files in and out.

Don't eject or sleep the Palm while an app is writing quickly, as the SD driver can get stuck until you reset. FAT32 cards imported from `mkfs.fat` can be read but not written by the T|X.

### Files

SD cards: Linux needs `mtools` (`sudo apt install mtools`). Files saves a recovery session, ejects the card, and opens a dialog where you list a card folder, make a folder, copy a host file to the card and copy a card file to the host. Existing names, including ones that differ only in case, are refused, and there is no replace or delete. **Apply and Reinsert** puts the edited card in, Cancel keeps the original card ejected (use Insert). Cards for Files are FAT16, 32 MB and up. The recovery session is kept under `~/.local/share/frond/sd-exchange/` until you delete it, and has your whole Palm in it. Android does the same from Files under SD card.

Memory Stick: Files works on the stick's own Palm OS filesystem, with no ejecting. Open Date Book on the Palm and try again if Sony Launcher is busy. You can add and export files, make folders, rename and delete. Changes happen at once. Names must be printable ASCII, paths at most 511 bytes, each transfer at most 8 MiB, and a folder lists at most 512 entries. Existing names are refused. Cancel stops a pending operation. Applications in `/PALM/Launcher` show in Sony Launcher's Card category. A protected stick can be read and exported only.

## Connections

Each device only gets the connections it really has. A control it doesn't have yet is greyed out, with "Not available on the <device> yet".

### Endpoints

Serial, USB HotSync and infrared beaming each have an endpoint setting:

- Off (the default).
- Terminal (PTY) on Linux, where Frond prints a `/dev/pts/N` path for a terminal program or pilot-link.
- Wait for a peer listens on a port on 127.0.0.1 on Linux (all addresses on Android, which shows the phone's address) and takes one client at a time.
- Connect to an address (Android) joins a peer that is waiting.

Linux keeps the choice between runs. A status line shows the endpoint and whether a peer is connected. Changing the choice while the machine runs closes the old connection. Nothing from the host is saved in a session, and reset, true sleep and loading a session close the connection.

Android has no PTYs, so its endpoints go over the network, to another Frond or to a link tool on a computer:

- `tools/link-irda.py` joins two TCP endpoints. Each is a local port, `HOST:PORT`, or `listen:PORT`.
- `tools/link-pty.py` puts a PTY in front of a port for pilot-link and terminals.
- `tools/link-bluetooth.py` shares one `btvirt` (see Bluetooth).

Default Android ports are 5030 for infrared, 5032 Bluetooth, 5033 serial and 5034 USB HotSync. A relay waits again after its peer goes, and Connect again retries.

### Network

Network is the T|X only. Choose a mode before opening a network app:

- Virtual Wi-Fi (Frond): turn on Wi-Fi in Palm OS, scan and connect to the open **Frond** network. A virtual router gives it an address, DNS and IPv4 through the computer's or phone's connection, with no root, bridge or Wi-Fi adapter needed. The router is 10.0.2.2, so use that for a service on your computer. Blazer loads HTTP pages, but its old HTTPS is refused by most sites. For Network HotSync run `pilot-xfer -p net:127.0.0.1 -l` and enter 10.0.2.2 as the PC address in the Palm's HotSync setup.
- Internet via host: forwards some 68k NetLib calls to host sockets. Blazer's Wi-Fi workflow doesn't work this way, use Virtual Wi-Fi.
- Simulated echo server, Simulated connection timeout and Simulated disconnection are fixtures for testing apps. The echo server answers on `frond.test` port 7.

The mode is saved. Live connections close when the machine resumes, so reload or reconnect in the app.

### Serial port

The cradle serial port goes to a PTY or a TCP port. Open the path with `screen` or `minicom`, or connect to the port, for example `nc 127.0.0.1 5023`. Peers get CTS, DSR and DCD while connected, RTS flow control is honoured, and bytes sent with no peer are dropped. On Android use Wait for a peer or Connect to an address, then `link-pty.py PHONE:5033 --link /tmp/palm-serial` on a computer.

### USB HotSync

Plugs the Palm's USB port into the computer. Start HotSync on the Palm (Local, Cradle/Cable), then run pilot-link, for example `pilot-xfer -p /dev/pts/7 -l`. The status line shows the bus step, and "enumerated" means it's ready. HotSync takes 15 to 20 seconds to start. pilot-link only opens serial devices, so for a TCP port put a PTY in front with `tools/link-pty.py` (a plain `socat` PTY doesn't work). Changing the setting or loading a session unplugs the cable and plugs it back in.

### Infrared

Infrared beaming links two Frond machines. Detach the infrared keyboard first, as they share the port. Put each machine on Wait for a peer with a different port, then join them:

```sh
python3 tools/link-irda.py 5030 5031
```

Use Palm OS's normal Beam command. Both Palms must be awake and the receiver needs Beam Receive on. For two Fronds on different machines use `HOST:PORT` or `listen:PORT` in the link tool. On Android, Infrared beaming has Wait for a peer and Connect to another Frond, so two phones need nothing else. The relay closes when either side goes, and `link-irda.py` reconnects when a Palm wakes. Beaming is off each time the emulator starts.

Infrared keyboard (Attached) makes your keyboard a Palm Universal Wireless Keyboard. Install Palm's keyboard driver (version 1.13, not included), reset if it asks, open Keyboard on the Palm and turn it ON. Then type in the Palm window. Left Alt is Fn and right Alt is AltGr, and the layout follows physical US keys, so pick the matching one in the driver. F1 to F4 and Escape keep their button and power uses. The attachment is saved with the machine.

### Bluetooth

Bluetooth is off by default, and uses the original Palm Bluetooth stack against a virtual controller from `btvirt`, which you start yourself. Build it once from a source checkout (needs Git, a C toolchain, autotools, pkg-config and the GLib, D-Bus and readline headers):

```sh
python3 tools/build-bluetooth-provider.py --fetch
install -d -m 700 build/bluetooth-sockets
build/bluetooth-provider/emulator/btvirt -s"$PWD/build/bluetooth-sockets"
```

Turn Bluetooth On, enter the absolute path of `build/bluetooth-sockets/bt-server-bredr`, then turn Bluetooth off and on again in Palm OS (Sony Preferences on the TG50). The status shows `radio=on`, the link state, `provider connected` and the number of open ACL links. Set Discoverable to Yes on the receiving Palm. Each Frond gets its own virtual controller, and only Fronds on the same `btvirt` can see each other. Keep the socket private to your user. Frond never touches a real Bluetooth adapter.

Over the network, one machine runs `btvirt` and the others join it:

```sh
python3 tools/link-bluetooth.py serve "$PWD/build/bluetooth-sockets/bt-server-bredr" listen:5032
python3 tools/link-bluetooth.py join 192.168.1.20:5032 "$PWD/build/bt-join"
```

Then start Frond with `--bluetooth "$PWD/build/bt-join"`. On Android, the Bluetooth row has Host other Fronds (runs `btvirt` in the app and shows the phone's addresses) and Join another Frond.

Identities: the T|X's address comes from its image, so two clones of one T|X image look like the same device. Use images with different addresses, or mix models. The E2, Zire 72 and TG50 are given a new address each time they connect, so trusted devices don't carry over. The Zire 72 makes one up the first time and keeps it in the session, so don't copy a session to run two.

Tested: discovery, sending a Memo or file by Bluetooth (OPP), PIN and Trusted Devices on all four, and serial (SPP) on the T|X, E2 and TG50. TG50 payload transfers aren't checked on Android. Bluetooth networking and other profiles aren't supported.

## Availability and limits

| | T\|X | E2 | Zire 72 | TG50 |
|---|---|---|---|---|
| Sound | yes | yes | yes | yes, see below |
| Low-latency audio | yes | yes | | Sony audio preview |
| Microphone | | | | yes |
| Network | yes | | | |
| Serial port | yes | | | yes |
| USB HotSync | yes | yes | yes | yes |
| Infrared beaming | yes | yes | yes | yes |
| Infrared keyboard | yes | yes | yes | yes |
| Bluetooth | yes | yes | yes | yes |
| Camera | | | no sensor | |
| Hold | | | | yes |
| Card | SD | SD | SD | Memory Stick, Pro |

The Zire 72's Camera shows a white screen and goes back to the launcher. The TG50 plays beeps, alarms and Sony Voice Recorder audio. Standard Palm SndStream (not in its firmware), the remote control, MagicGate and Memory Stick audio aren't there. Serial works at 9600, 57600 and 115200 baud. HotSync over the serial port isn't tested.

Microphone (TG50) is off each time it starts. Turn it on under Device while the machine runs. Android asks for permission the first time and arms it when you return to the device. Saving, file work, stopping, reset, sleep and leaving the app turn it off again. On Linux, hiding the device window does too.

## Troubleshooting

- Gaps in sound on a PipeWire desktop: check that PipeWire's real-time threads get priority. `systemctl status rtkit-daemon` and `ps -eLo pid,tid,cls,rtprio,comm | grep -E 'pipewire|data-loop|frond'` show it.
- If `journalctl --user -u pipewire -u xdg-desktop-portal` reports `Could not get pidns`, tell PipeWire to use RTKit directly. Put `module.rt.args = { rtportal.enabled = false }` in `~/.config/pipewire/pipewire.conf.d/90-direct-rtkit.conf`, `~/.config/pipewire/pipewire-pulse.conf.d/90-direct-rtkit.conf` and `~/.config/pipewire/client.conf.d/90-direct-rtkit.conf`, then run `systemctl --user restart pipewire pipewire-pulse wireplumber`, which briefly interrupts audio in other apps. Remove the files and restart to undo it.
- Low-latency audio drops back to normal on starvation until the next reset. Use normal audio for projects that need a bigger queue.
- A card that stops responding after an eject or sleep during writes: reset the Palm. The card image is intact.
- Files on Linux says to install mtools: `sudo apt install mtools`.
- "This session is open in another Frond": close the other window, or it's still running.
- Diagnostics and logs for bug reports: Emulator log, and [performance.md](performance.md) for speed and audio traces.
