# Using Frond

How to run Palm machines in Frond on Linux and Android. The two apps have the same features, so this covers both and says where they differ. The Palm IIIx is a 68k machine and has its own notes under Settings, Buttons and Connections. Install and firmware sizes are in the [README](../README.md). For command line options, console commands and the control socket see [developer-reference.md](developer-reference.md), for tests [testing.md](testing.md) and for speed and audio diagnostics [performance.md](performance.md).

## Firmware and machines

Each Palm you run is a machine on the Devices list (Home on Android). Frond works out the device from the NOR image and never changes your files.

- **Firmware…** (File menu on Linux, the menu on Android) imports a NOR image and, where the device has one, a NAND image. **Add device** does the same and then starts a new machine. Device can detect the model from the NOR image, or you can choose it. The IIIx takes one ROM file (OS 3.1 retail or debug, a generic OS 3.5 EZ ROM, or the IIIx/V OS 4.1 ROM), shown as ROM image, and no NAND. A NOR image that belongs to another device than the one chosen is refused. The images are copied into `~/.local/share/frond/firmware/<device>/` on Linux, so you can move the originals.
- The first start runs Palm OS setup. If taps land in the wrong place, calibrate the screen in Prefs.
- **Edit settings…** changes a machine's display and sound settings while it's stopped. Cancel keeps the old ones. Two machines of the same model can have different settings.
- **Rename…**, **Duplicate**, **Export…** and **Delete** are in the row's menu (right-click or the … button on Linux). **Import session…** in the File menu (Import session in the Android menu) brings an exported machine back. A row shows the device, when it was saved and its size. "Saved by an older Frond" rows can only be deleted.
- **Hard reset** erases everything on the Palm. **Save first** in its prompt saves the machine before erasing. The T|X and E2 then ask "Erase all data?", press up to erase. The Zire 72 erases at once and starts at calibration.
- **Reset** (a soft reset) is next to Rotate.

On Linux each machine is a folder, `~/.local/share/frond/sessions/<name>/`, with the session (`<name>.frond`), its settings (`<name>.frond.settings.json`), a lock file, the card files (`sd.frondsd`, `ms.frondms` or `ms.frondmsp`) and, on a device with NAND, its internal NAND (`nand.frondnand`). A machine that was never saved has a `<name>.frond.new` marker instead of the session. Export writes the same folder, so keep the settings with it when you move it. Android keeps the same folders in the app's storage. A machine is locked while it's open, so two windows can't overwrite it.

The 6 MiB E2 de-NVFS image is imported as Tungsten E2, with 32 MiB RAM by default and no NAND file needed. Its databases live in RAM, so save the session to keep them. The NOR must have the rePalm/Dmitry.GR card header at 0x20000, with the card running to the end of the image. Power and automatic sleep are disabled for this firmware; it stays awake while running.

The Tungsten E3 uses the modified 8 MiB ROM on the E2 board, with 32 MiB RAM by default. NAND is optional. Its saved device name stays `palm-e2-franken`. The 320×480 screen includes Palm OS's input area and status bar, with no extra silkscreen below it. Home and Menu use the status bar whether the input area is open or closed. Frond keeps sleep disabled for this ROM, so the E3 has no Power control.

A session has your firmware, installed apps and personal data in it. Keep it to yourself.

## Running a machine

On Linux the window shows the running machine's controls instead of the list, and the title is its name. Double-click a row, press Enter or click **Start** to start it.

- **Save** (Ctrl+S) saves the machine. **Save a copy…** writes a separate snapshot and leaves the machine as it is.
- **Save and Stop** saves and goes back to the list. Closing the window saves too, and the next launch resumes the last machine if **Resume last session on startup** is on (File → Settings…). On Android, leaving the machine saves it.
- If saving fails Frond stays open and offers Retry, Cancel or Discard. There is no autosave, so a crash or power loss loses everything since the last save.
- **Emulator log** shows the core's messages. On Android it has a Copy button.
- **Force Stop…** shows up if the emulator stops answering (Linux only). It stops without saving.
- **Rotate** turns the view, and the button bar stays under the screen.

### Android bar, menu and sheets

On Android the controls sit in a bar along the top of the running machine. Taps outside it go to the Palm, and the machine keeps running while a menu or sheet is open.

- The bar has back (saves and leaves), the machine's name and engine, then Applications, Card, Sound, Power and the ⋮ menu. Card shows the card's state (in, out, ejected or none) and only appears on devices with a slot. Power only appears where the device has a power key. The keyboard button shows when the infrared keyboard is attached.
- ⋮ lists Hold (TG50), Infrared keyboard, Microphone and Network with their state, then Display…, Connections… and Screenshot, then Hide bar, Save a copy… and Emulator log, then Reset and Hard reset…. Rows the device doesn't have are left out. Screenshot saves a PNG of the Palm to Pictures/Frond.
- Applications, Card, Display and Connections open as a sheet over the bottom of the screen, or at the side in landscape, so the Palm stays in view. Tap outside it or press back to close it. Card Files… opens full screen.
- Display has the display settings, Rotate and Audio. Connections has infrared, Bluetooth, serial, USB HotSync, the debugger and HotSync.
- Hide bar leaves a small handle at the top edge. Tap it or swipe down on it to bring the bar back. Whether the bar shows is remembered for each machine. In landscape the bar is shorter.
- Top bar (in Edit settings and the Display sheet) is Auto-hide or Own space. Auto-hide puts the bar over the Palm and hides it after a few seconds without a touch. Own space moves the Palm down to leave room for it, in portrait and landscape. With little room left, the bar goes over the Palm instead.

### Linux bar and panels

On Linux the bar sits at the top of Frond's window while a machine runs, above a Machine menu with the same rows as ⋮ on Android (Hard reset… is red). The Palm is inside Frond's window under the bar (on X11, and on Wayland through XWayland). Applications, Card, Display and Connections open as a panel at the right of the Palm and the window grows to the right, so the Palm keeps its size. Hide bar leaves a thin strip with a tab at the top, and the choice is remembered for each machine. The Machine menu shows each row's keys. Display has Size, Smooth pixels, Show keyboard and Jog controls (TG50) and Speed (Palm IIIx), which change while the Palm runs. A click on the Palm gives it the keys and a click in a panel takes them back, wherever the pointer is when you type; switching to another window and back keeps the choice. If the Palm's window can't be embedded, it stays in its own window, with the bar and the panels in Frond's window. Ctrl+1 to Ctrl+4 open and close them, and so does the bar button or the panel's ✕.

F11 or the Full screen row in the Machine menu fills the screen. The bar hides after 3 seconds and comes back when the pointer touches the top edge. Panels open over the Palm instead of widening the window.

Shortcuts work in the Palm window and in Frond's: Ctrl+S Save, Ctrl+Shift+S Save a copy, Ctrl+W Save and Stop, Ctrl+I Install, Ctrl+M Sound, Ctrl+B Hide bar, Ctrl+L Emulator log, Ctrl+R Reset, F12 Screenshot, Ctrl+Q Quit. With the infrared keyboard attached the Ctrl keys go to the Palm, and only F11 and F12 still come back to Frond.

- Applications lists the installed apps, each with Launch, Export and Delete. Double-click launches. Dropping PRC and PDB files on the window installs them.
- Card has the card controls below. Display has Contrast (Palm IIIx), Sound, Microphone and Rotate. Connections has infrared beaming, Bluetooth, serial, USB HotSync, HotSync and Pair with another Frond…, which opens Pairing.

### Apps

Under Applications (the Linux panel or the Android sheet):

- **Install…** takes PRC and PDB files (several on Linux and Android), once Palm OS has started. As after a HotSync, an app installed while the launcher shows appears in it at once, and the app in front stays.
- **Launch** and **Export** on an app's row start it or write its database as a ZIP. **Delete** asks first, then removes the app and the databases with its creator. The Palm refuses the app that is open or one in ROM, and leaves its data.
- **Other…** takes any database name, to launch or export it. Use it for an open system database, like `Shim Logs`, that stops the full export.
- **Export all…** writes a ZIP of everything in RAM, other apps and your data included.

Close Palm dialogs first if an install or export can't get through.

## Settings

Set these when you add a machine, or in Edit settings while it's stopped. On Android most are in the Display sheet while it runs too.

| Setting | What it does |
|---|---|
| Size (Linux) | 1× to 2× in quarter steps. It starts at the largest that fits the screen. |
| Size (Android) | Fit, Whole steps or Fill. |
| Top bar (Android) | Auto-hide or Own space. See above. |
| Smooth pixels | Softens the LCD. Pixels are sharp by default. |
| Show keyboard, Jog controls | TG50 only. Jog controls are Hidden, Left of screen or Left of keyboard. |
| Sound | Turns Palm sound on or off. It can also be switched in the Display panel or the bar. |
| Low-latency audio | Less delay between a tap and the sound. See below. |
| Contrast | Palm IIIx only. 0 to 100, default 50, lower fades the ink and higher darkens the panel. It's in the Display panel too and is saved with the machine. |
| Speed | Palm IIIx only. Original runs at the real 16.58 MHz, Uncapped lets the host run it as fast as it can (timers stay real time). On Android it's in the Display sheet too and applies at once. |
| Engine | Exact or Fast. It's hidden: on Linux set `FROND_ENGINE_SETTING=1`, on Android it shows in debug builds. See [performance.md](performance.md). The IIIx only has Exact, so it never shows there. |

Android also has Settings in the home menu: Orientation (follow the phone, portrait or landscape), the gdb server and its port, and the controller keys.

The button bars and silkscreens are drawn at the chosen size. The E3 uses the E2 button bar, with its input area drawn by Palm OS.

### Low-latency audio

Off by default. On the T|X, E2, E3 and Zire 72 with their qualified firmware, it plays one open 44.1 kHz stereo stream with less delay, for music apps like SunVox, and goes back to normal audio when your computer or phone can't keep up, which can pause the sound for a moment. Anything else, like several streams or other formats, uses normal audio.

On the TG50, and on a T|X, E2, E3 or Zire 72 with other firmware, it keeps a shorter audio queue while sound is playing. Set it before starting the machine. Diagnostics are in [performance.md](performance.md).

## Buttons, keys and controllers

Click or tap the buttons under the screen and the silkscreen areas. Click and hold to hold a button, and chords of different buttons work. Holds are released when the window loses focus.

Keys, with the infrared keyboard detached:

| Key | T\|X | E2 / E3 | Zire 72 | TG50 | IIIx |
|---|---|---|---|---|---|
| Arrows, Enter | five-way, select | five-way, select | five-way, select | scroll rocker | scroll rocker (up and down) |
| F1 | Home | Calendar | Calendar | Date Book | Date Book |
| F2 | Calendar | Contacts | Contacts | Address | Address |
| F3 | Contacts | Tasks | Music | To Do | To Do List |
| F4 | Web | Note Pad | Camera | Memo Pad | Memo Pad |
| F5 | | | Voice button (hold to record) | Voice button | |
| F6 | | | | Home | |
| Escape | Power | E2: Power | Power | Power | Power |

On the TG50 type with the computer keyboard, left Alt is Blue and right Alt is Red. Its jog wheel, jog press and Back are on the side tab. Help → Hardware Keyboard Controls lists the same keys.

The IIIx has the four app buttons, the scroll rocker and power under the screen, and the Applications, Menu, Calculator, Find and Graffiti areas on its silkscreen. Typed text from the computer keyboard goes into the Palm. Hold power for about two seconds to turn the backlight on or off. The cradle's HotSync button is HotSync in the Connections panel (Linux) or in the Connections sheet (Android).

**Hold** (TG50) latches the device's Hold switch, which turns off the screen and controls, and audio keeps playing. It's saved with the machine.

On Android a game controller presses the Palm's buttons. The d-pad and left stick are the five-way, A is select, L1, X, Y and R1 are the four app buttons and Select is Menu. To change them go to Settings → Controller → Set controls, tap a Palm button, then press the controller key. Reset to defaults puts them back.

## Storage cards

The T|X, E2, E3 and Zire 72 take an SD card, the TG50 a Memory Stick. A card is its own thing, kept in the Cards drawer, and sits in one machine at a time. Delete a machine and its card stays in the drawer.

| | T\|X | E2 / E3 | Zire 72 | TG50 |
|---|---|---|---|---|
| Card | SD | SD | SD | Memory Stick, Memory Stick Pro |
| New card sizes | 32 MB to 2 GB | 32 MB to 1 GB | 32 MB to 2 GB | 4 to 128 MB, Pro 256 MB to 2 GB |
| Images it takes | up to 2 GB | up to 1 GB | up to 2 GB | `.frondms`, Pro as raw `.img` |
| Import folder as card | yes | yes | yes | Pro only |

SDHC isn't supported, so 2 GB is the top. A new 2 GB card is 1886 MB, like a real one, and Palm OS formats it. New cards are blank, so let Palm OS format them. On the TG50 a blank stick gets Palm OS's Unrecognized Format prompt, and a Pro stick needs a saved machine.

### Cards

Open Cards from the main menu on Linux, or from the home menu on Android. SD cards and Memory Sticks are listed by name with their size, and the marker lights while a card is in a machine. A linked card lists its machines.

- **New card** asks for a name, a type and a size. Cards over 128 MB need room for the whole card, with progress and Cancel.
- **Import card image** takes a raw `.img` (SD or Pro) or a `.frondms`. The type comes from the file, and you're asked when SD and Pro both fit.
- **Import folder as card** (Linux: a button in the Cards window, Android: the top menu) copies a folder from your computer onto a new card once, with its folders and long names. Pick the folder, then a name, type and size. The smallest size it fits is chosen, and smaller ones show Too small. A folder that is too big for any card, or has names a card can't hold, is refused. It needs mtools on Linux (`sudo apt install mtools`). Classic Memory Sticks aren't offered, since only the Palm writes their file system. Later changes to the folder don't reach the card.
- The card's menu has Files (not for a classic stick), Rename, Duplicate, Export image, Link to machines, Take out and Delete. Export image writes `.img`, or `.frondms` for a classic stick. Delete asks first and the card's files are lost. Files, Duplicate, Export image, Take out and Delete wait while the card's machine runs. If Files finds an unfinished change that doesn't match the card, it offers to discard that change.
- **Link to machines** lets several machines share one card. Only one of them holds it at a time: the others show "Games is in Palm TX". Take it out of one machine and another can take it with Insert.
- **Take out** puts a card back in the drawer.

In a machine, the SD card or Memory Stick row shows the card's name or No card. Tap it to pick a card from the drawer, greyed when it is in another machine or too big for the device, or make a new one there. Changing the card ejects the old one first (close files on the Palm first, as Frond can't flush them), and the old card goes back to the drawer. A picked card is inserted at once.

Duplicating a machine with a card asks what to do with it: Link (the copy shares the card), Copy (a copy of the card) or No card. A machine's Export packs its card, and importing it makes a new card in the drawer.

Controls in the Card panel (Linux) or the Card sheet (Android) (only the ones that fit the state show):

- **Eject** and **Insert** take the card out and put it back.
- **Export image** (once ejected) writes the card as an image.
- **Write protected** (Memory Stick) is set while ejected, and goes with the card.
- **Files** opens the card's files, see [Files](#files).

Don't eject or sleep the Palm while an app is writing quickly, as the SD driver can get stuck until you reset. FAT32 cards imported from `mkfs.fat` can be read but not written by the T|X.

### Files

SD cards and Memory Sticks: Files opens the card in a file manager and changes it in place. In Cards it's on the card's menu, for a card in the drawer or in a machine that isn't running, and a card in a running machine shows Files… · In use. For the card of a running machine use Files in the Card panel or sheet: it ejects the card (close files on the Palm first), and closing Files puts it back in, ejected again if it was out before.

- Open a folder by double-clicking it (tap on Android), go back with Up or the path bar, and jump to `/PALM/Launcher` or `/DCIM`. The top line shows the free space and the card's size.
- Sort by name, size or date. Select several with Ctrl or Shift, or Select all.
- **Add files** and **Add folder** copy from your computer, folders with everything in them. On Linux you can also drop files and folders on the list, and drag them out of it.
- **Export…** copies the selected files and folders to a folder on your computer. A name already there gets " (2)", nothing is overwritten.
- **Cut**, **Copy** and **Paste** move or copy on the card. **Rename**, **New folder** and **Delete** (it asks first) work as usual.
- When a name is already there you pick Replace, Skip or Keep both, with For all for the rest. Long jobs show progress with Cancel.
- Each change is written as you make it, and only what it touches. If Frond is stopped part way, the card is fixed the next time it's opened, with the change done or not.
- A write protected card can be browsed and exported only. While a card is open in Files it can't be moved, taken out or deleted. Cards for Files are FAT16, 32 MB and up.

A classic Memory Stick opens in Files only when its last write is finished; if not, put it in the TG50 once. A blank stick is formatted the way the TG50 does it the first time Files opens it.

## Connections

Each device only gets the connections it really has. A control it doesn't have yet is greyed out, with "Not available on the <device> yet". The Zire 72 has no serial port, so that row isn't shown.

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

Network is the T|X only. The Palm IIIx has no network setting, it connects over PPP on its serial port, see [Palm IIIx serial and PPP](#palm-iiix-serial-and-ppp). Choose a mode before opening a network app:

- Virtual Wi-Fi (Frond): turn on Wi-Fi in Palm OS, scan and connect to the open **Frond** network. A virtual router gives it an address, DNS and IPv4 through the computer's or phone's connection, with no root, bridge or Wi-Fi adapter needed. The router is 10.0.2.2, so use that for a service on your computer. Blazer loads HTTP pages, but its old HTTPS is refused by most sites. For Network HotSync run `pilot-xfer -p net:127.0.0.1 -l` and enter 10.0.2.2 as the PC address in the Palm's HotSync setup.
- Internet via host: forwards some 68k NetLib calls to host sockets. Blazer's Wi-Fi workflow doesn't work this way, use Virtual Wi-Fi.
- Simulated echo server, Simulated connection timeout and Simulated disconnection are fixtures for testing apps. The echo server answers on `frond.test` port 7.

The mode is saved. Live connections close when the machine resumes, so reload or reconnect in the app.

### Serial port

The serial port (the T|X, E2 and E3 connector, the TG50 and the IIIx cradle) goes to a PTY or a TCP port. Open the path with `screen` or `minicom`, or connect to the port, for example `nc 127.0.0.1 5023`. Peers get CTS, DSR and DCD while connected, RTS flow control is honoured, and bytes sent with no peer are dropped. On Android use Wait for a peer or Connect to an address, then `link-pty.py PHONE:5033 --link /tmp/palm-serial` on a computer.

Serial HotSync works on the E2 with pilot-link (`pilot-xfer -p /dev/pts/N -l`): put the serial port on a PTY, press HotSync in the Connections panel (Linux) or in the Connections sheet (Android) on the E2 or T|X, which pulls the connector's HotSync line low for half a second like the cradle button. The Palm starts HotSync and pilot-link answers. The T|X's serial HotSync isn't tested.

### Palm IIIx serial and PPP

The IIIx's serial port is the cradle connector, set to Off, a PTY or a TCP port like the other serial ports. Serial HotSync works with pilot-link on that path (`pilot-xfer -p /dev/pts/N -l`): press HotSync in the Connections panel, then start HotSync on the Palm. Infrared beaming works as on the other devices.

The IIIx reaches the network the way the real one did, with PPP over the serial port to a PPP server on the host. Frond doesn't redirect NetLib for it. Put the serial port on a PTY or TCP port, from the command line:

```sh
out/frond-68k iiix.frond --serial pty     # prints: serial: pty /dev/pts/N connected
out/frond-68k iiix.frond --serial tcp:5000
```

Host with slirp (no root). Install Debian's `slirp` package (and `socat` for a TCP serial port), then run slirp on the port:

```sh
tools/palm-pppd.sh /dev/pts/N
tools/palm-pppd.sh tcp:5000
```

slirp gives the guest 10.0.2.15 and DNS through 10.0.2.3, and makes the guest's connections from the host as an ordinary user, so nothing else needs setting up. Add `--log DIR` to keep slirp's debug log and its packet and connection counts in `DIR/slirp_debug`. The script needs a C compiler: Debian's 64-bit slirp exits on its first idle wait, and `tools/slirp-select-fix.c` is preloaded to fix that. The second argument is the speed, 57600 by default.

Host with pppd (root). pppd needs root, or the `dip` group and `/dev/ppp` (the `ppp_generic` module):

```sh
sudo tools/palm-pppd.sh --pppd /dev/pts/N
```

The guest gets 10.68.0.2, the host is 10.68.0.1 and the DNS server is the host's first one from `/etc/resolv.conf`. For the guest to reach past the host, turn on IP forwarding and NAT, for example:

```sh
sudo sysctl net.ipv4.ip_forward=1
sudo iptables -t nat -A POSTROUTING -s 10.68.0.2 -j MASQUERADE
```

On the Palm, on OS 3.5 and later:

1. In Prefs, Network, pick a service (or make a new one) and set Connection to Direct Serial. Leave the user name empty.
2. In Details, connection type PPP, Query DNS on, IP address automatic, and an empty login script.
3. Tap Connect. The panel shows the connection come up and the button turns into Disconnect.

On 3.1 set the modem to Direct Connect if it's offered and the phone to `00`.

Programs that use NetLib, like a web clipping app or a telnet client, then work until you tap Disconnect or the machine sleeps. They also dial by themselves when the service isn't connected yet.

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

| | T\|X | E2 | Zire 72 | TG50 | IIIx |
|---|---|---|---|---|---|
| Sound | yes | yes | yes | yes, see below | yes, piezo |
| Low-latency audio | yes | yes | yes | shorter queue | |
| Microphone | | | yes | yes | |
| Network | yes | | | | PPP over serial |
| Serial port | yes | yes | not offered | yes | yes |
| USB HotSync | yes | yes | yes | yes | |
| Infrared beaming | yes | yes | yes | yes | yes |
| Infrared keyboard | yes | yes | yes | yes | |
| Bluetooth | yes | yes | yes | yes | |
| Camera | | | no sensor | | |
| Hold | | | | yes | |
| Card | SD | SD | SD | Memory Stick, Pro | none |

The Zire 72's Camera shows a white screen and goes back to the launcher. Its microphone is the built-in one that Voice Memo uses, with no choice of input. The TG50 plays beeps, alarms and Sony Voice Recorder audio. Standard Palm SndStream (not in its firmware), the remote control, MagicGate and Memory Stick audio aren't there. Serial works at 9600, 57600 and 115200 baud. HotSync over the serial port isn't tested on the TG50 or the T|X.

On the IIIx, serial HotSync is tested with pilot-link (install, fetch, backup). It has no Fast engine, no SD slot and no Bluetooth, USB or Wi-Fi, as on the real one. Its backlight shows as a green panel.

Microphone (Zire 72 and TG50) is off each time it starts. Turn it on under Microphone in the ⋮ menu (Android) or Device (Linux) while the machine runs. Android asks for permission the first time and arms it when you return to the device. Saving, file work, stopping, reset, sleep and leaving the app turn it off again. On Linux, hiding the device window does too.

## Troubleshooting

- Gaps in sound on a PipeWire desktop: check that PipeWire's real-time threads get priority. `systemctl status rtkit-daemon` and `ps -eLo pid,tid,cls,rtprio,comm | grep -E 'pipewire|data-loop|frond'` show it.
- If `journalctl --user -u pipewire -u xdg-desktop-portal` reports `Could not get pidns`, tell PipeWire to use RTKit directly. Put `module.rt.args = { rtportal.enabled = false }` in `~/.config/pipewire/pipewire.conf.d/90-direct-rtkit.conf`, `~/.config/pipewire/pipewire-pulse.conf.d/90-direct-rtkit.conf` and `~/.config/pipewire/client.conf.d/90-direct-rtkit.conf`, then run `systemctl --user restart pipewire pipewire-pulse wireplumber`, which briefly interrupts audio in other apps. Remove the files and restart to undo it.
- Low-latency audio drops back to normal on starvation until the next reset. Use normal audio for projects that need a bigger queue.
- A card that stops responding after an eject or sleep during writes: reset the Palm. The card image is intact.
- "This session is open in another Frond": close the other window, or it's still running.
- Diagnostics and logs for bug reports: Emulator log, and [performance.md](performance.md) for speed and audio traces.
