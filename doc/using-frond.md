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

On Linux each machine is a folder, `~/.local/share/frond/sessions/<name>/`, with the session (`<name>.frond`), its settings (`<name>.frond.settings.json`), a lock file, the card files (`sd.frondsd`, `ms.frondms` or `ms.frondmsp`) and, on a device with NAND, its internal NAND (`nand.frondnand`). A machine that was never saved has a `<name>.frond.new` marker instead of the session. Export writes the same folder with the NAND packed as `nand.frondnand.gz`, so keep the settings with it when you move it. A T|X exports at about 43 MB though its folder is about 160 MB, and an E2 at about 9 MB against 40 MB. Android exports the folder as a zip, and both apps import either. Android keeps the same folders in the app's storage. A machine is locked while it's open, so two windows can't overwrite it.

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

- The bar has back (saves and leaves), the machine's name and engine, then Applications, Storage, Sound, Power and the ⋮ menu. Storage shows the card's state (in, out, ejected or none) on devices with a slot. Power only appears where the device has a power key. The keyboard button shows when the infrared keyboard is attached.
- ⋮ lists Hold (TG50), Infrared keyboard, Microphone and Network with their state, then Display…, Connections… and Screenshot, then Hide bar, Save a copy… and Emulator log, then Reset and Hard reset…. Rows the device doesn't have are left out. Screenshot saves a PNG of the Palm to Pictures/Frond.
- Applications, Storage, Display and Connections open as a sheet over the bottom of the screen, or at the side in landscape, so the Palm stays in view. Tap outside it or press back to close it. Files… opens full screen.
- Display has the display settings, Rotate and Audio. Connections has infrared, Bluetooth, serial, USB HotSync, the debugger and HotSync.
- Hide bar leaves a small handle at the top edge. Tap it or swipe down on it to bring the bar back. Whether the bar shows is remembered for each machine. In landscape the bar is shorter.
- Top bar (in Edit settings and the Display sheet) is Auto-hide or Own space. Auto-hide puts the bar over the Palm and hides it after a few seconds without a touch. Own space moves the Palm down to leave room for it, in portrait and landscape. With little room left, the bar goes over the Palm instead.

### Linux bar and panels

On Linux the bar sits at the top of Frond's window while a machine runs, above a Machine menu with the same rows as ⋮ on Android (Hard reset… is red). The Palm is inside Frond's window under the bar (on X11, and on Wayland through XWayland). Applications, Storage, Display and Connections open as a panel at the right of the Palm and the window grows to the right, so the Palm keeps its size. Hide bar leaves a thin strip with a tab at the top, and the choice is remembered for each machine. The Machine menu shows each row's keys. Display has Size, Smooth pixels, Show keyboard and Jog controls (TG50) and Speed (Palm IIIx), which change while the Palm runs. A click on the Palm gives it the keys and a click in a panel takes them back, wherever the pointer is when you type; switching to another window and back keeps the choice. If the Palm's window can't be embedded, it stays in its own window, with the bar and the panels in Frond's window. Ctrl+1 to Ctrl+4 open and close them, and so does the bar button or the panel's ✕.

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
| Speed | Original or Uncapped. On the Palm IIIx, Original runs at the real 16.58 MHz and Uncapped lets the host run it as fast as it can (timers stay real time); on Android it's in the Display sheet too and applies at once. On the other devices Original runs at the device's own MIPS (150 on the T|X and Zire 72, 96 on the E2 and TG50, scaled from the 312 MHz figure by clock) and Uncapped passes `--mips 1000`; both apply at the next start. |
| Engine | Exact or Fast. See [performance.md](performance.md). The IIIx only has Exact, so it never shows there. |

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

### Dana screen and keyboard

Dana keeps its wide screen in both phone orientations. Screen zoom and the
onscreen keyboard have separate sizes. Choose Fit to see the whole screen, or
a larger zoom for reading. Select Pan to drag the view; select Touch to use the
Palm's pen. On desktop, the middle mouse button pans and the wheel zooms while
panning. The machine keeps your zoom and position when the window changes size.

The keyboard choices are Automatic, Compact, Full Dana and Hidden. Automatic
uses the compact keyboard in a narrow window, the full keyboard when it fits,
and hides it when a physical keyboard is detected. Desktop starts with the
onscreen keyboard hidden. Full Dana remains available as a manual choice.

On the compact keyboard, use the symbols page for punctuation and Fn for Dana's
app and function keys. Tap a modifier once for the next key, again to hold it,
then once more to release it. The full keyboard uses held keys, including
multi-finger chords. Connecting a physical keyboard does not override an explicit
onscreen keyboard choice.

These choices are saved with the machine. Android's Orientation setting chooses
portrait, landscape or following the phone; it does not rotate the guest image.
Panning is manual.

## Storage cards

The T|X, E2, E3 and Zire 72 take an SD card, the TG50 a Memory Stick. A card is its own thing, kept in the Cards drawer, and sits in one machine at a time. Delete a machine and its card stays in the drawer.

While a machine runs, Storage (the bar button, Ctrl+2 on Linux) lists its card slot, where the device has one, and Internal, the Palm's own databases. Each has Files…, the same file manager, see [Files](#files).

| | T\|X | E2 / E3 | Zire 72 | TG50 |
|---|---|---|---|---|
| Card | SD | SD | SD | Memory Stick, Memory Stick Pro |
| New card sizes | 32 MB to 2 GB | 32 MB to 1 GB | 32 MB to 2 GB | 4 to 128 MB, Pro 256 MB to 2 GB |
| Images it takes | up to 2 GB | up to 1 GB | up to 2 GB | `.frondms`, Pro as raw `.img` |
| Import folder as card | yes | yes | yes | Pro only |

SDHC isn't supported, so 2 GB is the top. A new 2 GB card is 1886 MB, like a real one, and Palm OS formats it. New cards are blank, so let Palm OS format them. On the TG50 a blank stick gets Palm OS's Unrecognized Format prompt, and a Pro stick needs a saved machine.

### Dana cards

While Dana runs, open Cards 1 and 2 on desktop, or the Card 1 and Card 2 rows on
Android. Each slot takes its own SD or MMC card. Choose the family when creating
or importing an image, and choose Write protected before attaching it. The SD
size choices are 32, 64, 128, 256 and 512 MB; MMC offers 32, 64, 128 and 256 MB.
Eject a card before removing it. Export writes that card as a `.frondsd` image.

The machine's normal session preserves both attachments, including ejected and
write-protected cards. Desktop can export a native portable session folder while
the machine is open. Keep that folder intact; moving it to another filesystem or
copying its files individually is unsupported.

Android can export each card image. Copying or importing a whole Dana session
with attached cards is not supported there yet. Those operations are refused
without discarding either card. Dana sessions with no attached cards keep the
ordinary session copy and import paths.

### Dana USB port

The Dana has one external USB port, and USB port in Connections (desktop panel, Android sheet) picks what is plugged into it: Off, Printer or Wi-Fi adapter. The choice is kept for the machine and applied at every start. Changing it while the Dana runs unplugs one and plugs in the other. The Dana Wireless offers Off and Printer only, as its Wi-Fi is built in, see [Dana Wireless](#dana-wireless).

With Printer, a print from the Dana is collected and "Print received" shows when it stops. Choose Save as PDF (desktop), Print, Save raw file or Discard. Only the Generic printer profile can be shown as pages.

The Dana can also print over infrared, see [Printing over infrared](#printing-over-infrared). With both on, "Print received" says which printer it came from. On Android, when both printers have a print waiting, one notice says "2 prints ready" and Show opens Connections, where each has its own choices.

With Wi-Fi adapter, the Dana sees a USB 802.11b adapter (a Microsoft MN-510) and the readout shows the link, for example "Connected, 10.0.2.15". To set it up, open Network in the Dana's Preferences:

- Make a new service, choose 802.11b and open its settings.
- Network Type is Access Point, and Access Point is "Use first available" or "Use specific AP" with the SSID `Frond`.
- Leave WEP off, or turn it on with any key, as Frond connects either way.
- In the IP settings keep DHCP.
- Connect. The Dana gets 10.0.2.15, the router is 10.0.2.2 and DNS is 10.0.2.3. Use 10.0.2.2 for a service on your computer.

Peer to Peer (ad hoc) can be chosen but finds nobody, so DHCP times out.

### Dana Wireless

The AlphaSmart Dana Wireless is its own device in the device list, with 16 MB of RAM and the Dana's built-in 802.11b module. It uses the same ROM file as the Dana (AlphaSmart-Dana-1.5.rom), the same screen, keyboard, buttons and two card slots, and the same import steps. Choose AlphaSmart Dana Wireless when you add the device, as the ROM itself says Dana. A ROM without the serial number token the Wireless needs is refused when it starts, with the reason shown.

Connections has a Wi-Fi row with On and Off for the built-in module, and a readout like "Connected, 10.0.2.15". It's kept for the machine and sent at every start. The USB port row offers Off and Printer. Set up Network in the Dana's Preferences as in Dana USB port above, using the built-in module; the network, address and limits are the same.

A session belongs to the device it was saved with, so a Dana session opens as a Dana and a Wireless session as a Wireless. Frond doesn't turn one into the other.

### Cards

Open Cards from the main menu on Linux, or from the home menu on Android. SD cards and Memory Sticks are listed by name with their size, and the marker lights while a card is in a machine. A linked card lists its machines.

- **New card** asks for a name, a type and a size. Cards over 128 MB need room for the whole card, with progress and Cancel.
- **Import card image** takes a raw `.img` (SD or Pro) or a `.frondms`. The type comes from the file, and you're asked when SD and Pro both fit.
- **Import folder as card** (Linux: a button in the Cards window, Android: the top menu) copies a folder from your computer onto a new card once, with its folders and long names. Pick the folder, then a name, type and size. The smallest size it fits is chosen, and smaller ones show Too small. A folder that is too big for any card, or has names a card can't hold, is refused. Classic Memory Sticks aren't offered. Later changes to the folder don't reach the card.
- The card's menu has Files, Rename, Duplicate, Export image, Link to machines, Take out and Delete. Export image writes `.img`, or `.frondms` for a classic stick. Delete asks first and the card's files are lost. Files, Duplicate, Export image, Take out and Delete wait while the card's machine runs. If Files finds an unfinished change that doesn't match the card, it offers **Discard unfinished change**, which drops that change and leaves the card as it was before it.
- **Link to machines** lets several machines share one card. Only one of them holds it at a time: the others show "Games is in Palm TX". Take it out of one machine and another can take it with Insert.
- **Take out** puts a card back in the drawer.

In a machine, the SD card or Memory Stick row shows the card's name or No card. Tap it to pick a card from the drawer, greyed when it is in another machine or too big for the device, or make a new one there. Changing the card ejects the old one first (close files on the Palm first, as Frond can't flush them), and the old card goes back to the drawer. A picked card is inserted at once.

Duplicating a machine with a card asks what to do with it: Link (the copy shares the card), Copy (a copy of the card) or No card. A machine's Export packs its card, and importing it makes a new card in the drawer.

Card controls in the Storage panel (Linux) or the Storage sheet (Android) (only the ones that fit the state show):

- **Eject** and **Insert** take the card out and put it back.
- **Export image** (once ejected) writes the card as an image.
- **Write protected** (Memory Stick) is set while ejected, and goes with the card.
- **Files** opens the card's files, see [Files](#files).

Don't eject or sleep the Palm while an app is writing quickly, as the SD driver can get stuck until you reset. FAT32 cards imported from `mkfs.fat` can be read but not written by the T|X.

### Files

SD cards and Memory Sticks: Files opens the card in a file manager and changes it in place. In Cards it's on the card's menu, for a card in the drawer or in a machine that isn't running, and a card in a running machine shows Files… · In use. For the card of a running machine use Files in the Storage panel or sheet: it ejects the card (close files on the Palm first), and closing Files puts it back in, ejected again if it was out before.

- Open a folder by double-clicking it (tap on Android), go back with Up or the path bar, and jump to `/PALM/Launcher` or `/DCIM`. The top line shows the free space and the card's size.
- Sort by name, size or date. Select several with Ctrl or Shift, or Select all.
- **Add files** and **Add folder** copy from your computer, folders with everything in them. On Linux you can also drop files and folders on the list, and drag them out of it.
- **Export…** copies the selected files and folders to a folder on your computer. A name already there gets " (2)", nothing is overwritten.
- **Cut**, **Copy** and **Paste** move or copy on the card. **Rename**, **New folder** and **Delete** (it asks first) work as usual.
- When a name is already there you pick Replace, Skip or Keep both, with For all for the rest. Long jobs show progress with Cancel.
- Each change is written as you make it, and only what it touches. If Frond is stopped part way, the card is fixed the next time it's opened, with the change done or not.
- A write protected card can be browsed and exported only. While a card is open in Files it can't be moved, taken out or deleted. Files reads and writes FAT12 and FAT16, and refuses a FAT32 card.

A classic Memory Stick opens in Files only when its last write is finished; if not, put it in the TG50 once. A blank stick, new or put in a TG50 but never formatted, is formatted the way the TG50 does it the first time Files opens it.

Internal lists the databases in the Palm's RAM by file name, with their type and creator, while the machine runs. Add files installs PRC and PDB files (on Linux a dropped folder installs the files in it), Export… writes the selected databases to a folder as PRC and PDB files, and Delete removes them. There are no folders, so Add folder, Cut, Copy, Paste, Rename and New folder aren't there. As in Applications, the Palm refuses a database that is open or in ROM.

## Connections

Each device only gets the connections it really has. A control it doesn't have yet is greyed out, with "Not available on the <device> yet". The Zire 72 has no serial port, so that row isn't shown.

### Connections between Fronds

A connection is a link between two Frond installs on the same network, paired once. A machine's Infrared, Serial or Bluetooth setting can name a connection, and then the two Palms talk to each other through it, for example beaming a memo from a Tungsten E2 on a phone to a T|X on your computer. One link carries all the channels between two installs, and it only runs while a machine wants it.

Open Connections from the File menu on Linux or the Home ⋮ menu on Android. At the top, This Frond is the name other installs see, with Rename. Each install picks a name the first time. Under Connections are the paired installs, each with a status:

- Connected, with the round trip time and what it carries, for example "Connected · 4 ms · Infrared: Palm TX – Tungsten E2".
- Seen at 192.168.1.20, which means it's on the network but nothing is using the link.
- Not seen.
- Key changed, pair again, see below.
- Not paired on <name>, pair again, which means that install forgot you.

On Linux "This computer" is the first row. It joins machines running in different windows on the same computer, needs no pairing and is always there.

A row's menu (right-click on Linux, ⋮ on Android) has Rename, Address…, Bluetooth radio and Forget. Rename is only your name for it. Address… sets an address to try when the install isn't found by itself, and Forget removes the pairing, after which the other install shows "pair again".

To pair, open Connections on both. The other install shows up under Nearby, and Pair (or Add by address… if it isn't listed, with an address like `192.168.1.20`) asks it for a code. It shows a 6-digit code, for example "dan-laptop wants to pair. Code 482 913", and you type that code in on the first one ("Enter the code shown on Pixel 8"). The code lasts 2 minutes. After 3 wrong tries, or once the code runs out, that install won't take a new pairing for a minute. Pairing swaps keys, and every later link checks them. If an install's key doesn't match, the row says "Key changed, pair again" and Frond doesn't link until you pair again. That happens after a reinstall, and it can also mean someone else is using that address.

Frond links over TCP and UDP port 5035. It only takes links from private, link-local and ULA addresses (`192.168.x.x`, `10.x.x.x`, `fe80::`, `fd..::` and the like), so a VPN works if it gives out addresses like those. A firewall that blocks 5035 on either computer shows the other as "Not seen". Allow both TCP and UDP on 5035.

Android asks for the local network permission when Connections opens and when a machine first starts with a connection set. Without it the status line says "local network access is off".

#### Using a connection

In a machine's settings (Edit settings… on Linux, Machine settings on Android) and in the Connections panel while it runs, the Infrared, Serial and Bluetooth rows list Off, the local choices, then the connections by name:

- Infrared and Serial: on Linux Off, Terminal (PTY), Wait for a peer, then This computer and the paired installs. On Android Off, then the paired installs.
- Bluetooth: Off, This Frond, then the paired installs. On Linux there is also Controller socket. This computer isn't listed, as machines on one computer share This Frond's radio.

Pick the same kind on both sides, for example Infrared on the E2 set to your phone's name and Infrared on the phone's machine set to the computer's name. A machine whose connection has been forgotten shows "Unknown connection" and acts as Off. The choice is saved with each machine and applied every time it starts.

The status line says what is happening:

- "Infrared: Pixel 8, joined to Tungsten E2", the other machine is there.
- "waiting for a machine on Pixel 8", the link is up or coming up but nothing on that install is set to you. It joins when the other machine starts.
- "Infrared on Pixel 8 is in use by Palm TX", only one machine per install can use a kind for a connection, and the first one has it. This is "busy".
- "Pixel 8 not found", and "Palm asleep", "Palm paused" or "port closed" when the far side is asleep, in the background or has closed its serial port.
- "key changed, pair again" and "pair again with Pixel 8" as in the list above.

Infrared is byte for byte, so Palm OS's normal Beam works, with the receiver on in both. Serial passes bytes with flow control, and the far Palm's port counts as open only once both ends are joined. Baud rate and the separate RTS and DTR lines aren't carried. A link that drops loses the bytes in flight. Changing the choice on a running machine takes effect at once.

Bluetooth over a connection uses one radio for the pair. Bluetooth radio in the row menu says whose it is, This Frond's or the other's, and Palms on both installs find each other through it. Moving the radio disconnects the controllers and they reconnect on their own. This Frond's radio is `btvirt`, which the Debian package and the Android app include. A source build looks in its build tree, see [Bluetooth](#bluetooth).

### Endpoints

Without a connection, serial, USB HotSync and infrared beaming have an endpoint setting on Linux:

- Off (the default).
- Terminal (PTY), where Frond prints a `/dev/pts/N` path for a terminal program or pilot-link.
- Wait for a peer listens on a port on 127.0.0.1 and takes one client at a time.

The choice is saved with each machine. A status line shows the endpoint and whether a peer is connected. Changing the choice while the machine runs closes the old connection. Nothing from the host is saved in a session, and reset, true sleep and loading a session close the connection.

Android has no PTYs. Its USB HotSync row has Off, Wait for a peer (all addresses, port 5034, and it shows the phone's address) and Connect to an address, to reach a link tool on a computer. Infrared, serial and Bluetooth on Android use connections only.

The link tools stay for scripts and for USB HotSync from a phone:

- `tools/link-irda.py` joins two TCP endpoints. Each is a local port, `HOST:PORT`, or `listen:PORT`.
- `tools/link-pty.py` puts a PTY in front of a port for pilot-link and terminals.
- `tools/link-bluetooth.py` shares one `btvirt`.

A relay waits again after its peer goes, and Connect again retries.

### Network

Network is the T|X only. The Palm IIIx has no network setting, it connects over PPP on its serial port, see [Palm IIIx serial and PPP](#palm-iiix-serial-and-ppp). Choose a mode before opening a network app:

- Virtual Wi-Fi (Frond): turn on Wi-Fi in Palm OS, scan and connect to the open **Frond** network. A virtual router gives it an address, DNS and IPv4 through the computer's or phone's connection, with no root, bridge or Wi-Fi adapter needed. The router is 10.0.2.2, so use that for a service on your computer. Blazer loads HTTP pages, but its old HTTPS is refused by most sites. For Network HotSync run `pilot-xfer -p net:127.0.0.1 -l` and enter 10.0.2.2 as the PC address in the Palm's HotSync setup.
- Internet via host: forwards some 68k NetLib calls to host sockets. Blazer's Wi-Fi workflow doesn't work this way, use Virtual Wi-Fi.
- Simulated echo server, Simulated connection timeout and Simulated disconnection are fixtures for testing apps. The echo server answers on `frond.test` port 7.

The mode is saved. Live connections close when the machine resumes, so reload or reconnect in the app.

### Serial port

The serial port (the T|X, E2 and E3 connector, the TG50 and the IIIx cradle) goes to a PTY or a TCP port. Open the path with `screen` or `minicom`, or connect to the port, for example `nc 127.0.0.1 5023`. Peers get CTS, DSR and DCD while connected, RTS flow control is honoured, and bytes sent with no peer are dropped. On Android the serial port goes to another Frond through a connection.

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

### Dial-up modem

The Palm IIIx has a snap-on modem on its serial port. Set Serial port to Modem in Edit settings (Linux, or in the Connections panel while it runs) or Machine settings (Android), then set up a connection in Palm OS the way you would for a real modem. Frond answers as a modem on a 33600 line and puts you on the internet through your computer's or phone's connection, with no root. Any number you dial connects. The line driver has to be on, so the modem only works while a program has the serial port open.

1. Palm OS 3.1: the Modem panel's defaults (Standard, 57,600 bps) work. In Prefs, Network set the service's phone number to anything, for example `5551234`.
2. Palm OS 3.5 and 4.1: open Prefs, Network, pick a service and set its Connection to the built-in Palm Modem US/Canada (the Modem settings it comes with work). Set the phone number to anything.
3. In Network details set PPP, Query DNS on, IP address automatic. With the Unix template the user name and password can be anything, with Windows RAS leave the script empty.
4. Tap Connect. The state line under Serial port goes from "Modem: idle" to "Modem: dialling" to "Modem: online at 33600".

Disconnect hangs up. Loading a session saved while online gives NO CARRIER, as if the line dropped, but Palm OS keeps thinking the link is up until you tap Disconnect and Connect in Network. The guest is 10.0.2.15, the router 10.0.2.2 and DNS 10.0.2.3.

### HotSync by modem

On Linux the modem can HotSync with pilot-link two ways. The state line shows the PTY for the first, for example "Modem: idle, HotSync on /dev/pts/5".

- Modem Sync to a number: dial 4687962 (HOTSYNC on a keypad) and the call goes straight to that PTY. Run `pilot-xfer -p /dev/pts/5 -l` (or another pilot-link command), then in the HotSync app tap Modem, enter 4687962 as the phone number, keep Palm Modem US/Canada and tap the HotSync button. With a dial prefix like `9,` it still works.
- Network HotSync: set up a dial-up connection as above. In the HotSync app open Menu, Primary PC Setup and enter 10.0.2.2 as the Primary PC Address and any name (a Modem Sync with `pilot-install-netsync -p /dev/pts/5 -e -n frond -a 10.0.2.2` sets the same). Then tap Modem, Menu, Modem Sync Prefs, Network, then Select Service and pick your Network service. Run `pilot-xfer -p net:127.0.0.1 -l` and tap the HotSync button: the Palm dials the service and reaches pilot-link through 10.0.2.2.

### Mobile phone over infrared

Devices with an infrared phone can use Mobile phone in the Infrared list (Linux, and Android in Machine settings and the Connections sheet) instead of Off or a connection. Frond then plays a phone with a modem, reached over IrCOMM, and the Palm gets the same internet as with the dial-up modem. Detach the infrared keyboard first, as they share the port. Mobile phone needs Palm OS 3.3 or later.

1. Palm OS 3.5: in Prefs, Connection, make a new connection with Via IrCOMM to Modem. Use it from Network as above.
2. Palm OS 4.1: use the built-in IR to Phone connection.
3. Dial any number, with PPP and Query DNS as above, and Connect.

The state line reads "Mobile phone: idle", "linked" once the Palm has found the phone, "dialling", then "online at 33600". It takes about 5 to 20 seconds to come up. Infrared beaming to another Frond is off while Mobile phone is on.

### Mobile phone over Bluetooth

Devices with Bluetooth can use Mobile phone in the Bluetooth list on Linux. It's This Frond's radio with a phone called "Frond Phone" on it, which offers Dial-up Networking and has the same modem and internet as the infrared phone. Machines on This Frond's radio still find each other.

1. In Prefs, Connection, make a connection to a phone via Bluetooth, and find Frond Phone with Device. Pair with the passkey 1234.
2. Dial any number, with PPP and Query DNS as above, and Connect.

The status reads "Bluetooth: This Frond's radio, Mobile phone: idle", then "dialling" and "online at 33600" during a call. The Debian package includes the phone; a source checkout builds it with `make bluetooth-opp-peer` once `btvirt` is built. Android doesn't have it yet.

Modem and phone by device:

| | Modem on serial | Mobile phone on infrared | Mobile phone on Bluetooth |
|---|---|---|---|
| Palm IIIx | yes, Palm OS 3.1, 3.5, 4.1 | yes, Palm OS 3.5 and 4.1 | no |
| T\|X, TG50, E2, Zire 72 | no | yes | yes, Linux |
| Tungsten E3 | no | yes | yes, Linux |

### USB HotSync

Plugs the Palm's USB port into the computer. On Android use Wait for a peer, then `link-pty.py PHONE:5034 --link /tmp/palm-usb` on a computer. Start HotSync on the Palm (Local, Cradle/Cable), then run pilot-link, for example `pilot-xfer -p /dev/pts/7 -l`. The status line shows the bus step, and "enumerated" means it's ready. HotSync takes 15 to 20 seconds to start. pilot-link only opens serial devices, so for a TCP port put a PTY in front with `tools/link-pty.py` (a plain `socat` PTY doesn't work). Changing the setting or loading a session unplugs the cable and plugs it back in.

The common USB host submits the next request in a direction 1 ms after completing
the previous request. This limits transfers to roughly 8–64 kB/s per
endpoint direction, depending on packet size, across all common-host devices.

### Infrared

Infrared beaming links two Frond machines through a connection, see Connections between Fronds above. Detach the infrared keyboard first, as they share the port. Set Infrared on each machine to the other install (or This computer for two windows on one computer) and use Palm OS's normal Beam command. Both Palms must be awake and the receiver needs Beam Receive on.

### Printing over infrared

Every device with infrared can print to Frond as an IrDA printer. Pick Printer in the Infrared list (Linux: the Infrared beaming row, and Edit settings…; Android: Infrared beaming in Connections, and Machine settings). The choice is kept for the machine and applied at every start. Detach the infrared keyboard first, as they share the port.

A print app that finds IrDA printers sees "Frond Printer". On the Dana, PrintBoy does: in its setup choose the IrDA port for your printer, then print as usual. Other IrDA print apps work the same way, and so do apps that print to the infrared serial port (IrCOMM).

Each print is collected and "Print received" shows when it stops, with the same choices as the Dana's USB printer: Save as PDF (desktop), Print, Save raw file or Discard. On Android a notification shows if Frond is in the background. More printing adds to the same job until you choose one. Only the Generic printer profile can be shown as pages, so for a printer's own language (PCL, ESC/P) use Save raw file. Changing the Infrared choice while a print is waiting or still coming in asks first, as it throws the print away. Nothing waiting is saved with the machine.

On Linux you can still put each machine on Wait for a peer with a different port and join them yourself, for scripts:

```sh
python3 tools/link-irda.py 5030 5031
```

`link-irda.py` reconnects when a Palm wakes.

Infrared keyboard (Attached) makes your keyboard a Palm Universal Wireless Keyboard. Install Palm's keyboard driver (version 1.13, not included), reset if it asks, open Keyboard on the Palm and turn it ON. Then type in the Palm window. Left Alt is Fn and right Alt is AltGr, and the layout follows physical US keys, so pick the matching one in the driver. F1 to F4 and Escape keep their button and power uses. The attachment is saved with the machine.

### Bluetooth

Bluetooth is off by default, and uses the original Palm Bluetooth stack against a virtual controller from `btvirt`. Set Bluetooth to This Frond and Frond runs `btvirt` for you, then turn Bluetooth off and on again in Palm OS (Sony Preferences on the TG50). Set Discoverable to Yes on the receiving Palm. The status shows the radio and the link state. Each machine gets its own virtual controller, and only machines on the same radio can see each other, so two machines on one computer set to This Frond find each other. To reach another install, pick a connection, see Connections between Fronds above. Frond never touches a real Bluetooth adapter.

The Debian package and the Android app include `btvirt`. A source checkout builds it once (needs Git, a C toolchain, autotools, pkg-config and the GLib, D-Bus and readline headers):

```sh
python3 tools/build-bluetooth-provider.py --fetch
```

On Linux, Controller socket takes the absolute path of the `bt-server-bredr` socket of a `btvirt` you run yourself, for example `build/bluetooth-sockets/bt-server-bredr` after `install -d -m 700 build/bluetooth-sockets` and `build/bluetooth-provider/emulator/btvirt -s"$PWD/build/bluetooth-sockets"`. Keep that socket private to your user.

Identities: the T|X's address comes from its image, so two clones of one T|X image look like the same device. Use images with different addresses, or mix models. The E2, Zire 72 and TG50 are given a new address each time they connect, so trusted devices don't carry over. The Zire 72 makes one up the first time and keeps it in the session, so don't copy a session to run two.

Tested: discovery, sending a Memo or file by Bluetooth (OPP), PIN and Trusted Devices on all four, and serial (SPP) on the T|X, E2 and TG50. TG50 payload transfers aren't checked on Android. Other Bluetooth networking (LAN access, PAN) and other profiles aren't supported.

## Availability and limits

Dana has a desktop and Android frontend, with its built-in
keyboard, sound, infrared beaming, two independent SD/MMC slots, a USB printer
and a USB Wi-Fi adapter. The Dana Wireless has the same, with built-in Wi-Fi
instead of the adapter. Its USB computer transport is available through the
native localhost debug path. The app's USB computer controls stay disabled.

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
| Mobile phone (infrared) | yes | yes | yes | yes | yes |
| Dial-up modem | | | | | yes |
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
- A full disk or a machine folder that can't be written: on a device with NAND the machine keeps running, Palm OS sees its writes to internal storage fail (an install says out of memory) and the Emulator log says "NAND: a write to the machine's NAND file failed". Free some space, then reset the Palm.
- A card that stops responding after an eject or sleep during writes: reset the Palm. The card image is intact.
- "This session is open in another Frond": close the other window, or it's still running.
- Diagnostics and logs for bug reports: Emulator log, and [performance.md](performance.md) for speed and audio traces.
