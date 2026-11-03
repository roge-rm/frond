# TG50 Memory Stick storage

The Sony CLIÉ TG50 takes one classic Memory Stick of 4, 8, 16, 32, 64 or
128 MB, or a Memory Stick Pro of 256 MB, 512 MB, 1 GB or 2 GB. Create a card under Memory Stick, pick its size, then let Palm OS
format it. Eject the card before exporting its image, replacing it or
changing Write protected. Insert puts the ejected card back. Sessions retain
the card, its physical extra bytes and protection.

Frond uses `.frondms` images, with a 64-byte versioned header and the stick's
physical bytes. Each page has 512 data bytes and nine extra bytes:

| Size | Block | Pages per block | Blocks | Physical bytes |
|---|---|---|---|---|
| 4 MB | 8 KB | 16 | 512 | 4,268,032 |
| 8 MB | 8 KB | 16 | 1,024 | 8,536,064 |
| 16 MB | 16 KB | 32 | 1,024 | 17,072,128 |
| 32 MB | 16 KB | 32 | 2,048 | 34,144,256 |
| 64 MB | 16 KB | 32 | 4,096 | 68,288,512 |
| 128 MB | 16 KB | 32 | 8,192 | 136,577,024 |

The header names the blocks and pages per block. Frond 0.9.0 and earlier only
take 128 MB sticks. Arbitrary Memory Stick dumps, SD images and FAT sector images
cannot be inserted directly. Export image produces a portable Frond card image.

## Memory Stick Pro

The TG50's own Sony driver already handles Memory Stick Pro, so Pro sticks work
with the stock ROM. New card lists them after the classic sizes as "256 MB Pro"
and so on. They need a saved session, since the stick is a file in its folder.
A new stick comes formatted with the same FAT16 layout as an SD card of that
size; "2 GB" is 1886 MiB like a real 2 GB card. A blank stick gets Palm OS's
Unrecognized Format prompt like a classic one. Sony's Memory Stick Pro update
for the TG50 works too; with it, formatting puts the stick back to the layout
it came with.

A Pro stick has no extra bytes, so Frond keeps it as plain 512-byte sectors in
`ms.frondmsp`, saved like an SD card's `sd.frondsd`, journals included. Export
image writes a raw image (`.img`), and Insert image takes a raw image of one of
the four sizes. Write protected works as on a classic stick.

The TG50 keeps running without drawing while its menu is visible, with audio
paused. Opening a file picker or another app pauses the guest.

Files browses the original Palm OS filesystem. Open Date Book on the Palm and
try again if Sony Launcher is busy. You can add or export files, make folders,
rename entries and delete them. Changes take effect immediately. File names
must use printable ASCII, paths are at most 511 bytes and each transfer is at
most 8 MiB. Existing names are refused, including case-only duplicates; Frond
does not overwrite them. Folder listings are limited to 512 entries.

Cancel stops a pending file operation and restores the card before saving.
Closing Files or stopping the machine also cancels a pending operation.
On Android, cancellation is available while Frond reads or transfers a file;
it ends when writing the exported document begins. A successful card change
remains successful while the folder listing refreshes.

The original Sony driver handles physical block mapping, flash programming,
spare blocks and filesystem writes through the CXD3452 controller. Files uses
the original guest VFS, without a host FAT editor. Protected cards allow reads
and exports. Original Palm applications receive a read-only error on writes.
Disk-full failures and cancelled writes restore physical data and extra bytes.

MagicGate and Memory Stick audio aren't emulated. The TG50 remains experimental. Its observed serial
configuration protocol starts with erased FFFF words; pending configuration
writes pause with the existing guest sleep clock. Hardware write timing is
not qualified.

Firmware-free checks include `heavy build make test-memory-stick-media` and
`heavy build make test-memory-stick-controller`. Full reconstruction uses
`heavy build make check-reconstruction`.
