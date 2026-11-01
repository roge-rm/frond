# TG50 Memory Stick storage

The Sony CLIÉ TG50 supports one classic 128 MB Memory Stick. Create a card
under Memory Stick, then let Palm OS format it. Eject the card before exporting
its image, replacing it or changing Write protected. Insert puts the ejected
card back. Sessions retain the card, its physical extra bytes and protection.

Frond uses `.frondms` images, with a 64-byte versioned header and 136,577,024
physical bytes. Each of 8,192 blocks has 32 pages, each with 512 data bytes and
nine extra bytes. Arbitrary Memory Stick dumps, SD images and FAT sector images
cannot be inserted directly. Export image produces a portable Frond card image.

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

Memory Stick Pro, MagicGate and Memory Stick audio are outside this classic
storage implementation. The TG50 remains experimental. Its observed serial
configuration protocol starts with erased FFFF words; pending configuration
writes pause with the existing guest sleep clock. Hardware write timing is
not qualified.

Firmware-free checks include `heavy build make test-memory-stick-media` and
`heavy build make test-memory-stick-controller`. Full reconstruction uses
`heavy build make check-reconstruction`.
