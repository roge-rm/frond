# Device button bars and silkscreens

How the button bar under the Palm screen and the printed silkscreen are made, and how to add
them for a new device. The T|X (bar only) and the E2 (bar and silkscreen) are the examples.

## How they're made

There are no image files. Both are drawn at run time as original vector art by the native
core, in `src/uarm/native/DeviceControls.{h,cpp}` and `Silkscreen.{h,cpp}` of the patched
source (`build/source`). The Qt desktop and the Android app only host the SDL runner, so a
new bar shows up on both with no frontend drawing code.

The patches that build them up, in order:

| Patch | What it adds |
|---|---|
| `0027-device-controls` | The T\|X strip, profiles, hit testing, pressed states |
| `0070-tx-button-strip-art` | T\|X redraw from reference photos |
| `0074-e2-device-controls` | E2 strip and silkscreen |
| `0076-device-art-redraw` | Redraw of both at the same quality |

### Profiles (DeviceControls.h)

- `DeviceControlProfile` is one device's bar: its size in strip coordinates (320 wide, the
  T\|X is 320x84 and the E2 320x80), its `DeviceButton`s, the five-way ring and select pill,
  a `DeviceControlStyle` and the app key corner radius.
- `DeviceButton` is a key (`keyIdHard1`..`keyIdHard4`, `keyIdUp`, ..., `keyIdSelect`), its
  rectangle in strip coordinates, a label for tooltips and a `DeviceControlIcon`.
- `deviceControls(DeviceType5)` returns the profile for a device. Add the new one there.
- Hit testing and drawing share the same geometry: app keys hit their whole rounded
  rectangle, the five-way hits the select pill first, then the ring split along its diagonals
  (`deviceNavigatorAt`). So moving a shape moves its hit area too.
- The strip is centred under the screen (`deviceButtonRect`), so a narrower screen needs
  its own strip width.

### Drawing (DeviceControls.cpp)

- Everything is drawn into a `Canvas` with signed distance functions (`box`, `ellipse`,
  `ring`, `segment`, `polygon`, `arc`), then shaded and anti-aliased, at the real window
  scale so it stays crisp at every display scale.
- Each style has its own painter: `paintTxControls`, `paintE2Controls`. A new style adds an
  enum value to `DeviceControlStyle` and a painter. Shared parts like `paintNavigator` and
  `iconDistance` (the button glyphs) are reused where they fit.
- `drawDeviceControls` is the entry point, with the pressed keys as a bit mask, so every
  key needs a visible pressed state.

### Silkscreen (DeviceControls.h, DeviceControls.cpp, Silkscreen.cpp)

- `DeviceSilkscreen` holds the size in LCD pixels (E2 320x120), the `SilkscreenButton`s
  (Home, Menu, Favorites, Find, clock, brightness, keyboard buttons) and the two Graffiti areas.
- Take the rectangles from the ROM, not from a photo: the E2's come from its `'aslk'` 19000
  resource, doubled from Palm OS coordinates to LCD pixels. Taps on the silkscreen go to the
  guest as pen input, so the art has to sit where Palm OS expects the buttons.
- `paintSilkscreen` draws the panel, discs with glyphs (`silkscreenDiscGlyph`), the clock
  and brightness glyphs and the ABC/123 areas. `createDeviceSilkscreen` makes the texture.

### Around the art

- Desktop: the per-device capability list (`capabilities()` in `desktop/FirmwareInput.h`)
  greys out what a device doesn't have yet.
- Android: `PalmButtons.kt` maps on-screen and controller buttons to keys, and `tapPoint()`
  gives the pen positions for Home and Menu where they're silkscreen or status bar taps.
  `Controller.kt` lists each device's buttons. `FrondActivity.BACKGROUND_HEX` is the dark end
  of the bar art and fills around the screen.

## Making one for a new device

1. Gather reference photos and keep them out of the repo, in
   `build/step-art-refs/<device>/` (mode 0700). They're for study only and never bundled.
2. Write the real layout down first: screen size, every hardware button and its place,
   the navigator kind (five-way, rocker, scroll buttons, jog dial), the silkscreen icons and
   Graffiti areas, colours and materials. Parse the ROM for the silkscreen rectangles.
3. Aim for a recognisable approximation, not a close copy: the right button count, shapes,
   arrangement, colours and bezel, with original glyphs. No logos, brand names or
   another emulator's skin.
4. Add the profile and, if it doesn't look like the T\|X or E2, a new style and painter, in a
   new patch after the last one. Keep each key's pressed state and the release on focus loss,
   hiding, closing and rotation.
5. Render the bar and silkscreen to PNGs at 1x and 2x, next to the reference photos, into
   `/srv/downloads/temp/debug/frond/` for Dan to compare. Adjust until it reads as the device
   at a glance.
6. Extend `tests/device-controls.cpp` with the new hit areas (each key, the five-way parts,
   gaps between keys), run `make test-device-controls`, `make -j4 test` and
   `make check-reconstruction`, and check the Qt tests.
7. Map the keys on Android (`PalmButtons.kt`, `Controller.kt`) and set the capabilities on
   the desktop.

Art and pixel work runs on Opus, not a smaller model.
