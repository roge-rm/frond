#ifndef _ROTATION_H_
#define _ROTATION_H_

enum class Rotation { portrait_0, landscape_90, portrait_180, landscape_270 };

// The rotation a fitted window draws and maps input with. A device whose frame is its panel turned a
// quarter clockwise into a landscape view (the Zodiac) shows the panel upright in a portrait window:
// the chosen rotation goes a quarter further counter-clockwise there.
inline Rotation windowRotation(Rotation chosen, bool quarterTurn, bool fitted, int windowWidth, int windowHeight) {
    const bool portrait = fitted && windowWidth > 0 && windowHeight > windowWidth;
    return quarterTurn && portrait ? Rotation((int(chosen) + 1) % 4) : chosen;
}

// The quarter turns, on top of the chosen rotation, that show a quarter-turned frame (the Zodiac's)
// upright for the guest OS's orientation in SysGetOrientation's numbers: none for landscape, one for
// portrait (the panel as wired), two for reverse landscape, three for reverse portrait. -1 when unknown.
inline int guestQuarterTurns(unsigned orientation) {
    switch (orientation) {
        case 1: return 1;
        case 2: return 0;
        case 3: return 3;
        case 4: return 2;
        default: return -1;
    }
}

inline const char* guestOrientationName(unsigned orientation) {
    switch (orientation) {
        case 1: return "portrait";
        case 2: return "landscape";
        case 3: return "reverse-portrait";
        case 4: return "reverse-landscape";
        default: return "";
    }
}

#endif  // _ROTATION_H_
